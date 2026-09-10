#include "city/road/road.hpp"

#include "derived_view.hpp"
#include "fixtures/layouts.hpp"
#include "../src/generation/generation.hpp"
#include "../src/lookup.hpp"
#include "../src/persistence/archive.hpp"

#include <bit>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>

namespace {

#define ROAD_CONTRACT_EXPECT(condition, message) \
  do {                                           \
    if (!(condition)) {                          \
      failure = (message);                       \
      return false;                              \
    }                                            \
  } while (false)

using namespace city::road;

void append_boundary(std::ostringstream& out, const SectionBoundarySample& value) {
  out << value.boundary_id << ',' << static_cast<int>(value.role) << ',' << value.lateral_m << ','
      << value.height_m << ',' << value.marking.enabled << ','
      << static_cast<int>(value.marking.role) << ',' << value.marking.style_id.value << ';';
}

void append_approach(std::ostringstream& out, const ApproachKey& value) {
  out << value.node_id << ',' << value.segment_id << ','
      << static_cast<int>(value.endpoint_role);
}

void append_gate(std::ostringstream& out, const ConnectionGate& value) {
  append_approach(out, value.approach);
  out << ':' << value.segment_id << ',' << value.node_id << ',' << value.position.x << ',' << value.position.y << ','
      << value.position.z << ',' << value.tangent.x << ',' << value.tangent.y << ',' << value.tangent.z << ','
      << value.lateral.x << ',' << value.lateral.y << ',' << value.lateral.z << ','
      << value.normal.x << ',' << value.normal.y << ',' << value.normal.z << ':';
  for (const auto& boundary : value.boundaries) append_boundary(out, boundary);
}

void append_meshes(std::ostringstream& out, const std::vector<Mesh>& meshes) {
  out << meshes.size() << ':';
  for (const auto& mesh : meshes) {
    out << mesh.owner_segment_id << ',' << static_cast<int>(mesh.style.domain)
        << ':' << mesh.style.value << ',' << mesh.vertices.size() << ','
        << mesh.indices.size() << ':';
    for (const auto& vertex : mesh.vertices) out << vertex.x << ',' << vertex.y << ',' << vertex.z << ';';
    for (const auto index : mesh.indices) out << index << ',';
  }
}

std::string path_observation(const Path& path) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::hexfloat;
  for (const BezierSpan& span : path.spans) {
    out << span.p0.x << ',' << span.p0.y << ',' << span.p1.x << ','
        << span.p1.y << ',' << span.p2.x << ',' << span.p2.y << ','
        << span.p3.x << ',' << span.p3.y << ';';
  }
  return out.str();
}

void append_owner(std::ostringstream& out, const MarkingOwner& owner) {
  out << static_cast<int>(owner.kind) << ',' << owner.segment_id << ','
      << owner.node_id << ',' << owner.manual_id;
}

std::string derived_observation(const DerivedRoad& derived) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << std::hexfloat;
  out << derived.segments.size() << ':';
  for (const DerivedSegment& segment : derived.segments) {
    out << segment.id << ',' << segment.length_m << ',' << segment.surface_start_m << ','
        << segment.surface_end_m << ',' << segment.alignment.spans.size() << ':';
    for (const auto& span : segment.alignment.spans) {
      out << span.p0.x << ',' << span.p0.y << ',' << span.p1.x << ',' << span.p1.y << ','
          << span.p2.x << ',' << span.p2.y << ',' << span.p3.x << ',' << span.p3.y << ';';
    }
    for (double distance : segment.semantic_segment_distances_m) out << distance << ',';
    out << ':';
    for (double distance : segment.surface_segment_distances_m) out << distance << ',';
    out << ':' << segment.sections.size() << ':';
    for (const SectionEvaluation& section : segment.sections) {
      out << section.segment_id << ',' << section.segment_distance_m << ','
          << section.resolved_template_id << ':';
      for (const auto& boundary : section.boundaries) append_boundary(out, boundary);
      for (const auto& style : section.surface_styles) {
        out << static_cast<int>(style.domain) << ':' << style.value << ';';
      }
    }
  }
  out << derived.connections.size() << ':';
  for (const ResolvedConnection& connection : derived.connections) {
    out << connection.node_id << ',' << static_cast<int>(connection.kind) << ','
        << connection.corner_radius_m << ',' << connection.corner_control_m << ','
        << connection.applied_policy_override_id << ':';
    for (const ApproachKey& key : connection.ordered_approaches) {
      append_approach(out, key);
      out << ';';
    }
    for (const ResolvedApproach& approach : connection.approaches) {
      append_approach(out, approach.key);
      out << ',' << approach.endpoint_template_id << ',' << approach.auto_setback_m << ','
          << approach.resolved_setback_m << ',' << approach.resolved_lateral_shift_m << ','
          << approach.gate_segment_distance_m << ';';
      append_gate(out, approach.gate);
    }
    for (const auto& corner : connection.junction_corners) {
      append_approach(out, corner.first_approach);
      append_approach(out, corner.second_approach);
      out << ',' << corner.radius_m << ';';
    }
    out << connection.connection_geometry.surface_strips.size() << ':';
    for (const ResolvedSurfaceStrip& strip : connection.connection_geometry.surface_strips) {
      out << static_cast<int>(strip.style.domain) << ':' << strip.style.value << ','
          << strip.left_boundary_id << ',' << strip.right_boundary_id << ':';
      for (const Vec3d& point : strip.left) {
        out << point.x << ',' << point.y << ',' << point.z << ';';
      }
      for (const Vec3d& point : strip.right) {
        out << point.x << ',' << point.y << ',' << point.z << ';';
      }
    }
    out << connection.junction_geometry.surface_regions.size() << ','
        << connection.junction_geometry.surface_strips.size() << ':';
    for (const ResolvedSurfaceRegion& region : connection.junction_geometry.surface_regions) {
      out << static_cast<int>(region.style.domain) << ':' << region.style.value << ':';
      for (const Vec3d& point : region.perimeter) {
        out << point.x << ',' << point.y << ',' << point.z << ';';
      }
    }
  }
  out << derived.markings.size() << ':';
  for (const DerivedMarking& marking : derived.markings) {
    append_owner(out, marking.owner);
    out << ':' << marking.boundary_id << ',' << static_cast<int>(marking.role) << ','
        << marking.style_id.value << ',' << marking.width_m << ':';
    for (const Vec3d& point : marking.points) {
      out << point.x << ',' << point.y << ',' << point.z << ';';
    }
    out << ':';
    for (const Vec3d& point : marking.polygon) {
      out << point.x << ',' << point.y << ',' << point.z << ';';
    }
  }
  out << derived.terrain_masks.size() << ':';
  for (const auto& mask : derived.terrain_masks) {
    out << mask.segment_id << ',' << mask.points.size() << ':';
    for (const auto& point : mask.points) out << point.x << ',' << point.y << ';';
  }
  append_meshes(out, derived.segment_meshes);
  append_meshes(out, derived.marking_meshes);
  append_meshes(out, derived.connection_meshes);
  append_meshes(out, derived.junction_meshes);
  return out.str();
}

std::uint64_t next_id_observation(std::string_view authoritative) {
  const std::string_view key = "next_id=";
  const std::size_t begin = authoritative.find(key);
  if (begin == std::string_view::npos) return 0;
  const std::size_t value_begin = begin + key.size();
  const std::size_t end = authoritative.find('\n', value_begin);
  return static_cast<std::uint64_t>(std::stoull(std::string(authoritative.substr(value_begin, end - value_begin))));
}

std::string query_observation(const RoadState& state) {
  std::ostringstream out;
  out << state.graph().nodes.size() << ',' << state.graph().segments.size() << ','
      << state.graph().layout_templates.size() << ',' << state.graph().transitions.size() << ','
      << state.graph().connection_policy_overrides.size() << ',' << state.graph().manual_lines.size() << ','
      << state.graph().manual_areas.size() << ':';
  for (const auto& node : state.graph().nodes) out << 'n' << node.id << ';';
  for (const auto& segment : state.graph().segments) out << 's' << segment.id << ';';
  for (const auto& gate : road_test_view::gates(state.derived())) out << 'g' << gate.segment_id << ',' << gate.node_id << ';';
  return out.str();
}

struct StateObservation {
  std::string authoritative{};
  std::string derived{};
  std::uint64_t next_id = 0;
  std::string query{};

  bool operator==(const StateObservation&) const = default;
};

StateObservation observe(const RoadState& state) {
  const auto saved = state.Save();
  if (!saved.ok) return {};
  return StateObservation{saved.value, derived_observation(state.derived()), next_id_observation(saved.value),
                          query_observation(state)};
}

template <typename Operation>
bool expect_failed_unchanged(RoadState& state, Operation&& operation, std::string_view label, std::string& failure) {
  const StateObservation before = observe(state);
  const auto result = operation();
  if (result.ok) {
    failure = std::string(label) + " unexpectedly succeeded";
    return false;
  }
  if (!(observe(state) == before)) {
    failure = std::string(label) + " changed authoritative, derived, next ID, or query state after failure";
    return false;
  }
  return true;
}

bool all_public_operation_validation_failures_are_atomic(std::string& failure) {
  RoadState state{};
  const auto section = road_fixture::AddLayout(state, road_fixture::BidirectionalLayout(0));
  ROAD_CONTRACT_EXPECT(expect_failed_unchanged(state, [&] { return state.AddSegment(city::road::AddSegmentRequest{{}, section}); }, "AddSegment", failure),
                       failure);
  const Path line = MakePath({MakeLine({0.0, 0.0}, {20.0, 0.0})});
  ROAD_CONTRACT_EXPECT(expect_failed_unchanged(
                           state,
                           [&] {
                              return state.ExtendCorridorFromEnd(
                                  ExtendCorridorFromEndRequest{999999, 999998,
                                                               line, section});
                           },
                           "ExtendCorridorFromEnd", failure),
                       failure);
  ROAD_CONTRACT_EXPECT(expect_failed_unchanged(
                           state, [&] { return state.AddSegmentConnectedTo(city::road::AddSegmentConnectedToRequest{line, section, 999999}); },
                           "AddSegmentConnectedTo", failure), failure);
  ROAD_CONTRACT_EXPECT(expect_failed_unchanged(
                           state, [&] { return state.AddSegmentConnectedToSegment(city::road::AddSegmentConnectedToSegmentRequest{line, section, 999999, 10.0}); },
                           "AddSegmentConnectedToSegment", failure), failure);
  ROAD_CONTRACT_EXPECT(
      expect_failed_unchanged(
          state,
          [&] {
            return state.AddSegmentBetween(AddSegmentBetweenRequest{
                line, section, RoadConnectionTarget{999998, 0, 0.0},
                RoadConnectionTarget{999999, 0, 0.0}});
          },
          "AddSegmentBetween", failure),
      failure);
  ROAD_CONTRACT_EXPECT(
      expect_failed_unchanged(
          state,
          [&] {
            return state.SplitSegmentAtDistance(
                SplitSegmentAtDistanceRequest{999999, 10.0});
          },
          "SplitSegmentAtDistance", failure),
      failure);
  ROAD_CONTRACT_EXPECT(expect_failed_unchanged(
                           state, [&] { return state.DeleteSegment(city::road::DeleteSegmentRequest{999999}); }, "DeleteSegment", failure), failure);
  ROAD_CONTRACT_EXPECT(expect_failed_unchanged(
                           state, [&] { return state.EditSegmentShape(city::road::EditSegmentShapeRequest{999999, {}}); }, "EditSegmentShape", failure),
                       failure);
  ROAD_CONTRACT_EXPECT(expect_failed_unchanged(
                           state, [&] { return state.MoveNode(city::road::MoveNodeRequest{999999, {0.0, 0.0}}); }, "MoveNode", failure), failure);
  ROAD_CONTRACT_EXPECT(expect_failed_unchanged(
                           state, [&] { return state.AddRoadLayoutTemplate(city::road::AddRoadLayoutTemplateRequest{{}}); }, "AddRoadLayoutTemplate", failure),
                       failure);
  RoadLayoutTemplate missing = road_fixture::BidirectionalLayout(999999);
  ROAD_CONTRACT_EXPECT(expect_failed_unchanged(
                           state, [&] { return state.EditRoadLayoutTemplate(city::road::EditRoadLayoutTemplateRequest{missing}); }, "EditRoadLayoutTemplate", failure),
                       failure);
  AddLaneRequest lane{};
  lane.corridor_id = 999999;
  lane.transition_start = SegmentPosition{999998, 0.1};
  lane.transition_complete = SegmentPosition{999998, 0.9};
  lane.lane_width_m = 3.0;
  ROAD_CONTRACT_EXPECT(expect_failed_unchanged(
                           state, [&] { return state.AddLane(lane); }, "AddLane", failure),
                       failure);
  return true;
}

