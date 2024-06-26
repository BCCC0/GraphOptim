// BSD 3-Clause License

// Copyright (c) 2021, Chenyu
// All rights reserved.

// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:

// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.

// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.

// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from
//    this software without specific prior written permission.

// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "graph/view_graph.h"

#include <Eigen/Geometry>

#include "geometry/rotation.h"
#include "rotation_averaging/lagrange_dual_rotation_estimator.h"
#include "rotation_averaging/robust_l1l2_rotation_estimator.h"
#include "rotation_averaging/hybrid_rotation_estimator.h"
#include "translation_averaging/lud_position_estimator.h"
#include "translation_averaging/linear_position_estimator.h"
#include "utils/random.h"

namespace gopt {
namespace graph {

ViewGraph::ViewGraph() {}

bool ViewGraph::ReadG2OFile(const std::string& filename) {
  // A string used to contain the contents of a single line.
  std::string line;

  // A string used to extract tokens from each line one-by-one.
  std::string token;

  // Preallocate various useful quantities.
  double tx, ty, tz, qx, qy, qz, qw;
  double I11, I12, I13, I14, I15, I16, I22, I23, I24, I25, I26,
         I33, I34, I35, I36, I44, I45, I46, I55, I56, I66;

  node_t i, j;
  ViewEdge edge;

  std::ifstream infile(filename);
  if (!infile.is_open()) {
    LOG(ERROR) << "Cannot read g2o file: " << filename;
    return false;
  }

  while (std::getline(infile, line)) {
    // Construct a stream from the string.
    std::stringstream strstrm(line);

    // Extract the first token from the string.
    strstrm >> token;

    if (token == "EDGE_SE3:QUAT") {
      // The g2o format specifies a 3D relative pose measurement in the
      // following form:
      // EDGE_SE3:QUAT id1, id2, tx, ty, tz, qx, qy, qz, qw
      // I11 I12 I13 I14 I15 I16
      //     I22 I23 I24 I25 I26
      //         I33 I34 I35 I36
      //             I44 I45 I46
      //                 I55 I56
      //                     I66
      //

      // Extract formatted output.
      strstrm >> i >> j >> tx >> ty >> tz >> qx >> qy >> qz >> qw >> I11 >>
          I12 >> I13 >> I14 >> I15 >> I16 >> I22 >> I23 >> I24 >> I25 >> I26 >>
          I33 >> I34 >> I35 >> I36 >> I44 >> I45 >> I46 >> I55 >> I56 >> I66;

      edge.src = (i > j) ? j : i;
      edge.dst = (i > j) ? i : j;

      // Fill in elements of the measurement.
      const Eigen::Quaterniond quat(qw, qx, qy, qz);
      const Eigen::AngleAxisd angle_axis =
          (i < j) ? Eigen::AngleAxisd(quat) : Eigen::AngleAxisd(quat.conjugate());

      edge.rel_translation = Eigen::Vector3d(tx, ty, tz);
      if (i > j) {
        edge.rel_translation = -angle_axis.toRotationMatrix() * edge.rel_translation;
      }
      edge.rel_rotation = angle_axis.angle() * angle_axis.axis();
      AddEdge(edge);
    } else if (token == "VERTEX_SE3:QUAT") {
      // This is just initialization information, so do nothing.
      continue;
    } else {
      LOG(ERROR) << "Unrecognized type: " << token << "!" << std::endl;
      return false;
    }
  }

  infile.close();

  return true;
}

void ViewGraph::WriteG2OFile(const std::string& filename) {
  std::ofstream ofs(filename);
  if (!ofs.is_open()) {
    LOG(ERROR) << filename << " cannot be opened!";
    return;
  }
  
  // Output nodes's data.
  for (const auto& node_iter : nodes_) {
    const image_t node_id =  node_iter.second.id;
    const Eigen::Vector3d& rotation = node_iter.second.rotation;
    const Eigen::Vector3d& position = node_iter.second.position;
    const Eigen::Vector4d qvec = AngleAxisToQuaternion(rotation);
    ofs << "VERTEX_SE3:QUAT " << node_id << " " << position[0] << " "
        << position[1] << " " << position[2] << " " << qvec[1] << " "
        << qvec[2] << " " << qvec[3] << " " << qvec[0] << std::endl;
  }

  // Output edges' data.
  for (const auto& edge_iter : edges_) {
    const auto& em = edge_iter.second;
    for (const auto& em_iter : em) {
      const size_t src = em_iter.second.src;
      const size_t dst = em_iter.second.dst;
      const Eigen::Vector3d& tvec = em_iter.second.rel_translation;
      const Eigen::Vector3d& angle_axis = em_iter.second.rel_rotation;
      const Eigen::Vector4d qvec = AngleAxisToQuaternion(angle_axis);
      ofs << "EDGE_SE3:QUAT " << src << " " << dst << " " << tvec[0]
          << " " << tvec[1] << " " << tvec[2] << " " << qvec[1] << " "
          << qvec[2] << " " << qvec[3] << " " << qvec[0] << " "
          << "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0" << std::endl;
    }
  }

  ofs.close();
}

bool ViewGraph::MotionAveraging(
    const RotationEstimatorOptions& rotation_estimator_options,
    const PositionEstimatorOptions& position_estimator_options,
    std::unordered_map<image_t, Eigen::Vector3d>* global_rotations,
    std::unordered_map<image_t, Eigen::Vector3d>* positions) {
  return RotationAveraging(rotation_estimator_options, global_rotations) &&
         TranslationAveraging(position_estimator_options, positions);
}

bool ViewGraph::RotationAveraging(
    const RotationEstimatorOptions& options,
    std::unordered_map<image_t, Eigen::Vector3d>* global_rotations) {
  std::unique_ptr<RotationEstimator> rotation_estimator =
      CreateRotationEstimator(options);

  InitializeGlobalRotations(options, global_rotations);

  std::unordered_map<ImagePair, TwoViewGeometry> view_pairs;
  ViewEdgesToViewPairs(&view_pairs);

  bool success =
      rotation_estimator->EstimateRotations(view_pairs, global_rotations);

  // Assign global rotations to each node.
  if (success) {
    for (const auto& rotation_iter : *global_rotations) {
      const node_t node_id = static_cast<node_t>(rotation_iter.first);
      nodes_[node_id].rotation = rotation_iter.second;
    }
  }

  return success;
}

bool ViewGraph::TranslationAveraging(
    const PositionEstimatorOptions& options,
    std::unordered_map<image_t, Eigen::Vector3d>* positions) {
  options.Check();
  std::unique_ptr<PositionEstimator> position_estimator =
      CreatePositionEstimator(options);

  InitializeGlobalPositions(positions);

  std::unordered_map<ImagePair, TwoViewGeometry> view_pairs;
  ViewEdgesToViewPairs(&view_pairs);

  std::unordered_map<image_t, Eigen::Vector3d> global_rotations;
  for (const auto& node_iter : nodes_) {
    const node_t node_id = node_iter.second.id;
    global_rotations[node_id] = node_iter.second.rotation;
  }

  bool success =
      position_estimator->EstimatePositions(view_pairs, global_rotations, positions);

  // Assigning global positions to each node.
  if (success) {
    for (const auto& position_iter : *positions) {
      const node_t node_id = static_cast<node_t>(position_iter.first);
      nodes_[node_id].position = position_iter.second;
    }
  }
  
  return true;
}

void ViewGraph::ViewEdgesToViewPairs(
    std::unordered_map<ImagePair, TwoViewGeometry>* view_pairs) {
  for (const auto& edge_iter : edges_) {
    const auto& em = edge_iter.second;
    for (const auto& em_iter : em) {
      const ViewEdge& view_edge = em_iter.second;

      const node_t src = static_cast<image_t>(view_edge.src);
      const node_t dst = static_cast<image_t>(view_edge.dst);

      const ImagePair image_pair =
          (src > dst) ? ImagePair(dst, src) : ImagePair(src, dst);

      TwoViewGeometry two_view_geometry;
      two_view_geometry.visibility_score = static_cast<int>(view_edge.weight);
      two_view_geometry.rel_rotation = view_edge.rel_rotation;
      two_view_geometry.rel_translation = view_edge.rel_translation;

      (*view_pairs)[image_pair] = two_view_geometry;
    }
  }
}

void ViewGraph::InitializeGlobalRotations(
    const RotationEstimatorOptions& options,
    std::unordered_map<image_t, Eigen::Vector3d>* global_rotations) {
  switch (options.init_method) {
    case GlobalRotationEstimatorInitMethod::RANDOM:
      InitializeGlobalRotationsRandomly(global_rotations);
      break;
    case GlobalRotationEstimatorInitMethod::MAXIMUM_SPANNING_TREE:
      InitializeGlobalRotationsFromMST(global_rotations);
      break;
    default:
      LOG(FATAL) << "Initialize method invalid!";
      break;
  }
}

void ViewGraph::InitializeGlobalRotationsRandomly(
    std::unordered_map<image_t, Eigen::Vector3d>* global_rotations) {
  RandomNumberGenerator rng;
  for (const auto& node : nodes_) {
    (*global_rotations)[node.first] = Eigen::Vector3d::Zero();
  }
}

void ViewGraph::InitializeGlobalRotationsFromMST(
    std::unordered_map<image_t, Eigen::Vector3d>* global_rotations) {
  auto graph = this->Clone();
  graph.CountOutDegrees();
  graph.CountInDegrees();
  graph.CountDegrees();
  const auto& degrees = graph.GetDegrees();

  auto& edges = graph.GetEdges();
  for (auto& edge_iter : edges) {
    auto& em = edge_iter.second;
    for (auto& em_iter : em) {
      em_iter.second.weight =
        1.0 / (degrees.at(em_iter.second.src) * degrees.at(em_iter.second.dst));
    }
  }

  const std::vector<ViewEdge> mst_edges = graph.Kruskal();
  const node_t root_node_id = mst_edges[0].src;
  (*global_rotations)[root_node_id] = Eigen::Vector3d::Zero();
  
  SmallerEdgePriorityQueue<ViewEdge> heap;
  for (const ViewEdge& edge : mst_edges) {
    ViewEdge inv_edge(edge.dst, edge.src, edge.weight);
    const Eigen::Matrix3d rel_R = AngleAxisToRotationMatrix(edge.rel_rotation);
    inv_edge.rel_rotation = RotationMatrixToAngleAxis(rel_R.transpose());
    inv_edge.rel_translation = -rel_R.transpose() * edge.rel_translation;
    heap.push(edge);
    heap.push(inv_edge);
  }

  while (!heap.empty()) {
    const ViewEdge& edge = heap.top();
    heap.pop();
    if (global_rotations->count(edge.dst) > 0) {
      continue;
    }

    (*global_rotations)[edge.dst] = geometry::ApplyRelativeRotation(
      (*global_rotations)[edge.src], edge.rel_rotation);
  }
}

void ViewGraph::InitializeGlobalPositions(
    std::unordered_map<image_t, Eigen::Vector3d>* positions) {
  for (const auto& node : nodes_) {
    (*positions)[node.first] = Eigen::Vector3d::Zero();
  }
}

std::unique_ptr<RotationEstimator> ViewGraph::CreateRotationEstimator(
    const RotationEstimatorOptions& options) {
  std::unique_ptr<RotationEstimator> rotation_estimator = nullptr;
  switch (options.estimator_type) {
    case GlobalRotationEstimatorType::LAGRANGIAN_DUAL: {
      rotation_estimator.reset(new LagrangeDualRotationEstimator(
          size_, 3, options.sdp_solver_options));
      break;
    }
    case GlobalRotationEstimatorType::HYBRID: {
      HybridRotationEstimator::HybridRotationEstimatorOptions hybrid_options;
      hybrid_options.sdp_solver_options = options.sdp_solver_options;
      hybrid_options.irls_options = options.irls_options;
      rotation_estimator.reset(
          new HybridRotationEstimator(size_, 3, hybrid_options));
      break;
    }
    case GlobalRotationEstimatorType::ROBUST_L1L2: {
      RobustL1L2RotationEstimator::RobustL1L2RotationEstimatorOptions
          robust_l1l2_options;
      robust_l1l2_options.verbose = options.verbose;
      robust_l1l2_options.l1_options = options.l1_options;
      robust_l1l2_options.irls_options = options.irls_options;
      rotation_estimator.reset(
          new RobustL1L2RotationEstimator(robust_l1l2_options));
      break;
    }
    default:
      break;
  }

  return rotation_estimator;
}

std::unique_ptr<PositionEstimator> ViewGraph::CreatePositionEstimator(
    const PositionEstimatorOptions& options) {
  std::unique_ptr<PositionEstimator> position_estimator = nullptr;
  switch (options.estimator_type) {
    case PositionEstimatorType::LUD: {
      LUDPositionEstimator::Options lud_options;
      lud_options.verbose = options.verbose;
      lud_options.max_num_iterations = options.max_num_iterations;
      lud_options.convergence_criterion = options.convergence_criterion;
      position_estimator.reset(new LUDPositionEstimator(lud_options));
      break;
    }
    case PositionEstimatorType::BATA: {
      // TODO(chenyu): implement BATA position estimator.
      break;
    }
    case PositionEstimatorType::LIGT: {
      position_estimator.reset(new LinearPositionEstimator(
          options.tracks, options.normalized_keypoints));
      break;
    }
    default:
      break;
  }
  return position_estimator;
}

}  // namespace graph
}  // namespace gopt
