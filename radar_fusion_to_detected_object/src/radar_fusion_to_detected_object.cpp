
// Copyright 2022 TIER IV, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "radar_fusion_to_detected_object.hpp"

#include <boost/geometry.hpp>

#include <algorithm>
#include <iostream>
#include <numeric>

namespace radar_fusion_to_detected_object
{
using autoware_auto_perception_msgs::msg::DetectedObject;
using autoware_auto_perception_msgs::msg::DetectedObjects;
using geometry_msgs::msg::Point;
using geometry_msgs::msg::PoseWithCovariance;
using geometry_msgs::msg::Twist;
using geometry_msgs::msg::TwistWithCovariance;
using tier4_autoware_utils::LinearRing2d;
using tier4_autoware_utils::Point2d;

void RadarFusionToDetectedObject::setParam(const Param & param)
{
  // Radar fusion param
  param_.bounding_box_margin = param.bounding_box_margin;
  param_.split_threshold_velocity = param.split_threshold_velocity;

  // normalize weight param
  double sum_weight = param.velocity_weight_median + param.velocity_weight_average +
                      param.velocity_weight_target_value_average +
                      param.velocity_weight_target_value_top;
  if (sum_weight < 0.01) {
    param_.velocity_weight_median = 1.0;
    param_.velocity_weight_average = 0.0;
    param_.velocity_weight_target_value_average = 0.0;
    param_.velocity_weight_target_value_top = 0.0;
  } else {
    param_.velocity_weight_median = param.velocity_weight_median / sum_weight;
    param_.velocity_weight_average = param.velocity_weight_average / sum_weight;
    param_.velocity_weight_target_value_average =
      param.velocity_weight_target_value_average / sum_weight;
    param_.velocity_weight_target_value_top = param.velocity_weight_target_value_top / sum_weight;
  }

  // Parameters for fixing object information
  param_.threshold_probability = param.threshold_probability;
  param_.convert_doppler_to_twist = param.convert_doppler_to_twist;
}

RadarFusionToDetectedObject::Output RadarFusionToDetectedObject::update(
  const RadarFusionToDetectedObject::Input & input)
{
  RadarFusionToDetectedObject::Output output{};
  output.objects.header = input.objects.header;

  for (const auto & object : input.objects.objects) {
    // Link between 3d bounding box and radar data
    std::vector<RadarInput> radars_within_object = filterRadarWithinObject(object, input.radars);

    // Split the object going in a different direction
    std::vector<DetectedObject> split_objects = splitObject(object, radars_within_object);

    for (auto & split_object : split_objects) {
      std::vector<RadarInput> radars_within_split_object;
      if (split_objects.size() == 1) {
        // If object is not split, radar data within object is same
        radars_within_split_object = radars_within_object;
      } else {
        // If object is split, then filter radar again
        radars_within_split_object = filterRadarWithinObject(object, radars_within_object);
      }

      // Estimate twist of object
      split_object.kinematics.has_twist = true;
      split_object.kinematics.twist_with_covariance =
        estimateTwist(split_object, radars_within_split_object);

      // Delete objects with low probability
      if (isQualified(split_object, radars_within_split_object)) {
        split_object.classification.at(0).probability =
          std::max(split_object.classification.at(0).probability, param_.threshold_probability);
        output.objects.objects.emplace_back(split_object);
      }
    }
  }
  return output;
}

std::vector<RadarFusionToDetectedObject::RadarInput>
RadarFusionToDetectedObject::filterRadarWithinObject(
  const DetectedObject & object, const std::vector<RadarInput> & radars)
{
  std::vector<RadarInput> outputs{};

  tier4_autoware_utils::Point2d object_size{object.shape.dimensions.x, object.shape.dimensions.y};
  LinearRing2d object_box =
    tier4_autoware_utils::createObject2d(object_size, param_.bounding_box_margin);
  object_box = tier4_autoware_utils::transformVector(
    object_box, tier4_autoware_utils::pose2transform(object.kinematics.pose_with_covariance.pose));

  for (const auto & radar : radars) {
    Point2d radar_point{
      radar.pose_with_covariance.pose.position.x, radar.pose_with_covariance.pose.position.y};
    if (boost::geometry::within(radar_point, object_box)) {
      outputs.emplace_back(radar);
    }
  }
  return outputs;
}

std::vector<DetectedObject> RadarFusionToDetectedObject::splitObject(
  const DetectedObject & object, const std::vector<RadarInput> & radars)
{
  // [TODO] Implementation
  std::vector<DetectedObject> output{};
  output.emplace_back(object);
  return output;
}

TwistWithCovariance RadarFusionToDetectedObject::estimateTwist(
  const DetectedObject & object, std::vector<RadarInput> & radars)
{
  TwistWithCovariance twist_with_covariance{};
  if (radars.empty()) {
    return twist_with_covariance;
  }

  // calculate median
  Twist twist_median{};
  if (param_.velocity_weight_median > 0.0) {
    auto ascending_func = [&](const RadarInput & a, const RadarInput & b) {
      return getTwistNorm(a.twist_with_covariance.twist) <
             getTwistNorm(b.twist_with_covariance.twist);
    };
    std::sort(radars.begin(), radars.end(), ascending_func);

    if (radars.size() % 2 == 1) {
      int median_index = (radars.size() - 1) / 2;
      twist_median = radars.at(median_index).twist_with_covariance.twist;
    } else {
      int median_index = radars.size() / 2;

      twist_median = scaleTwist(
        addTwist(
          radars.at(median_index - 1).twist_with_covariance.twist,
          radars.at(median_index).twist_with_covariance.twist),
        0.5);
    }
  }

  // calculate average
  Twist twist_average{};
  if (param_.velocity_weight_average > 0.0) {
    for (const auto & radar : radars) {
      twist_average = addTwist(twist_average, radar.twist_with_covariance.twist);
    }
    twist_average = scaleTwist(twist_average, (1.0 / radars.size()));
  }

  // calculate top target_value
  Twist twist_top_target_value{};
  if (param_.velocity_weight_target_value_top > 0.0) {
    auto comp_func = [](const RadarInput & a, const RadarInput & b) {
      return a.target_value < b.target_value;
    };
    auto iter = std::max_element(std::begin(radars), std::end(radars), comp_func);
    twist_top_target_value = iter->twist_with_covariance.twist;
  }

  // calculate target_value * average
  Twist twist_target_value_average{};
  double sum_target_value = 0.0;
  if (param_.velocity_weight_target_value_average > 0.0) {
    for (const auto & radar : radars) {
      twist_target_value_average = scaleTwist(
        addTwist(twist_target_value_average, radar.twist_with_covariance.twist),
        radar.target_value);
      sum_target_value += radar.target_value;
    }
    twist_target_value_average = scaleTwist(twist_target_value_average, 1.0 / sum_target_value);
  }

  // estimate doppler velocity with cost weight
  std::vector<Twist> weight_twists{};
  weight_twists.emplace_back(scaleTwist(twist_median, param_.velocity_weight_median));
  weight_twists.emplace_back(scaleTwist(twist_average, param_.velocity_weight_average));
  weight_twists.emplace_back(
    scaleTwist(twist_top_target_value, param_.velocity_weight_target_value_top));
  weight_twists.emplace_back(
    scaleTwist(twist_target_value_average, param_.velocity_weight_target_value_average));

  twist_with_covariance.twist = sumTwist(weight_twists);

  // Convert doppler velocity to twist
  if (param_.convert_doppler_to_twist) {
    twist_with_covariance = convertDopplerToTwist(object, twist_with_covariance);
  }
  return twist_with_covariance;
}

bool RadarFusionToDetectedObject::isQualified(
  const DetectedObject & object, const std::vector<RadarInput> & radars)
{
  if (object.classification[0].probability > param_.threshold_probability) {
    return true;
  } else {
    if (!radars.empty()) {
      return true;
    } else {
      return false;
    }
  }
}

TwistWithCovariance RadarFusionToDetectedObject::convertDopplerToTwist(
  const DetectedObject & object, const TwistWithCovariance & twist_with_covariance)
{
  // [TODO] implement for radar pointcloud fusion
  std::cout << "debug" << object.classification.at(0).probability << std::endl;
  return twist_with_covariance;
}

Twist RadarFusionToDetectedObject::addTwist(const Twist & twist_1, const Twist & twist_2)
{
  Twist output{};
  output.linear.x = twist_1.linear.x + twist_2.linear.x;
  output.linear.y = twist_1.linear.y + twist_2.linear.y;
  output.linear.z = twist_1.linear.z + twist_2.linear.z;
  output.angular.x = twist_1.angular.x + twist_2.angular.x;
  output.angular.y = twist_1.angular.y + twist_2.angular.y;
  output.angular.z = twist_1.angular.z + twist_2.angular.z;
  return output;
}

Twist RadarFusionToDetectedObject::scaleTwist(const Twist & twist, const double scale)
{
  Twist output{};
  output.linear.x = twist.linear.x * scale;
  output.linear.y = twist.linear.y * scale;
  output.linear.z = twist.linear.z * scale;
  output.angular.x = twist.angular.x * scale;
  output.angular.y = twist.angular.y * scale;
  output.angular.z = twist.angular.z * scale;
  return output;
}

double RadarFusionToDetectedObject::getTwistNorm(const Twist & twist)
{
  double output = std::sqrt(
    twist.linear.x * twist.linear.x + twist.linear.y * twist.linear.y +
    twist.linear.z * twist.linear.z);
  return output;
}

Twist RadarFusionToDetectedObject::sumTwist(const std::vector<Twist> & twists)
{
  Twist output{};
  for (const auto & twist : twists) {
    output = addTwist(output, twist);
  }
  return output;
}

}  // namespace radar_fusion_to_detected_object