bool extension_leaves_the_existing_segment_bit_identical(std::string& failure) {
  RoadState state{};
  const auto layout = road_fixture::AddLayout(state, road_fixture::BidirectionalLayout(0));
  const auto first = state.AddSegment(AddSegmentRequest{
      MakePath({MakeLine({0.0, 0.0}, {20.0, 0.0})}), layout});
  ROAD_CONTRACT_EXPECT(first.ok, first.error);

  const RoadSegment before = state.graph().segments.front();
  const Path* alignment_before = FindCanonicalAlignment(state.derived(), first.value);
  ROAD_CONTRACT_EXPECT(alignment_before != nullptr, "extension baseline alignment is missing");
  const std::string shape_before = path_observation(*alignment_before);
  const std::vector<RoadNode> nodes_before = state.graph().nodes;
  const RoadCorridorId corridor_id = state.graph().corridors.front().id;

  const auto extended = state.ExtendCorridorFromEnd(ExtendCorridorFromEndRequest{
      corridor_id, before.node_b,
      MakePath({MakeLine({20.0, 0.0}, {44.0, 0.0})}), layout});
  ROAD_CONTRACT_EXPECT(extended.ok, extended.error);

  // Extending appends. It does not rewrite the segment or the nodes that were
  // already confirmed.
  ROAD_CONTRACT_EXPECT(state.graph().segments.size() == 2 &&
                           state.graph().nodes.size() == nodes_before.size() + 1 &&
                           state.graph().corridors.size() == 1,
                       "extension did not append exactly one segment and one node");
  const auto retained =
      std::find_if(state.graph().segments.begin(), state.graph().segments.end(),
                   [id = first.value](const RoadSegment& segment) {
                     return segment.id == id;
                   });
  ROAD_CONTRACT_EXPECT(retained != state.graph().segments.end(),
                       "extension removed the segment it extended from");
  ROAD_CONTRACT_EXPECT(retained->node_a == before.node_a &&
                           retained->node_b == before.node_b &&
                           retained->layout_template == before.layout_template,
                       "extension changed the endpoints of the existing segment");
  const Path* alignment_after = FindCanonicalAlignment(state.derived(), first.value);
  ROAD_CONTRACT_EXPECT(
      alignment_after != nullptr && path_observation(*alignment_after) == shape_before,
      "extension changed the shape of the existing segment");
  for (const RoadNode& node : nodes_before) {
    const auto same = std::find_if(state.graph().nodes.begin(), state.graph().nodes.end(),
                                   [&node](const RoadNode& candidate) {
                                     return candidate.id == node.id;
                                   });
    ROAD_CONTRACT_EXPECT(
        same != state.graph().nodes.end() &&
            std::bit_cast<std::uint64_t>(same->position.x) ==
                std::bit_cast<std::uint64_t>(node.position.x) &&
            std::bit_cast<std::uint64_t>(same->position.y) ==
                std::bit_cast<std::uint64_t>(node.position.y),
        "extension moved a node that already existed");
  }
  return true;
}

bool confirmation_boundaries_define_segment_units(std::string& failure) {
  const Path first =
      MakePath({MakeLine({0.0, 0.0}, {20.0, 0.0})});
  const Path second =
      MakePath({MakeLine({20.0, 0.0}, {23.0, 4.0})});
  const Path complete =
      MakePath({first.spans.front(), second.spans.front()});

  RoadState one_operation{};
  const auto one_operation_section = road_fixture::AddLayout(one_operation, road_fixture::BidirectionalLayout(0));
  const auto complete_added =
      one_operation.AddSegment(AddSegmentRequest{complete, one_operation_section});
  ROAD_CONTRACT_EXPECT(complete_added.ok, complete_added.error);

  RoadState incremental{};
  const auto incremental_section = road_fixture::AddLayout(incremental, road_fixture::BidirectionalLayout(0));
  const auto first_added =
      incremental.AddSegment(AddSegmentRequest{first, incremental_section});
  ROAD_CONTRACT_EXPECT(first_added.ok, first_added.error);
  const RoadSegment& segment = incremental.graph().segments.front();
  const RoadCorridor* corridor =
      FindCorridorForSegment(incremental.graph(), segment.id);
  ROAD_CONTRACT_EXPECT(corridor != nullptr,
                       "initial segment has no road corridor");
  const auto extended = incremental.ExtendCorridorFromEnd(
      ExtendCorridorFromEndRequest{corridor->id, segment.node_b, second, incremental_section});
  ROAD_CONTRACT_EXPECT(extended.ok, extended.error);
  ROAD_CONTRACT_EXPECT(extended.value != first_added.value,
                       "extension reused the existing segment identity");
  ROAD_CONTRACT_EXPECT(incremental.graph().segments.size() == 2 &&
                           incremental.graph().nodes.size() == 3 &&
                           incremental.graph().corridors.size() == 1,
                       "extension did not create a local segment chain");
  ROAD_CONTRACT_EXPECT(
      one_operation.graph().segments.size() == 1 &&
          one_operation.graph().nodes.size() == 2 &&
          one_operation.graph().segments.front().shape.internal_knots.size() == 1,
      "one confirmed multi-span path was not retained as one user segment");
  ROAD_CONTRACT_EXPECT(
      incremental.graph().segments.size() == 2 &&
          incremental.graph().nodes.size() == 3,
      "a later confirmed extension did not create a new user segment");

  const auto before_first = observe(incremental);
  const Path prepend =
      MakePath({MakeLine({0.0, 0.0}, {-12.0, 16.0})});
  const auto prepended = incremental.ExtendCorridorFromEnd(
      ExtendCorridorFromEndRequest{corridor->id, segment.node_a, prepend, incremental_section});
  ROAD_CONTRACT_EXPECT(!prepended.ok && observe(incremental) == before_first,
                       "corridor start extension was not atomic unsupported");
  return true;
}

RoadLayoutTemplate off_centre_layout() {
  RoadLayoutTemplate layout = road_fixture::BidirectionalLayout(0);
  layout.alignment_offset_from_left_m = 3.0;
  return layout;
}

bool layout_alignment_offset_failures_are_atomic(std::string& failure) {
  RoadState state{};
  const auto accepted = road_fixture::AddLayout(state, off_centre_layout());
  ROAD_CONTRACT_EXPECT(accepted != 0, "an off-centre layout was rejected");
  const auto added = state.AddSegment(AddSegmentRequest{
      MakePath({MakeLine({0.0, 0.0}, {40.0, 0.0})}), accepted});
  ROAD_CONTRACT_EXPECT(added.ok, added.error);

  // An alignment that falls outside the layout it belongs to has no meaning,
  // and neither adding nor editing such a layout may leave anything behind.
  for (const double offset : {-0.5, 11.0,
                              std::numeric_limits<double>::quiet_NaN()}) {
    RoadLayoutTemplate rejected = road_fixture::BidirectionalLayout(0);
    rejected.alignment_offset_from_left_m = offset;
    ROAD_CONTRACT_EXPECT(
        expect_failed_unchanged(
            state,
            [&] {
              return state.AddRoadLayoutTemplate(
                  AddRoadLayoutTemplateRequest{rejected});
            },
            "AddRoadLayoutTemplate with an out-of-range alignment", failure),
        failure);

    RoadLayoutTemplate edited = road_fixture::BidirectionalLayout(accepted);
    edited.alignment_offset_from_left_m = offset;
    ROAD_CONTRACT_EXPECT(
        expect_failed_unchanged(
            state,
            [&] {
              return state.EditRoadLayoutTemplate(
                  EditRoadLayoutTemplateRequest{edited});
            },
            "EditRoadLayoutTemplate with an out-of-range alignment", failure),
        failure);
  }

  // A layout that never said where its alignment runs is not a layout Core can
  // place, so it is rejected the same way.
  RoadLayoutTemplate unset = road_fixture::BidirectionalLayout(0);
  unset.alignment_offset_from_left_m = RoadLayoutTemplate{}.alignment_offset_from_left_m;
  ROAD_CONTRACT_EXPECT(
      expect_failed_unchanged(
          state,
          [&] {
            return state.AddRoadLayoutTemplate(AddRoadLayoutTemplateRequest{unset});
          },
          "AddRoadLayoutTemplate without an alignment offset", failure),
      failure);
  return true;
}

bool boundary_profile_failures_are_atomic(std::string& failure) {
  RoadState state{};
  const auto accepted = road_fixture::AddLayout(state, road_fixture::GutteredLayout(0));
  ROAD_CONTRACT_EXPECT(accepted != 0, "a guttered layout was rejected");
  ROAD_CONTRACT_EXPECT(
      state
          .AddSegment(AddSegmentRequest{
              MakePath({MakeLine({0.0, 0.0}, {40.0, 0.0})}), accepted})
          .ok,
      "a guttered road could not be drawn");

  // A profile reaching further than the strip it reaches into, one running
  // backwards, one with no face for a gap it spans, and one with a face style
  // that does not exist. None of them describes a section that can be built.
  std::vector<RoadLayoutTemplate> rejected{};
  RoadLayoutTemplate overruns = road_fixture::GutteredLayout(0);
  overruns.boundaries.front().contour.back().lateral_m = 9.0;
  rejected.push_back(overruns);
  RoadLayoutTemplate backwards = road_fixture::GutteredLayout(0);
  std::reverse(backwards.boundaries.front().contour.begin(),
               backwards.boundaries.front().contour.end());
  rejected.push_back(backwards);
  RoadLayoutTemplate unfaced = road_fixture::GutteredLayout(0);
  unfaced.boundaries.front().segment_styles.pop_back();
  rejected.push_back(unfaced);
  RoadLayoutTemplate unknown_face = road_fixture::GutteredLayout(0);
  unknown_face.boundaries.front().segment_styles.front() = SurfaceStyleId{4242};
  rejected.push_back(unknown_face);

  for (RoadLayoutTemplate& candidate : rejected) {
    ROAD_CONTRACT_EXPECT(
        expect_failed_unchanged(
            state,
            [&] {
              return state.AddRoadLayoutTemplate(
                  AddRoadLayoutTemplateRequest{candidate});
            },
            "AddRoadLayoutTemplate with an unbuildable profile", failure),
        failure);
    candidate.id = accepted;
    ROAD_CONTRACT_EXPECT(
        expect_failed_unchanged(
            state,
            [&] {
              return state.EditRoadLayoutTemplate(
                  EditRoadLayoutTemplateRequest{candidate});
            },
            "EditRoadLayoutTemplate with an unbuildable profile", failure),
        failure);
  }
  return true;
}

bool off_centre_layout_survives_reverse_input_and_repeat(std::string& failure) {
  const auto build = [](bool reversed) {
    RoadState state{};
    const auto layout = road_fixture::AddLayout(state, off_centre_layout());
    const Path path =
        reversed ? MakePath({MakeLine({40.0, 0.0}, {0.0, 0.0})})
                 : MakePath({MakeLine({0.0, 0.0}, {40.0, 0.0})});
    const auto added = state.AddSegment(AddSegmentRequest{path, layout});
    return std::pair{added.ok, derived_observation(state.derived())};
  };
  const auto forward = build(false);
  const auto repeated = build(false);
  const auto reversed = build(true);
  ROAD_CONTRACT_EXPECT(forward.first && repeated.first && reversed.first,
                       "an off-centre road could not be drawn");
  ROAD_CONTRACT_EXPECT(forward.second == repeated.second,
                       "the same off-centre input produced different geometry");

  // Reversed input draws the same road the other way round, so the layout keeps
  // the same distances to the alignment, measured from its own left outer end.
  RoadState reverse_state{};
  const auto reverse_layout = road_fixture::AddLayout(reverse_state, off_centre_layout());
  ROAD_CONTRACT_EXPECT(
      reverse_state
          .AddSegment(AddSegmentRequest{
              MakePath({MakeLine({40.0, 0.0}, {0.0, 0.0})}), reverse_layout})
          .ok,
      "the reversed off-centre road could not be drawn");
  for (const SectionEvaluation* section :
       road_test_view::sections(reverse_state.derived())) {
    ROAD_CONTRACT_EXPECT(
        std::abs(section->boundaries.front().lateral_m + 3.0) < 1e-9 &&
            std::abs(section->boundaries.back().lateral_m - 7.0) < 1e-9,
        "reversed input changed what the layout measures from");
  }
  return true;
}

