// Copyright (c) 2017 Computer Vision Center (CVC) at the Universitat Autonoma
// de Barcelona (UAB).
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "carla/road/Map.h"

#include "carla/Exception.h"
#include "carla/road/element/LaneCrossingCalculator.h"
#include "carla/road/element/RoadInfoGeometry.h"
#include "carla/road/element/RoadInfoLaneWidth.h"
#include "carla/road/element/RoadInfoMarkRecord.h"
#include "carla/geom/Math.h"

#include <stdexcept>

namespace carla {
namespace road {

  using namespace carla::road::element;

  // ===========================================================================
  // -- Error handling ---------------------------------------------------------
  // ===========================================================================

  [[ noreturn ]] static void throw_invalid_input(const char *message) {
    throw_exception(std::invalid_argument(message));
  }

#define THROW_INVALID_INPUT_ASSERT(pred) if (!(pred)) { throw_invalid_input("assert failed: " #pred); }

  // ===========================================================================
  // -- Static local methods ---------------------------------------------------
  // ===========================================================================

  template <typename T>
  static std::vector<T> ConcatVectors(std::vector<T> dst, std::vector<T> src) {
    if (src.size() > dst.size()) {
      return ConcatVectors(src, dst);
    }
    dst.insert(
        dst.end(),
        std::make_move_iterator(src.begin()),
        std::make_move_iterator(src.end()));
    return dst;
  }

  /// Return a waypoint for each drivable lane on @a lane_section.
  template <typename FuncT>
  static void ForEachDrivableLane(RoadId road_id, const LaneSection &lane_section, FuncT &&func) {
    for (const auto &pair : lane_section.GetLanes()) {
      const auto &lane = pair.second;
      if (lane.GetType() == "driving") {
        std::forward<FuncT>(func)(Waypoint{road_id, lane_section.GetId(), lane.GetId(), lane_section.GetDistance()});
      }
    }
  }

  /// Return a waypoint for each drivable lane on each lane section of @a road.
  template <typename FuncT>
  static void ForEachDrivableLane(const Road &road, FuncT &&func) {
    for (const auto &lane_section : road.GetLaneSections()) {
      ForEachDrivableLane(road.GetId(), lane_section, std::forward<FuncT>(func));
    }
  }

  /// Return a waypoint for each drivable lane at @a distance on @a road.
  template <typename FuncT>
  static void ForEachDrivableLaneAt(const Road &road, float distance, FuncT &&func) {
    for (const auto &lane_section : road.GetLaneSectionsAt(distance)) {
      ForEachDrivableLane(road.GetId(), lane_section, std::forward<FuncT>(func));
    }
  }

  /// Returns a pair containing first = width, second = tangent,
  /// for an specific Lane given an s and a iterator over lanes
  template <typename T>
  static std::pair<float, float> ComputeTotalLaneWidth(
      const T container,
      const float s, const
      LaneId lane_id) {
    const bool negative_lane_id = lane_id < 0;
    auto dist = 0.0;
    auto tangent = 0.0;
    for (const auto &lane : container) {
      const auto current_polynomial =
          lane.second->template GetInfo<RoadInfoLaneWidth>(s)->GetPolynomial();
      auto current_dist = current_polynomial.Evaluate(s);
      auto current_tangent = current_polynomial.Tangent(s);
      if (lane.first != lane_id) {
        dist += negative_lane_id ? current_dist : - current_dist;
        tangent += current_tangent;
      } else if (lane.first == lane_id) {
        current_dist *= 0.5;
        current_tangent *= 0.5;
        dist += negative_lane_id ? current_dist : - current_dist;
        tangent += current_tangent;
        break;
      }
    }
    return std::make_pair(dist, tangent);
  }

  /// Assumes road_id and section_id are valid.
  static bool IsLanePresent(const MapData &data, Waypoint waypoint) {
    const auto &section = data.GetRoad(waypoint.road_id).GetLaneSectionById(waypoint.section_id);
    return section.ContainsLane(waypoint.lane_id);
  }

  static float GetDistanceAtStartOfLane(const Lane &lane) {
    if (lane.GetId() <= 0) {
      return lane.GetDistance();
    } else {
      return lane.GetDistance() + lane.GetLength();
    }
  }

  // ===========================================================================
  // -- Map: Geometry ----------------------------------------------------------
  // ===========================================================================

