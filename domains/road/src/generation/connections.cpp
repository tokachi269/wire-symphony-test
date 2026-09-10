#include "generation.hpp"

#include "../geometry/geometry.hpp"
#include "../geometry/junction.hpp"
#include "../geometry/section.hpp"
#include "../lookup.hpp"
#include "vertical.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <tuple>

namespace city::road::generation {
namespace {

using internal::cross;
using internal::dot;
using internal::find_approach_override;
using internal::find_node;
using internal::find_policy_override;
using internal::find_segment;
using internal::find_template;
using internal::find_transition;
using internal::inward_tangent;
using internal::is_finite;
using internal::normalize;
using internal::scale;
using internal::distance_epsilon;
using internal::surface_sample_step_m;
using internal::subtract;
using internal::tangent_at;
using internal::to3;

struct policy {
  double straight_tolerance_rad = 5.0 * std::numbers::pi / 180.0;
  double curve_control_factor = 0.45;
  double parallel_sine_tolerance = 1e-3;
  std::size_t maximum_approaches = 4;
};

constexpr policy rules{};

struct ordered_approach {
  ApproachKey key{};
  Vec2d tangent{};
  Vec2d chord{};
  std::int64_t angle_key = 0;
};

std::int64_t angle_key(Vec2d tangent) {
  constexpr double scale_factor = 1.0e12;
  return static_cast<std::int64_t>(std::llround(
      (std::atan2(tangent.y, tangent.x) + std::numbers::pi) * scale_factor));
}

bool approach_key_less(const ApproachKey &a, const ApproachKey &b) {
  return std::tie(a.node_id, a.segment_id, a.endpoint_role) <
         std::tie(b.node_id, b.segment_id, b.endpoint_role);
}

struct endpoint_side_reaches {
  double left_m = 0.0;
  double right_m = 0.0;
};

endpoint_side_reaches endpoint_reaches(const SavedRoadGraph &graph,
                                       const RoadSegment &segment,
                                       const ApproachKey &key) {
  RoadLayoutTemplateId template_id = segment.layout_template;
  if (segment.transition.has_value()) {
    const RoadLayoutTransition *transition =
        find_transition(graph, *segment.transition);
    if (transition == nullptr)
      return {};
    template_id = key.endpoint_role == EndpointRole::kStart
                      ? transition->from_template
                      : transition->to_template;
  }
  const RoadLayoutTemplate *section = find_template(graph, template_id);
  if (section == nullptr)
    return {};
  double width = 0.0;
  for (const RoadLayoutStrip &strip : section->strips)
    width += strip.width_m;
  const double offset = section->alignment_offset_from_left_m;
  const endpoint_side_reaches along_segment{offset, width - offset};
  if (key.endpoint_role == EndpointRole::kStart)
    return along_segment;
  return endpoint_side_reaches{along_segment.right_m, along_segment.left_m};
}

double signed_endpoint_reach_toward(const SavedRoadGraph &graph,
                                    const RoadSegment &segment,
                                    const ordered_approach &from,
                                    Vec2d from_direction,
                                    Vec2d toward_direction) {
  const endpoint_side_reaches reaches =
      endpoint_reaches(graph, segment, from.key);
  return cross(from_direction, toward_direction) > 0.0 ? reaches.left_m
                                                        : -reaches.right_m;
}

struct junction_corner_resolution {
  ResolvedJunctionCorner corner{};
  double first_setback_m = 0.0;
  double second_setback_m = 0.0;
};

Result<junction_corner_resolution> resolve_junction_corner(
    const SavedRoadGraph &graph, const std::vector<DerivedSegment> &segments,
    const ordered_approach &first, const ordered_approach &second) {
  using Out = Result<junction_corner_resolution>;
  const RoadSegment *first_segment = find_segment(graph, first.key.segment_id);
  const RoadSegment *second_segment = find_segment(graph, second.key.segment_id);
  const auto find_derived = [&segments](RoadSegmentId id) {
    const auto found =
        std::find_if(segments.begin(), segments.end(),
                     [id](const DerivedSegment &segment) {
                       return segment.id == id;
                     });
    return found == segments.end() ? nullptr : &*found;
  };
  const DerivedSegment *first_derived = find_derived(first.key.segment_id);
  const DerivedSegment *second_derived = find_derived(second.key.segment_id);
  if (first_segment == nullptr || second_segment == nullptr ||
      first_derived == nullptr || second_derived == nullptr) {
    return Out::Fail(CommitFailureCategory::kInternalError,
                     "road junction corner source segment is missing");
  }
  junction_corner_resolution resolved{};
  resolved.corner.first_approach = first.key;
  resolved.corner.second_approach = second.key;
  resolved.corner.radius_m =
      std::min(first_segment->corner_radius_m, second_segment->corner_radius_m);

  Vec2d first_direction = first.tangent;
  Vec2d second_direction = second.tangent;
  double sine = cross(first_direction, second_direction);
  if (std::abs(sine) <= rules.parallel_sine_tolerance) {
    const double chord_sine = cross(first.chord, second.chord);
    if (std::abs(chord_sine) <= rules.parallel_sine_tolerance) {
      return Out::Ok(resolved);
    }
    first_direction = first.chord;
    second_direction = second.chord;
    sine = chord_sine;
  }

  const Vec2d first_lateral{-first_direction.y, first_direction.x};
  const Vec2d second_lateral{-second_direction.y, second_direction.x};
  const double first_reach =
      signed_endpoint_reach_toward(graph, *first_segment, first,
                                   first_direction, second_direction);
  const double second_reach =
      signed_endpoint_reach_toward(graph, *second_segment, second,
                                   second_direction, first_direction);
  const Vec2d first_origin = scale(first_lateral, first_reach);
  const Vec2d second_origin = scale(second_lateral, second_reach);
  const Vec2d delta = subtract(second_origin, first_origin);
  const double first_sharp_m = cross(delta, second.tangent) / sine;
  const double second_sharp_m = cross(delta, first.tangent) / sine;
  if (!is_finite(first_sharp_m) || !is_finite(second_sharp_m)) {
    return Out::Fail(CommitFailureCategory::kNotImplemented,
                     "road junction side-line intersection is not finite");
  }

  const double angle = std::acos(std::clamp(
      dot(first_direction, second_direction), -1.0, 1.0));
  const double half_angle_tangent = std::tan(angle * 0.5);
  if (half_angle_tangent <= rules.parallel_sine_tolerance) {
    return Out::Fail(CommitFailureCategory::kNotImplemented,
                     "road junction corner angle is too small");
  }
  double tangent_extension_m = resolved.corner.radius_m / half_angle_tangent;
  const double first_sharp_setback_m = std::max(0.0, first_sharp_m);
  const double second_sharp_setback_m = std::max(0.0, second_sharp_m);
  const double first_radius_room_m =
      first_derived->length_m - first_sharp_setback_m;
  const double second_radius_room_m =
      second_derived->length_m - second_sharp_setback_m;
  const double radius_room_m = std::min(first_radius_room_m, second_radius_room_m);
  if (radius_room_m < -distance_epsilon) {
    return Out::Fail(CommitFailureCategory::kNotImplemented,
                     "road approach setback exceeds the segment length");
  }
  tangent_extension_m =
      std::min(tangent_extension_m, std::max(0.0, radius_room_m));
  resolved.corner.radius_m = tangent_extension_m * half_angle_tangent;
  resolved.first_setback_m = first_sharp_setback_m + tangent_extension_m;
  resolved.second_setback_m = second_sharp_setback_m + tangent_extension_m;
  return Out::Ok(resolved);
}

Result<junction_corner_resolution> resolve_degree_two_corner(
    const SavedRoadGraph &graph, const std::vector<DerivedSegment> &segments,
    const ordered_approach &first, const ordered_approach &second) {
  using Out = Result<junction_corner_resolution>;
  const auto find_derived = [&segments](RoadSegmentId id) {
    const auto found =
        std::find_if(segments.begin(), segments.end(),
                     [id](const DerivedSegment &segment) {
                       return segment.id == id;
                     });
    return found == segments.end() ? nullptr : &*found;
  };
  const RoadSegment *first_segment = find_segment(graph, first.key.segment_id);
  const RoadSegment *second_segment = find_segment(graph, second.key.segment_id);
  const DerivedSegment *first_derived = find_derived(first.key.segment_id);
  const DerivedSegment *second_derived = find_derived(second.key.segment_id);
  if (first_segment == nullptr || second_segment == nullptr ||
      first_derived == nullptr || second_derived == nullptr) {
    return Out::Fail(CommitFailureCategory::kInternalError,
                     "road corner source segment is missing");
  }

  junction_corner_resolution resolved{};
  resolved.corner.first_approach = first.key;
  resolved.corner.second_approach = second.key;
  resolved.corner.radius_m =
      std::min(first_segment->corner_radius_m, second_segment->corner_radius_m);

  const double sine = cross(first.tangent, second.tangent);
  if (std::abs(sine) <= rules.parallel_sine_tolerance) {
    return Out::Ok(resolved);
  }
  const double angle = std::acos(
      std::clamp(dot(first.tangent, second.tangent), -1.0, 1.0));
  const endpoint_side_reaches first_reaches =
      endpoint_reaches(graph, *first_segment, first.key);
  const endpoint_side_reaches second_reaches =
      endpoint_reaches(graph, *second_segment, second.key);
  const double turn_angle = std::numbers::pi - angle;
  const double turn_half_tangent = std::tan(turn_angle * 0.5);
  const double full_section_reach =
      std::max(std::max(first_reaches.left_m, first_reaches.right_m),
               std::max(second_reaches.left_m, second_reaches.right_m));
  double max_radius_m = resolved.corner.radius_m;
  const auto limit_radius = [&](double limit) {
    max_radius_m = std::min(max_radius_m, limit);
  };
  if (turn_half_tangent > rules.parallel_sine_tolerance) {
    limit_radius(first_derived->length_m / turn_half_tangent -
                 full_section_reach);
    limit_radius(second_derived->length_m / turn_half_tangent -
                 full_section_reach);
  }
  if (max_radius_m < -distance_epsilon) {
    return Out::Fail(CommitFailureCategory::kNotImplemented,
                     "road approach setback exceeds the segment length");
  }
  resolved.corner.radius_m = std::max(0.0, max_radius_m);

  const double full_section_setback =
      (resolved.corner.radius_m + full_section_reach) * turn_half_tangent;
  resolved.first_setback_m = full_section_setback;
  resolved.second_setback_m = full_section_setback;
  return Out::Ok(resolved);
}

RoadLayoutTemplateId endpoint_template_id(const SavedRoadGraph &graph,
                                            const RoadSegment &segment,
                                            const ApproachKey &key) {
  if (!segment.transition.has_value())
    return segment.layout_template;
  const RoadLayoutTransition *transition =
      find_transition(graph, *segment.transition);
  if (transition == nullptr)
    return 0;
  return key.endpoint_role == EndpointRole::kStart ? transition->from_template
                                                   : transition->to_template;
}

const ResolvedApproach *approach_of(const ResolvedConnection &connection,
                                   const ApproachKey &key) {
  const auto found =
      std::find_if(connection.approaches.begin(), connection.approaches.end(),
                   [&key](const ResolvedApproach &approach) {
                     return approach.key == key;
                   });
  return found == connection.approaches.end() ? nullptr : &*found;
}

const DerivedSegment *segment_of(const std::vector<DerivedSegment> &segments,
                                 RoadSegmentId segment_id) {
  const auto found = std::find_if(segments.begin(), segments.end(),
                                  [segment_id](const DerivedSegment &segment) {
                                    return segment.id == segment_id;
                                  });
  return found == segments.end() ? nullptr : &*found;
}

const ResolvedConnection *connection_of(
    const std::vector<ResolvedConnection> &connections, RoadNodeId node_id) {
  const auto found = std::find_if(
      connections.begin(), connections.end(),
      [node_id](const ResolvedConnection &connection) {
        return connection.node_id == node_id;
      });
  return found == connections.end() ? nullptr : &*found;
}

Result<internal::LaneSectionPosition>
lane_section_position(const LaneBand &lane, const SectionEvaluation &section) {
  RoadLayoutTemplate resolved{};
  resolved.strips.push_back(RoadLayoutStrip{lane.surface_strip_id});
  const Result<internal::LaneSectionPosition> position =
      internal::lane_position(resolved, lane, section);
  return position.ok
             ? Result<internal::LaneSectionPosition>::Ok(position.value)
             : Result<internal::LaneSectionPosition>::Fail(
                   position.failure_category, position.error);
}

Result<double> lane_lateral(const LaneBand &lane,
                            const SectionEvaluation &section) {
  const Result<internal::LaneSectionPosition> position =
      lane_section_position(lane, section);
  return position.ok
             ? Result<double>::Ok(position.value.lateral_m)
             : Result<double>::Fail(position.failure_category, position.error);
}

Result<double> boundary_lateral(BoundaryId id,
                                const SectionEvaluation &section) {
  double minimum = std::numeric_limits<double>::infinity();
  double maximum = -std::numeric_limits<double>::infinity();
  for (const SectionBoundarySample &boundary : section.boundaries) {
    if (boundary.boundary_id != id)
      continue;
    minimum = std::min(minimum, boundary.lateral_m);
    maximum = std::max(maximum, boundary.lateral_m);
  }
  if (!std::isfinite(minimum) || !std::isfinite(maximum)) {
    return Result<double>::Fail(CommitFailureCategory::kInternalError,
                                "connected boundary sample is missing");
  }
  return Result<double>::Ok((minimum + maximum) * 0.5);
}

Vec2d endpoint_point(const ResolvedApproach &approach, double lateral_m) {
  const double section_sign =
      approach.key.endpoint_role == EndpointRole::kStart ? 1.0 : -1.0;
  return Vec2d{approach.gate.position.x +
                   approach.gate.lateral.x * lateral_m * section_sign,
               approach.gate.position.y +
               approach.gate.lateral.y * lateral_m * section_sign};
}

Vec3d endpoint_lane_point(const ResolvedApproach &approach,
                          const internal::LaneSectionPosition &position) {
  const double section_sign =
      approach.key.endpoint_role == EndpointRole::kStart ? 1.0 : -1.0;
  return Vec3d{
      approach.gate.position.x +
          approach.gate.lateral.x * position.lateral_m * section_sign +
          approach.gate.normal.x * position.height_m,
      approach.gate.position.y +
          approach.gate.lateral.y * position.lateral_m * section_sign +
          approach.gate.normal.y * position.height_m,
      approach.gate.position.z +
          approach.gate.lateral.z * position.lateral_m * section_sign +
          approach.gate.normal.z * position.height_m,
  };
}

Result<std::vector<Vec3d>> sample_lane_path_points(const Path &path,
                                                   double length_m,
                                                   Vec3d start,
                                                   Vec3d end) {
  constexpr int kLanePathSamples = 16;
  std::vector<Vec3d> points{};
  points.reserve(static_cast<std::size_t>(kLanePathSamples + 1));
  for (int index = 0; index <= kLanePathSamples; ++index) {
    const double t = static_cast<double>(index) /
                     static_cast<double>(kLanePathSamples);
    const double distance_m = length_m * t;
    const Result<Vec2d> xy = EvaluatePath(path, distance_m);
    if (!xy.ok) {
      return Result<std::vector<Vec3d>>::Fail(xy.failure_category, xy.error);
    }
    points.push_back(Vec3d{xy.value.x, xy.value.y,
                           start.z + (end.z - start.z) * t});
  }
  return Result<std::vector<Vec3d>>::Ok(std::move(points));
}

Path connect_g1_endpoint_points(const ResolvedApproach &source,
                                double source_lateral_m,
                                const ResolvedApproach &target,
                                double target_lateral_m) {
  const Vec2d start = endpoint_point(source, source_lateral_m);
  const Vec2d end = endpoint_point(target, target_lateral_m);
  const Vec2d source_motion{-source.tangent.x, -source.tangent.y};
  const Vec2d target_motion{target.tangent.x, target.tangent.y};
  const double control = std::hypot(end.x - start.x, end.y - start.y) / 3.0;
  return MakePath({MakeBezier(
      start, internal::add(start, scale(source_motion, control)),
      subtract(end, scale(target_motion, control)), end)});
}

Path resolve_lane_transition_path(const ResolvedApproach &source,
                                  double source_lateral_m,
                                  const ResolvedApproach &target,
                                  double target_lateral_m) {
  return connect_g1_endpoint_points(source, source_lateral_m, target,
                                    target_lateral_m);
}

Result<Path> resolve_junction_movement_path(
    const ResolvedConnection &connection, const ResolvedApproach &source,
    double source_lateral_m, const ResolvedApproach &target,
    double target_lateral_m) {
  if (connection.kind != NodeConnectionKind::kJunction) {
    return Result<Path>::Fail(
        CommitFailureCategory::kNotImplemented,
        "lane junction movement requires a junction connection");
  }
  return Result<Path>::Ok(connect_g1_endpoint_points(
      source, source_lateral_m, target, target_lateral_m));
}

double minimum_radius(const Path &path) {
  if (path.spans.empty())
    return 0.0;
  const BezierSpan &span = path.spans.front();
  double minimum = std::numeric_limits<double>::infinity();
  for (int index = 0; index <= 24; ++index) {
    const double t = static_cast<double>(index) / 24.0;
    const double u = 1.0 - t;
    const Vec2d first{
        3.0 * u * u * (span.p1.x - span.p0.x) +
            6.0 * u * t * (span.p2.x - span.p1.x) +
            3.0 * t * t * (span.p3.x - span.p2.x),
        3.0 * u * u * (span.p1.y - span.p0.y) +
            6.0 * u * t * (span.p2.y - span.p1.y) +
            3.0 * t * t * (span.p3.y - span.p2.y)};
    const Vec2d second{
        6.0 * u * (span.p2.x - 2.0 * span.p1.x + span.p0.x) +
            6.0 * t * (span.p3.x - 2.0 * span.p2.x + span.p1.x),
        6.0 * u * (span.p2.y - 2.0 * span.p1.y + span.p0.y) +
            6.0 * t * (span.p3.y - 2.0 * span.p2.y + span.p1.y)};
    const double speed = std::hypot(first.x, first.y);
    const double bend = std::abs(cross(first, second));
    if (speed <= distance_epsilon || bend <= distance_epsilon)
      continue;
    minimum = std::min(minimum, speed * speed * speed / bend);
  }
  return std::isfinite(minimum) ? minimum
                                : std::numeric_limits<double>::infinity();
}

Vec2d evaluate_span(const BezierSpan &span, double t) {
  const double u = 1.0 - t;
  return Vec2d{
      u * u * u * span.p0.x + 3.0 * u * u * t * span.p1.x +
          3.0 * u * t * t * span.p2.x + t * t * t * span.p3.x,
      u * u * u * span.p0.y + 3.0 * u * u * t * span.p1.y +
          3.0 * u * t * t * span.p2.y + t * t * t * span.p3.y};
}

ConnectionGate gate_at(const ApproachKey &key, Vec3d position, Vec3d tangent,
                       Vec3d lateral, Vec3d normal) {
  ConnectionGate gate{};
  gate.approach = key;
  gate.segment_id = key.segment_id;
  gate.node_id = key.node_id;
  gate.position = position;
  gate.tangent = tangent;
  gate.lateral = lateral;
  gate.normal = normal;
  return gate;
}

// The auto value, the user override and the frame that follows from them are
// decided together so no second table has to be kept in step.
Result<ResolvedApproach> resolve_approach(const SavedRoadGraph &graph,
                                          const DerivedSegment &segment,
                                          const ApproachKey &key,
                                          RoadLayoutTemplateId endpoint_section,
                                          double auto_setback_m,
                                          double auto_lateral_shift_m) {
  double setback = auto_setback_m;
  double lateral_shift = auto_lateral_shift_m;
  if (const ApproachGeometryOverride *override =
          find_approach_override(graph, key)) {
    if (override->setback_m.has_value)
      setback = override->setback_m.value;
    if (override->lateral_shift_m.has_value)
      lateral_shift = override->lateral_shift_m.value;
  }
  if (!is_finite(setback) || setback < 0.0 ||
      setback > segment.length_m + distance_epsilon ||
      !is_finite(lateral_shift)) {
    return Result<ResolvedApproach>::Fail(
        CommitFailureCategory::kNotImplemented,
        "road approach override exceeds supported layout range");
  }
  const double distance = key.endpoint_role == EndpointRole::kStart
                             ? setback
                             : segment.length_m - setback;
  const Result<RoadFrame> frame = road_frame_at(segment, distance);
  if (!frame.ok) {
    return Result<ResolvedApproach>::Fail(
        CommitFailureCategory::kInternalError, "road approach frame could not be evaluated");
  }
  const Vec3d tangent = key.endpoint_role == EndpointRole::kStart
                            ? frame.value.tangent
                            : Vec3d{-frame.value.tangent.x,
                                    -frame.value.tangent.y,
                                    -frame.value.tangent.z};
  const Vec3d lateral = key.endpoint_role == EndpointRole::kStart
                            ? frame.value.lateral
                            : Vec3d{-frame.value.lateral.x,
                                    -frame.value.lateral.y,
                                    -frame.value.lateral.z};
  const Vec3d shifted{
      frame.value.position.x + lateral.x * lateral_shift,
      frame.value.position.y + lateral.y * lateral_shift,
      frame.value.position.z + lateral.z * lateral_shift};

  ResolvedApproach approach{};
  approach.key = key;
  approach.endpoint_template_id = endpoint_section;
  approach.position = shifted;
  approach.tangent = tangent;
  approach.lateral = lateral;
  approach.normal = frame.value.normal;
  approach.auto_setback_m = auto_setback_m;
  approach.resolved_setback_m = setback;
  approach.auto_lateral_shift_m = auto_lateral_shift_m;
  approach.resolved_lateral_shift_m = lateral_shift;
  approach.gate_segment_distance_m = distance;
  approach.gate = gate_at(key, shifted, tangent, lateral, frame.value.normal);
  return Result<ResolvedApproach>::Ok(std::move(approach));
}

struct approach_location {
  ResolvedConnection *connection = nullptr;
  std::size_t index = 0;
};

bool has_setback_override(const SavedRoadGraph &graph, ApproachKey key) {
  const ApproachGeometryOverride *override = find_approach_override(graph, key);
  return override != nullptr && override->setback_m.has_value;
}

Result<bool> refit_approach_setback(const SavedRoadGraph &graph,
                                    const std::vector<DerivedSegment> &segments,
                                    approach_location location,
                                    double setback_m) {
  if (location.connection == nullptr ||
      location.index >= location.connection->approaches.size()) {
    return Result<bool>::Fail(CommitFailureCategory::kInternalError,
                              "road approach fit target is missing");
  }
  const ResolvedApproach &current =
      location.connection->approaches[location.index];
  const RoadSegment *source = find_segment(graph, current.key.segment_id);
  const auto derived =
      std::find_if(segments.begin(), segments.end(),
                   [&current](const DerivedSegment &segment) {
                     return segment.id == current.key.segment_id;
                   });
  if (source == nullptr || derived == segments.end()) {
    return Result<bool>::Fail(CommitFailureCategory::kInternalError,
                              "road approach fit source is missing");
  }
  const RoadLayoutTemplateId endpoint_section =
      endpoint_template_id(graph, *source, current.key);
  if (endpoint_section == 0) {
    return Result<bool>::Fail(
        CommitFailureCategory::kInvalidInput,
        "road approach endpoint section template is missing");
  }
  Result<ResolvedApproach> resolved =
      resolve_approach(graph, *derived, current.key, endpoint_section,
                       setback_m, current.auto_lateral_shift_m);
  if (!resolved.ok) {
    return Result<bool>::Fail(resolved.failure_category, resolved.error);
  }
  location.connection->approaches[location.index] = std::move(resolved.value);
  return Result<bool>::Ok(true);
}

Result<bool> fit_short_segment_gates(
    const SavedRoadGraph &graph, const std::vector<DerivedSegment> &segments,
    std::vector<ResolvedConnection> &connections) {
  for (const DerivedSegment &segment : segments) {
    approach_location start{};
    approach_location end{};
    for (ResolvedConnection &connection : connections) {
      for (std::size_t index = 0; index < connection.approaches.size();
           ++index) {
        const ResolvedApproach &approach = connection.approaches[index];
        if (approach.key.segment_id != segment.id)
          continue;
        approach_location location{&connection, index};
        if (approach.key.endpoint_role == EndpointRole::kStart) {
          start = location;
        } else {
          end = location;
        }
      }
    }

    const auto setback_of = [](approach_location location) {
      return location.connection->approaches[location.index].resolved_setback_m;
    };
    const auto can_refit = [&graph](approach_location location) {
      if (location.connection == nullptr)
        return false;
      const ResolvedApproach &approach =
          location.connection->approaches[location.index];
      return !has_setback_override(graph, approach.key);
    };

    if (start.connection != nullptr && end.connection != nullptr) {
      const double start_setback = setback_of(start);
      const double end_setback = setback_of(end);
      const double total = start_setback + end_setback;
      if (total > segment.length_m + distance_epsilon) {
        const bool both_junctions =
            start.connection->kind == NodeConnectionKind::kJunction &&
            end.connection->kind == NodeConnectionKind::kJunction;
        if (!both_junctions || !can_refit(start) || !can_refit(end)) {
          return Result<bool>::Fail(CommitFailureCategory::kNotImplemented,
                                    "road segment connection gates overlap");
        }
        const double usable_length_m = segment.length_m - surface_sample_step_m;
        if (usable_length_m < -distance_epsilon) {
          return Result<bool>::Fail(CommitFailureCategory::kNotImplemented,
                                    "road segment connection gates overlap");
        }
        const double scale =
            usable_length_m <= distance_epsilon ? 0.0 : usable_length_m / total;
        Result<bool> fitted =
            refit_approach_setback(graph, segments, start, start_setback * scale);
        if (!fitted.ok)
          return fitted;
        fitted =
            refit_approach_setback(graph, segments, end, end_setback * scale);
        if (!fitted.ok)
          return fitted;
      }
    } else {
      const approach_location location =
          start.connection != nullptr ? start : end;
      if (location.connection == nullptr)
        continue;
      const double setback = setback_of(location);
      if (setback > segment.length_m + distance_epsilon) {
        if (!can_refit(location)) {
          return Result<bool>::Fail(
              CommitFailureCategory::kNotImplemented,
              "road approach setback exceeds the segment length");
        }
        Result<bool> fitted =
            refit_approach_setback(graph, segments, location, segment.length_m);
        if (!fitted.ok)
          return fitted;
      }
    }
  }

  for (ResolvedConnection &connection : connections) {
    if (connection.kind != NodeConnectionKind::kCorner)
      continue;
    double minimum_setback = std::numeric_limits<double>::infinity();
    for (const ResolvedApproach &approach : connection.approaches)
      minimum_setback = std::min(minimum_setback, approach.resolved_setback_m);
    if (std::isfinite(minimum_setback))
      connection.corner_control_m = minimum_setback * rules.curve_control_factor;
  }
  return Result<bool>::Ok(true);
}

} // namespace

Result<std::vector<ResolvedConnection>>
resolve_connections(const SavedRoadGraph &graph,
                    const std::vector<NodeIncidence> &incidence,
                    const std::vector<DerivedSegment> &segments) {
  using Out = Result<std::vector<ResolvedConnection>>;
  std::vector<ResolvedConnection> connections{};
  connections.reserve(incidence.size());

  for (const NodeIncidence &topo : incidence) {
    const NodeConnectionPolicyOverride *policy_override =
        find_policy_override(graph, topo.node_id);
    // Endpoints and lone nodes carry no connection entity at all.
    if (topo.endpoints.size() <= 1 && policy_override == nullptr) {
      continue;
    }
    ResolvedConnection connection{};
    connection.node_id = topo.node_id;

    std::vector<ordered_approach> ordered{};
    ordered.reserve(topo.endpoints.size());
    for (const NodeEndpoint &incident : topo.endpoints) {
      const ApproachKey key{topo.node_id, incident.segment_id, incident.role};
      const RoadSegment *segment = find_segment(graph, key.segment_id);
      const DerivedSegment *derived = nullptr;
      for (const DerivedSegment &candidate : segments) {
        if (candidate.id == key.segment_id)
          derived = &candidate;
      }
      if (segment == nullptr || derived == nullptr) {
        return Out::Fail(CommitFailureCategory::kInternalError,
                         "road approach read model is missing");
      }
      const Result<Vec2d> tangent =
          inward_tangent(*segment, derived->alignment, key);
      if (!tangent.ok)
        return Out::Fail(tangent.failure_category, tangent.error);
      const RoadNode *node = find_node(graph, key.node_id);
      const RoadNodeId other_id = key.endpoint_role == EndpointRole::kStart
                                      ? segment->node_b
                                      : segment->node_a;
      const RoadNode *other = find_node(graph, other_id);
      if (node == nullptr || other == nullptr) {
        return Out::Fail(CommitFailureCategory::kInternalError,
                         "road approach chord endpoint is missing");
      }
      const Vec2d chord = normalize(subtract(other->position, node->position));
      ordered.push_back(ordered_approach{key, tangent.value, chord,
                                         angle_key(tangent.value)});
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const ordered_approach &a, const ordered_approach &b) {
                if (a.angle_key != b.angle_key)
                  return a.angle_key < b.angle_key;
                return approach_key_less(a.key, b.key);
              });
    for (const ordered_approach &approach : ordered) {
      connection.ordered_approaches.push_back(approach.key);
    }
    if (policy_override != nullptr) {
      connection.applied_policy_override_id = policy_override->id;
      if (policy_override->policy == NodeConnectionPolicy::kForcePassThrough) {
        connection.kind = NodeConnectionKind::kPassThrough;
      } else if (policy_override->policy ==
                 NodeConnectionPolicy::kForceCorner) {
        connection.kind = NodeConnectionKind::kCorner;
      } else {
        connection.kind = NodeConnectionKind::kJunction;
      }
      connection.reason = "explicit policy override";
    } else if (ordered.size() <= 1) {
      connection.kind = NodeConnectionKind::kPassThrough;
      connection.reason = "endpoint";
    } else if (ordered.size() == 2) {
      const bool straight = dot(ordered[0].tangent, ordered[1].tangent) <=
                            -std::cos(rules.straight_tolerance_rad);
      connection.kind = straight ? NodeConnectionKind::kPassThrough
                                 : NodeConnectionKind::kCorner;
      connection.reason =
          straight ? "two aligned approaches" : "two turning approaches";
    } else if (ordered.size() <= rules.maximum_approaches) {
      connection.kind = NodeConnectionKind::kJunction;
      connection.reason = "three or four approaches";
    } else {
      connection.kind = NodeConnectionKind::kUnsupported;
      connection.reason = "more than four approaches";
      return Out::Fail(CommitFailureCategory::kNotImplemented,
                       "road node has more than four approaches");
    }

    std::vector<junction_corner_resolution> junction_corners{};
    if (connection.kind == NodeConnectionKind::kCorner) {
      Result<junction_corner_resolution> resolved =
          resolve_degree_two_corner(graph, segments, ordered[0], ordered[1]);
      if (!resolved.ok) {
        return Out::Fail(resolved.failure_category, resolved.error);
      }
      connection.corner_radius_m = resolved.value.corner.radius_m;
      junction_corners.push_back(std::move(resolved.value));
    } else if (connection.kind == NodeConnectionKind::kJunction) {
      junction_corners.reserve(ordered.size());
      connection.junction_corners.reserve(ordered.size());
      for (std::size_t index = 0; index < ordered.size(); ++index) {
        Result<junction_corner_resolution> resolved = resolve_junction_corner(
            graph, segments, ordered[index],
            ordered[(index + 1) % ordered.size()]);
        if (!resolved.ok) {
          return Out::Fail(resolved.failure_category, resolved.error);
        }
        connection.junction_corners.push_back(resolved.value.corner);
        junction_corners.push_back(std::move(resolved.value));
      }
    }

    double minimum_setback = std::numeric_limits<double>::infinity();
    for (const ordered_approach &approach : ordered) {
      const RoadSegment *segment = find_segment(graph, approach.key.segment_id);
      const DerivedSegment *derived = nullptr;
      for (const DerivedSegment &candidate : segments) {
        if (candidate.id == approach.key.segment_id)
          derived = &candidate;
      }
      if (segment == nullptr || derived == nullptr) {
        return Out::Fail(CommitFailureCategory::kInternalError, "road approach source is missing");
      }

      double setback = 0.0;
      if (connection.kind == NodeConnectionKind::kCorner) {
        if (junction_corners.size() != 1) {
          return Out::Fail(CommitFailureCategory::kInternalError,
                           "road degree-two corner resolution is missing");
        }
        const junction_corner_resolution &corner = junction_corners.front();
        if (corner.corner.first_approach == approach.key) {
          setback = corner.first_setback_m;
        } else if (corner.corner.second_approach == approach.key) {
          setback = corner.second_setback_m;
        } else {
          return Out::Fail(CommitFailureCategory::kInternalError,
                           "road degree-two corner approach is missing");
        }
      } else if (connection.kind == NodeConnectionKind::kJunction) {
        setback = 0.0;
        for (const junction_corner_resolution &corner : junction_corners) {
          if (corner.corner.first_approach == approach.key) {
            setback = std::max(setback, corner.first_setback_m);
          } else if (corner.corner.second_approach == approach.key) {
            setback = std::max(setback, corner.second_setback_m);
          }
        }
      }
      if (!is_finite(setback) || setback < 0.0 ||
          setback > derived->length_m + distance_epsilon) {
        return Out::Fail(CommitFailureCategory::kNotImplemented,
                         "road approach setback exceeds the segment length");
      }
      const RoadLayoutTemplateId endpoint_section =
          endpoint_template_id(graph, *segment, approach.key);
      if (endpoint_section == 0) {
        return Out::Fail(CommitFailureCategory::kInvalidInput,
                         "road approach endpoint section template is missing");
      }
      Result<ResolvedApproach> resolved = resolve_approach(
          graph, *derived, approach.key, endpoint_section, setback, 0.0);
      if (!resolved.ok)
        return Out::Fail(resolved.failure_category, resolved.error);
      connection.approaches.push_back(std::move(resolved.value));
      if (connection.kind == NodeConnectionKind::kCorner)
        minimum_setback = std::min(minimum_setback, setback);
    }
    if (connection.kind == NodeConnectionKind::kCorner) {
      if (!std::isfinite(minimum_setback)) {
        return Out::Fail(CommitFailureCategory::kInternalError,
                         "road degree-two corner setback is missing");
      }
      connection.corner_control_m =
          minimum_setback * rules.curve_control_factor;
    }
    connections.push_back(std::move(connection));
  }
  Result<bool> fitted = fit_short_segment_gates(graph, segments, connections);
  if (!fitted.ok) {
    return Out::Fail(fitted.failure_category, fitted.error);
  }
  return Out::Ok(std::move(connections));
}