bool reverse_input_has_equivalent_geometry(std::string& failure) {
  const Path forward_path = MakePath({
      MakeLine({0.0, 0.0}, {20.0, 0.0}),
      MakeLine({20.0, 0.0}, {32.0, 16.0}),
  });
  Path reverse_path{};
  for (auto it = forward_path.spans.rbegin();
       it != forward_path.spans.rend(); ++it) {
    reverse_path.spans.push_back(
        BezierSpan{it->p3, it->p2, it->p1, it->p0});
  }
  RoadState forward{};
  const auto forward_section = road_fixture::AddLayout(forward, road_fixture::BidirectionalLayout(0));
  RoadState reverse{};
  const auto reverse_section = road_fixture::AddLayout(reverse, road_fixture::BidirectionalLayout(0));
  const auto forward_added =
      forward.AddSegment(AddSegmentRequest{forward_path, forward_section});
  const auto reverse_added =
      reverse.AddSegment(AddSegmentRequest{reverse_path, reverse_section});
  ROAD_CONTRACT_EXPECT(forward_added.ok && reverse_added.ok,
                       "reverse geometry setup failed");
  ROAD_CONTRACT_EXPECT(forward.graph().corridors.size() == 1 &&
                           reverse.graph().corridors.size() == 1,
                       "reverse corridor is missing");
  const RoadCorridor& forward_corridor = forward.graph().corridors.front();
  const RoadCorridor& reverse_corridor = reverse.graph().corridors.front();
  ROAD_CONTRACT_EXPECT(forward_corridor.segments.size() ==
                           reverse_corridor.segments.size(),
                       "reverse corridor segment count differs");
  const auto close = [](Vec2d a, Vec2d b) {
    return std::abs(a.x - b.x) <= 1e-9 &&
           std::abs(a.y - b.y) <= 1e-9;
  };
  const Path* forward_alignment = FindCanonicalAlignment(
      forward.derived(), forward_corridor.segments.front().segment_id);
  const Path* reverse_alignment = FindCanonicalAlignment(
      reverse.derived(), reverse_corridor.segments.front().segment_id);
  ROAD_CONTRACT_EXPECT(forward_alignment != nullptr &&
                           reverse_alignment != nullptr &&
                           forward_alignment->spans.size() ==
                               reverse_alignment->spans.size(),
                       "reverse canonical alignment is missing");
  for (std::size_t index = 0; index < forward_alignment->spans.size();
       ++index) {
    const BezierSpan& a = forward_alignment->spans[index];
    const BezierSpan& b =
        reverse_alignment->spans[reverse_alignment->spans.size() - 1 - index];
    ROAD_CONTRACT_EXPECT(
        close(a.p0, b.p3) && close(a.p1, b.p2) &&
            close(a.p2, b.p1) && close(a.p3, b.p0),
        "reverse canonical Bezier geometry differs");
  }
  const auto vertex_signature = [](const std::vector<Mesh>& meshes) {
    std::vector<std::string> signature{};
    for (const Mesh& mesh : meshes) {
      std::vector<std::array<long long, 3>> vertices{};
      vertices.reserve(mesh.vertices.size());
      for (const Vec3d& vertex : mesh.vertices) {
        vertices.push_back({
            std::llround(vertex.x * 1e6),
            std::llround(vertex.y * 1e6),
            std::llround(vertex.z * 1e6),
        });
      }
      std::sort(vertices.begin(), vertices.end());
      std::ostringstream row;
      row << static_cast<int>(mesh.style.domain) << ':' << mesh.style.value << ':';
      for (const auto& vertex : vertices) {
        row << vertex[0] << ',' << vertex[1] << ',' << vertex[2] << ';';
      }
      signature.push_back(row.str());
    }
    std::sort(signature.begin(), signature.end());
    return signature;
  };
  ROAD_CONTRACT_EXPECT(
      vertex_signature(forward.derived().segment_meshes) ==
          vertex_signature(reverse.derived().segment_meshes),
      "reverse surface mesh geometry differs");
  const auto equivalent_vertices = [](const std::vector<Mesh>& a,
                                      const std::vector<Mesh>& b,
                                      std::string& diagnostic) {
    if (a.size() != b.size()) {
      diagnostic = "mesh count";
      return false;
    }
    for (const Mesh& source : a) {
      const auto target = std::find_if(
          b.begin(), b.end(), [&source](const Mesh& candidate) {
            return candidate.style == source.style &&
                   candidate.vertices.size() == source.vertices.size();
          });
      if (target == b.end()) {
        diagnostic = "style or vertex count";
        return false;
      }
      std::vector<bool> matched(target->vertices.size(), false);
      for (const Vec3d& vertex : source.vertices) {
        bool found = false;
        for (std::size_t index = 0; index < target->vertices.size(); ++index) {
          if (matched[index]) continue;
          const Vec3d& candidate = target->vertices[index];
          if (std::abs(vertex.x - candidate.x) <= 1e-6 &&
              std::abs(vertex.y - candidate.y) <= 1e-6 &&
              std::abs(vertex.z - candidate.z) <= 1e-6) {
            matched[index] = true;
            found = true;
            break;
          }
        }
        if (!found) {
          double nearest = std::numeric_limits<double>::infinity();
          for (const Vec3d& candidate : target->vertices) {
            nearest = std::min(
                nearest,
                std::hypot(vertex.x - candidate.x,
                           vertex.y - candidate.y));
          }
          diagnostic = "unmatched " + std::to_string(vertex.x) + "," +
                       std::to_string(vertex.y) + " nearest=" +
                       std::to_string(nearest);
          return false;
        }
      }
    }
    return true;
  };
  const auto marking_centers = [](const std::vector<DerivedMarking>& markings) {
    std::vector<Mesh> meshes{};
    for (const DerivedMarking& marking : markings) {
      Mesh mesh{};
      mesh.style = RenderStyleFromMarking(marking.style_id);
      mesh.vertices = marking.points;
      mesh.vertices.insert(mesh.vertices.end(), marking.polygon.begin(),
                           marking.polygon.end());
      meshes.push_back(std::move(mesh));
    }
    return meshes;
  };
  const std::vector<Mesh> forward_markings =
      marking_centers(forward.derived().markings);
  const std::vector<Mesh> reverse_markings =
      marking_centers(reverse.derived().markings);
  ROAD_CONTRACT_EXPECT(
      vertex_signature(forward_markings) == vertex_signature(reverse_markings),
      "reverse marking geometry differs");
  return true;
}

bool extension_semantic_boundaries_are_atomic(std::string& failure) {
  const Path base_path =
      MakePath({MakeLine({0.0, 0.0}, {20.0, 0.0})});
  const Path extension =
      MakePath({MakeLine({20.0, 0.0}, {32.0, 16.0})});

  RoadState connected{};
  const auto connected_section = road_fixture::AddLayout(connected, road_fixture::BidirectionalLayout(0));
  const auto connected_base =
      connected.AddSegment(AddSegmentRequest{base_path, connected_section});
  ROAD_CONTRACT_EXPECT(connected_base.ok, connected_base.error);
  const RoadSegment connected_segment = connected.graph().segments.front();
  const auto branch = connected.AddSegmentConnectedTo(
      AddSegmentConnectedToRequest{extension, connected_section, connected_segment.node_b});
  ROAD_CONTRACT_EXPECT(branch.ok, branch.error);
  ROAD_CONTRACT_EXPECT(
      expect_failed_unchanged(
          connected,
          [&] {
            const RoadCorridor* corridor =
                FindCorridorForSegment(connected.graph(),
                                       connected_segment.id);
            return connected.ExtendCorridorFromEnd(
                ExtendCorridorFromEndRequest{
                    corridor == nullptr ? 0 : corridor->id,
                    connected_segment.node_b, extension, connected_section});
          },
          "degree-two ExtendCorridorFromEnd", failure),
      failure);

  RoadState mixed_section{};
  const auto mixed_section_section = road_fixture::AddLayout(mixed_section, road_fixture::BidirectionalLayout(0));
  const auto section =
      mixed_section.AddRoadLayoutTemplate(
          AddRoadLayoutTemplateRequest{road_fixture::ExtraLaneLayout(0)});
  ROAD_CONTRACT_EXPECT(section.ok, section.error);
  const auto mixed_base =
      mixed_section.AddSegment(AddSegmentRequest{base_path, mixed_section_section});
  ROAD_CONTRACT_EXPECT(mixed_base.ok, mixed_base.error);
  const RoadSegment mixed_segment = mixed_section.graph().segments.front();
  const RoadCorridor* mixed_corridor =
      FindCorridorForSegment(mixed_section.graph(), mixed_segment.id);
  ROAD_CONTRACT_EXPECT(mixed_corridor != nullptr,
                       "mixed-section base has no corridor");
  const auto mixed_extension = mixed_section.ExtendCorridorFromEnd(
      ExtendCorridorFromEndRequest{mixed_corridor->id, mixed_segment.node_b,
                                   extension, section.value});
  ROAD_CONTRACT_EXPECT(mixed_extension.ok,
                       "mixed-section ExtendCorridorFromEnd failed: " +
                           mixed_extension.error);
  ROAD_CONTRACT_EXPECT(mixed_section.derived().connections.size() == 1,
                       "mixed-section extension did not create one connection");
  ROAD_CONTRACT_EXPECT(
      !mixed_section.derived().connections.front().connection_geometry
           .surface_strips.empty(),
      "mixed-section extension created no connector surface");

  RoadState from_start{};
  const auto from_start_section = road_fixture::AddLayout(from_start, road_fixture::BidirectionalLayout(0));
  const auto start_base =
      from_start.AddSegment(AddSegmentRequest{base_path, from_start_section});
  ROAD_CONTRACT_EXPECT(start_base.ok, start_base.error);
  const RoadSegment start_segment = from_start.graph().segments.front();
  const Path prepend =
      MakePath({MakeLine({0.0, 0.0}, {-12.0, 16.0})});
  ROAD_CONTRACT_EXPECT(
      expect_failed_unchanged(
          from_start,
          [&] {
            const RoadCorridor* corridor =
                FindCorridorForSegment(from_start.graph(), start_segment.id);
            return from_start.ExtendCorridorFromEnd(
                ExtendCorridorFromEndRequest{
                    corridor == nullptr ? 0 : corridor->id,
                    start_segment.node_a, prepend, from_start_section});
          },
          "corridor-start ExtendCorridorFromEnd", failure),
      failure);
  return true;
}

bool isolated_segment_uses_simple_path(std::string& failure) {
  RoadState state{};
  const auto section = road_fixture::AddLayout(state, road_fixture::BidirectionalLayout(0));
  const auto added = state.AddSegment(AddSegmentRequest{
      MakePath({MakeLine({0.0, 0.0}, {30.0, 0.0})}), section});
  ROAD_CONTRACT_EXPECT(added.ok, added.error);
  ROAD_CONTRACT_EXPECT(state.derived().connections.empty(),
                       "isolated segment created a connection entity");
  ROAD_CONTRACT_EXPECT(road_test_view::gates(state.derived()).empty(),
                       "isolated segment created connection gates");
  ROAD_CONTRACT_EXPECT(state.derived().connection_meshes.empty() &&
                           state.derived().junction_meshes.empty(),
                       "isolated segment entered connection geometry");
  return true;
}

bool approach_override_resolves_and_persists_manual_fields(std::string& failure) {
  RoadState state{};
  const auto section = road_fixture::AddLayout(state, road_fixture::BidirectionalLayout(0));
  const auto base = state.AddSegment(
      AddSegmentRequest{MakePath({MakeLine({0.0, 0.0}, {40.0, 0.0})}), section});
  ROAD_CONTRACT_EXPECT(base.ok, base.error);
  const auto branch = state.AddSegmentConnectedToSegment(
      AddSegmentConnectedToSegmentRequest{
          MakePath({MakeLine({20.0, 0.0}, {32.0, 24.0})}), section, base.value, 20.0});
  ROAD_CONTRACT_EXPECT(branch.ok, branch.error);
  ROAD_CONTRACT_EXPECT(!state.derived().connections.empty(),
                       "T branch did not derive a resolved connection");
  const ResolvedConnection& connection = state.derived().connections.front();
  const ResolvedApproach& original = connection.approaches.front();
  const ApproachKey key = original.key;
  const double manual_setback = original.resolved_setback_m + 0.5;
  const auto state_saved = state.Save();
  ROAD_CONTRACT_EXPECT(state_saved.ok, state_saved.error);
  const std::uint64_t next_id = next_id_observation(state_saved.value);

  // The override is a saved row, not an operation. Connection resolution is the
  // only thing that reads it, so the fixture hands it straight to generation.
  SavedRoadGraph graph = state.graph();
  graph.approach_geometry_overrides.push_back(ApproachGeometryOverride{
      key, ManualDoubleOverride{true, manual_setback},
      ManualDoubleOverride{true, 0.75}});
  const auto generated = generation::generate_road(graph);
  ROAD_CONTRACT_EXPECT(generated.ok, generated.error);
  const ResolvedApproach* resolved = FindResolvedApproach(generated.value, key);
  ROAD_CONTRACT_EXPECT(resolved != nullptr, "manual override lost the resolved approach");
  ROAD_CONTRACT_EXPECT(std::abs(resolved->resolved_setback_m - manual_setback) < 1e-9 &&
                           std::abs(resolved->resolved_lateral_shift_m - 0.75) < 1e-9,
                       "manual override was not consumed by the resolved approach");
  ROAD_CONTRACT_EXPECT(std::abs(resolved->auto_setback_m - (manual_setback - 0.5)) < 1e-9 &&
                           std::abs(resolved->auto_lateral_shift_m) < 1e-9,
                       "resolved approach lost the automatic values it overrode");
  ROAD_CONTRACT_EXPECT(resolved->gate.approach == key &&
                           resolved->gate.position.x == resolved->position.x &&
                           resolved->gate.position.y == resolved->position.y &&
                           resolved->gate.position.z == resolved->position.z,
                       "connection gate did not follow the resolved approach");
  const auto saved = persistence::SaveRoad(graph, next_id);
  ROAD_CONTRACT_EXPECT(saved.ok, saved.error);
  ROAD_CONTRACT_EXPECT(saved.value.find("approach_geometry_override.count=1\n") != std::string::npos &&
                           saved.value.find(".setback.value=") != std::string::npos &&
                           saved.value.find(".lateral_shift.value=") != std::string::npos,
                       "manual approach override fields were not persisted");
  const auto loaded = RoadState::Load(saved.value);
  ROAD_CONTRACT_EXPECT(loaded.ok, loaded.error);
  ROAD_CONTRACT_EXPECT(loaded.value.graph().approach_geometry_overrides.size() == 1,
                       "manual approach override did not survive load");
  const auto reloaded_resolved =
      FindResolvedApproach(loaded.value.derived(), key);
  ROAD_CONTRACT_EXPECT(reloaded_resolved != nullptr &&
                           std::abs(reloaded_resolved->resolved_setback_m - manual_setback) < 1e-9,
                       "loaded manual override was not consumed by the resolved approach");

  SavedRoadGraph negative = graph;
  negative.approach_geometry_overrides.front().setback_m =
      ManualDoubleOverride{true, -1.0};
  const auto rejected = persistence::SaveRoad(negative, next_id);
  ROAD_CONTRACT_EXPECT(!rejected.ok && rejected.failure_category ==
                                           CommitFailureCategory::kInvalidInput,
                       "a negative setback override was accepted as authoritative data");
  return true;
}

