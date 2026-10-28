// Copyright (c) 2026. Licensed under the GNU AGPLv3, as Bambu Studio.
#ifndef slic3r_ExtendedAMSLite_Planner_hpp_
#define slic3r_ExtendedAMSLite_Planner_hpp_

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r { namespace ExtendedAMSLite {

// IDs are zero based. A virtual filament always retains its profile and color;
// a slot is only a position in this simulation, never a printer tool number.
struct Filament {
    std::string name;
    std::string material;
    std::string color;
};

struct Request {
    int filament = -1;
    unsigned int layer = 0; // 0 means startup/unknown, not layer 1
    std::size_t move = 0;
    unsigned int gcode_line = 0;
};

struct Slot {
    int filament = -1; // -1: choose the initial occupant automatically
    bool locked = false; // occupant may never be replaced
};

struct Replacement {
    std::size_t slot = 0;
    int remove = -1;
    int insert = -1;
};

struct Stop {
    std::size_t before_request = 0;
    std::vector<Replacement> replacements;
};

struct Plan {
    std::vector<int> initial_slots;
    std::vector<std::size_t> request_slots;
    std::vector<Stop> stops;
};

struct LayerOrder {
    unsigned int layer = 0;
    std::vector<int> filaments; // zero based, each required identity once
};

struct BatchPlan {
    Plan schedule;
    std::vector<Request> requests;
    std::vector<int> filament_slots; // fixed for the whole job; -1 if unused
    std::vector<std::vector<int>> banks;
    std::vector<LayerOrder> layers;
    std::size_t spool_replacements = 0;
    std::size_t color_changes = 0; // excludes the initial selection
    bool reordered = false; // positions require a new slice when true
};

// Minimize visits rather than individual replacements. Each stop prepares the
// longest upcoming sequence with no conflicting identities in a physical slot.
// Unlike the cache optimizer, a virtual identity keeps one slot for the job.
// Optional layer reordering is a proposal for Studio's existing sequence API;
// it never edits or reorders already generated G-code.
BatchPlan plan_batches(const std::vector<Request>& requests, std::size_t filament_count,
                       const std::vector<Slot>& slots,
                       const std::vector<int>& first_layer_order = {},
                       bool reorder_layers = false);
void verify_batches(const std::vector<Slot>& slots, const BatchPlan& result);
// Studio serializes each native sequence with an equal record width. Append
// unused identities after each layer's required prefix to retain that contract.
std::vector<LayerOrder> complete_layer_orders(const BatchPlan& result);
std::string color_name(const std::string& color);
std::string format_batch_schedule(const std::vector<Filament>& filaments, const BatchPlan& result);
std::string format_batch_schedule_html(const std::vector<Filament>& filaments, const BatchPlan& result);

// Throws invalid_argument for invalid inputs, runtime_error for impossible locks.
// Minimizes demand replacements with Belady's farthest-next-use policy and
// deterministic ties by slot index. Initial automatic loads are free.
Plan plan(const std::vector<Request>& requests, std::size_t filament_count,
          const std::vector<Slot>& slots);

// Independently replay every operation; throws on stale or inconsistent plans.
void verify(const std::vector<Request>& requests, std::size_t filament_count,
            const std::vector<Slot>& slots, const Plan& plan);

// These are hypothetical stops. No supported execution backend exists yet.
const char* execution_blocker();
std::string format_schedule(const std::vector<Filament>& filaments,
                            const std::vector<Request>& requests, const Plan& plan);
std::string format_schedule_html(const std::vector<Filament>& filaments,
                                 const std::vector<Request>& requests, const Plan& plan);

// Consume the actual processed move order, including tool selections before
// purge/extrusion. Do not infer order from a per-layer set or a sorted palette.
// Templated so the trace contract can be tested without the slicer's dependencies.
template<class Moves, class MoveType>
std::vector<Request> collect_requests(const Moves& moves, MoveType tool_change, MoveType extrude,
                                      bool preserve_layers = false)
{
    std::vector<Request> requests;
    for (std::size_t i = 0; i < moves.size(); ++i) {
        const auto& move = moves[i];
        if (move.type != tool_change && move.type != extrude)
            continue;
        const int filament = static_cast<int>(move.extruder_id);
        if (requests.empty() || requests.back().filament != filament ||
            (preserve_layers && requests.back().layer != move.layer_id))
            requests.push_back({filament, move.layer_id, i, move.gcode_id});
    }
    return requests;
}

}} // namespace Slic3r::ExtendedAMSLite
#endif