Result<bool> resolve_connection_geometry(std::vector<ResolvedConnection> &connections,
                                         const std::vector<DerivedSegment> &segments) {
  for (ResolvedConnection &connection : connections) {
    for (ResolvedApproach &approach : connection.approaches) {
      const DerivedSegment *segment = nullptr;
      for (const DerivedSegment &candidate : segments) {
        if (candidate.id == approach.key.segment_id)
          segment = &candidate;
      }
      const SectionEvaluation *section =
          segment == nullptr
              ? nullptr
              : FindSectionAt(*segment, approach.gate_segment_distance_m);
      if (section == nullptr) {
        return Result<bool>::Fail(
            CommitFailureCategory::kInternalError,
            "road connection gate has no unique section evaluation");
      }
      approach.gate.boundaries = section->boundaries;
    }

    if (connection.kind == NodeConnectionKind::kPassThrough)
      continue;
    if (connection.kind == NodeConnectionKind::kCorner) {
      if (connection.approaches.size() != 2) {
        return Result<bool>::Fail(CommitFailureCategory::kInternalError,
                                  "road corner approach order is invalid");
      }
      const ResolvedApproach *first = approach_of(
          connection, connection.ordered_approaches[0]);
      const ResolvedApproach *second = approach_of(
          connection, connection.ordered_approaches[1]);
      if (first == nullptr || second == nullptr) {
        return Result<bool>::Fail(CommitFailureCategory::kInternalError,
                                  "road corner approach is missing");
      }
      const DerivedSegment *first_segment = nullptr;
      const DerivedSegment *second_segment = nullptr;
      for (const DerivedSegment &candidate : segments) {
        if (candidate.id == first->key.segment_id)
          first_segment = &candidate;
        if (candidate.id == second->key.segment_id)
          second_segment = &candidate;
      }
      const SectionEvaluation *first_section =
          first_segment == nullptr
              ? nullptr
              : FindSectionAt(*first_segment, first->gate_segment_distance_m);
      const SectionEvaluation *second_section =
          second_segment == nullptr
              ? nullptr
              : FindSectionAt(*second_segment, second->gate_segment_distance_m);
      if (first_section == nullptr || second_section == nullptr) {
        return Result<bool>::Fail(
            CommitFailureCategory::kInternalError,
            "road connection section read model is missing");
      }
      Result<ConnectionGeometry> geometry = internal::generate_connection_geometry(
          connection.node_id, first->gate, second->gate, *first_section,
          *second_section, connection.corner_control_m);
      if (!geometry.ok)
        return Result<bool>::Fail(geometry.failure_category, geometry.error);
      connection.connection_geometry = std::move(geometry.value);
      continue;
    }
    if (connection.kind != NodeConnectionKind::kJunction) {
      return Result<bool>::Fail(CommitFailureCategory::kNotImplemented,
                                "road connection decision is unsupported");
    }

    std::vector<ConnectionGate> gates{};
    std::vector<const SectionEvaluation *> sections{};
    for (const ApproachKey &key : connection.ordered_approaches) {
      const ResolvedApproach *approach = approach_of(connection, key);
      const DerivedSegment *segment = nullptr;
      for (const DerivedSegment &candidate : segments) {
        if (candidate.id == key.segment_id)
          segment = &candidate;
      }
      if (approach == nullptr || segment == nullptr) {
        return Result<bool>::Fail(CommitFailureCategory::kInternalError,
                                  "road junction approach is missing");
      }
      gates.push_back(approach->gate);
      sections.push_back(FindSectionAt(*segment, approach->gate_segment_distance_m));
    }
    Result<JunctionGeometry> geometry = internal::generate_junction_geometry(
        connection.node_id, connection.ordered_approaches, gates, sections,
        connection.junction_corners);
    if (!geometry.ok)
      return Result<bool>::Fail(geometry.failure_category, geometry.error);
    connection.junction_geometry = std::move(geometry.value);
  }
  return Result<bool>::Ok(true);
}