bool add_segment_build_failure_is_atomic(std::string& failure) {
  RoadState state{};
  RoadLayoutTemplate unusable{};
  unusable.strips = {
      RoadLayoutStrip{10, StripFunction::kSidewalk, 1.0e308, 0.0, builtin_surface_styles::kSidewalk},
      RoadLayoutStrip{20, StripFunction::kCarriageway, 1.0e308, 0.0, builtin_surface_styles::kAsphalt},
      RoadLayoutStrip{30, StripFunction::kSidewalk, 1.0e308, 0.0, builtin_surface_styles::kSidewalk},
  };
  unusable.boundaries = {
      road_fixture::CurbBoundary(11, 0.0, 0.15, {}),
      road_fixture::CurbBoundary(21, 0.0, -0.15, {}),
  };
  // The widths overflow when they are summed, so the alignment sits at the left
  // outer end: the layout is accepted and the geometry build is what fails.
  unusable.alignment_offset_from_left_m = 0.0;
  const auto template_id = state.AddRoadLayoutTemplate(city::road::AddRoadLayoutTemplateRequest{std::move(unusable)});
  ROAD_CONTRACT_EXPECT(template_id.ok, template_id.error);
  ROAD_CONTRACT_EXPECT(expect_failed_unchanged(
                           state,
                           [&] {
                             return state.AddSegment(city::road::AddSegmentRequest{MakePath({MakeLine({0.0, 0.0}, {20.0, 0.0})}),
                                                     template_id.value});
                           },
                           "AddSegment build failure", failure),
                       failure);
  return true;
}

bool node_identity_does_not_come_from_position(std::string& failure) {
  RoadState state{};
  const auto section = road_fixture::AddLayout(state, road_fixture::BidirectionalLayout(0));
  const auto first = state.AddSegment(city::road::AddSegmentRequest{MakePath({MakeLine({0.0, 0.0}, {20.0, 0.0})}), section});
  const auto second = state.AddSegment(city::road::AddSegmentRequest{MakePath({MakeLine({0.0, 0.0}, {0.0, 20.0})}), section});
  const auto near = state.AddSegment(city::road::AddSegmentRequest{MakePath({MakeLine({0.0, 0.0000001}, {-20.0, 0.0000001})}), section});
  ROAD_CONTRACT_EXPECT(first.ok && second.ok && near.ok, "independent segments could not be created");
  ROAD_CONTRACT_EXPECT(state.graph().nodes.size() == 6, "equal or near node positions were merged by geometry");
  ROAD_CONTRACT_EXPECT(state.graph().segments[0].node_a != state.graph().segments[1].node_a &&
                           state.graph().segments[0].node_a != state.graph().segments[2].node_a,
                       "node identity was inferred from equal or epsilon-near positions");
  return true;
}

bool segment_shape_edit_does_not_move_endpoint_authority(std::string& failure) {
  RoadState state{};
  const auto section = road_fixture::AddLayout(state, road_fixture::BidirectionalLayout(0));
  const auto added = state.AddSegment(city::road::AddSegmentRequest{MakePath({MakeLine({0.0, 0.0}, {20.0, 0.0})}), section});
  ROAD_CONTRACT_EXPECT(added.ok, added.error);
  const RoadNode before_a = state.graph().nodes[0];
  const RoadNode before_b = state.graph().nodes[1];
  const auto shape = SegmentShapeFromPath(
      MakePath({MakeBezier({3.0, 4.0}, {7.0, 10.0}, {16.0, -5.0}, {24.0, 2.0})}));
  ROAD_CONTRACT_EXPECT(shape.ok, shape.error);
  const auto edited = state.EditSegmentShape(city::road::EditSegmentShapeRequest{added.value, shape.value});
  ROAD_CONTRACT_EXPECT(edited.ok, edited.error);
  ROAD_CONTRACT_EXPECT(state.graph().nodes[0].id == before_a.id && state.graph().nodes[1].id == before_b.id &&
                           state.graph().nodes[0].position.x == before_a.position.x &&
                           state.graph().nodes[0].position.y == before_a.position.y &&
                           state.graph().nodes[1].position.x == before_b.position.x &&
                           state.graph().nodes[1].position.y == before_b.position.y,
                       "segment shape edit changed RoadNode endpoint authority");
  return true;
}

bool move_node_rederives_incident_alignment_endpoints(std::string& failure) {
  RoadState state{};
  const auto section = road_fixture::AddLayout(state, road_fixture::BidirectionalLayout(0));
  const auto first = state.AddSegment(city::road::AddSegmentRequest{MakePath({MakeLine({0.0, 0.0}, {20.0, 0.0})}), section});
  ROAD_CONTRACT_EXPECT(first.ok, first.error);
  const RoadNodeId shared = state.graph().segments.front().node_b;
  const auto second = state.AddSegmentConnectedTo(city::road::AddSegmentConnectedToRequest{MakePath({MakeLine({20.0, 0.0}, {40.0, 0.0})}), section, shared});
  ROAD_CONTRACT_EXPECT(second.ok, second.error);
  const auto moved = state.MoveNode(city::road::MoveNodeRequest{shared, {20.0, 15.0}});
  ROAD_CONTRACT_EXPECT(moved.ok, moved.error);
  const Path* first_path = FindCanonicalAlignment(state.derived(), first.value);
  const Path* second_path = FindCanonicalAlignment(state.derived(), second.value);
  ROAD_CONTRACT_EXPECT(first_path != nullptr && second_path != nullptr, "incident canonical alignment is missing");
  ROAD_CONTRACT_EXPECT(first_path->spans.back().p3.x == 20.0 && first_path->spans.back().p3.y == 15.0 &&
                           second_path->spans.front().p0.x == 20.0 && second_path->spans.front().p0.y == 15.0,
                       "MoveNode did not rederive every incident canonical endpoint");
  ROAD_CONTRACT_EXPECT(IsLinearSpan(first_path->spans.back()) &&
                           IsLinearSpan(second_path->spans.front()),
                       "MoveNode did not preserve straight segment intent");
  ROAD_CONTRACT_EXPECT(state.graph().segments[0].node_b == shared && state.graph().segments[1].node_a == shared,
                       "MoveNode changed segment connectivity");
  return true;
}

bool generate_road_is_pure(std::string& failure) {
  RoadState state{};
  const auto section = road_fixture::AddLayout(state, road_fixture::BidirectionalLayout(0));
  const auto added = state.AddSegment(
      AddSegmentRequest{MakePath({MakeLine({0.0, 0.0}, {40.0, 0.0})}), section});
  ROAD_CONTRACT_EXPECT(added.ok, added.error);

  const SavedRoadGraph graph = state.graph();
  const auto state_saved = state.Save();
  ROAD_CONTRACT_EXPECT(state_saved.ok, state_saved.error);
  const std::uint64_t graph_next_id = next_id_observation(state_saved.value);
  const auto before = persistence::SaveRoad(graph, graph_next_id);
  ROAD_CONTRACT_EXPECT(before.ok, before.error);
  const auto first = generation::generate_road(graph);
  const auto second = generation::generate_road(graph);
  ROAD_CONTRACT_EXPECT(first.ok && second.ok, "generate_road failed for a valid graph");
  ROAD_CONTRACT_EXPECT(derived_observation(first.value) == derived_observation(second.value),
                       "generate_road is not deterministic for the same graph");
  const auto after = persistence::SaveRoad(graph, graph_next_id);
  ROAD_CONTRACT_EXPECT(after.ok && after.value == before.value,
                       "generate_road changed its authoritative input");

  // Two roads of different width meeting at one node, with no transition
  // between them.
  SavedRoadGraph unsupported{};
  unsupported.layout_templates = {road_fixture::BidirectionalLayout(1),
                                   road_fixture::ExtraLaneLayout(2)};
  unsupported.nodes = {RoadNode{10, {0.0, 0.0}}, RoadNode{11, {20.0, 0.0}},
                       RoadNode{12, {40.0, 0.0}}};
  unsupported.segments = {
      RoadSegment{20, 10, 11, {}, 1, std::nullopt},
      RoadSegment{21, 11, 12, {}, 2, std::nullopt},
  };
  unsupported.corridors = {
      RoadCorridor{22, 1, {DirectedSegmentRef{20, false}}},
      RoadCorridor{23, 2, {DirectedSegmentRef{21, false}}},
  };
  const auto unsupported_before = persistence::SaveRoad(unsupported, 24);
  ROAD_CONTRACT_EXPECT(unsupported_before.ok, unsupported_before.error);
  const auto failed = generation::generate_road(unsupported);
  ROAD_CONTRACT_EXPECT(!failed.ok, "generate_road unexpectedly accepted an unsupported graph");
  const auto unsupported_after = persistence::SaveRoad(unsupported, 24);
  ROAD_CONTRACT_EXPECT(unsupported_after.ok && unsupported_after.value == unsupported_before.value,
                       "failed generate_road changed its authoritative input");
  return true;
}

bool corridor_layout_template_is_extension_default_only(std::string& failure) {
  RoadState state{};
  const auto first_section =
      road_fixture::AddLayout(state, road_fixture::BidirectionalLayout(0));
  const auto second_section =
      road_fixture::AddLayout(state, road_fixture::ShoulderedLayout(0));
  const auto segment = state.AddSegment(AddSegmentRequest{
      MakePath({MakeLine({0.0, 0.0}, {40.0, 0.0})}), first_section});
  ROAD_CONTRACT_EXPECT(segment.ok, segment.error);
  SavedRoadGraph graph = state.graph();
  ROAD_CONTRACT_EXPECT(!graph.corridors.empty() &&
                           graph.corridors.front().layout_template_id ==
                               first_section,
                       "corridor default template fixture is incomplete");
  const auto baseline = generation::generate_road(graph);
  ROAD_CONTRACT_EXPECT(baseline.ok, baseline.error);
  graph.corridors.front().layout_template_id = second_section;
  const auto changed_default = generation::generate_road(graph);
  ROAD_CONTRACT_EXPECT(changed_default.ok, changed_default.error);
  ROAD_CONTRACT_EXPECT(
      derived_observation(changed_default.value) ==
          derived_observation(baseline.value),
      "corridor layout_template_id reinterpreted existing segment geometry");
  const auto source = std::find_if(
      graph.segments.begin(), graph.segments.end(),
      [id = segment.value](const RoadSegment& item) { return item.id == id; });
  ROAD_CONTRACT_EXPECT(source != graph.segments.end() &&
                           source->layout_template == first_section,
                       "test changed the segment template instead of the corridor default");
  return true;
}

bool generate_is_deterministic(std::string& failure) {
  RoadState state{};
  const auto section = road_fixture::AddLayout(state, road_fixture::BidirectionalLayout(0));
  const auto base = state.AddSegment(
      AddSegmentRequest{MakePath({MakeLine({0.0, 0.0}, {40.0, 0.0})}), section});
  ROAD_CONTRACT_EXPECT(base.ok, base.error);
  const auto branch = state.AddSegmentConnectedToSegment(
      AddSegmentConnectedToSegmentRequest{
          MakePath({MakeLine({20.0, 0.0}, {32.0, 24.0})}), section, base.value, 20.0});
  ROAD_CONTRACT_EXPECT(branch.ok, branch.error);
  const std::string first = derived_observation(state.derived());
  const auto saved = state.Save();
  ROAD_CONTRACT_EXPECT(saved.ok, saved.error);
  const auto reloaded = RoadState::Load(saved.value);
  ROAD_CONTRACT_EXPECT(reloaded.ok, reloaded.error);
  ROAD_CONTRACT_EXPECT(derived_observation(reloaded.value.derived()) == first,
                       "generate from the same authoritative state was not deterministic");
  return true;
}

bool same_boundaries(const std::vector<SectionBoundarySample>& a,
                     const std::vector<SectionBoundarySample>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t index = 0; index < a.size(); ++index) {
    if (a[index].boundary_id != b[index].boundary_id ||
        a[index].role != b[index].role ||
        a[index].lateral_m != b[index].lateral_m ||
        a[index].height_m != b[index].height_m ||
        a[index].marking != b[index].marking) {
      return false;
    }
  }
  return true;
}

