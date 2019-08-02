/******************************************************************************
 * Copyright 2019 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

/**
 * @file
 **/

#include "modules/planning/math/discretized_points_smoothing/fem_pos_deviation_sqp_osqp_interface.h"

#include <cmath>
#include <limits>

#include "cyber/common/log.h"

namespace apollo {
namespace planning {

bool FemPosDeviationSqpOsqpInterface::Solve() {
  // Sanity Check
  if (ref_points_.empty()) {
    AERROR << "reference points empty, solver early terminates";
    return false;
  }

  if (ref_points_.size() != bounds_around_refs_.size()) {
    AERROR << "ref_points and bounds size not equal, solver early terminates";
    return false;
  }

  if (ref_points_.size() < 3) {
    AERROR << "ref_points size smaller than 3, solver early terminates";
    return false;
  }

  if (ref_points_.size() > std::numeric_limits<int>::max()) {
    AERROR << "ref_points size too large, solver early terminates";
    return false;
  }

  // Calculate optimization states definitions
  num_of_points_ = static_cast<int>(ref_points_.size());
  num_of_pos_variables_ = num_of_points_ * 2;
  num_of_slack_variables_ = num_of_points_ - 2;
  num_of_variables_ = num_of_pos_variables_ + num_of_slack_variables_;

  num_of_variable_constraints_ = num_of_variables_;
  num_of_curvature_constraints_ = num_of_points_ - 2;
  num_of_constraints_ =
      num_of_variable_constraints_ + num_of_curvature_constraints_;

  // Calculate kernel
  std::vector<c_float> P_data;
  std::vector<c_int> P_indices;
  std::vector<c_int> P_indptr;
  CalculateKernel(&P_data, &P_indices, &P_indptr);

  // Calculate offset
  std::vector<c_float> q;
  CalculateOffset(&q);

  // Calculate affine constraints
  std::vector<c_float> A_data;
  std::vector<c_int> A_indices;
  std::vector<c_int> A_indptr;
  std::vector<c_float> lower_bounds;
  std::vector<c_float> upper_bounds;
  CalculateAffineConstraint(&A_data, &A_indices, &A_indptr, &lower_bounds,
                            &upper_bounds);

  // Set primal warm start
  std::vector<c_float> primal_warm_start;
  SetPrimalWarmStart(&primal_warm_start);

  // Load matrices and vectors into OSQPData
  OSQPData* data = reinterpret_cast<OSQPData*>(c_malloc(sizeof(OSQPData)));
  data->n = num_of_variables_;
  data->m = num_of_constraints_;
  data->P = csc_matrix(data->n, data->n, P_data.size(), P_data.data(),
                       P_indices.data(), P_indptr.data());
  data->q = q.data();
  data->A = csc_matrix(data->m, data->n, A_data.size(), A_data.data(),
                       A_indices.data(), A_indptr.data());
  data->l = lower_bounds.data();
  data->u = upper_bounds.data();

  // Define osqp solver settings
  OSQPSettings* settings =
      reinterpret_cast<OSQPSettings*>(c_malloc(sizeof(OSQPSettings)));
  osqp_set_default_settings(settings);
  settings->max_iter = max_iter_;
  settings->time_limit = time_limit_;
  settings->verbose = verbose_;
  settings->scaled_termination = scaled_termination_;
  settings->warm_start = warm_start_;

  // Define osqp workspace
  OSQPWorkspace* work = osqp_setup(data, settings);

  // Initial solution
  bool initial_solve_res = OptimizeWithOsqp(primal_warm_start, &work);

  if (!initial_solve_res) {
    AERROR << "initial iteration solving fails";
    osqp_cleanup(work);
    c_free(data->A);
    c_free(data->P);
    c_free(data);
    c_free(settings);
    return false;
  }

  // Sequential solution
  int itr = 1;
  bool iterative_solve_res = true;
  double eps = 1.0;
  double last_jvalue = work->info->obj_val;

  while (itr < sqp_max_iter_) {
    // linearize and update matrices
    UpdateAffineConstraint(&A_data, &A_indices, &A_indptr, &lower_bounds,
                           &upper_bounds);
    osqp_update_A(work, A_data.data(), OSQP_NULL, A_data.size());
    osqp_update_bounds(work, lower_bounds.data(), upper_bounds.data());
    iterative_solve_res = OptimizeWithOsqp(primal_warm_start, &work);
    if (!iterative_solve_res) {
      AERROR << "iteration at " << itr << "solving fails with max iter "
             << sqp_max_iter_;
      osqp_cleanup(work);
      c_free(data->A);
      c_free(data->P);
      c_free(data);
      c_free(settings);
      return false;
    }

    // check objective meets sqp_jtol, if return true
    eps = std::abs((last_jvalue - static_cast<double>(work->info->obj_val)) /
                   last_jvalue);
    if (eps < sqp_jtol_) {
      ADEBUG << "objective value converges to " << work->info->obj_val
             << "with eps " << eps << "under jtol " << sqp_jtol_;
      osqp_cleanup(work);
      c_free(data->A);
      c_free(data->P);
      c_free(data);
      c_free(settings);
      return true;
    }

    last_jvalue = work->info->obj_val;
  }

  AERROR << "objective not converged with eps " << eps << "over jtol "
         << sqp_jtol_;
  osqp_cleanup(work);
  c_free(data->A);
  c_free(data->P);
  c_free(data);
  c_free(settings);
  return false;
}

void FemPosDeviationSqpOsqpInterface::CalculateKernel(
    std::vector<c_float>* P_data, std::vector<c_int>* P_indices,
    std::vector<c_int>* P_indptr) {
  CHECK_GT(num_of_points_, 2);

  // Three quadratic penalties are involved:
  // 1. Penalty x on distance between middle point and point by finite element
  // estimate;
  // 2. Penalty y on path length;
  // 3. Penalty z on difference between points and reference points

  // General formulation of P matrix is as below(with 6 points as an example):
  // I is a two by two identity matrix, X, Y, Z represents x * I, y * I, z * I
  // 0 is a two by two zero matrix
  // |X+Y+Z, -2X-Y,   X,       0,       0,       0,       ...|
  // |0,     5X+2Y+Z, -4X-Y,   X,       0,       0,       ...|
  // |0,     0,       6X+2Y+Z, -4X-Y,   X,       0,       ...|
  // |0,     0,       0,       6X+2Y+Z, -4X-Y,   X        ...|
  // |0,     0,       0,       0,       5X+2Y+Z, -2X-Y    ...|
  // |0,     0,       0,       0,       0,       X+Y+Z    ...|
  // |0,     0,       0,       0,       0,       0,       0,       ...|
  // |0,     0,       0,       0,       0,       0,       0, 0,       ...|
  // |0,     0,       0,       0,       0,       0,       0, 0, 0,       ...|
  // |0,     0,       0,       0,       0,       0,       0, 0, 0, 0,       ...|
  // Only upper triangle needs to be filled
  std::vector<std::vector<std::pair<c_int, c_float>>> columns;
  columns.resize(num_of_variables_);
  int col_num = 0;

  for (int col = 0; col < 2; ++col) {
    columns[col].emplace_back(col, weight_fem_pos_deviation_ +
                                       weight_path_length_ +
                                       weight_ref_deviation_);
    ++col_num;
  }

  for (int col = 2; col < 4; ++col) {
    columns[col].emplace_back(
        col - 2, -2.0 * weight_fem_pos_deviation_ - weight_path_length_);
    columns[col].emplace_back(col, 5.0 * weight_fem_pos_deviation_ +
                                       2.0 * weight_path_length_ +
                                       weight_ref_deviation_);
    ++col_num;
  }

  int second_point_from_last_index = num_of_points_ - 2;
  for (int point_index = 2; point_index < second_point_from_last_index;
       ++point_index) {
    int col_index = point_index * 2;
    for (int col = 0; col < 2; ++col) {
      col_index += col;
      columns[col_index].emplace_back(col_index - 4, weight_fem_pos_deviation_);
      columns[col_index].emplace_back(
          col_index - 2,
          -4.0 * weight_fem_pos_deviation_ - weight_path_length_);
      columns[col_index].emplace_back(
          col_index, 6.0 * weight_fem_pos_deviation_ +
                         2.0 * weight_path_length_ + weight_ref_deviation_);
      ++col_num;
    }
  }

  int second_point_col_from_last_col = num_of_pos_variables_ - 4;
  int last_point_col_from_last_col = num_of_pos_variables_ - 2;
  for (int col = second_point_col_from_last_col;
       col < last_point_col_from_last_col; ++col) {
    columns[col].emplace_back(col - 4, weight_fem_pos_deviation_);
    columns[col].emplace_back(
        col - 2, -4.0 * weight_fem_pos_deviation_ - weight_path_length_);
    columns[col].emplace_back(col, 5.0 * weight_fem_pos_deviation_ +
                                       2.0 * weight_path_length_ +
                                       weight_ref_deviation_);
    ++col_num;
  }

  for (int col = last_point_col_from_last_col; col < num_of_pos_variables_;
       ++col) {
    columns[col].emplace_back(col - 4, weight_fem_pos_deviation_);
    columns[col].emplace_back(
        col - 2, -2.0 * weight_fem_pos_deviation_ - weight_path_length_);
    columns[col].emplace_back(col, weight_fem_pos_deviation_ +
                                       weight_path_length_ +
                                       weight_ref_deviation_);
    ++col_num;
  }

  CHECK_EQ(col_num, num_of_pos_variables_);

  int ind_p = 0;
  for (int i = 0; i < col_num; ++i) {
    P_indptr->push_back(ind_p);
    for (const auto& row_data_pair : columns[i]) {
      // Rescale by 2.0 as the quadratic term in osqp default qp problem setup
      // is set as (1/2) * x' * P * x
      P_data->push_back(row_data_pair.second * 2.0);
      P_indices->push_back(row_data_pair.first);
      ++ind_p;
    }
  }
  P_indptr->push_back(ind_p);
}

void FemPosDeviationSqpOsqpInterface::CalculateOffset(std::vector<c_float>* q) {
  q->resize(num_of_variables_);
  for (int i = 0; i < num_of_points_; ++i) {
    const auto& ref_point_xy = ref_points_[i];
    (*q)[2 * i] = -2.0 * weight_ref_deviation_ * ref_point_xy.first;
    (*q)[2 * i + 1] = -2.0 * weight_ref_deviation_ * ref_point_xy.second;
  }
  for (int i = 0; i < num_of_slack_variables_; ++i) {
    (*q)[num_of_pos_variables_ + i] = weight_curvature_constraint_slack_var_;
  }
}

void FemPosDeviationSqpOsqpInterface::CalculateAffineConstraint(
    std::vector<c_float>* A_data, std::vector<c_int>* A_indices,
    std::vector<c_int>* A_indptr, std::vector<c_float>* lower_bounds,
    std::vector<c_float>* upper_bounds) {
  int ind_A = 0;
  for (int i = 0; i < num_of_variables_; ++i) {
    A_data->push_back(1.0);
    A_indices->push_back(i);
    A_indptr->push_back(ind_A);
    ++ind_A;
  }
  A_indptr->push_back(ind_A);

  lower_bounds->resize(num_of_constraints_);
  upper_bounds->resize(num_of_constraints_);

  for (int i = 0; i < num_of_points_; ++i) {
    const auto& ref_point_xy = ref_points_[i];
    (*upper_bounds)[i * 2] = ref_point_xy.first + bounds_around_refs_[i];
    (*upper_bounds)[i * 2 + 1] = ref_point_xy.second + bounds_around_refs_[i];
    (*lower_bounds)[i * 2] = ref_point_xy.first - bounds_around_refs_[i];
    (*lower_bounds)[i * 2 + 1] = ref_point_xy.second - bounds_around_refs_[i];
  }

  for (int i = 0; i < num_of_slack_variables_; ++i) {
    // add constant part of linearization equation
    (*upper_bounds)[i] = 1.0;
    (*lower_bounds)[i] = -1e20;
  }
}

void FemPosDeviationSqpOsqpInterface::UpdateAffineConstraint(
    std::vector<c_float>* A_data, std::vector<c_int>* A_indices,
    std::vector<c_int>* A_indptr, std::vector<c_float>* lower_bounds,
    std::vector<c_float>* upper_bounds) {}

void FemPosDeviationSqpOsqpInterface::SetPrimalWarmStart(
    std::vector<c_float>* primal_warm_start) {
  CHECK_EQ(ref_points_.size(), num_of_points_);
  primal_warm_start->resize(num_of_variables_);
  for (size_t i = 0; i < ref_points_.size(); ++i) {
    (*primal_warm_start)[2 * i] = ref_points_[i].first;
    (*primal_warm_start)[2 * i + 1] = ref_points_[i].second;
  }
}

void FemPosDeviationSqpOsqpInterface::UpdatePrimalWarmStart(
    std::vector<c_float>* primal_warm_start) {
  CHECK_EQ(ref_points_.size(), num_of_points_);
  primal_warm_start->resize(num_of_variables_);
  for (size_t i = 0; i < ref_points_.size(); ++i) {
    (*primal_warm_start)[2 * i] = ref_points_[i].first;
    (*primal_warm_start)[2 * i + 1] = ref_points_[i].second;
  }
}

bool FemPosDeviationSqpOsqpInterface::OptimizeWithOsqp(
    const std::vector<c_float>& primal_warm_start, OSQPWorkspace** work) {
  osqp_warm_start_x(*work, primal_warm_start.data());

  // Solve Problem
  osqp_solve(*work);

  auto status = (*work)->info->status_val;

  if (status < 0) {
    AERROR << "failed optimization status:\t" << (*work)->info->status;
    return false;
  }

  if (status != 1 && status != 2) {
    AERROR << "failed optimization status:\t" << (*work)->info->status;
    return false;
  }

  // Extract primal results
  x_.resize(num_of_points_);
  y_.resize(num_of_points_);
  for (int i = 0; i < num_of_points_; ++i) {
    int index = i * 2;
    x_.at(i) = (*work)->solution->x[index];
    y_.at(i) = (*work)->solution->x[index + 1];
  }

  return true;
}

}  // namespace planning
}  // namespace apollo