  Waypoint Map::GetClosestWaypointOnRoad(const geom::Location &pos) const {
    // max_nearests represents the max nearests roads
    // where we will search for nearests lanes
    constexpr int max_nearests = 10;
    // in case that map has less than max_nearests lanes,
    // we will use the maximum lanes
    const int max_nearest_allowed = _data.GetRoadCount() < max_nearests ?
        _data.GetRoadCount() : max_nearests;

    // Unreal's Y axis hack
    const auto pos_inverted_y = geom::Location(pos.x, -pos.y, pos.z);

    double nearest_dist[max_nearests];
    std::fill(nearest_dist, nearest_dist + max_nearest_allowed,
        std::numeric_limits<double>::max());

    RoadId ids[max_nearests];
    std::fill(ids, ids + max_nearest_allowed, 0);

    double dists[max_nearests];
    std::fill(dists, dists + max_nearest_allowed, 0.0);

    for (const auto &road_pair : _data.GetRoads()) {
      const auto road = &road_pair.second;
      const auto current_dist = road->GetNearestPoint(pos_inverted_y);

      for (int i = 0; i < max_nearest_allowed; ++i) {
        if (current_dist.second < nearest_dist[i]) {
          // reorder nearest_dist
          for (int j = max_nearest_allowed - 1; j > i; --j) {
            nearest_dist[j] = nearest_dist[j - 1];
            ids[j] = ids[j - 1];
            dists[j] = dists[j - 1];
          }
          nearest_dist[i] = current_dist.second;
          ids[i] = road->GetId();
          dists[i] = current_dist.first;
          break;
        }
      }
    }

    // search for the nearest lane in nearest_dist
    Waypoint waypoint;
    auto nearest_lane_dist = std::numeric_limits<float>::max();
    for (int i = 0; i < max_nearest_allowed; ++i) {
      auto lane_dist = _data.GetRoad(ids[i]).GetNearestLane(dists[i], pos_inverted_y);

      if (lane_dist.second < nearest_lane_dist) {
        nearest_lane_dist = lane_dist.second;
        waypoint.lane_id = lane_dist.first->GetId();
        waypoint.road_id = ids[i];
        waypoint.s = dists[i];
      }
    }

    THROW_INVALID_INPUT_ASSERT(
        waypoint.s <= _data.GetRoad(waypoint.road_id).GetLength());
    THROW_INVALID_INPUT_ASSERT(waypoint.lane_id != 0);

    return waypoint;
  }

  boost::optional<Waypoint> Map::GetWaypoint(const geom::Location &pos) const {
    Waypoint w = GetClosestWaypointOnRoad(pos);
    const auto dist = geom::Math::Distance2D(ComputeTransform(w).location, pos);
    const auto lane_width_info = GetLane(w).GetInfo<RoadInfoLaneWidth>(w.s);
    const auto half_lane_width =
        lane_width_info->GetPolynomial().Evaluate(w.s) * 0.5f;

    if (dist < half_lane_width) {
      return w;
    }

    return boost::optional<Waypoint>{};
  }

  geom::Transform Map::ComputeTransform(Waypoint waypoint) const {
    // lane_id can't be 0
    THROW_INVALID_INPUT_ASSERT(waypoint.lane_id != 0);

    const auto &road = _data.GetRoad(waypoint.road_id);

    // must s be smaller (or eq) than road lenght and bigger (or eq) than 0?
    THROW_INVALID_INPUT_ASSERT(waypoint.s <= road.GetLength());
    THROW_INVALID_INPUT_ASSERT(waypoint.s >= 0.0f);

    const std::map<LaneId, const Lane *> lanes = road.GetLanesAt(waypoint.s);
    // check that lane_id exists on the current s
    THROW_INVALID_INPUT_ASSERT(waypoint.lane_id >= lanes.begin()->first);
    THROW_INVALID_INPUT_ASSERT(waypoint.lane_id <= lanes.end()->first);

    float lane_width = 0;
    float lane_tangent = 0;
    if (waypoint.lane_id < 0) {
      // right lane
      const auto side_lanes = MakeListView(
          std::make_reverse_iterator(lanes.lower_bound(0)), lanes.rend());
      const auto computed_width =
          ComputeTotalLaneWidth(side_lanes, waypoint.s, waypoint.lane_id);
      lane_width = computed_width.first;
      lane_tangent = computed_width.second;
    } else {
      // left lane
      const auto side_lanes = MakeListView(lanes.lower_bound(1), lanes.end());
      const auto computed_width =
          ComputeTotalLaneWidth(side_lanes, waypoint.s, waypoint.lane_id);
      lane_width = computed_width.first;
      lane_tangent = computed_width.second;
    }

    // Unreal's Y axis hack
    lane_tangent *= -1;

    // get a directed point in s and apply the computed lateral offet
    DirectedPoint dp = road.GetDirectedPointIn(waypoint.s);

    geom::Rotation rot(
        geom::Math::to_degrees(dp.pitch),
        geom::Math::to_degrees(-dp.tangent), // Unreal's Y axis hack
        0.0);

    dp.ApplyLateralOffset(lane_width);

    if (waypoint.lane_id > 0) {
      rot.yaw += 180.0f + geom::Math::to_degrees(lane_tangent);
      rot.pitch = 360.0f - rot.pitch;
    } else {
      rot.yaw -= geom::Math::to_degrees(lane_tangent);
    }

    // Unreal's Y axis hack
    dp.location.y *= -1;

    return geom::Transform(dp.location, rot);
  }