bool connection_section_and_gate_have_single_owners(std::string& failure) {
  RoadState state{};
  const auto section = road_fixture::AddLayout(state, road_fixture::BidirectionalLayout(0));
  const auto base = state.AddSegment(
      AddSegmentRequest{MakePath({MakeLine({0.0, 0.0}, {40.0, 0.0})}), section});
  ROAD_CONTRACT_EXPECT(base.ok, base.error);
  const auto branch = state.AddSegmentConnectedToSegment(
      AddSegmentConnectedToSegmentRequest{
          MakePath({MakeLine({20.0, 0.0}, {32.0, 24.0})}), section, base.value, 20.0});
  ROAD_CONTRACT_EXPECT(branch.ok, branch.error);

  const DerivedRoad& derived = state.derived();
  for (const ResolvedConnection& connection : derived.connections) {
    ROAD_CONTRACT_EXPECT(connection.approaches.size() == connection.ordered_approaches.size(),
                         "resolved approaches and deterministic order differ in size");
    for (const ResolvedApproach& approach : connection.approaches) {
      ROAD_CONTRACT_EXPECT(
          std::count_if(connection.approaches.begin(), connection.approaches.end(),
                        [&approach](const ResolvedApproach& candidate) {
                          return candidate.key == approach.key;
                        }) == 1,
          "ApproachKey has more than one resolved row");
    }
  }
  for (const ResolvedConnection& connection : derived.connections) {
    for (const ResolvedApproach& approach : connection.approaches) {
      const DerivedSegment* segment = FindDerivedSegment(derived, approach.key.segment_id);
      ROAD_CONTRACT_EXPECT(segment != nullptr, "approach segment is missing");
      ROAD_CONTRACT_EXPECT(
          std::count(segment->semantic_segment_distances_m.begin(),
                     segment->semantic_segment_distances_m.end(),
                     approach.gate_segment_distance_m) == 1,
          "gate distance is not a unique semantic distance");
      ROAD_CONTRACT_EXPECT(
          std::count_if(segment->sections.begin(), segment->sections.end(),
                        [&approach](const SectionEvaluation& section) {
                          return section.segment_distance_m == approach.gate_segment_distance_m;
                        }) == 1,
          "gate distance has no unique section evaluation");
      const SectionEvaluation* section = FindSectionAt(*segment, approach.gate_segment_distance_m);
      ROAD_CONTRACT_EXPECT(section != nullptr, "gate section evaluation is missing");
      ROAD_CONTRACT_EXPECT(same_boundaries(approach.gate.boundaries, section->boundaries),
                           "gate boundaries differ from their section evaluation");
    }
  }
  return true;
}

std::vector<ApproachKey> sorted_gate_keys(const DerivedRoad& derived) {
  std::vector<ApproachKey> keys{};
  for (const ConnectionGate& gate : road_test_view::gates(derived)) keys.push_back(gate.approach);
  std::sort(keys.begin(), keys.end(), [](const ApproachKey& a, const ApproachKey& b) {
    if (a.node_id != b.node_id) return a.node_id < b.node_id;
    if (a.segment_id != b.segment_id) return a.segment_id < b.segment_id;
    return static_cast<int>(a.endpoint_role) < static_cast<int>(b.endpoint_role);
  });
  return keys;
}

bool approach_identity_survives_geometry_changes(std::string& failure) {
  RoadState state{};
  const auto section = road_fixture::AddLayout(state, road_fixture::BidirectionalLayout(0));
  const auto first = state.AddSegment(
      AddSegmentRequest{MakePath({MakeLine({0.0, 0.0}, {20.0, 0.0})}), section});
  ROAD_CONTRACT_EXPECT(first.ok, first.error);
  const RoadNodeId shared = state.graph().segments.front().node_b;
  const auto second = state.AddSegmentConnectedTo(
      AddSegmentConnectedToRequest{
          MakePath({MakeLine({20.0, 0.0}, {32.0, 16.0})}), section, shared});
  ROAD_CONTRACT_EXPECT(second.ok, second.error);
  const std::vector<ApproachKey> before = sorted_gate_keys(state.derived());
  ROAD_CONTRACT_EXPECT(before.size() == 2 && before[0] != before[1],
                       "connected approaches collapsed to one identity");

  const RoadSegment& segment = state.graph().segments.front();
  const auto moved = state.MoveNode(MoveNodeRequest{segment.node_a, {-1.0, 1.0}});
  ROAD_CONTRACT_EXPECT(moved.ok, moved.error);
  ROAD_CONTRACT_EXPECT(sorted_gate_keys(state.derived()) == before,
                       "MoveNode changed ApproachKey identity");
  const Path* alignment = FindCanonicalAlignment(state.derived(), first.value);
  ROAD_CONTRACT_EXPECT(alignment != nullptr, "edited approach alignment is missing");
  const auto shape = SegmentShapeFromPath(*alignment);
  ROAD_CONTRACT_EXPECT(shape.ok, shape.error);
  SegmentShape edited = shape.value;
  edited.start_handle.y += 0.25;
  const auto changed = state.EditSegmentShape(EditSegmentShapeRequest{first.value, edited});
  ROAD_CONTRACT_EXPECT(changed.ok, changed.error);
  ROAD_CONTRACT_EXPECT(sorted_gate_keys(state.derived()) == before,
                       "EditSegmentShape changed ApproachKey identity");
  return true;
}

bool same_angle_approaches_use_id_tie_break(std::string& failure) {
  RoadState state{};
  const auto section = road_fixture::AddLayout(state, road_fixture::BidirectionalLayout(0));
  const auto base = state.AddSegment(
      AddSegmentRequest{MakePath({MakeLine({-20.0, 0.0}, {0.0, 0.0})}), section});
  ROAD_CONTRACT_EXPECT(base.ok, base.error);
  const RoadNodeId shared = state.graph().segments.front().node_b;
  const auto upper = state.AddSegmentConnectedTo(AddSegmentConnectedToRequest{
      MakePath({MakeBezier({0.0, 0.0}, {5.0, 0.0}, {15.0, 10.0}, {20.0, 20.0})}),
      section, shared});
  ROAD_CONTRACT_EXPECT(upper.ok, upper.error);
  const auto lower = state.AddSegmentConnectedTo(AddSegmentConnectedToRequest{
      MakePath({MakeBezier({0.0, 0.0}, {5.0, 0.0}, {15.0, -10.0}, {20.0, -20.0})}),
      section, shared});
  ROAD_CONTRACT_EXPECT(lower.ok, lower.error);
  const ResolvedConnection* decision = FindResolvedConnection(state.derived(), shared);
  ROAD_CONTRACT_EXPECT(decision != nullptr && decision->ordered_approaches.size() == 3,
                       "same-angle junction decision is missing");
  const ApproachKey upper_key{shared, upper.value, EndpointRole::kStart};
  const ApproachKey lower_key{shared, lower.value, EndpointRole::kStart};
  const auto upper_position =
      std::find(decision->ordered_approaches.begin(), decision->ordered_approaches.end(),
                upper_key);
  const auto lower_position =
      std::find(decision->ordered_approaches.begin(), decision->ordered_approaches.end(),
                lower_key);
  ROAD_CONTRACT_EXPECT(upper_position != decision->ordered_approaches.end() &&
                           lower_position != decision->ordered_approaches.end(),
                       "same-angle approaches are absent from the order");
  ROAD_CONTRACT_EXPECT((upper.value < lower.value) == (upper_position < lower_position),
                       "same-angle approaches are not tie-broken by stable ID");
  const ResolvedApproach* upper_approach =
      FindResolvedApproach(state.derived(), upper_key);
  const ResolvedApproach* lower_approach =
      FindResolvedApproach(state.derived(), lower_key);
  // The two road-body chords meet at 90 degrees. Their 10 m symmetric
  // sections intersect 5 m along each chord, then the 4 m corner radius adds
  // 4 m / tan(45 degrees), so both gates must be exactly 9 m from the node.
  constexpr double expected_same_angle_setback_m = 9.0;
  ROAD_CONTRACT_EXPECT(
      upper_approach != nullptr && lower_approach != nullptr &&
          std::abs(upper_approach->auto_setback_m -
                   expected_same_angle_setback_m) < 1e-9 &&
          std::abs(lower_approach->auto_setback_m -
                   expected_same_angle_setback_m) < 1e-9,
      "same-angle chord fallback did not preserve the side-line intersection");
  const auto saved = state.Save();
  const auto loaded = saved.ok ? RoadState::Load(saved.value) : Result<RoadState>{};
  ROAD_CONTRACT_EXPECT(loaded.ok, "same-angle state did not round-trip");
  const ResolvedConnection* loaded_decision =
      FindResolvedConnection(loaded.value.derived(), shared);
  ROAD_CONTRACT_EXPECT(loaded_decision != nullptr &&
                           loaded_decision->ordered_approaches ==
                               decision->ordered_approaches,
                       "same-angle approach order is not deterministic");

  std::istringstream archive(saved.value);
  std::vector<std::string> prefix{};
  std::vector<std::vector<std::string>> segment_blocks{};
  std::vector<std::string> suffix{};
  std::string line;
  bool in_segments = false;
  bool after_segments = false;
  while (std::getline(archive, line)) {
    if (line.starts_with("segment=")) {
      in_segments = true;
      segment_blocks.push_back({line});
    } else if (in_segments &&
               (line.starts_with("segment_shape=") ||
                line.starts_with("segment_knot="))) {
      segment_blocks.back().push_back(line);
    } else if (!in_segments) {
      prefix.push_back(line);
    } else {
      after_segments = true;
      suffix.push_back(line);
    }
    if (after_segments) in_segments = true;
  }
  std::ostringstream reordered_archive;
  for (const std::string& value : prefix) reordered_archive << value << '\n';
  for (auto block = segment_blocks.rbegin(); block != segment_blocks.rend(); ++block) {
    for (const std::string& value : *block) reordered_archive << value << '\n';
  }
  for (const std::string& value : suffix) reordered_archive << value << '\n';
  const auto reordered = RoadState::Load(reordered_archive.str());
  ROAD_CONTRACT_EXPECT(reordered.ok, "reordered segment archive did not load");
  const ResolvedConnection* reordered_decision =
      FindResolvedConnection(reordered.value.derived(), shared);
  ROAD_CONTRACT_EXPECT(reordered_decision != nullptr &&
                           reordered_decision->ordered_approaches ==
                               decision->ordered_approaches,
                       "segment storage order changed deterministic approach order");
  return true;
}

bool unsupported_connection_section_is_atomic(std::string& failure) {
  RoadState state{};
  const auto section = road_fixture::AddLayout(state, road_fixture::BidirectionalLayout(0));
  RoadLayoutTemplate walkway_only{};
  walkway_only.strips = {
      RoadLayoutStrip{10, StripFunction::kSidewalk, 2.0, 0.01,
                      builtin_surface_styles::kSidewalk},
      RoadLayoutStrip{20, StripFunction::kSidewalk, 2.0, -0.01,
                      builtin_surface_styles::kSidewalk},
  };
  walkway_only.boundaries = {
      road_fixture::PaintedLineBoundary(160, BoundaryRole::kOuterEdge, {})};
  walkway_only.alignment_offset_from_left_m =
      road_fixture::CentredAlignmentOffset(walkway_only);
  const auto template_id = state.AddRoadLayoutTemplate(
      AddRoadLayoutTemplateRequest{std::move(walkway_only)});
  ROAD_CONTRACT_EXPECT(template_id.ok,
                       "walkway-only junction fixture could not be created");
  const auto base = state.AddSegment(
      AddSegmentRequest{MakePath({MakeLine({-20.0, 0.0}, {0.0, 0.0})}),
                        template_id.value});
  ROAD_CONTRACT_EXPECT(base.ok, base.error);
  const RoadNodeId shared = state.graph().segments.front().node_b;
  ROAD_CONTRACT_EXPECT(
      expect_failed_unchanged(
          state,
          [&] {
            return state.AddSegmentConnectedTo(AddSegmentConnectedToRequest{
                MakePath({MakeLine({0.0, 0.0}, {20.0, 20.0})}),
                template_id.value, shared});
          },
          "unsupported degree-two section", failure),
      failure);
  return true;
}

bool derived_segment_owns_all_semantic_distances(std::string& failure) {
  RoadState state{};
  const auto section = road_fixture::AddLayout(state, road_fixture::BidirectionalLayout(0));
  const auto three_lane = road_fixture::AddLayout(state, road_fixture::ExtraLaneLayout(0));
  const Path alignment = MakePath({
      MakeLine({0.0, 0.0}, {20.0, 0.0}),
      MakeLine({20.0, 0.0}, {50.0, 0.0}),
  });
  const auto segment = state.AddSegment(AddSegmentRequest{alignment, section});
  ROAD_CONTRACT_EXPECT(segment.ok, segment.error);
  // A transition anywhere inside a segment is saved data, so the fixture writes
  // one and asks generation for the distances it must carry.
  SavedRoadGraph graph = state.graph();
  for (RoadSegment& item : graph.segments) {
    if (item.id == segment.value) item.transition = 9001;
  }
  graph.transitions.push_back(RoadLayoutTransition{
      9001, section, three_lane, DistanceRef{DistanceRefKind::kFromStart, 5.0},
      DistanceRef{DistanceRefKind::kFromStart, 20.0}, TransitionAnchor::kCenter,
      0, {RoadLayoutTransitionRule{35, TransitionAction::kTaperIn}}});
  const auto generated = generation::generate_road(graph);
  ROAD_CONTRACT_EXPECT(generated.ok, generated.error);
  const DerivedSegment* derived_segment =
      FindDerivedSegment(generated.value, segment.value);
  ROAD_CONTRACT_EXPECT(derived_segment != nullptr, "derived segment is missing");
  for (const double distance : std::array<double, 4>{0.0, 5.0, 20.0, 50.0}) {
    ROAD_CONTRACT_EXPECT(
        std::count(derived_segment->semantic_segment_distances_m.begin(),
                   derived_segment->semantic_segment_distances_m.end(), distance) == 1,
        "required endpoint/span/transition semantic distance is missing or duplicated");
    ROAD_CONTRACT_EXPECT(
        std::count_if(derived_segment->sections.begin(),
                      derived_segment->sections.end(),
                      [distance](const SectionEvaluation& section) {
                        return section.segment_distance_m == distance;
                      }) == 1,
        "semantic distance has no unique section evaluation");
  }
  ROAD_CONTRACT_EXPECT(
      std::is_sorted(derived_segment->semantic_segment_distances_m.begin(),
                     derived_segment->semantic_segment_distances_m.end()),
      "semantic distances are not sorted");
  return true;
}

