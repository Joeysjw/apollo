/******************************************************************************
 * Copyright 2017 The Apollo Authors. All Rights Reserved.
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

#include "modules/planning/tasks/dp_poly_path/trajectory_cost.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "modules/common/proto/pnc_point.pb.h"

#include "modules/common/configs/vehicle_config_helper.h"
#include "modules/common/math/vec2d.h"
#include "modules/common/util/util.h"
#include "modules/planning/common/planning_gflags.h"

namespace apollo {
namespace planning {

using apollo::common::math::Box2d;
using apollo::common::math::Vec2d;
using apollo::common::TrajectoryPoint;

TrajectoryCost::TrajectoryCost(
    const DpPolyPathConfig &config, const ReferenceLine &reference_line,
    const std::vector<const PathObstacle *> &obstacles,
    const common::VehicleParam &vehicle_param,
    const SpeedData &heuristic_speed_data, const common::SLPoint &init_sl_point)
    : config_(config),
      reference_line_(&reference_line),
      vehicle_param_(vehicle_param),
      heuristic_speed_data_(heuristic_speed_data),
      init_sl_point_(init_sl_point) {
  const double total_time =
      std::min(heuristic_speed_data_.TotalTime(), FLAGS_prediction_total_time);

  num_of_time_stamps_ = static_cast<uint32_t>(
      std::floor(total_time / config.eval_time_interval()));

  for (const auto ptr_path_obstacle : obstacles) {
    if (ptr_path_obstacle->IsIgnore()) {
      continue;
    }
    auto sl_boundary = ptr_path_obstacle->perception_sl_boundary();
    const auto &vehicle_param =
        common::VehicleConfigHelper::instance()->GetConfig().vehicle_param();

    const double adc_left_l =
        init_sl_point_.l() + vehicle_param.left_edge_to_center();
    const double adc_right_l =
        init_sl_point_.l() - vehicle_param.right_edge_to_center();

    constexpr double kLateralIgnoreDistance = 4.0;

    if (adc_left_l + kLateralIgnoreDistance < sl_boundary.start_l() ||
        adc_right_l - kLateralIgnoreDistance > sl_boundary.end_l()) {
      continue;
    }

    const auto ptr_obstacle = ptr_path_obstacle->obstacle();
    if (Obstacle::IsVirtualObstacle(ptr_obstacle->Perception())) {
      // Virtual obstacle
      continue;
    } else if (Obstacle::IsStaticObstacle(ptr_obstacle->Perception())) {
      const double kDefaultLaneWidth = 1.5;
      const double adc_width = vehicle_param.width();

      if (sl_boundary.start_l() + kDefaultLaneWidth < adc_width &&
          kDefaultLaneWidth - sl_boundary.end_l() < adc_width) {
        // lane blocking obstacle
        continue;
      }

      const double adc_length = vehicle_param.length();
      if (sl_boundary.start_s() - init_sl_point_.s() < 2.0 * adc_length) {
        // if too close, we treat the obstacle as non-nudgable
        continue;
      }

      static_obstacle_sl_boundaries_.push_back(std::move(sl_boundary));
    } else {
      std::vector<Box2d> box_by_time;
      for (uint32_t t = 0; t <= num_of_time_stamps_; ++t) {
        TrajectoryPoint trajectory_point =
            ptr_obstacle->GetPointAtTime(t * config.eval_time_interval());

        Box2d obstacle_box = ptr_obstacle->GetBoundingBox(trajectory_point);
        constexpr double kBuff = 0.5;
        Box2d expanded_obstacle_box =
            Box2d(obstacle_box.center(), obstacle_box.heading(),
                  obstacle_box.length() + kBuff, obstacle_box.width() + kBuff);
        box_by_time.push_back(expanded_obstacle_box);
      }
      dynamic_obstacle_boxes_.push_back(std::move(box_by_time));
    }
  }
}

double TrajectoryCost::CalculatePathCost(const QuinticPolynomialCurve1d &curve,
                                         const double start_s,
                                         const double end_s,
                                         const uint32_t curr_level,
                                         const uint32_t total_level) const {
  double path_cost = 0.0;
  for (double path_s = 0.0; path_s < (end_s - start_s);
       path_s += config_.path_resolution()) {
    const double l = curve.Evaluate(0, path_s);

    std::function<double(const double)> quasi_softmax = [this](const double x) {
      const double l0 = this->config_.path_l_cost_param_l0();
      const double b = this->config_.path_l_cost_param_b();
      const double k = this->config_.path_l_cost_param_k();
      return (b + std::exp(-k * (x - l0))) / (1.0 + std::exp(-k * (x - l0)));
    };

    path_cost += l * l * config_.path_l_cost() * quasi_softmax(std::fabs(l));

    double left_width = 0.0;
    double right_width = 0.0;
    reference_line_->GetLaneWidth(path_s, &left_width, &right_width);

    const auto &vehicle_config =
        common::VehicleConfigHelper::instance()->GetConfig();
    const double width = vehicle_config.vehicle_param().width();

    constexpr double kBuff = 0.2;
    if (std::fabs(l) > std::fabs(init_sl_point_.l()) &&
        (l + width / 2.0 + kBuff > left_width ||
         l - width / 2.0 - kBuff < right_width)) {
      path_cost += config_.path_out_lane_cost();
    }

    const double dl = std::fabs(curve.Evaluate(1, path_s));
    path_cost += dl * dl * config_.path_dl_cost();

    const double ddl = std::fabs(curve.Evaluate(2, path_s));
    path_cost += ddl * ddl * config_.path_ddl_cost();
  }
  path_cost *= config_.path_resolution();

  if (curr_level == total_level) {
    const double end_l = curve.Evaluate(0, end_s - start_s);
    path_cost +=
        std::sqrt(end_l - init_sl_point_.l() / 2.0) * config_.path_end_l_cost();
  }
  return path_cost;
}

double TrajectoryCost::CalculateStaticObstacleCost(
    const QuinticPolynomialCurve1d &curve, const double start_s,
    const double end_s) const {
  double obstacle_cost = 0.0;

  for (double curr_s = start_s; curr_s <= end_s;
       curr_s += config_.path_resolution()) {
    const double s = curr_s - start_s;  // spline curve s
    const double l = curve.Evaluate(0, s);
    for (const auto &obs_sl_boundary : static_obstacle_sl_boundaries_) {
      obstacle_cost += GetCostFromObsSL(s, l, obs_sl_boundary);
    }
  }
  return obstacle_cost * config_.path_resolution();
}

double TrajectoryCost::CalculateDynamicObstacleCost(
    const QuinticPolynomialCurve1d &curve, const double start_s,
    const double end_s) const {
  double obstacle_cost = 0.0;
  double time_stamp = 0.0;

  for (size_t index = 0; index < num_of_time_stamps_;
       ++index, time_stamp += config_.eval_time_interval()) {
    common::SpeedPoint speed_point;
    heuristic_speed_data_.EvaluateByTime(time_stamp, &speed_point);
    if (speed_point.s() < start_s - init_sl_point_.s()) {
      continue;
    }
    if (speed_point.s() > end_s - init_sl_point_.s()) {
      break;
    }

    const double s =
        init_sl_point_.s() + speed_point.s() - start_s;  // s on spline curve
    const double l = curve.Evaluate(0, s);
    const double dl = curve.Evaluate(1, s);

    const common::SLPoint sl =
        common::util::MakeSLPoint(init_sl_point_.s() + speed_point.s(), l);
    const Box2d ego_box = GetBoxFromSLPoint(sl, dl);
    for (const auto &obstacle_trajectory : dynamic_obstacle_boxes_) {
      obstacle_cost +=
          GetCostBetweenObsBoxes(ego_box, obstacle_trajectory.at(index));
    }
  }
  constexpr double kDynamicObsWeight = 1e-3;
  return obstacle_cost * config_.eval_time_interval() * kDynamicObsWeight;
}

double TrajectoryCost::GetCostFromObsSL(
    const double adc_s, const double adc_l,
    const SLBoundary &obs_sl_boundary) const {
  const auto &vehicle_param =
      common::VehicleConfigHelper::instance()->GetConfig().vehicle_param();

  const double adc_front_s = adc_s + vehicle_param.front_edge_to_center();
  const double adc_end_s = adc_s - vehicle_param.back_edge_to_center();
  const double adc_left_l = adc_l + vehicle_param.left_edge_to_center();
  const double adc_right_l = adc_l - vehicle_param.right_edge_to_center();

  constexpr double ignore_s_distance = 10.0;
  constexpr double ignore_l_distance = 3.0;
  if (adc_front_s + ignore_s_distance < obs_sl_boundary.start_s() ||
      adc_end_s - ignore_s_distance > obs_sl_boundary.end_s()) {
    return 0.0;
  }
  if (adc_left_l + ignore_l_distance < obs_sl_boundary.start_l() ||
      adc_right_l - ignore_l_distance > obs_sl_boundary.end_l()) {
    return 0.0;
  }

  std::function<double(const double, const double)> softmax = [](
      const double x, const double x0) {
    return std::exp(-(x - x0)) / (1.0 + std::exp(-(x - x0)));
  };

  double obstacle_cost = 0.0;
  if (!(adc_front_s < obs_sl_boundary.start_s() ||
        adc_end_s > obs_sl_boundary.end_s()) &&
      !(adc_left_l < obs_sl_boundary.end_l() ||
        adc_right_l > obs_sl_boundary.start_l())) {
    return config_.obstacle_collision_cost();
  }

  const double delta_l = std::fabs(
      adc_l - (obs_sl_boundary.start_l() + obs_sl_boundary.end_l()) / 2.0);
  obstacle_cost += config_.obstacle_collision_cost() *
                   softmax(delta_l, config_.obstacle_collision_distance());

  const double delta_s = std::fabs(
      adc_s - (obs_sl_boundary.start_s() + obs_sl_boundary.end_s()) / 2.0);
  obstacle_cost += config_.obstacle_collision_cost() *
                   softmax(delta_s, config_.obstacle_collision_distance());
  return obstacle_cost;
}

double TrajectoryCost::GetCostBetweenObsBoxes(const Box2d &ego_box,
                                              const Box2d &obstacle_box) const {
  // Simple version: calculate obstacle cost by distance
  const double distance = obstacle_box.DistanceTo(ego_box);
  if (distance > config_.obstacle_ignore_distance()) {
    return 0.0;
  }

  std::function<double(const double, const double)> softmax = [](
      const double x, const double x0) {
    return std::exp(-(x - x0)) / (1.0 + std::exp(-(x - x0)));
  };

  double obstacle_cost = 0.0;
  obstacle_cost += config_.obstacle_collision_cost() *
                   softmax(distance, config_.obstacle_collision_distance());
  obstacle_cost += 20.0 * softmax(distance, config_.obstacle_risk_distance());
  return obstacle_cost;
}

Box2d TrajectoryCost::GetBoxFromSLPoint(const common::SLPoint &sl,
                                        const double dl) const {
  Vec2d xy_point;
  reference_line_->SLToXY(sl, &xy_point);

  ReferencePoint reference_point = reference_line_->GetReferencePoint(sl.s());

  const double one_minus_kappa_r_d = 1 - reference_point.kappa() * sl.l();
  const double delta_theta = std::atan2(dl, one_minus_kappa_r_d);
  const double theta =
      common::math::NormalizeAngle(delta_theta + reference_point.heading());
  return Box2d(xy_point, theta, vehicle_param_.length(),
               vehicle_param_.width());
}

// TODO(All): optimize obstacle cost calculation time
double TrajectoryCost::Calculate(const QuinticPolynomialCurve1d &curve,
                                 const double start_s, const double end_s,
                                 const uint32_t curr_level,
                                 const uint32_t total_level) const {
  double total_cost = 0.0;
  // path cost
  total_cost +=
      CalculatePathCost(curve, start_s, end_s, curr_level, total_level);

  // static obstacle cost
  total_cost += CalculateStaticObstacleCost(curve, start_s, end_s);

  // dynamic obstacle cost
  total_cost += CalculateDynamicObstacleCost(curve, start_s, end_s);
  return total_cost;
}

double TrajectoryCost::RiskDistanceCost(const double distance) const {
  return (5.0 - distance) * ((5.0 - distance)) * 10;
}

double TrajectoryCost::RegularDistanceCost(const double distance) const {
  return std::max(20.0 - distance, 0.0);
}

}  // namespace planning
}  // namespace apollo