  // ===========================================================================
  // -- Map: Road information --------------------------------------------------
  // ===========================================================================

  std::string Map::GetLaneType(const Waypoint waypoint) const {
    return GetLane(waypoint).GetType();
  }

  double Map::GetLaneWidth(const Waypoint waypoint) const {
    const auto s = waypoint.s;

    const auto &lane = GetLane(waypoint);
    THROW_INVALID_INPUT_ASSERT(s <= lane.GetRoad()->GetLength());

    const auto lane_width_info = lane.GetInfo<RoadInfoLaneWidth>(s);
    THROW_INVALID_INPUT_ASSERT(lane_width_info != nullptr);

    return lane_width_info->GetPolynomial().Evaluate(s);
  }

  bool Map::IsJunction(const RoadId road_id) const {
    return _data.GetRoad(road_id).IsJunction();
  }

  std::pair<const RoadInfoMarkRecord *, const RoadInfoMarkRecord *>
  Map::GetMarkRecord(const Waypoint waypoint) const {
    const auto s = waypoint.s;

    const auto &current_lane = GetLane(waypoint);
    THROW_INVALID_INPUT_ASSERT(s <= current_lane.GetRoad()->GetLength());

    const auto inner_lane_id = waypoint.lane_id < 0 ?
        waypoint.lane_id + 1 :
        waypoint.lane_id - 1;

    const auto &inner_lane = current_lane.GetRoad()->GetLaneById(waypoint.section_id, inner_lane_id);

    auto current_lane_info = current_lane.GetInfo<RoadInfoMarkRecord>(s);
    THROW_INVALID_INPUT_ASSERT(current_lane_info != nullptr);
    auto inner_lane_info = inner_lane.GetInfo<RoadInfoMarkRecord>(s);
    THROW_INVALID_INPUT_ASSERT(inner_lane_info != nullptr);

    return std::make_pair(current_lane_info, inner_lane_info);
  }

  std::vector<LaneMarking> Map::CalculateCrossedLanes(
      const geom::Location &origin,
      const geom::Location &destination) const {
    return LaneCrossingCalculator::Calculate(*this, origin, destination);
  }

  // ===========================================================================
  // -- Map: Waypoint generation -----------------------------------------------
  // ===========================================================================

  std::vector<Waypoint> Map::GetSuccessors(const Waypoint waypoint) const {
    const auto &next_lanes = GetLane(waypoint).GetNextLanes();
    std::vector<Waypoint> result;
    result.reserve(next_lanes.size());
    for (auto *next_lane : next_lanes) {
      THROW_INVALID_INPUT_ASSERT(next_lane != nullptr);
      const auto lane_id = next_lane->GetId();
      THROW_INVALID_INPUT_ASSERT(lane_id != 0);
      const auto *section = next_lane->GetLaneSection();
      THROW_INVALID_INPUT_ASSERT(section != nullptr);
      const auto *road = next_lane->GetRoad();
      THROW_INVALID_INPUT_ASSERT(road != nullptr);
      const auto distance = GetDistanceAtStartOfLane(*next_lane);
      result.emplace_back(Waypoint{road->GetId(), section->GetId(), lane_id, distance});
    }
    return result;
  }