// Distances along a corridor. What these tests fix is that a corridor distance
// keeps its world position across edits, not how the samples are produced.
std::vector<double> corridor_sample_distances(const DerivedRoad& derived,
                                              const RoadCorridor& corridor,
                                              double step_m) {
  double total = 0.0;
  for (const DirectedSegmentRef& ref : corridor.segments) {
    const DerivedSegment* segment = FindDerivedSegment(derived, ref.segment_id);
    if (segment == nullptr) return {};
    total += segment->length_m;
  }
  std::vector<double> distances{};
  for (double value = 0.0; value <= total + 1e-9; value += step_m) {
    distances.push_back(std::min(value, total));
  }
  return distances;
}

// Editing one road must not move a road that shares nothing with it. The far
// corridor is compared bit-for-bit, not within a tolerance.
// Bit-for-bit comparison of two alignments. Locality is not a tolerance.
bool same_alignment_bits(const Path& before, const Path& after) {
  if (before.spans.size() != after.spans.size()) return false;
  for (std::size_t index = 0; index < before.spans.size(); ++index) {
    const BezierSpan& a = before.spans[index];
    const BezierSpan& b = after.spans[index];
    const std::array<double, 8> lhs{a.p0.x, a.p0.y, a.p1.x, a.p1.y, a.p2.x, a.p2.y, a.p3.x, a.p3.y};
    const std::array<double, 8> rhs{b.p0.x, b.p0.y, b.p1.x, b.p1.y, b.p2.x, b.p2.y, b.p3.x, b.p3.y};
    for (std::size_t i = 0; i < lhs.size(); ++i) {
      if (std::bit_cast<std::uint64_t>(lhs[i]) != std::bit_cast<std::uint64_t>(rhs[i])) return false;
    }
  }
  return true;
}

bool add_lane_leaves_unrelated_corridors_bit_identical(std::string& failure) {
  RoadState state{};
  const auto section = road_fixture::AddLayout(state, road_fixture::BidirectionalLayout(0));
  const auto widened = state.AddSegment(
      AddSegmentRequest{MakePath({MakeLine({0.0, 0.0}, {80.0, 0.0})}), section});
  ROAD_CONTRACT_EXPECT(widened.ok, widened.error);
  const RoadCorridor* corridor = FindCorridorForSegment(state.graph(), widened.value);
  ROAD_CONTRACT_EXPECT(corridor != nullptr, "widened corridor is missing");
  const RoadCorridorId corridor_id = corridor->id;

  const auto far_road = state.AddSegment(
      AddSegmentRequest{MakePath({MakeLine({500.0, 500.0}, {540.0, 500.0})}), section});
  ROAD_CONTRACT_EXPECT(far_road.ok, far_road.error);
  const Path* far_before = FindCanonicalAlignment(state.derived(), far_road.value);
  ROAD_CONTRACT_EXPECT(far_before != nullptr, "unrelated alignment is missing");
  const Path snapshot = *far_before;

  AddLaneRequest request{};
  request.corridor_id = corridor_id;
  request.direction = LaneTravelDirection::kAlongSegment;
  request.side = RoadSide::kRight;
  request.transition_start = SegmentPosition{widened.value, 0.25};
  request.transition_complete = SegmentPosition{widened.value, 0.75};
  const RoadSegment* widened_segment = nullptr;
  for (const RoadSegment& segment : state.graph().segments) {
    if (segment.id == widened.value) widened_segment = &segment;
  }
  ROAD_CONTRACT_EXPECT(widened_segment != nullptr, "widened segment is missing");
  request.lane_width_m = 3.0;
  const auto added = state.AddLane(request);
  ROAD_CONTRACT_EXPECT(added.ok, added.error);

  const Path* far_after = FindCanonicalAlignment(state.derived(), far_road.value);
  ROAD_CONTRACT_EXPECT(far_after != nullptr, "unrelated alignment disappeared");
  ROAD_CONTRACT_EXPECT(same_alignment_bits(snapshot, *far_after),
                       "adding a lane moved an unrelated corridor");
  return true;
}

bool template_edit_reaches_only_roads_using_that_template(std::string& failure) {
  RoadState state{};
  const auto three_lane = road_fixture::AddLayout(state, road_fixture::ExtraLaneLayout(0));
  const auto section = road_fixture::AddLayout(state, road_fixture::BidirectionalLayout(0));
  const auto edited_road = state.AddSegment(
      AddSegmentRequest{MakePath({MakeLine({0.0, 0.0}, {40.0, 0.0})}), section});
  ROAD_CONTRACT_EXPECT(edited_road.ok, edited_road.error);
  const auto other_road = state.AddSegment(
      AddSegmentRequest{MakePath({MakeLine({500.0, 500.0}, {540.0, 500.0})}), three_lane});
  ROAD_CONTRACT_EXPECT(other_road.ok, other_road.error);

  const auto section_width = [&state](RoadSegmentId id) {
    const DerivedSegment* segment = FindDerivedSegment(state.derived(), id);
    if (segment == nullptr || segment->sections.empty()) return 0.0;
    const auto& boundaries = segment->sections.front().boundaries;
    if (boundaries.size() < 2) return 0.0;
    return boundaries.back().lateral_m - boundaries.front().lateral_m;
  };
  const double edited_before = section_width(edited_road.value);
  const double other_before = section_width(other_road.value);
  ROAD_CONTRACT_EXPECT(edited_before > 0.0 && other_before > 0.0,
                       "section widths were not derived");

  RoadLayoutTemplate wider = road_fixture::BidirectionalLayout(section);
  for (auto& strip : wider.strips) strip.width_m += 0.5;
  const auto applied = state.EditRoadLayoutTemplate(EditRoadLayoutTemplateRequest{wider});
  ROAD_CONTRACT_EXPECT(applied.ok, applied.error);

  ROAD_CONTRACT_EXPECT(section_width(edited_road.value) > edited_before,
                       "shared template edit did not reach the road using it");
  ROAD_CONTRACT_EXPECT(
      std::bit_cast<std::uint64_t>(section_width(other_road.value)) ==
          std::bit_cast<std::uint64_t>(other_before),
      "shared template edit changed a road using another template");
  return true;
}

bool segment_delete_leaves_unrelated_corridors_bit_identical(std::string& failure) {
  RoadState state{};
  const auto section = road_fixture::AddLayout(state, road_fixture::BidirectionalLayout(0));
  const auto doomed = state.AddSegment(
      AddSegmentRequest{MakePath({MakeLine({0.0, 0.0}, {40.0, 0.0})}), section});
  ROAD_CONTRACT_EXPECT(doomed.ok, doomed.error);
  const auto far_road = state.AddSegment(
      AddSegmentRequest{MakePath({MakeLine({500.0, 500.0}, {540.0, 500.0})}), section});
  ROAD_CONTRACT_EXPECT(far_road.ok, far_road.error);
  const Path* far_before = FindCanonicalAlignment(state.derived(), far_road.value);
  ROAD_CONTRACT_EXPECT(far_before != nullptr, "unrelated alignment is missing");
  const Path snapshot = *far_before;

  const auto deleted = state.DeleteSegment(DeleteSegmentRequest{doomed.value});
  ROAD_CONTRACT_EXPECT(deleted.ok, deleted.error);

  const Path* far_after = FindCanonicalAlignment(state.derived(), far_road.value);
  ROAD_CONTRACT_EXPECT(far_after != nullptr, "unrelated alignment disappeared");
  ROAD_CONTRACT_EXPECT(same_alignment_bits(snapshot, *far_after),
                       "deleting a road moved an unrelated corridor");
  return true;
}

bool node_move_leaves_unrelated_corridors_bit_identical(std::string& failure) {
  RoadState state{};
  const auto section = road_fixture::AddLayout(state, road_fixture::BidirectionalLayout(0));
  const auto near_road = state.AddSegment(
      AddSegmentRequest{MakePath({MakeLine({0.0, 0.0}, {40.0, 0.0})}), section});
  ROAD_CONTRACT_EXPECT(near_road.ok, near_road.error);
  const auto far_road = state.AddSegment(
      AddSegmentRequest{MakePath({MakeLine({500.0, 500.0}, {540.0, 500.0})}), section});
  ROAD_CONTRACT_EXPECT(far_road.ok, far_road.error);

  const Path* far_before_path = FindCanonicalAlignment(state.derived(), far_road.value);
  ROAD_CONTRACT_EXPECT(far_before_path != nullptr, "unrelated alignment is missing");
  const Path far_before = *far_before_path;

  const RoadSegment* near_segment = nullptr;
  for (const RoadSegment& segment : state.graph().segments) {
    if (segment.id == near_road.value) near_segment = &segment;
  }
  ROAD_CONTRACT_EXPECT(near_segment != nullptr, "edited segment is missing");
  const auto moved = state.MoveNode(MoveNodeRequest{near_segment->node_b, {40.0, 12.0}});
  ROAD_CONTRACT_EXPECT(moved.ok, moved.error);

  const Path* far_after = FindCanonicalAlignment(state.derived(), far_road.value);
  ROAD_CONTRACT_EXPECT(far_after != nullptr, "unrelated alignment disappeared");
  ROAD_CONTRACT_EXPECT(far_after->spans.size() == far_before.spans.size(),
                       "unrelated corridor changed its span count");
  for (std::size_t index = 0; index < far_before.spans.size(); ++index) {
    const BezierSpan& before = far_before.spans[index];
    const BezierSpan& after = far_after->spans[index];
    ROAD_CONTRACT_EXPECT(
        std::bit_cast<std::uint64_t>(before.p0.x) == std::bit_cast<std::uint64_t>(after.p0.x) &&
            std::bit_cast<std::uint64_t>(before.p0.y) == std::bit_cast<std::uint64_t>(after.p0.y) &&
            std::bit_cast<std::uint64_t>(before.p1.x) == std::bit_cast<std::uint64_t>(after.p1.x) &&
            std::bit_cast<std::uint64_t>(before.p1.y) == std::bit_cast<std::uint64_t>(after.p1.y) &&
            std::bit_cast<std::uint64_t>(before.p2.x) == std::bit_cast<std::uint64_t>(after.p2.x) &&
            std::bit_cast<std::uint64_t>(before.p2.y) == std::bit_cast<std::uint64_t>(after.p2.y) &&
            std::bit_cast<std::uint64_t>(before.p3.x) == std::bit_cast<std::uint64_t>(after.p3.x) &&
            std::bit_cast<std::uint64_t>(before.p3.y) == std::bit_cast<std::uint64_t>(after.p3.y),
        "moving a node moved an unrelated corridor");
  }
  return true;
}

bool corridor_distance_crosses_segment_boundaries(
    std::string& failure) {
  RoadState state{};
  const auto section = road_fixture::AddLayout(state, road_fixture::BidirectionalLayout(0));
  const Path path = MakePath({
      MakeLine({0.0, 0.0}, {15.0, 0.0}),
  });
  const auto added = state.AddSegment(AddSegmentRequest{path, section});
  ROAD_CONTRACT_EXPECT(added.ok, added.error);
  const RoadSegment first_segment = state.graph().segments.front();
  const RoadCorridorId corridor_id = state.graph().corridors.front().id;
  const auto extended = state.ExtendCorridorFromEnd(
      ExtendCorridorFromEndRequest{
          corridor_id, first_segment.node_b,
          MakePath({MakeLine({15.0, 0.0}, {40.0, 0.0})}), section});
  ROAD_CONTRACT_EXPECT(extended.ok, extended.error);
  ROAD_CONTRACT_EXPECT(state.graph().corridors.size() == 1 &&
                           state.graph().corridors.front().segments.size() == 2,
                       "confirmed extension did not create one two-segment corridor");
  const RoadCorridor& corridor = state.graph().corridors.front();
  const auto boundary = ResolveCorridorDistance(
      state.graph(), state.derived(),
      CorridorDistanceRef{corridor.id, 15.0});
  ROAD_CONTRACT_EXPECT(
      boundary.ok &&
          boundary.value.segment_id == corridor.segments[1].segment_id &&
          std::abs(boundary.value.segment_distance_m) <= 1e-9,
      "internal corridor boundary did not resolve to following local zero");
  SavedRoadGraph reversed_graph = state.graph();
  std::reverse(reversed_graph.corridors.front().segments.begin(),
               reversed_graph.corridors.front().segments.end());
  for (DirectedSegmentRef& ref : reversed_graph.corridors.front().segments) {
    ref.reversed = true;
  }
  const auto reversed_derived = city::road::generation::generate_road(reversed_graph);
  ROAD_CONTRACT_EXPECT(reversed_derived.ok, reversed_derived.error);
  const auto reversed_start = ResolveCorridorDistance(
      reversed_graph, reversed_derived.value,
      CorridorDistanceRef{corridor.id, 0.0});
  const DerivedSegment* original_last =
      FindDerivedSegment(reversed_derived.value,
                         reversed_graph.corridors.front().segments.front().segment_id);
  ROAD_CONTRACT_EXPECT(
      reversed_start.ok && original_last != nullptr &&
          reversed_start.value.reversed &&
          std::abs(reversed_start.value.segment_distance_m -
                   original_last->length_m) <= 1e-9,
      "reversed corridor start did not resolve to segment end");
  return true;
}