Result<bool> derive_topology_paths(
    const SavedRoadGraph &graph,
    const std::vector<ResolvedConnection> &connections,
    const std::vector<DerivedSegment> &segments,
    std::vector<DerivedLanePath> &lane_paths,
    std::vector<DerivedBoundaryPath> &boundary_paths,
    std::vector<DerivedSeparationArea> &separation_areas) {
  lane_paths.clear();
  boundary_paths.clear();
  separation_areas.clear();
  for (const LaneConnection &topology : graph.lane_connections) {
    const internal::LaneEndpointLookup source_lookup =
        internal::find_lane_endpoint(graph, topology.source);
    const internal::LaneEndpointLookup target_lookup =
        internal::find_lane_endpoint(graph, topology.target);
    const ResolvedConnection *connection =
        connection_of(connections, source_lookup.node_id);
    const ResolvedApproach *source =
        connection == nullptr ? nullptr
                              : approach_of(*connection,
                                            ApproachKey{source_lookup.node_id,
                                                        topology.source.segment_id,
                                                        topology.source.endpoint_role});
    const ResolvedApproach *target =
        connection == nullptr ? nullptr
                              : approach_of(*connection,
                                            ApproachKey{target_lookup.node_id,
                                                        topology.target.segment_id,
                                                        topology.target.endpoint_role});
    const DerivedSegment *source_segment =
        segment_of(segments, topology.source.segment_id);
    const DerivedSegment *target_segment =
        segment_of(segments, topology.target.segment_id);
    const SectionEvaluation *source_section =
        source_segment == nullptr || source == nullptr
            ? nullptr
            : FindSectionAt(*source_segment,
                            source->gate_segment_distance_m);
    const SectionEvaluation *target_section =
        target_segment == nullptr || target == nullptr
            ? nullptr
            : FindSectionAt(*target_segment,
                            target->gate_segment_distance_m);
    if (source_lookup.lane == nullptr || target_lookup.lane == nullptr ||
        source == nullptr || target == nullptr || source_section == nullptr ||
        target_section == nullptr) {
      return Result<bool>::Fail(
          CommitFailureCategory::kInternalError,
          "lane connection path input is missing from resolved road");
    }
    const Result<internal::LaneSectionPosition> source_position =
        lane_section_position(*source_lookup.lane, *source_section);
    const Result<internal::LaneSectionPosition> target_position =
        lane_section_position(*target_lookup.lane, *target_section);
    if (!source_position.ok || !target_position.ok) {
      return Result<bool>::Fail(
          CommitFailureCategory::kInternalError,
          "lane connection path cannot resolve a lane center");
    }
    Result<Path> resolved_path =
        topology.kind == LaneConnectionKind::kJunctionMovement
            ? resolve_junction_movement_path(
                  *connection, *source, source_position.value.lateral_m,
                  *target, target_position.value.lateral_m)
            : Result<Path>::Ok(resolve_lane_transition_path(
                  *source, source_position.value.lateral_m, *target,
                  target_position.value.lateral_m));
    if (!resolved_path.ok) {
      return Result<bool>::Fail(resolved_path.failure_category,
                                resolved_path.error);
    }
    Path path = std::move(resolved_path.value);
    if (std::hypot(path.spans.front().p3.x - path.spans.front().p0.x,
                   path.spans.front().p3.y - path.spans.front().p0.y) <=
        distance_epsilon) {
      lane_paths.push_back(DerivedLanePath{
          topology.id, Path{},
          {endpoint_lane_point(*source, source_position.value)}, 0.0,
          std::numeric_limits<double>::infinity()});
      continue;
    }
    const Result<double> length = PathLength(path);
    if (!length.ok) {
      return Result<bool>::Fail(length.failure_category, length.error);
    }
    const Result<std::vector<Vec3d>> points = sample_lane_path_points(
        path, length.value, endpoint_lane_point(*source, source_position.value),
        endpoint_lane_point(*target, target_position.value));
    if (!points.ok) {
      return Result<bool>::Fail(points.failure_category, points.error);
    }
    lane_paths.push_back(DerivedLanePath{
        topology.id, std::move(path), std::move(points.value), length.value,
        0.0});
    lane_paths.back().minimum_radius_m =
        minimum_radius(lane_paths.back().centerline);
  }

  for (const BoundaryContinuation &topology :
       graph.boundary_continuations) {
    const internal::BoundaryEndpointLookup source_lookup =
        internal::find_boundary_endpoint(graph, topology.source);
    const internal::BoundaryEndpointLookup target_lookup =
        internal::find_boundary_endpoint(graph, topology.target);
    const ResolvedConnection *connection =
        connection_of(connections, source_lookup.node_id);
    const ResolvedApproach *source =
        connection == nullptr ? nullptr
                              : approach_of(*connection,
                                            ApproachKey{source_lookup.node_id,
                                                        topology.source.segment_id,
                                                        topology.source.endpoint_role});
    const ResolvedApproach *target =
        connection == nullptr ? nullptr
                              : approach_of(*connection,
                                            ApproachKey{target_lookup.node_id,
                                                        topology.target.segment_id,
                                                        topology.target.endpoint_role});
    const DerivedSegment *source_segment =
        segment_of(segments, topology.source.segment_id);
    const DerivedSegment *target_segment =
        segment_of(segments, topology.target.segment_id);
    const SectionEvaluation *source_section =
        source_segment == nullptr || source == nullptr
            ? nullptr
            : FindSectionAt(*source_segment,
                            source->gate_segment_distance_m);
    const SectionEvaluation *target_section =
        target_segment == nullptr || target == nullptr
            ? nullptr
            : FindSectionAt(*target_segment,
                            target->gate_segment_distance_m);
    if (source_lookup.boundary == nullptr ||
        target_lookup.boundary == nullptr || source == nullptr ||
        target == nullptr || source_section == nullptr ||
        target_section == nullptr) {
      return Result<bool>::Fail(
          CommitFailureCategory::kInternalError,
          "boundary continuation path input is missing from resolved road");
    }
    const Result<double> source_lateral =
        boundary_lateral(topology.source.boundary_id, *source_section);
    const Result<double> target_lateral =
        boundary_lateral(topology.target.boundary_id, *target_section);
    if (!source_lateral.ok || !target_lateral.ok) {
      return Result<bool>::Fail(
          CommitFailureCategory::kInternalError,
          "boundary continuation path cannot resolve a boundary");
    }
    Path path = resolve_lane_transition_path(
        *source, source_lateral.value, *target, target_lateral.value);
    if (std::hypot(path.spans.front().p3.x - path.spans.front().p0.x,
                   path.spans.front().p3.y - path.spans.front().p0.y) <=
        distance_epsilon) {
      boundary_paths.push_back(
          DerivedBoundaryPath{topology.id, Path{}, 0.0});
      continue;
    }
    const Result<double> length = PathLength(path);
    if (!length.ok) {
      return Result<bool>::Fail(length.failure_category, length.error);
    }
    boundary_paths.push_back(
        DerivedBoundaryPath{topology.id, std::move(path), length.value});
  }
  for (const LaneConnection &connection : graph.lane_connections) {
    if (connection.kind != LaneConnectionKind::kSplit)
      continue;
    std::vector<const DerivedBoundaryPath *> sides{};
    for (const BoundaryContinuation &boundary :
         graph.boundary_continuations) {
      if (boundary.source.segment_id != connection.source.segment_id ||
          boundary.target.segment_id != connection.target.segment_id)
        continue;
      const auto path = std::find_if(
          boundary_paths.begin(), boundary_paths.end(),
          [&boundary](const DerivedBoundaryPath &candidate) {
            return candidate.continuation_id == boundary.id;
          });
      if (path != boundary_paths.end() && path->path.spans.size() == 1)
        sides.push_back(&*path);
    }
    if (sides.size() != 2)
      continue;
    DerivedSeparationArea area{};
    area.connection_id = connection.id;
    constexpr int samples = 12;
    for (int index = 0; index <= samples; ++index) {
      const Vec2d point = evaluate_span(
          sides[0]->path.spans.front(),
          static_cast<double>(index) / static_cast<double>(samples));
      area.perimeter.push_back(to3(point));
    }
    for (int index = samples; index >= 0; --index) {
      const Vec2d point = evaluate_span(
          sides[1]->path.spans.front(),
          static_cast<double>(index) / static_cast<double>(samples));
      area.perimeter.push_back(to3(point));
    }
    separation_areas.push_back(std::move(area));
  }
  return Result<bool>::Ok(true);
}

} // namespace city::road::generation