  std::vector<Waypoint> Map::GetNext(
      const Waypoint waypoint,
      const float distance) const {
    THROW_INVALID_INPUT_ASSERT(distance > 0.0f);
    const auto &lane = GetLane(waypoint);
    const bool forward = (waypoint.lane_id <= 0);
    const float signed_distance = forward ? distance : -distance;
    const float relative_s = waypoint.s - lane.GetDistance();
    const float remaining_lane_length = forward ? lane.GetLength() - relative_s : relative_s;
    DEBUG_ASSERT(remaining_lane_length >= 0.0f);

    // If after subtracting the distance we are still in the same lane, return
    // same waypoint with the extra distance.
    if (distance < remaining_lane_length) {
      Waypoint result = waypoint;
      result.s += signed_distance;
      return { result };
    }

    // If we run out of remaining_lane_length we have to go to the successors.
    std::vector<Waypoint> result;
    for (const auto &successor : GetSuccessors(waypoint)) {
      DEBUG_ASSERT(
          successor.road_id != waypoint.road_id ||
          successor.section_id != waypoint.section_id ||
          successor.lane_id != waypoint.lane_id ||
          successor.s != waypoint.s);
      result = ConcatVectors(result, GetNext(successor, distance - remaining_lane_length));
    }
    return result;
  }

  boost::optional<Waypoint> Map::GetRight(Waypoint waypoint) const {
    THROW_INVALID_INPUT_ASSERT(waypoint.lane_id != 0);
    if (waypoint.lane_id > 0) {
      ++waypoint.lane_id;
    } else {
      --waypoint.lane_id;
    }
    return IsLanePresent(_data, waypoint) ? waypoint : boost::optional<Waypoint>{};
  }

  boost::optional<Waypoint> Map::GetLeft(Waypoint waypoint) const {
    THROW_INVALID_INPUT_ASSERT(waypoint.lane_id != 0);
    if (std::abs(waypoint.lane_id) == 1) {
      waypoint.lane_id *= -1;
    } else if (waypoint.lane_id > 0) {
      --waypoint.lane_id;
    } else {
      ++waypoint.lane_id;
    }
    return IsLanePresent(_data, waypoint) ? waypoint : boost::optional<Waypoint>{};
  }

  std::vector<Waypoint> Map::GenerateWaypoints(const float distance) const {
    std::vector<Waypoint> result;
    for (const auto &pair : _data.GetRoads()) {
      const auto &road = pair.second;
      for (float s = 0.0f; s < road.GetLength(); s += distance) {
        ForEachDrivableLaneAt(road, s, [&](auto &&waypoint) {
          result.emplace_back(waypoint);
        });
      }
    }
    return result;
  }

  // std::vector<Waypoint> Map::GenerateLaneBegin() const {
  //   // std::vector<Waypoint> result;
  //   // for (auto &&road_segment : map.GetData().GetRoadSegments()) {
  //   //   ForEachDrivableLane(road_segment, 0.0, [&](auto lane_id) {
  //   //     auto distance = lane_id < 0 ? 0.0 : road_segment.GetLength();
  //   //     auto this_waypoint = Waypoint(
  //   //         map.shared_from_this(),
  //   //         road_segment.GetId(),
  //   //         lane_id,
  //   //         distance);
  //   //     result.push_back(this_waypoint);
  //   //   });
  //   // }
  //   // return result;
  //   throw_exception(std::runtime_error("not implemented"));
  //   return {};
  // }

  // std::vector<Waypoint> Map::GenerateLaneEnd() const {
  //   // std::vector<Waypoint> result;
  //   // for (auto &&road_segment : map.GetData().GetRoadSegments()) {
  //   //   ForEachDrivableLane(road_segment, 0.0, [&](auto lane_id) {
  //   //     auto distance = lane_id > 0 ? 0.0 : road_segment.GetLength();
  //   //     auto this_waypoint = Waypoint(
  //   //         map.shared_from_this(),
  //   //         road_segment.GetId(),
  //   //         lane_id,
  //   //         distance);
  //   //     result.push_back(this_waypoint);
  //   //   });
  //   // }
  //   // return result;
  //   throw_exception(std::runtime_error("not implemented"));
  //   return {};
  // }

  std::vector<std::pair<Waypoint, Waypoint>> Map::GenerateTopology() const {
    std::vector<std::pair<Waypoint, Waypoint>> result;
    for (const auto &pair : _data.GetRoads()) {
      const auto &road = pair.second;
      ForEachDrivableLane(road, [&](auto &&waypoint) {
        for (auto &&successor : GetSuccessors(waypoint)) {
          result.push_back({waypoint, successor});
        }
      });
    }
    return result;
  }

  // ===========================================================================
  // -- Map: Private functions -------------------------------------------------
  // ===========================================================================

  const Lane &Map::GetLane(Waypoint waypoint) const {
    return _data.GetRoad(waypoint.road_id).GetLaneById(waypoint.section_id, waypoint.lane_id);
  }

} // namespace road
} // namespace carla

#undef THROW_INVALID_INPUT_ASSERT