bool branch_and_tail_extension_preserve_existing_corridor(
    std::string& failure) {
  RoadState state{};
  const auto section = road_fixture::AddLayout(state, road_fixture::BidirectionalLayout(0));
  const auto added = state.AddSegment(AddSegmentRequest{
      MakePath({MakeLine({0.0, 0.0}, {15.0, 0.0})}),
      section});
  ROAD_CONTRACT_EXPECT(added.ok, added.error);
  const RoadSegment initial_segment = state.graph().segments.front();
  const RoadCorridorId initial_corridor_id =
      state.graph().corridors.front().id;
  const auto initial_extension = state.ExtendCorridorFromEnd(
      ExtendCorridorFromEndRequest{
          initial_corridor_id, initial_segment.node_b,
          MakePath({MakeLine({15.0, 0.0}, {40.0, 0.0})}), section});
  ROAD_CONTRACT_EXPECT(initial_extension.ok, initial_extension.error);
  const RoadCorridor main_before = state.graph().corridors.front();
  std::map<RoadSegmentId, std::string> alignments_before{};
  for (const DirectedSegmentRef& ref : main_before.segments) {
    const Path* path = FindCanonicalAlignment(state.derived(), ref.segment_id);
    ROAD_CONTRACT_EXPECT(path != nullptr, "main alignment is missing");
    alignments_before.emplace(ref.segment_id, path_observation(*path));
  }
  const auto periodic_before = corridor_sample_distances(state.derived(), *FindRoadCorridor(state.graph(), main_before.id), 10.0);
  ROAD_CONTRACT_EXPECT(!periodic_before.empty(), "main corridor produced no sample distances");

  const RoadSegment& first =
      *std::find_if(state.graph().segments.begin(), state.graph().segments.end(),
                    [id = main_before.segments.front().segment_id](
                        const RoadSegment& segment) { return segment.id == id; });
  const auto branch = state.AddSegmentConnectedTo(AddSegmentConnectedToRequest{
      MakePath({MakeLine({15.0, 0.0}, {15.0, 20.0})}), section, first.node_b});
  ROAD_CONTRACT_EXPECT(branch.ok, branch.error);
  const RoadCorridor* main_after_branch =
      FindRoadCorridor(state.graph(), main_before.id);
  ROAD_CONTRACT_EXPECT(
      main_after_branch != nullptr &&
          main_after_branch->segments == main_before.segments &&
          state.graph().corridors.size() == 2,
      "branch changed the main corridor or was inserted into it");
  const auto periodic_after_branch = corridor_sample_distances(state.derived(), *FindRoadCorridor(state.graph(), main_before.id), 10.0);
  ROAD_CONTRACT_EXPECT(
      periodic_after_branch == periodic_before,
      "branch changed main corridor periodic placement");

  const DirectedSegmentRef tail_ref = main_after_branch->segments.back();
  const RoadSegment& tail =
      *std::find_if(state.graph().segments.begin(), state.graph().segments.end(),
                    [id = tail_ref.segment_id](const RoadSegment& segment) {
                      return segment.id == id;
                    });
  const RoadNodeId tail_node = tail_ref.reversed ? tail.node_a : tail.node_b;
  const auto extended = state.ExtendCorridorFromEnd(
      ExtendCorridorFromEndRequest{
          main_before.id, tail_node,
          MakePath({MakeLine({40.0, 0.0}, {60.0, 0.0})}), section});
  ROAD_CONTRACT_EXPECT(extended.ok, extended.error);
  for (const auto& [segment_id, observation] : alignments_before) {
    const Path* path = FindCanonicalAlignment(state.derived(), segment_id);
    ROAD_CONTRACT_EXPECT(
        path != nullptr && path_observation(*path) == observation,
        "tail extension changed an existing segment alignment");
  }
  const auto periodic_after_extension = corridor_sample_distances(state.derived(), *FindRoadCorridor(state.graph(), main_before.id), 10.0);
  ROAD_CONTRACT_EXPECT(
      periodic_after_extension.size() >= periodic_before.size() &&
          std::equal(periodic_before.begin(), periodic_before.end(),
                     periodic_after_extension.begin()),
      "tail extension moved existing periodic placement distances");
  const RoadCorridor main_before_branch_delete =
      *FindRoadCorridor(state.graph(), main_before.id);
  const auto periodic_before_branch_delete = periodic_after_extension;
  const auto deleted_branch =
      state.DeleteSegment(DeleteSegmentRequest{branch.value});
  ROAD_CONTRACT_EXPECT(deleted_branch.ok, deleted_branch.error);
  const RoadCorridor* main_after_branch_delete =
      FindRoadCorridor(state.graph(), main_before.id);
  const auto periodic_after_branch_delete = corridor_sample_distances(state.derived(), *FindRoadCorridor(state.graph(), main_before.id), 10.0);
  ROAD_CONTRACT_EXPECT(
      main_after_branch_delete != nullptr &&
          main_after_branch_delete->segments ==
              main_before_branch_delete.segments &&
          periodic_after_branch_delete == periodic_before_branch_delete,
      "branch deletion changed the main corridor or its periodic placement");
  return true;
}

bool split_preserves_corridor_distance_and_world_position(
    std::string& failure) {
  RoadState state{};
  const auto section = road_fixture::AddLayout(state, road_fixture::BidirectionalLayout(0));
  const auto added = state.AddSegment(AddSegmentRequest{
      MakePath({MakeBezier({0.0, 0.0}, {12.0, 8.0}, {28.0, -8.0},
                           {40.0, 0.0})}),
      1});
  ROAD_CONTRACT_EXPECT(added.ok, added.error);
  const RoadCorridor& before_corridor = state.graph().corridors.front();
  const RoadCorridorId corridor_id = before_corridor.id;
  const auto distances_before = corridor_sample_distances(state.derived(), *FindRoadCorridor(state.graph(), before_corridor.id), 5.0);
  ROAD_CONTRACT_EXPECT(!distances_before.empty(), "corridor produced no sample distances");
  std::vector<Vec2d> world_before{};
  for (const double distance : distances_before) {
    const auto resolved = ResolveCorridorDistance(
        state.graph(), state.derived(),
        CorridorDistanceRef{corridor_id, distance});
    ROAD_CONTRACT_EXPECT(resolved.ok, resolved.error);
    const Path* alignment =
        FindCanonicalAlignment(state.derived(), resolved.value.segment_id);
    ROAD_CONTRACT_EXPECT(alignment != nullptr,
                         "split baseline alignment is missing");
    const auto point =
        EvaluatePath(*alignment, resolved.value.segment_distance_m);
    ROAD_CONTRACT_EXPECT(point.ok, point.error);
    world_before.push_back(point.value);
  }
  const auto split =
      state.SplitSegmentAtDistance(SplitSegmentAtDistanceRequest{added.value,
                                                               17.0});
  ROAD_CONTRACT_EXPECT(split.ok, split.error);
  const RoadCorridor& after_corridor = state.graph().corridors.front();
  ROAD_CONTRACT_EXPECT(after_corridor.id == corridor_id &&
                           after_corridor.segments.size() == 2,
                       "split changed corridor identity or segment count");
  const auto distances_after = corridor_sample_distances(state.derived(), *FindRoadCorridor(state.graph(), after_corridor.id), 5.0);
  ROAD_CONTRACT_EXPECT(distances_after == distances_before,
                       "split changed corridor periodic distances");
  for (std::size_t index = 0; index < distances_after.size(); ++index) {
    const auto resolved = ResolveCorridorDistance(
        state.graph(), state.derived(),
        CorridorDistanceRef{after_corridor.id, distances_after[index]});
    ROAD_CONTRACT_EXPECT(resolved.ok, resolved.error);
    const Path* alignment =
        FindCanonicalAlignment(state.derived(), resolved.value.segment_id);
    ROAD_CONTRACT_EXPECT(alignment != nullptr,
                         "split result alignment is missing");
    const auto point =
        EvaluatePath(*alignment, resolved.value.segment_distance_m);
    const double drift =
        point.ok ? std::hypot(point.value.x - world_before[index].x,
                              point.value.y - world_before[index].y)
                 : std::numeric_limits<double>::infinity();
    ROAD_CONTRACT_EXPECT(
        point.ok && drift <= 1e-6,
        "split changed a corridor-distance world position by " +
            std::to_string(drift));
  }
  return true;
}

bool standard_delete_removes_only_requested_segment(std::string& failure) {
  RoadState state{};
  const auto section = road_fixture::AddLayout(state, road_fixture::BidirectionalLayout(0));
  const auto first = state.AddSegment(AddSegmentRequest{
      MakePath({MakeLine({0.0, 0.0}, {20.0, 0.0})}), section});
  ROAD_CONTRACT_EXPECT(first.ok, first.error);
  const RoadCorridorId corridor_id = state.graph().corridors.front().id;
  const RoadNodeId first_end = state.graph().segments.front().node_b;
  const auto middle = state.ExtendCorridorFromEnd(ExtendCorridorFromEndRequest{
      corridor_id, first_end,
      MakePath({MakeLine({20.0, 0.0}, {40.0, 0.0})}), section});
  ROAD_CONTRACT_EXPECT(middle.ok, middle.error);
  const RoadNodeId middle_end =
      std::find_if(state.graph().segments.begin(), state.graph().segments.end(),
                   [id = middle.value](const RoadSegment& segment) {
                     return segment.id == id;
                   })
          ->node_b;
  const auto last = state.ExtendCorridorFromEnd(ExtendCorridorFromEndRequest{
      corridor_id, middle_end,
      MakePath({MakeLine({40.0, 0.0}, {60.0, 0.0})}), section});
  ROAD_CONTRACT_EXPECT(last.ok, last.error);
  const Path* first_before =
      FindCanonicalAlignment(state.derived(), first.value);
  const Path* last_before =
      FindCanonicalAlignment(state.derived(), last.value);
  ROAD_CONTRACT_EXPECT(first_before != nullptr && last_before != nullptr,
                       "standard deletion baseline alignment is missing");
  const std::string first_shape = path_observation(*first_before);
  const std::string last_shape = path_observation(*last_before);

  const auto deleted =
      state.DeleteSegment(DeleteSegmentRequest{middle.value});
  ROAD_CONTRACT_EXPECT(deleted.ok, deleted.error);
  ROAD_CONTRACT_EXPECT(
      state.graph().segments.size() == 2 &&
          std::none_of(state.graph().segments.begin(),
                       state.graph().segments.end(),
                       [id = middle.value](const RoadSegment& segment) {
                         return segment.id == id;
                       }),
      "standard deletion removed more or less than the requested RoadSegment");
  ROAD_CONTRACT_EXPECT(
      state.graph().corridors.size() == 2 &&
          state.graph().corridors.front().id == corridor_id,
      "middle segment deletion did not deterministically split its corridor");
  const Path* first_after =
      FindCanonicalAlignment(state.derived(), first.value);
  const Path* last_after =
      FindCanonicalAlignment(state.derived(), last.value);
  ROAD_CONTRACT_EXPECT(
      first_after != nullptr && last_after != nullptr &&
          path_observation(*first_after) == first_shape &&
          path_observation(*last_after) == last_shape,
      "standard deletion changed an unrelated segment shape");
  return true;
}

