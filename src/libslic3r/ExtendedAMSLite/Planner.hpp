// Copyright (c) 2026. Licensed under the GNU AGPLv3, as Bambu Studio.
#ifndef slic3r_ExtendedAMSLite_Planner_hpp_
#define slic3r_ExtendedAMSLite_Planner_hpp_

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r { namespace ExtendedAMSLite {

// IDs are zero based. A virtual filament always retains its profile and color;
// a slot is a physical feeder position, never a virtual printer tool number.
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
// Internal entry used by the optimizer: the same planner with the bank partition and/or the
// order inside each layer supplied instead of derived. Both are validated; null keeps the default.
BatchPlan plan_batches_custom(const std::vector<Request>& requests, std::size_t filament_count,
                              const std::vector<Slot>& slots, const std::vector<int>& first_layer_order,
                              bool reorder_layers, const std::vector<std::vector<int>>* banks,
                              const std::vector<std::vector<int>>* layer_orders);
void verify_batches(const std::vector<Slot>& slots, const BatchPlan& result);

// Purge cost of changing from one virtual filament to another (the project's flush table,
// mm^3, row = from). flow converts volume to an estimated purge time.
struct PurgeModel {
    std::size_t count = 0;
    std::vector<double> volume; // count x count
    double flow = 5.; // mm^3/s, estimate
    bool valid() const { return count > 0 && volume.size() == count * count && flow > 0.; }
    double at(int from, int to) const;
};
struct PlanScore {
    std::size_t visits = 0, replacements = 0, color_changes = 0;
    double purge_mm3 = 0., purge_seconds = 0.;
};
double purge_volume(const std::vector<Request>& requests, const PurgeModel& model);
PlanScore score_plan(const BatchPlan& plan, const PurgeModel& model);

struct OptimizedBatches {
    BatchPlan plan; // fewest visits, then least purge
    PlanScore score;
    BatchPlan baseline; // the plain planner result, for comparison
    PlanScore baseline_score;
    // One more visit than `plan` with enough less purge time to be worth suggesting.
    std::optional<BatchPlan> faster;
    PlanScore faster_score;
};
// Visits (the manual work) always come first; purge is only a tie-breaker among plans with the
// same number of visits. A plan with exactly one more visit is offered when it saves at least
// `suggest_seconds` of purging. A user-chosen first-layer order is never changed.
OptimizedBatches optimize_batches(const std::vector<Request>& requests, std::size_t filament_count,
                                  const std::vector<Slot>& slots, const std::vector<int>& first_layer_order,
                                  const PurgeModel& purge, double suggest_seconds = 600.);
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

// Planning reports do not activate execution. Execution.hpp provides the
// separately validated native A1 printing path.
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
