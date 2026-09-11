#include "vertical.hpp"

#include "../geometry/geometry.hpp"

#include <algorithm>
#include <cmath>

namespace city::road::generation {
namespace {

[[nodiscard]] Vec3d add3(Vec3d a, Vec3d b) {
  return Vec3d{a.x + b.x, a.y + b.y, a.z + b.z};
}

[[nodiscard]] Vec3d scale3(Vec3d value, double factor) {
  return Vec3d{value.x * factor, value.y * factor, value.z * factor};
}

[[nodiscard]] Vec3d cross3(Vec3d a, Vec3d b) {
  return Vec3d{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
               a.x * b.y - a.y * b.x};
}

[[nodiscard]] double length3(Vec3d value) {
  return std::sqrt(value.x * value.x + value.y * value.y +
                   value.z * value.z);
}

[[nodiscard]] Vec3d normalize3(Vec3d value) {
  const double length = length3(value);
  if (length <= internal::distance_epsilon) return Vec3d{};
  return scale3(value, 1.0 / length);
}

} // namespace

double elevation_at(const DerivedSegment& segment,
                    double segment_distance_m) {
  if (segment.length_m <= internal::distance_epsilon)
    return segment.start_elevation_m;
  const double t =
      std::clamp(segment_distance_m / segment.length_m, 0.0, 1.0);
  return segment.start_elevation_m +
         (segment.end_elevation_m - segment.start_elevation_m) * t;
}

Result<RoadFrame> road_frame_at(const DerivedSegment& segment,
                                double segment_distance_m) {
  const Result<Vec2d> center =
      EvaluatePath(segment.alignment, segment_distance_m);
  const Result<Vec2d> horizontal_tangent =
      internal::tangent_at(segment.alignment, segment_distance_m);
  const Result<Vec2d> horizontal_lateral =
      internal::lateral_at(segment.alignment, segment_distance_m);
  if (!center.ok || !horizontal_tangent.ok || !horizontal_lateral.ok) {
    return Result<RoadFrame>::Fail(
        CommitFailureCategory::kInternalError,
        "road vertical frame alignment sample is missing");
  }
  const double grade = segment.length_m > internal::distance_epsilon
                           ? (segment.end_elevation_m -
                              segment.start_elevation_m) /
                                 segment.length_m
                           : 0.0;
  RoadFrame frame{};
  frame.position =
      Vec3d{center.value.x, center.value.y,
            elevation_at(segment, segment_distance_m)};
  frame.tangent =
      normalize3(Vec3d{horizontal_tangent.value.x, horizontal_tangent.value.y,
                       grade});
  frame.lateral = Vec3d{horizontal_lateral.value.x,
                        horizontal_lateral.value.y, 0.0};
  frame.normal = normalize3(cross3(frame.tangent, frame.lateral));
  if (length3(frame.tangent) <= internal::distance_epsilon ||
      length3(frame.lateral) <= internal::distance_epsilon ||
      length3(frame.normal) <= internal::distance_epsilon) {
    return Result<RoadFrame>::Fail(CommitFailureCategory::kInternalError,
                                   "road vertical frame is degenerate");
  }
  return Result<RoadFrame>::Ok(frame);
}

Vec3d place_on_frame(const RoadFrame& frame, double lateral_m,
                     double local_height_m) {
  return add3(add3(frame.position, scale3(frame.lateral, lateral_m)),
              scale3(frame.normal, local_height_m));
}

} // namespace city::road::generation