bool standard_delete_preserves_unrelated_approach_override(
    std::string& failure) {
  RoadState authored{};
  const auto authored_section = road_fixture::AddLayout(authored, road_fixture::BidirectionalLayout(0));
  const auto main = authored.AddSegment(AddSegmentRequest{
      MakePath({MakeLine({0.0, 0.0}, {40.0, 0.0})}), authored_section});
  ROAD_CONTRACT_EXPECT(main.ok, main.error);
  const auto branch = authored.AddSegmentConnectedToSegment(
      AddSegmentConnectedToSegmentRequest{
          MakePath({MakeLine({20.0, 0.0}, {20.0, 24.0})}), authored_section,
          main.value, 20.0});
  ROAD_CONTRACT_EXPECT(branch.ok, branch.error);
  const auto branch_segment =
      std::find_if(authored.graph().segments.begin(), authored.graph().segments.end(),
                   [id = branch.value](const RoadSegment& segment) {
                     return segment.id == id;
                   });
  ROAD_CONTRACT_EXPECT(branch_segment != authored.graph().segments.end(),
                       "branch segment is missing before standard deletion");
  const auto connection =
      std::find_if(authored.derived().connections.begin(),
                   authored.derived().connections.end(),
                   [node_id = branch_segment->node_a](
                       const ResolvedConnection& value) {
                     return value.node_id == node_id;
                   });
  ROAD_CONTRACT_EXPECT(connection != authored.derived().connections.end(),
                       "branch connection is missing before standard deletion");
  const auto retained_approach =
      std::find_if(connection->approaches.begin(), connection->approaches.end(),
                   [branch_id = branch.value](const ResolvedApproach& value) {
                     return value.key.segment_id != branch_id;
                   });
  ROAD_CONTRACT_EXPECT(retained_approach != connection->approaches.end(),
                       "main-road approach is missing before branch deletion");
  const ApproachKey retained_key = retained_approach->key;
  const auto authored_saved = authored.Save();
  ROAD_CONTRACT_EXPECT(authored_saved.ok, authored_saved.error);
  SavedRoadGraph graph = authored.graph();
  graph.approach_geometry_overrides.push_back(ApproachGeometryOverride{
      retained_key, {}, ManualDoubleOverride{true, 0.1}});
  const auto archived =
      persistence::SaveRoad(graph, next_id_observation(authored_saved.value));
  ROAD_CONTRACT_EXPECT(archived.ok, archived.error);
  auto loaded = RoadState::Load(archived.value);
  ROAD_CONTRACT_EXPECT(loaded.ok, loaded.error);
  RoadState& state = loaded.value;

  const auto deleted = state.DeleteSegment(DeleteSegmentRequest{branch.value});
  ROAD_CONTRACT_EXPECT(deleted.ok, deleted.error);
  ROAD_CONTRACT_EXPECT(
      state.graph().approach_geometry_overrides.size() == 1 &&
          state.graph().approach_geometry_overrides.front().key == retained_key,
      "standard deletion removed an unrelated approach override");
  return true;
}
bool marking_invalid_interval_splits_polyline_runs(std::string& failure) {
  DerivedSegment segment{};
  segment.id = 77;
  segment.alignment =
      MakePath({MakeLine({0.0, 0.0}, {40.0, 0.0})});
  segment.length_m = 40.0;
  segment.surface_segment_distances_m = {0.0, 10.0, 20.0, 30.0, 40.0};
  for (const double distance : segment.surface_segment_distances_m) {
    SectionBoundarySample boundary{};
    boundary.boundary_id = 200;
    boundary.role = BoundaryRole::kLaneDivider;
    boundary.marking = AutoMarkingPolicy{
        true, MarkingRole::kCenterLine,
        builtin_marking_styles::kCenterLine, MarkingPlacement::kCenter};
    boundary.left_strip_id = 20;
    boundary.right_strip_id = 30;
    boundary.left_strip_width_m = distance == 20.0 ? 0.0 : 3.0;
    boundary.right_strip_width_m = 3.0;
    segment.sections.push_back(
        SectionEvaluation{77, distance, 1, {boundary}, {}});
  }
  const auto markings = city::road::generation::derive_markings(
      SavedRoadGraph{}, {segment}, {}, {});
  std::ostringstream diagnostic;
  diagnostic << "ok=" << markings.ok << " runs=" << markings.value.size();
  for (std::size_t index = 0; index < markings.value.size(); ++index) {
    const auto& points = markings.value[index].points;
    diagnostic << " run[" << index << "]=" << points.size();
    if (!points.empty()) {
      diagnostic << ':' << points.front().x << "->" << points.back().x;
    }
  }
  ROAD_CONTRACT_EXPECT(
      markings.ok && markings.value.size() == 2 &&
          markings.value[0].points.size() == 2 &&
          markings.value[1].points.size() == 2 &&
          std::abs(markings.value[0].points.back().x - 10.0) <= 1e-6 &&
          std::abs(markings.value[1].points.front().x - 30.0) <= 1e-6,
      "marking polyline bridged a degenerate interval: " + diagnostic.str());
  return true;
}

bool lane_endpoint_identity_ignores_template_order(std::string& failure) {
  RoadState state{};
  const auto section = road_fixture::AddLayout(state, road_fixture::BidirectionalLayout(0));
  const auto added = state.AddSegment(AddSegmentRequest{
      MakePath({MakeLine({0.0, 0.0}, {20.0, 0.0})}), section});
  ROAD_CONTRACT_EXPECT(added.ok, added.error);
  const RoadSegment& segment = state.graph().segments.front();
  SavedRoadGraph reordered = state.graph();
  auto reordered_section = std::find_if(
      reordered.layout_templates.begin(), reordered.layout_templates.end(),
      [&segment](const RoadLayoutTemplate& candidate) {
        return candidate.id == segment.layout_template;
      });
  ROAD_CONTRACT_EXPECT(reordered_section != reordered.layout_templates.end(),
                       "lane endpoint template is missing");
  std::reverse(reordered_section->lane_bands.begin(),
               reordered_section->lane_bands.end());

  const LaneEndpointKey lane_key{
      segment.id, 1010, EndpointRole::kEnd};
  const auto lane = city::road::internal::find_lane_endpoint(reordered, lane_key);
  ROAD_CONTRACT_EXPECT(
      lane.segment != nullptr && lane.section != nullptr &&
          lane.lane != nullptr && lane.lane->id == 1010 &&
          lane.node_id == segment.node_b &&
          lane.lane->direction == LaneTravelDirection::kAlongSegment,
      "lane endpoint lookup depended on lane array order");

  const BoundaryEndpointKey boundary_key{
      segment.id, 200, EndpointRole::kEnd};
  const auto boundary =
      city::road::internal::find_boundary_endpoint(reordered, boundary_key);
  ROAD_CONTRACT_EXPECT(
      boundary.boundary != nullptr && boundary.boundary->boundary_id == 200 &&
          boundary.node_id == segment.node_b,
      "boundary endpoint lookup did not use stable boundary identity");
  ROAD_CONTRACT_EXPECT(
      city::road::internal::find_lane_endpoint(
          reordered, LaneEndpointKey{segment.id, 999999, EndpointRole::kEnd})
              .lane == nullptr,
      "lane endpoint lookup guessed a missing lane");
  return true;
}

bool seeded_operation_sequences_preserve_contracts(std::string& failure) {
  for (std::uint32_t seed = 1; seed <= 4; ++seed) {
    std::mt19937 random(seed);
    RoadState state{};
    const auto section = road_fixture::AddLayout(state, road_fixture::BidirectionalLayout(0));
    for (int step = 0; step < 8; ++step) {
      const double x = static_cast<double>(step * 30);
      const double y = static_cast<double>(random() % 21) - 10.0;
      const auto added = state.AddSegment(AddSegmentRequest{
          MakePath({MakeLine({x, y}, {x + 20.0, y + static_cast<double>(random() % 5)})}), section});
      ROAD_CONTRACT_EXPECT(added.ok, "seed " + std::to_string(seed) + " AddSegment: " + added.error);
      const RoadSegment& segment = state.graph().segments.back();
      if (step % 3 == 0) {
        const auto node = std::find_if(state.graph().nodes.begin(), state.graph().nodes.end(),
                                       [&segment](const auto& item) { return item.id == segment.node_b; });
        ROAD_CONTRACT_EXPECT(node != state.graph().nodes.end(), "seed node lookup failed");
        const auto moved = state.MoveNode(MoveNodeRequest{node->id, {node->position.x, node->position.y + 0.5}});
        ROAD_CONTRACT_EXPECT(moved.ok, "seed " + std::to_string(seed) + " MoveNode: " + moved.error);
      }
      if (step % 4 == 0) {
        const Path* path = FindCanonicalAlignment(state.derived(), added.value);
        ROAD_CONTRACT_EXPECT(path != nullptr, "seed canonical alignment lookup failed");
        const auto shape = SegmentShapeFromPath(*path);
        ROAD_CONTRACT_EXPECT(shape.ok, "seed shape conversion failed");
        SegmentShape edited_shape = shape.value;
        edited_shape.start_handle.y += 0.25;
        edited_shape.end_handle.y -= 0.25;
        const auto edited = state.EditSegmentShape(EditSegmentShapeRequest{added.value, edited_shape});
        ROAD_CONTRACT_EXPECT(edited.ok, "seed " + std::to_string(seed) + " EditSegmentShape: " + edited.error);
      }
      ROAD_CONTRACT_EXPECT(ValidateGraphInvariants(state.graph(), state.derived()).ok,
                           "seed " + std::to_string(seed) + " invariant failed at step " + std::to_string(step));
      const auto saved = state.Save();
      ROAD_CONTRACT_EXPECT(saved.ok, "seed save failed");
      const auto loaded = RoadState::Load(saved.value);
      ROAD_CONTRACT_EXPECT(loaded.ok && loaded.value.Save().value == saved.value,
                           "seed " + std::to_string(seed) + " round-trip failed at step " + std::to_string(step));
      ROAD_CONTRACT_EXPECT(expect_failed_unchanged(
                               state, [&state] { return state.MoveNode(MoveNodeRequest{999999, {0.0, 0.0}}); },
                               "seed invalid MoveNode", failure),
                           failure);
    }
  }
  return true;
}

struct Test {
  const char* name;
  bool (*run)(std::string&);
};

} // namespace

int main() {
  const Test tests[] = {
      {"all_public_operation_validation_failures_are_atomic", all_public_operation_validation_failures_are_atomic},
      {"add_segment_build_failure_is_atomic", add_segment_build_failure_is_atomic},
      {"node_identity_does_not_come_from_position", node_identity_does_not_come_from_position},
      {"segment_shape_edit_does_not_move_endpoint_authority", segment_shape_edit_does_not_move_endpoint_authority},
      {"move_node_rederives_incident_alignment_endpoints", move_node_rederives_incident_alignment_endpoints},
      {"generate_road_is_pure", generate_road_is_pure},
      {"corridor_layout_template_is_extension_default_only",
       corridor_layout_template_is_extension_default_only},
      {"generate_is_deterministic", generate_is_deterministic},
      {"confirmation_boundaries_define_segment_units",
       confirmation_boundaries_define_segment_units},
      {"extension_leaves_the_existing_segment_bit_identical",
       extension_leaves_the_existing_segment_bit_identical},
      {"reverse_input_has_equivalent_geometry",
       reverse_input_has_equivalent_geometry},
      {"layout_alignment_offset_failures_are_atomic",
       layout_alignment_offset_failures_are_atomic},
      {"boundary_profile_failures_are_atomic",
       boundary_profile_failures_are_atomic},
      {"off_centre_layout_survives_reverse_input_and_repeat",
       off_centre_layout_survives_reverse_input_and_repeat},
      {"extension_semantic_boundaries_are_atomic",
       extension_semantic_boundaries_are_atomic},
      {"isolated_segment_uses_simple_path", isolated_segment_uses_simple_path},
      {"approach_override_resolves_and_persists_manual_fields",
       approach_override_resolves_and_persists_manual_fields},
      {"connection_section_and_gate_have_single_owners",
       connection_section_and_gate_have_single_owners},
      {"approach_identity_survives_geometry_changes",
       approach_identity_survives_geometry_changes},
      {"same_angle_approaches_use_id_tie_break",
       same_angle_approaches_use_id_tie_break},
      {"unsupported_connection_section_is_atomic",
       unsupported_connection_section_is_atomic},
      {"derived_segment_owns_all_semantic_distances",
       derived_segment_owns_all_semantic_distances},
      {"add_lane_leaves_unrelated_corridors_bit_identical",
       add_lane_leaves_unrelated_corridors_bit_identical},
      {"template_edit_reaches_only_roads_using_that_template",
       template_edit_reaches_only_roads_using_that_template},
      {"segment_delete_leaves_unrelated_corridors_bit_identical",
       segment_delete_leaves_unrelated_corridors_bit_identical},
      {"node_move_leaves_unrelated_corridors_bit_identical",
       node_move_leaves_unrelated_corridors_bit_identical},
      {"corridor_distance_crosses_segment_boundaries",
       corridor_distance_crosses_segment_boundaries},
      {"branch_and_tail_extension_preserve_existing_corridor",
       branch_and_tail_extension_preserve_existing_corridor},
      {"split_preserves_corridor_distance_and_world_position",
       split_preserves_corridor_distance_and_world_position},
      {"standard_delete_removes_only_requested_segment",
       standard_delete_removes_only_requested_segment},
      {"standard_delete_preserves_unrelated_approach_override",
       standard_delete_preserves_unrelated_approach_override},
      {"marking_invalid_interval_splits_polyline_runs",
       marking_invalid_interval_splits_polyline_runs},
      {"lane_endpoint_identity_ignores_template_order",
       lane_endpoint_identity_ignores_template_order},
      {"seeded_operation_sequences_preserve_contracts", seeded_operation_sequences_preserve_contracts},
  };
  int failed = 0;
  for (const auto& test : tests) {
    std::string failure;
    const bool ok = test.run(failure);
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << test.name << '\n';
    if (!ok) {
      std::cerr << "  reason: " << failure << '\n';
      ++failed;
    }
  }
  if (failed != 0) return 1;
  std::cout << "road architecture contract tests passed (" << std::size(tests) << " cases)\n";
  return 0;
}
