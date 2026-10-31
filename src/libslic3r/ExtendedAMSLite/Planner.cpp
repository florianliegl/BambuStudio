#include "Planner.hpp"
#include "Execution.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace Slic3r { namespace ExtendedAMSLite {
namespace {
constexpr std::size_t never = std::numeric_limits<std::size_t>::max();

void validate_inputs(const std::vector<Request>& requests, std::size_t count, const std::vector<Slot>& slots)
{
    if (slots.empty() || slots.size() > 32)
        throw std::invalid_argument("Configure between 1 and 32 physical slots (A1 AMS Lite: 4).");
    if (count == 0 || count > 254)
        throw std::invalid_argument("Virtual filament count must be between 1 and 254.");
    std::set<int> occupants;
    for (const Slot& slot : slots) {
        if (slot.filament < -1 || (slot.filament >= 0 && std::size_t(slot.filament) >= count))
            throw std::invalid_argument("Initial slot references an unknown virtual filament.");
        if (slot.locked && slot.filament == -1)
            throw std::invalid_argument("Select a virtual filament before locking a slot.");
        if (slot.filament >= 0 && !occupants.insert(slot.filament).second)
            throw std::invalid_argument("Initial slots must have distinct virtual filaments.");
    }
    if (requests.empty())
        throw std::invalid_argument("Slice the plate first: no filament requests were found.");
    for (std::size_t i = 0; i < requests.size(); ++i) {
        if (requests[i].filament < 0 || std::size_t(requests[i].filament) >= count)
            throw std::invalid_argument("Toolpath references an unknown virtual filament. Re-slice the plate.");
        if (i > 0 && requests[i].move < requests[i - 1].move)
            throw std::invalid_argument("Filament requests must be in actual toolpath order.");
    }
}

std::string label(const std::vector<Filament>& filaments, int id)
{
    if (id < 0)
        return "Empty";
    const auto& filament = filaments.at(std::size_t(id));
    return "V" + std::to_string(id + 1) + " " + filament.name + " [" + filament.material + ", " + filament.color + "]";
}

std::string escape_html(const std::string& value)
{
    std::string escaped;
    for (char ch : value) {
        switch (ch) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        case '\'': escaped += "&#39;"; break;
        default: escaped += ch; break;
        }
    }
    return escaped;
}

std::string html_label(const std::vector<Filament>& filaments, int id)
{
    if (id < 0) return "Empty";
    const auto& filament = filaments.at(std::size_t(id));
    std::string color = filament.color;
    if ((color.size() != 7 && color.size() != 9) || color[0] != '#' ||
        !std::all_of(color.begin() + 1, color.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; }))
        color = "#808080";
    return "<span class=swatch style=\"background:" + color + "\"></span><span>" +
        escape_html(label(filaments, id)) + "</span>";
}
}

Plan plan(const std::vector<Request>& requests, std::size_t count, const std::vector<Slot>& slots)
{
    validate_inputs(requests, count, slots);
    Plan result;
    for (const Slot& slot : slots)
        result.initial_slots.push_back(slot.filament);

    // Preload the first distinct future requirements into unspecified slots.
    // Explicit occupants (including unlocked ones) are user overrides.
    for (const Request& request : requests) {
        if (std::find(result.initial_slots.begin(), result.initial_slots.end(), request.filament) != result.initial_slots.end())
            continue;
        auto empty = std::find(result.initial_slots.begin(), result.initial_slots.end(), -1);
        if (empty == result.initial_slots.end())
            break;
        *empty = request.filament;
    }

    std::vector<std::size_t> next_use(count, never), next(requests.size(), never);
    for (std::size_t i = requests.size(); i-- > 0;) {
        const int filament = requests[i].filament;
        next[i] = next_use[filament];
        next_use[filament] = i;
    }
    auto loaded = result.initial_slots;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const int filament = requests[i].filament;
        auto found = std::find(loaded.begin(), loaded.end(), filament);
        std::size_t slot = std::size_t(found - loaded.begin());
        if (found == loaded.end()) {
            slot = never;
            std::size_t farthest = 0;
            for (std::size_t s = 0; s < slots.size(); ++s) {
                if (slots[s].locked)
                    continue;
                const std::size_t future = loaded[s] == -1 ? never : next_use[loaded[s]];
                if (slot == never || future > farthest) {
                    slot = s;
                    farthest = future;
                }
            }
            if (slot == never)
                throw std::runtime_error("All slots are locked. Unlock a slot to schedule virtual filament " + std::to_string(filament + 1) + ".");
            result.stops.push_back({i, {{slot, loaded[slot], filament}}});
            loaded[slot] = filament;
        }
        result.request_slots.push_back(slot);
        next_use[filament] = next[i];
    }
    verify(requests, count, slots, result);
    return result;
}

BatchPlan plan_batches(const std::vector<Request>& input, std::size_t count,
                       const std::vector<Slot>& slots, const std::vector<int>& first_order,
                       bool reorder_layers)
{
    return plan_batches_custom(input, count, slots, first_order, reorder_layers, nullptr, nullptr);
}

BatchPlan plan_batches_custom(const std::vector<Request>& input, std::size_t count,
                              const std::vector<Slot>& slots, const std::vector<int>& first_order,
                              bool reorder_layers, const std::vector<std::vector<int>>* custom_banks,
                              const std::vector<std::vector<int>>* custom_orders)
{
    validate_inputs(input, count, slots);
    if ((custom_banks || custom_orders) && !reorder_layers)
        throw std::invalid_argument("Custom banks and layer orders require layer reordering.");
    BatchPlan result;
    result.reordered = reorder_layers;
    result.filament_slots.assign(count, -1);
    std::vector<int> palette;
    std::set<int> manual_seen;
    for (int id : first_order) {
        if (id < 0 || std::size_t(id) >= count || !manual_seen.insert(id).second)
            throw std::invalid_argument("First-layer order must contain distinct, valid filament numbers.");
        if (std::any_of(input.begin(), input.end(), [id](const Request& r) { return r.filament == id; }))
            palette.push_back(id);
    }
    const std::size_t manual_prefix = palette.size();
    const auto add_palette = [&](int id) {
        if (std::find(palette.begin(), palette.end(), id) == palette.end()) palette.push_back(id);
    };
    for (const auto& request : input) add_palette(request.filament);
    for (const auto& slot : slots) if (slot.filament >= 0) add_palette(slot.filament);
    // Reopening and applying the same optimization must not rotate the banks
    // again merely because the previous proposal changed first-use order.
    if (reorder_layers) std::sort(palette.begin() + manual_prefix, palette.end());

    std::vector<int> free_slots;
    std::set<int> locked_filaments;
    std::vector<int> forced(count, -1), bank_of(count, -1);
    for (std::size_t s = 0; s < slots.size(); ++s) {
        if (slots[s].filament >= 0) forced[slots[s].filament] = static_cast<int>(s);
        if (slots[s].locked) {
            locked_filaments.insert(slots[s].filament);
            result.filament_slots[slots[s].filament] = static_cast<int>(s);
        } else free_slots.push_back(static_cast<int>(s));
    }
    std::vector<int> mutable_colors;
    for (int id : palette) if (!locked_filaments.count(id)) mutable_colors.push_back(id);
    if (free_slots.empty() && !mutable_colors.empty())
        throw std::runtime_error("All slots are locked. Unlock a slot for the remaining colors.");
    std::vector<std::vector<int>> chunks;
    if (custom_banks) {
        std::vector<int> flat, expected = mutable_colors;
        for (const auto& bank : *custom_banks) {
            if (bank.empty() || bank.size() > free_slots.size())
                throw std::invalid_argument("A custom bank must hold between one and four colors.");
            flat.insert(flat.end(), bank.begin(), bank.end());
        }
        std::sort(flat.begin(), flat.end());
        std::sort(expected.begin(), expected.end());
        if (flat != expected) throw std::invalid_argument("Custom banks must cover exactly the colors that can be swapped.");
        chunks = *custom_banks;
    } else {
        for (std::size_t offset = 0; offset < mutable_colors.size(); offset += free_slots.size()) {
            const std::size_t end = std::min(offset + free_slots.size(), mutable_colors.size());
            chunks.emplace_back(mutable_colors.begin() + offset, mutable_colors.begin() + end);
        }
    }
    for (const auto& bank : chunks) {
        std::set<int> taken;
        for (int id : bank) if (forced[id] >= 0) {
            result.filament_slots[id] = forced[id];
            taken.insert(forced[id]);
        }
        for (int id : bank) {
            if (result.filament_slots[id] < 0) {
                auto available = std::find_if(free_slots.begin(), free_slots.end(), [&](int s) { return !taken.count(s); });
                if (available == free_slots.end()) throw std::runtime_error("Conflicting initial slot overrides.");
                result.filament_slots[id] = *available;
                taken.insert(*available);
            }
            bank_of[id] = static_cast<int>(result.banks.size());
        }
        result.banks.push_back(bank);
    }

    if (!reorder_layers) result.requests = input;
    else {
        // Layer numbers must represent one non-sequential, single-extruder job.
        for (const auto& request : input) {
            if (request.layer == 0) continue; // startup is regenerated by Studio
            if (!result.layers.empty() && request.layer < result.layers.back().layer)
                throw std::invalid_argument("Batch reordering requires printing by layer. Sequential objects are unsupported.");
            if (result.layers.empty() || result.layers.back().layer != request.layer)
                result.layers.push_back({request.layer, {}});
            auto& colors = result.layers.back().filaments;
            if (std::find(colors.begin(), colors.end(), request.filament) == colors.end()) colors.push_back(request.filament);
        }
        if (result.layers.empty())
            throw std::invalid_argument("Layer information is unavailable. Open the original project and slice it first.");
        int previous = -1, carry_bank = -1;
        for (std::size_t l = 0; l < result.layers.size(); ++l) {
            auto& layer = result.layers[l];
            const auto required = layer.filaments;
            std::vector<int> ordered;
            const auto required_here = [&](int id) { return std::find(required.begin(), required.end(), id) != required.end(); };
            const auto append = [&](int id) {
                if (required_here(id) && std::find(ordered.begin(), ordered.end(), id) == ordered.end()) ordered.push_back(id);
            };
            if (custom_orders) {
                if (l >= custom_orders->size()) throw std::invalid_argument("A custom layer order is missing.");
                auto given = (*custom_orders)[l], expected = required;
                std::sort(given.begin(), given.end());
                std::sort(expected.begin(), expected.end());
                if (given != expected) throw std::invalid_argument("A custom layer order must hold exactly the colors of its layer.");
                ordered = (*custom_orders)[l];
            } else if (l == 0 && !first_order.empty()) {
                for (int id : first_order) append(id);
                for (int id : palette) append(id);
            } else {
                if (previous >= 0) append(previous);
                std::vector<int> bank_order;
                if (carry_bank >= 0 && std::any_of(required.begin(), required.end(), [&](int id) { return bank_of[id] == carry_bank; }))
                    bank_order.push_back(carry_bank);
                for (int id : palette) if (required_here(id) && bank_of[id] >= 0 &&
                    std::find(bank_order.begin(), bank_order.end(), bank_of[id]) == bank_order.end())
                    bank_order.push_back(bank_of[id]);
                // Preserve the carry bank first; end on another bank used next
                // layer when possible, so the next layer can reuse that load.
                const std::size_t first_free = !bank_order.empty() && (l == 0 || bank_order[0] == carry_bank) ? 1 : 0;
                if (l + 1 < result.layers.size() && bank_order.size() > first_free + 1) {
                    const auto& next = result.layers[l + 1].filaments;
                    auto useful = bank_order.end();
                    for (auto candidate = bank_order.begin() + first_free; candidate != bank_order.end(); ++candidate)
                        if (std::any_of(next.begin(), next.end(), [&](int id) { return bank_of[id] == *candidate; }) &&
                            (useful == bank_order.end() || *candidate > *useful)) useful = candidate;
                    if (useful != bank_order.end()) std::rotate(useful, useful + 1, bank_order.end());
                }
                for (std::size_t b = 0; b < bank_order.size(); ++b) {
                    std::vector<int> group;
                    for (int id : result.banks[bank_order[b]])
                        if (required_here(id) && std::find(ordered.begin(), ordered.end(), id) == ordered.end()) group.push_back(id);
                    if (b + 1 == bank_order.size() && l + 1 < result.layers.size()) {
                        const auto& next = result.layers[l + 1].filaments;
                        auto common = group.end();
                        for (auto candidate = group.begin(); candidate != group.end(); ++candidate)
                            if (std::find(next.begin(), next.end(), *candidate) != next.end() &&
                                (common == group.end() || *candidate < *common)) common = candidate;
                        if (common != group.end()) std::rotate(common, common + 1, group.end());
                    }
                    for (int id : group) append(id);
                    if (b == 0) for (int id : palette) if (locked_filaments.count(id)) append(id);
                }
                for (int id : palette) append(id); // all-locked layers as well
            }
            if (ordered.size() != required.size()) throw std::runtime_error("Layer ordering lost a required filament.");
            layer.filaments = ordered;
            for (int id : ordered) {
                if (result.requests.empty() || result.requests.back().filament != id)
                    result.requests.push_back({id, layer.layer, result.requests.size(), 0});
                previous = id;
                if (bank_of[id] >= 0) carry_bank = bank_of[id];
            }
        }
    }

    for (const Slot& slot : slots) result.schedule.initial_slots.push_back(slot.filament);
    for (std::size_t s = 0; s < slots.size(); ++s) if (result.schedule.initial_slots[s] < 0) {
        for (const auto& request : result.requests) if (result.filament_slots[request.filament] == static_cast<int>(s)) {
            result.schedule.initial_slots[s] = request.filament;
            break;
        }
    }
    auto loaded = result.schedule.initial_slots;
    for (std::size_t i = 0; i < result.requests.size(); ++i) {
        const auto& request = result.requests[i];
        const std::size_t s = static_cast<std::size_t>(result.filament_slots[request.filament]);
        if (loaded[s] != request.filament) {
            std::vector<int> upcoming(slots.size(), -1);
            for (std::size_t j = i; j < result.requests.size(); ++j) {
                const int id = result.requests[j].filament;
                const int physical = result.filament_slots[id];
                if (upcoming[physical] >= 0 && upcoming[physical] != id) break;
                upcoming[physical] = id;
            }
            Stop stop{i, {}};
            for (std::size_t physical = 0; physical < slots.size(); ++physical)
                if (upcoming[physical] >= 0 && loaded[physical] != upcoming[physical]) {
                    if (slots[physical].locked) throw std::runtime_error("A planned batch would replace a locked spool.");
                    stop.replacements.push_back({physical, loaded[physical], upcoming[physical]});
                    loaded[physical] = upcoming[physical];
                    ++result.spool_replacements;
                }
            result.schedule.stops.push_back(std::move(stop));
        }
        result.schedule.request_slots.push_back(s);
        if (i > 0 && result.requests[i - 1].filament != request.filament) ++result.color_changes;
    }
    verify_batches(slots, result);
    return result;
}

void verify_batches(const std::vector<Slot>& slots, const BatchPlan& result)
{
    verify(result.requests, result.filament_slots.size(), slots, result.schedule);
    std::size_t replacements = 0, changes = 0;
    for (std::size_t i = 0; i < result.requests.size(); ++i) {
        const int id = result.requests[i].filament;
        if (result.filament_slots.at(id) != static_cast<int>(result.schedule.request_slots[i]))
            throw std::invalid_argument("A virtual filament changed physical slots during the job.");
        if (i > 0 && result.requests[i - 1].filament != id) ++changes;
    }
    for (const auto& stop : result.schedule.stops) replacements += stop.replacements.size();
    if (replacements != result.spool_replacements || changes != result.color_changes)
        throw std::invalid_argument("Batch statistics do not match the verified schedule.");
    for (const auto& layer : result.layers) {
        std::set<int> unique(layer.filaments.begin(), layer.filaments.end());
        if (unique.size() != layer.filaments.size()) throw std::invalid_argument("A layer repeats a virtual filament.");
    }
}

double PurgeModel::at(int from, int to) const
{
    if (from < 0 || to < 0 || from == to || std::size_t(from) >= count || std::size_t(to) >= count || volume.size() != count * count)
        return 0.;
    return volume[std::size_t(from) * count + std::size_t(to)];
}

double purge_volume(const std::vector<Request>& requests, const PurgeModel& model)
{
    double total = 0.;
    for (std::size_t i = 1; i < requests.size(); ++i) total += model.at(requests[i - 1].filament, requests[i].filament);
    return total;
}

PlanScore score_plan(const BatchPlan& plan, const PurgeModel& model)
{
    PlanScore score;
    score.visits = plan.schedule.stops.size();
    score.replacements = plan.spool_replacements;
    score.color_changes = plan.color_changes;
    score.purge_mm3 = purge_volume(plan.requests, model);
    score.purge_seconds = model.flow > 0. ? score.purge_mm3 / model.flow : 0.;
    return score;
}

namespace {
using Orders = std::vector<std::vector<int>>;
using Clock = std::chrono::steady_clock;

struct Eval { std::size_t visits = 0, replacements = 0; double purge = 0.; };

// Replays the batch scheduler on the printing order alone. It mirrors the scheduling loop in
// plan_batches_custom so thousands of candidate orders can be compared cheaply.
Eval evaluate_orders(const Orders& orders, const std::vector<int>& filament_slots, const std::vector<Slot>& slots, const PurgeModel& model)
{
    std::vector<int> ids;
    for (const auto& layer : orders)
        for (int id : layer) if (ids.empty() || ids.back() != id) ids.push_back(id);
    std::vector<int> loaded;
    for (const Slot& slot : slots) loaded.push_back(slot.filament);
    for (std::size_t s = 0; s < loaded.size(); ++s) if (loaded[s] < 0)
        for (int id : ids) if (filament_slots[std::size_t(id)] == int(s)) { loaded[s] = id; break; }
    Eval eval;
    std::vector<int> upcoming(slots.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const int id = ids[i];
        if (loaded[std::size_t(filament_slots[std::size_t(id)])] != id) {
            ++eval.visits;
            std::fill(upcoming.begin(), upcoming.end(), -1);
            for (std::size_t j = i; j < ids.size(); ++j) {
                const std::size_t physical = std::size_t(filament_slots[std::size_t(ids[j])]);
                if (upcoming[physical] >= 0 && upcoming[physical] != ids[j]) break;
                upcoming[physical] = ids[j];
            }
            for (std::size_t physical = 0; physical < slots.size(); ++physical)
                if (upcoming[physical] >= 0 && loaded[physical] != upcoming[physical]) {
                    loaded[physical] = upcoming[physical];
                    ++eval.replacements;
                }
        }
        if (i > 0) eval.purge += model.at(ids[i - 1], id);
    }
    return eval;
}

// Visits beyond `cap` are the first thing to avoid, then purge, then visits.
bool better(const Eval& a, const Eval& b, std::size_t cap)
{
    const std::size_t over_a = a.visits > cap ? a.visits - cap : 0, over_b = b.visits > cap ? b.visits - cap : 0;
    if (over_a != over_b) return over_a < over_b;
    if (std::abs(a.purge - b.purge) > 1e-6) return a.purge < b.purge;
    if (a.visits != b.visits) return a.visits < b.visits;
    return a.replacements < b.replacements;
}

// Every ordering of `groups` (as whole groups) with every ordering inside each group.
template<class F>
void for_each_grouped_order(std::vector<std::vector<int>> groups, F&& visit)
{
    for (auto& g : groups) std::sort(g.begin(), g.end());
    std::vector<std::size_t> sequence(groups.size());
    std::iota(sequence.begin(), sequence.end(), std::size_t(0));
    std::vector<int> current;
    std::function<void(std::size_t)> expand = [&](std::size_t position) {
        if (position == sequence.size()) { visit(current); return; }
        auto group = groups[sequence[position]];
        do {
            const auto size = current.size();
            current.insert(current.end(), group.begin(), group.end());
            expand(position + 1);
            current.resize(size);
        } while (std::next_permutation(group.begin(), group.end()));
    };
    do expand(0); while (std::next_permutation(sequence.begin(), sequence.end()));
}

std::size_t factorial(std::size_t n) { std::size_t r = 1; for (std::size_t i = 2; i <= n; ++i) r *= i; return r; }

// Coordinate descent over the per-layer color order, keeping the bank partition fixed.
Orders refine_orders(Orders orders, const BatchPlan& base, const std::vector<Slot>& slots, const std::vector<int>& first_order,
                     const PurgeModel& model, std::size_t cap, Clock::time_point deadline)
{
    std::vector<int> bank_of(base.filament_slots.size(), -1);
    for (std::size_t b = 0; b < base.banks.size(); ++b) for (int id : base.banks[b]) bank_of[std::size_t(id)] = int(b);
    auto best = evaluate_orders(orders, base.filament_slots, slots, model);
    for (int pass = 0; pass < 3; ++pass) {
        bool improved = false;
        for (std::size_t l = 0; l < orders.size() && Clock::now() < deadline; ++l) {
            // A layer-1 color the user ordered by hand keeps its place and position.
            std::vector<int> prefix, rest;
            if (l == 0) for (int id : first_order)
                if (std::find(orders[l].begin(), orders[l].end(), id) != orders[l].end()) prefix.push_back(id);
            for (int id : orders[l]) if (std::find(prefix.begin(), prefix.end(), id) == prefix.end()) rest.push_back(id);
            if (rest.size() < 2) continue;
            std::map<int, std::vector<int>> by_bank;
            std::vector<std::vector<int>> groups;
            for (int id : rest) {
                if (bank_of[std::size_t(id)] < 0) groups.push_back({id});
                else by_bank[bank_of[std::size_t(id)]].push_back(id);
            }
            for (auto& entry : by_bank) groups.push_back(entry.second);
            std::size_t combos = factorial(groups.size());
            for (const auto& g : groups) combos *= factorial(g.size());
            if (combos > 20000) continue;
            Orders trial = orders;
            std::vector<int> chosen = orders[l];
            for_each_grouped_order(groups, [&](const std::vector<int>& candidate) {
                trial[l] = prefix;
                trial[l].insert(trial[l].end(), candidate.begin(), candidate.end());
                const auto eval = evaluate_orders(trial, base.filament_slots, slots, model);
                if (better(eval, best, cap)) { best = eval; chosen = trial[l]; improved = true; }
            });
            orders[l] = chosen;
        }
        if (!improved || Clock::now() >= deadline) break;
    }
    return orders;
}

// Splits `colors` (ascending) into groups of the given sizes. Groups of equal size are
// unordered, so each partition is produced once.
void for_each_partition(const std::vector<int>& colors, const std::vector<std::size_t>& sizes, std::size_t limit,
                        const std::function<void(const Orders&)>& visit)
{
    Orders current;
    std::size_t produced = 0;
    std::function<void(const std::vector<int>&, std::size_t)> split = [&](const std::vector<int>& remaining, std::size_t k) {
        if (produced >= limit) return;
        if (k == sizes.size()) { ++produced; visit(current); return; }
        const std::size_t n = sizes[k];
        if (n > remaining.size()) return;
        std::vector<bool> mask(remaining.size(), false);
        std::fill(mask.begin(), mask.begin() + std::ptrdiff_t(n), true);
        do {
            std::vector<int> group, left;
            for (std::size_t i = 0; i < remaining.size(); ++i) (mask[i] ? group : left).push_back(remaining[i]);
            if (k > 0 && sizes[k] == sizes[k - 1] && group.front() < current.back().front()) continue;
            current.push_back(group);
            split(left, k + 1);
            current.pop_back();
            if (produced >= limit) return;
        } while (std::prev_permutation(mask.begin(), mask.end()));
    };
    split(colors, 0);
}
} // namespace

OptimizedBatches optimize_batches(const std::vector<Request>& input, std::size_t count, const std::vector<Slot>& slots,
                                  const std::vector<int>& first_order, const PurgeModel& purge, double suggest_seconds)
{
    OptimizedBatches out;
    out.baseline = plan_batches(input, count, slots, first_order, true);
    out.baseline_score = score_plan(out.baseline, purge);
    out.plan = out.baseline;
    out.score = out.baseline_score;
    if (!purge.valid() || purge.count < count) return out;
    const auto deadline = Clock::now() + std::chrono::milliseconds(2500);

    std::vector<int> colors;
    std::vector<std::size_t> sizes;
    for (const auto& bank : out.baseline.banks) { colors.insert(colors.end(), bank.begin(), bank.end()); sizes.push_back(bank.size()); }
    std::sort(colors.begin(), colors.end());
    // Larger banks first keeps the partition pattern canonical.
    std::stable_sort(sizes.begin(), sizes.end(), std::greater<std::size_t>());

    struct Candidate { Orders banks; BatchPlan plan; Eval eval; };
    std::vector<Candidate> candidates;
    const auto consider = [&](const Orders& banks) {
        try {
            auto plan = plan_batches_custom(input, count, slots, first_order, true, &banks, nullptr);
            Orders orders;
            for (const auto& layer : plan.layers) orders.push_back(layer.filaments);
            auto eval = evaluate_orders(orders, plan.filament_slots, slots, purge);
            candidates.push_back({banks, std::move(plan), eval});
        } catch (const std::exception&) { /* conflicting slot overrides: this partition is not usable */ }
    };
    if (!colors.empty()) for_each_partition(colors, sizes, 600, [&](const Orders& banks) { if (Clock::now() < deadline) consider(banks); });
    if (candidates.empty()) return out;

    std::size_t best_visits = out.score.visits;
    for (const auto& c : candidates) best_visits = std::min(best_visits, c.eval.visits);

    struct Result { BatchPlan plan; PlanScore score; };
    std::vector<Result> results;
    results.push_back({out.baseline, out.baseline_score});
    for (std::size_t cap : {best_visits, best_visits + 1}) {
        std::vector<const Candidate*> seeds;
        for (const auto& c : candidates) seeds.push_back(&c);
        std::stable_sort(seeds.begin(), seeds.end(), [&](const Candidate* a, const Candidate* b) { return better(a->eval, b->eval, cap); });
        for (std::size_t i = 0; i < seeds.size() && i < 4 && Clock::now() < deadline; ++i) {
            Orders orders;
            for (const auto& layer : seeds[i]->plan.layers) orders.push_back(layer.filaments);
            orders = refine_orders(std::move(orders), seeds[i]->plan, slots, first_order, purge, cap, deadline);
            try {
                auto plan = plan_batches_custom(input, count, slots, first_order, true, &seeds[i]->banks, &orders);
                auto score = score_plan(plan, purge);
                results.push_back({std::move(plan), score});
            } catch (const std::exception&) { /* never offer a plan that fails verification */ }
        }
    }

    const auto lexicographic = [](const PlanScore& a, const PlanScore& b) {
        if (a.visits != b.visits) return a.visits < b.visits;
        return a.purge_mm3 < b.purge_mm3 - 1e-6;
    };
    const Result* best = &results.front();
    for (const auto& r : results) if (lexicographic(r.score, best->score)) best = &r;
    // A user-chosen first-layer order is a hard constraint, whatever the optimizer prefers.
    const auto keeps_first_order = [&](const BatchPlan& plan) {
        if (first_order.empty() || plan.layers.empty()) return true;
        std::vector<int> wanted;
        for (int id : first_order) if (std::find(plan.layers.front().filaments.begin(), plan.layers.front().filaments.end(), id) != plan.layers.front().filaments.end()) wanted.push_back(id);
        return std::equal(wanted.begin(), wanted.end(), plan.layers.front().filaments.begin());
    };
    if (keeps_first_order(best->plan)) { out.plan = best->plan; out.score = best->score; }

    const Result* faster = nullptr;
    for (const auto& r : results)
        if (r.score.visits == out.score.visits + 1 && keeps_first_order(r.plan) &&
            out.score.purge_seconds - r.score.purge_seconds >= suggest_seconds &&
            (!faster || r.score.purge_mm3 < faster->score.purge_mm3)) faster = &r;
    if (faster) { out.faster = faster->plan; out.faster_score = faster->score; }
    return out;
}

std::vector<LayerOrder> complete_layer_orders(const BatchPlan& result)
{
    auto layers = result.layers;
    for (auto& layer : layers)
        for (std::size_t id = 0; id < result.filament_slots.size(); ++id)
            if (std::find(layer.filaments.begin(), layer.filaments.end(), int(id)) == layer.filaments.end())
                layer.filaments.push_back(int(id));
    return layers;
}

std::string color_name(const std::string& color)
{
    if (color.size() != 7 && color.size() != 9) return "Unknown color";
    if (color[0] != '#' || !std::all_of(color.begin() + 1, color.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) return "Unknown color";
    const int r = std::stoi(color.substr(1, 2), nullptr, 16), g = std::stoi(color.substr(3, 2), nullptr, 16), b = std::stoi(color.substr(5, 2), nullptr, 16);
    const int hi = std::max({r, g, b}), lo = std::min({r, g, b});
    if (hi < 48) return "Black";
    if (lo > 218) return "White";
    if (hi - lo < 28) return hi < 128 ? "Dark gray" : hi >= 160 ? "Light gray" : "Gray";
    if (r > g * 1.3 && g > b * 1.25 && hi < 175) return "Brown";
    if (r > 190 && g > 155 && b < 115) return "Yellow";
    if (r > 180 && g > 65 && g < 170 && b < 110 && g * 5 > r * 2) return "Orange";
    if (r > 180 && b > 110 && g < 150) return "Pink";
    if (b >= hi && r > b * 0.5 && g < b * 0.7) return "Purple";
    if (g >= hi && b > g * 0.72 && r < g * 0.75) return "Cyan";
    if (g >= hi) return "Green";
    if (b >= hi) return "Blue";
    return "Red";
}

void verify(const std::vector<Request>& requests, std::size_t count, const std::vector<Slot>& slots, const Plan& result)
{
    validate_inputs(requests, count, slots);
    const auto fail = [] { throw std::invalid_argument("Swap schedule failed replay verification. Recalculate the plan."); };
    if (result.initial_slots.size() != slots.size() || result.request_slots.size() != requests.size())
        fail();
    std::set<int> initial;
    for (std::size_t s = 0; s < slots.size(); ++s) {
        const int id = result.initial_slots[s];
        if (id < -1 || (id >= 0 && std::size_t(id) >= count) ||
            (id >= 0 && !initial.insert(id).second) ||
            (slots[s].filament >= 0 && slots[s].filament != id))
            fail();
    }
    auto loaded = result.initial_slots;
    std::size_t stop = 0;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        if (stop < result.stops.size() && result.stops[stop].before_request == i) {
            const auto& replacements = result.stops[stop].replacements;
            if (replacements.empty())
                fail();
            std::set<std::size_t> changed;
            for (const auto& replacement : replacements) {
                if (replacement.slot >= slots.size() || slots[replacement.slot].locked ||
                    !changed.insert(replacement.slot).second ||
                    loaded[replacement.slot] != replacement.remove ||
                    replacement.insert < 0 || std::size_t(replacement.insert) >= count ||
                    replacement.remove == replacement.insert)
                    fail();
                loaded[replacement.slot] = replacement.insert;
            }
            std::set<int> occupants;
            for (int id : loaded)
                if (id >= 0 && !occupants.insert(id).second)
                    fail();
            ++stop;
        }
        const std::size_t slot = result.request_slots[i];
        if (slot >= loaded.size() || loaded[slot] != requests[i].filament)
            fail();
    }
    if (stop != result.stops.size())
        fail();
}

const char* execution_blocker()
{
    return "Planning report only. No pauses or print assignments are applied by this report. "
           "Use Prepare print in the custom Studio to generate and validate A1 PLA swap pauses, "
           "then print with its fixed slot assignments over LAN. Complete every swap before pressing resume.";
}

std::string format_batch_schedule(const std::vector<Filament>& filaments, const BatchPlan& result)
{
    std::ostringstream out;
    out << "Extended AMS Lite - BATCH PLAN\n" << execution_blocker() << "\n\n"
        << (result.reordered ? "PROPOSED layer order. Apply in Studio and re-slice to obtain actual positions.\n" : "Current slice order.\n")
        << "Visits: " << result.schedule.stops.size() << "; spool replacements: " << result.spool_replacements
        << "; color changes: " << result.color_changes << "\n\nInitial loading\n";
    for (std::size_t s = 0; s < result.schedule.initial_slots.size(); ++s)
        out << "Slot " << s + 1 << ": " << label(filaments, result.schedule.initial_slots[s]) << '\n';
    out << "\nBatch swaps (all listed slots at the same visit)\n";
    for (std::size_t i = 0; i < result.schedule.stops.size(); ++i) {
        const auto& stop = result.schedule.stops[i];
        const auto& request = result.requests.at(stop.before_request);
        out << "\nVisit " << i + 1 << " of " << result.schedule.stops.size() << "; layer " << request.layer;
        if (!result.reordered) out << "; move " << request.move << "; G-code line ID " << request.gcode_line;
        out << '\n';
        for (const auto& change : stop.replacements)
            out << "Slot " << change.slot + 1 << ": remove " << label(filaments, change.remove)
                << " -> insert " << label(filaments, change.insert) << '\n';
        out << "Next color: " << label(filaments, request.filament) << '\n';
    }
    if (result.schedule.stops.empty()) out << "No manual replacements required.\n";
    if (result.reordered) {
        out << "\nProposed layer sequences (virtual filament numbers)\n";
        for (const auto& layer : result.layers) {
            out << "Layer " << layer.layer << ':';
            for (int id : layer.filaments) out << " V" << id + 1;
            out << '\n';
        }
    }
    out << "\nFixed slot assignment for this plan\n";
    for (std::size_t id = 0; id < result.filament_slots.size(); ++id)
        if (result.filament_slots[id] >= 0) out << "V" << id + 1 << " -> slot " << result.filament_slots[id] + 1 << '\n';
    return out.str();
}

std::string format_batch_schedule_html(const std::vector<Filament>& filaments, const BatchPlan& result)
{
    std::ostringstream out;
    out << R"(<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Extended AMS Lite - Batch swaps</title><style>
*{box-sizing:border-box}body{font:16px/1.6 system-ui,sans-serif;background:#f3f5f4;color:#20342c;margin:0}
main{max-width:1040px;margin:auto;padding:32px}h1{margin-bottom:8px}.notice{background:#fff2d8;padding:16px;border-radius:8px}
.stats{font-size:22px;font-weight:650;margin:24px 0}.slots{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:12px}
.slot{padding:16px;background:white;border:1px solid #d8e2dc;border-radius:10px}.slot b{display:block}.swatch{display:inline-block;width:16px;height:16px;border:1px solid #999;border-radius:50%;margin-right:6px}
section{margin:28px 0;break-inside:avoid}.muted{color:#64776d}details{margin-top:28px}pre{white-space:pre-wrap;font-size:13px}
@media print{body{background:white}main{padding:0}}</style><main><h1>Your batch swaps</h1><p>Replace the listed spools together at each visit.</p><div class=notice>)";
    out << escape_html(execution_blocker()) << "</div><div class=stats>" << result.schedule.stops.size()
        << " visits &nbsp; / &nbsp; " << result.spool_replacements << " spool replacements &nbsp; / &nbsp; "
        << result.color_changes << " color changes</div><p class=muted>"
        << (result.reordered ? "Proposed layer order. Apply in Studio and re-slice before reviewing actual positions." : "Based on the current slice order.")
        << "</p><section><h2>Load these first</h2><div class=slots>";
    for (std::size_t s = 0; s < result.schedule.initial_slots.size(); ++s)
        out << "<div class=slot><b>Slot " << s + 1 << "</b>" << html_label(filaments, result.schedule.initial_slots[s]) << "</div>";
    out << "</div></section>";
    for (std::size_t i = 0; i < result.schedule.stops.size(); ++i) {
        const auto& stop = result.schedule.stops[i];
        const auto& request = result.requests.at(stop.before_request);
        out << "<section><h2>Visit " << i + 1 << " of " << result.schedule.stops.size() << " &middot; Layer " << request.layer
            << "</h2><div class=slots>";
        for (const auto& change : stop.replacements)
            out << "<div class=slot><b>Slot " << change.slot + 1 << "</b><div class=muted>Remove "
                << html_label(filaments, change.remove) << "</div><div>Insert " << html_label(filaments, change.insert) << "</div></div>";
        out << "</div><p>Next color: " << html_label(filaments, request.filament) << "</p></section>";
    }
    if (result.schedule.stops.empty()) out << "<p>No manual replacements required.</p>";
    out << "<details><summary>Full plan and fixed slot assignments</summary><pre>"
        << escape_html(format_batch_schedule(filaments, result)) << "</pre></details></main></html>";
    return out.str();
}

std::string format_schedule(const std::vector<Filament>& filaments, const std::vector<Request>& requests, const Plan& result)
{
    std::ostringstream out;
    out << "Extended AMS Lite Mode - DRY RUN\n" << execution_blocker() << "\n\n";
    out << "Virtual filaments: " << filaments.size() << "; physical slots: " << result.initial_slots.size()
        << "; filament runs: " << requests.size() << "; planned stops: " << result.stops.size() << "\n\nInitial loading\n";
    for (std::size_t s = 0; s < result.initial_slots.size(); ++s)
        out << "Slot " << s + 1 << ": " << label(filaments, result.initial_slots[s]) << '\n';
    out << "\nChronological swap schedule (positions are before filament selection, not executable pause points)\n";
    for (std::size_t i = 0; i < result.stops.size(); ++i) {
        const Stop& stop = result.stops[i];
        const Request& request = requests.at(stop.before_request);
        out << "\nStop " << i + 1 << " of " << result.stops.size() << "; ";
        if (request.layer == 0) out << "Startup / unknown layer";
        else out << "Layer " << request.layer;
        out << "; move " << request.move << "; G-code line ID " << request.gcode_line << '\n';
        for (const Replacement& replacement : stop.replacements)
            out << "Slot " << replacement.slot + 1 << ": remove " << label(filaments, replacement.remove)
                << " -> insert " << label(filaments, replacement.insert) << '\n';
        out << "Next required: " << label(filaments, request.filament) << '\n';
    }
    return out.str();
}

std::string format_schedule_html(const std::vector<Filament>& filaments, const std::vector<Request>& requests, const Plan& result)
{
    std::ostringstream out;
    out << R"(<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Extended AMS Lite - Dry-run swap schedule</title>
<style>
*{box-sizing:border-box}body{font:15px/1.55 system-ui,sans-serif;margin:0;background:#f3f5f5;color:#18252a}
main{max-width:1160px;margin:auto;padding:36px 28px}h1{font-size:32px;margin:4px 0 12px;letter-spacing:-1px}
h2{font-size:20px;margin:28px 0 12px}.eyebrow{color:#386555;font-weight:700;font-size:12px;letter-spacing:2px}
.warning{background:#fff3d7;border-left:4px solid #c58b15;padding:16px 20px;border-radius:5px}
.stats{display:flex;gap:12px;flex-wrap:wrap;margin:24px 0}.stat{background:white;padding:16px 24px;border:1px solid #dce3e2;border-radius:8px}
.stat strong{font-size:27px;display:block}.slots{display:grid;grid-template-columns:repeat(auto-fit,minmax(240px,1fr));gap:12px}
.slot{padding:16px;background:white;border:1px solid #dce3e2;border-radius:8px}.slot b{display:block;margin-bottom:10px}
.swatch{display:inline-block;width:17px;height:17px;border-radius:50%;border:1px solid #0003;margin-right:8px;vertical-align:middle}
.table{overflow:auto;background:white;border:1px solid #dce3e2;border-radius:8px}table{border-collapse:collapse;width:100%;text-align:left}
th,td{padding:15px 18px;border-bottom:1px solid #e4e9e8;vertical-align:top}th{background:#eaf0ed;font-size:12px;text-transform:uppercase;white-space:nowrap}
small{display:block;color:#5d6d70}footer{margin-top:24px;color:#5d6d70;font-size:13px}
@media print{body{background:white}main{padding:0}.table{overflow:visible}tr{break-inside:avoid}}
</style><main><div class="eyebrow">EXTENDED AMS LITE / SIMULATION</div><h1>Your filament swap schedule</h1>
<div class="warning"><strong>Dry run - printing support unavailable.</strong><br>)";
    out << escape_html(execution_blocker()) << "</div><div class=stats>";
    out << "<div class=stat><strong>" << filaments.size() << "</strong>virtual filaments</div>"
        << "<div class=stat><strong>" << result.initial_slots.size() << "</strong>physical slots</div>"
        << "<div class=stat><strong>" << result.stops.size() << "</strong>planned stops</div>"
        << "<div class=stat><strong>" << requests.size() << "</strong>filament runs</div></div>";
    out << "<h2>Initial loading</h2><div class=slots>";
    for (std::size_t s = 0; s < result.initial_slots.size(); ++s)
        out << "<div class=slot><b>AMS Lite slot " << s + 1 << "</b>" << html_label(filaments, result.initial_slots[s]) << "</div>";
    out << "</div><h2>Chronological swap instructions</h2><p>Each position is before the next filament selection. These are hypothetical stops, not inserted pause commands.</p>"
           "<div class=table><table><thead><tr><th>Stop / layer</th><th>Physical slot</th><th>Remove</th><th>Insert / next required</th><th>Position</th></tr></thead><tbody>";
    for (std::size_t i = 0; i < result.stops.size(); ++i) {
        const auto& stop = result.stops[i];
        const auto& request = requests.at(stop.before_request);
        for (const auto& replacement : stop.replacements) {
            out << "<tr><td><b>" << i + 1 << " of " << result.stops.size() << "</b><small>";
            if (request.layer) out << "Layer " << request.layer;
            else out << "Startup / unknown";
            out << "</small></td><td>Slot " << replacement.slot + 1 << "</td><td>" << html_label(filaments, replacement.remove)
                << "</td><td>" << html_label(filaments, replacement.insert) << "</td><td>Move " << request.move
                << "<small>G-code line ID " << request.gcode_line << "</small></td></tr>";
        }
    }
    if (result.stops.empty()) out << "<tr><td colspan=5>All required filaments fit in the configured slots. No manual replacements are planned.</td></tr>";
    out << "</tbody></table></div><footer>Belady farthest-next-use planning, with deterministic slot ties and replay verification. "
           "Material compatibility, purge, and printer behavior are not validated by this report. "
           "IDs shown as V1, V2, ... retain the original filament profiles.</footer></main></html>";
    return out.str();
}

std::vector<Request> normalize_execution_requests(std::vector<Request> requests, int initial)
{
    auto first = requests.begin();
    while (first != requests.end() && first->layer == 0 && first->filament != initial) ++first;
    requests.erase(requests.begin(), first);
    if (requests.empty() || requests.front().filament != initial)
        throw std::invalid_argument("The initial tool selection is inconsistent. Re-slice with the standard A1 profile.");
    return requests;
}

BatchPlan plan_fixed_batches(const std::vector<Request>& requests, const std::vector<Slot>& slots,
                             const std::vector<int>& mapping)
{
    validate_inputs(requests, mapping.size(), slots);
    BatchPlan result;
    result.requests = requests;
    result.filament_slots = mapping;
    for (const auto& r : requests)
        if (mapping[r.filament] < 0 || std::size_t(mapping[r.filament]) >= slots.size())
            throw std::invalid_argument("A required color has no physical AMS slot.");
    for (std::size_t s = 0; s < slots.size(); ++s) {
        int initial = slots[s].filament;
        if (initial < 0) for (const auto& r : requests) if (mapping[r.filament] == int(s)) { initial = r.filament; break; }
        if (initial >= 0 && mapping[initial] != int(s))
            throw std::invalid_argument("Initial loading conflicts with a fixed slot assignment.");
        result.schedule.initial_slots.push_back(initial);
    }
    auto loaded = result.schedule.initial_slots;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        const auto& r = requests[i];
        const auto slot = std::size_t(mapping[r.filament]);
        if (loaded[slot] != r.filament) {
            std::vector<int> upcoming(slots.size(), -1);
            for (std::size_t j = i; j < requests.size(); ++j) {
                const int id = requests[j].filament, physical = mapping[id];
                if (upcoming[physical] >= 0 && upcoming[physical] != id) break;
                upcoming[physical] = id;
            }
            Stop stop{i, {}};
            for (std::size_t s = 0; s < slots.size(); ++s) if (upcoming[s] >= 0 && loaded[s] != upcoming[s]) {
                if (slots[s].locked) throw std::invalid_argument("A swap would replace a locked spool.");
                stop.replacements.push_back({s, loaded[s], upcoming[s]});
                loaded[s] = upcoming[s];
                ++result.spool_replacements;
            }
            result.schedule.stops.push_back(std::move(stop));
        }
        result.schedule.request_slots.push_back(slot);
        if (i && requests[i - 1].filament != r.filament) ++result.color_changes;
    }
    verify_batches(slots, result);
    return result;
}

void verify_execution(const ExecutionSpec& spec)
{
    if (spec.job_id.size() != 16 || !std::all_of(spec.job_id.begin(), spec.job_id.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))
        throw std::invalid_argument("Invalid Extended AMS Lite job identifier.");
    if (spec.slots.size() != 4 || spec.filaments.empty() || spec.filaments.size() > 254 ||
        spec.plan.requests.empty() || spec.plan.requests.size() > 200000 || spec.plan.reordered ||
        spec.plan.filament_slots.size() != spec.filaments.size())
        throw std::invalid_argument("Extended AMS Lite needs a current, verified four-slot plan.");
    for (const auto& f : spec.filaments)
        if (f.material != "PLA") throw std::invalid_argument("Extended AMS Lite printing currently supports PLA colors only.");
    if (spec.magnet_pauses.size() > 1000) throw std::invalid_argument("Too many magnet insertion pauses.");
    for (std::size_t i = 0; i < spec.magnet_pauses.size(); ++i)
        if (!std::isfinite(spec.magnet_pauses[i]) || spec.magnet_pauses[i] <= 0. || (i && spec.magnet_pauses[i] <= spec.magnet_pauses[i-1]))
            throw std::invalid_argument("Invalid magnet insertion pause heights.");
    verify_batches(spec.slots, spec.plan);
    if (!spec.plan.schedule.stops.empty() && spec.plan.schedule.stops.front().before_request == 0)
        throw std::invalid_argument("Load the first required color before starting. Use Auto for the initial slots and prepare again.");
}

std::string serialize_execution(const ExecutionSpec& spec)
{
    verify_execution(spec);
    std::ostringstream out;
    out << "EAMS1 " << spec.job_id << ' ' << spec.filaments.size() << ' ' << spec.plan.requests.size() << ' ' << spec.plan.schedule.stops.size() << '\n';
    for (const auto& f : spec.filaments) out << std::quoted(f.name) << ' ' << std::quoted(f.material) << ' ' << std::quoted(f.color) << '\n';
    for (std::size_t s = 0; s < 4; ++s) out << spec.slots[s].filament << ' ' << spec.slots[s].locked << ' ' << spec.plan.schedule.initial_slots[s] << '\n';
    for (int slot : spec.plan.filament_slots) out << slot << ' ';
    out << '\n';
    for (const auto& r : spec.plan.requests) out << r.filament << ' ' << r.layer << ' ' << r.move << ' ' << r.gcode_line << '\n';
    for (const auto& stop : spec.plan.schedule.stops) {
        out << stop.before_request << ' ' << stop.replacements.size() << '\n';
        for (const auto& r : stop.replacements) out << r.slot << ' ' << r.remove << ' ' << r.insert << '\n';
    }
    if (!spec.magnet_pauses.empty()) {
        out << "MAGNETS " << spec.magnet_pauses.size() << std::setprecision(12);
        for (double z : spec.magnet_pauses) out << ' ' << z;
        out << '\n';
    }
    out << "END\n";
    return out.str();
}

ExecutionSpec deserialize_execution(const std::string& text)
{
    const auto invalid = [] { throw std::invalid_argument("The Extended AMS Lite plan is incomplete. Disable swaps, slice, and prepare again."); };
    if (text.size() > 10 * 1024 * 1024) invalid();
    std::istringstream in(text);
    ExecutionSpec spec;
    std::string tag;
    std::size_t filaments = 0, requests = 0, stops = 0;
    if (!(in >> tag >> spec.job_id >> filaments >> requests >> stops) || tag != "EAMS1" || filaments == 0 || filaments > 254 || requests == 0 || requests > 200000 || stops > requests) invalid();
    spec.filaments.resize(filaments);
    for (auto& f : spec.filaments) if (!(in >> std::quoted(f.name) >> std::quoted(f.material) >> std::quoted(f.color))) invalid();
    spec.slots.resize(4);
    spec.plan.schedule.initial_slots.resize(4);
    for (std::size_t s = 0; s < 4; ++s)
        if (!(in >> spec.slots[s].filament >> spec.slots[s].locked >> spec.plan.schedule.initial_slots[s])) invalid();
    spec.plan.filament_slots.resize(filaments);
    for (auto& slot : spec.plan.filament_slots) if (!(in >> slot)) invalid();
    spec.plan.requests.resize(requests);
    for (auto& r : spec.plan.requests) {
        if (!(in >> r.filament >> r.layer >> r.move >> r.gcode_line) || r.filament < 0 || std::size_t(r.filament) >= filaments) invalid();
        const int slot = spec.plan.filament_slots[r.filament];
        if (slot < 0 || slot > 3) invalid();
        spec.plan.schedule.request_slots.push_back(std::size_t(slot));
    }
    for (std::size_t i = 0; i < stops; ++i) {
        Stop stop;
        std::size_t count = 0;
        if (!(in >> stop.before_request >> count) || count == 0 || count > 4) invalid();
        stop.replacements.resize(count);
        for (auto& r : stop.replacements) if (!(in >> r.slot >> r.remove >> r.insert)) invalid();
        spec.plan.spool_replacements += count;
        spec.plan.schedule.stops.push_back(std::move(stop));
    }
    for (std::size_t i = 1; i < requests; ++i)
        if (spec.plan.requests[i - 1].filament != spec.plan.requests[i].filament) ++spec.plan.color_changes;
    if (!(in >> tag)) invalid();
    if (tag == "MAGNETS") {
        std::size_t count = 0;
        if (!(in >> count) || count > 1000) invalid();
        spec.magnet_pauses.resize(count);
        for (auto& z : spec.magnet_pauses) if (!(in >> z)) invalid();
        if (!(in >> tag)) invalid();
    }
    if (tag != "END" || (in >> tag)) invalid();
    verify_execution(spec);
    return spec;
}

namespace {
constexpr const char* execution_begin = "[[EXTENDED_AMS_LITE_V1]]";
constexpr const char* execution_end = "[[/EXTENDED_AMS_LITE]]";
}

std::optional<ExecutionSpec> execution_from_notes(const std::string& notes)
{
    const auto begin = notes.find(execution_begin);
    if (begin == std::string::npos) return std::nullopt;
    const auto data = begin + std::string(execution_begin).size();
    const auto end = notes.find(execution_end, data);
    if (end == std::string::npos || notes.find(execution_begin, data) != std::string::npos)
        throw std::invalid_argument("The Extended AMS Lite settings are incomplete. Disable swaps and prepare again.");
    return deserialize_execution(notes.substr(data, end - data));
}

std::string notes_with_execution(const std::string& notes, const std::optional<ExecutionSpec>& spec)
{
    std::string result = notes;
    for (auto position = result.find(execution_preparing_tag); position != std::string::npos; position = result.find(execution_preparing_tag))
        result.erase(position, std::string(execution_preparing_tag).size());
    const auto begin = result.find(execution_begin);
    if (begin != std::string::npos) {
        const auto end = result.find(execution_end, begin);
        result.erase(begin, end == std::string::npos ? std::string::npos : end + std::string(execution_end).size() - begin);
    }
    if (spec) result += std::string("\n") + execution_begin + "\n" + serialize_execution(*spec) + execution_end;
    return result;
}

std::string execution_job_prefix(const ExecutionSpec& spec) { return "EAMS-" + spec.job_id; }

std::string format_execution_guide(const ExecutionSpec& spec, bool html)
{
    auto display = [&](int id) {
        if (id < 0) return std::string("Not used at start");
        const auto& f = spec.filaments.at(std::size_t(id));
        return "V" + std::to_string(id + 1) + " " + color_name(f.color) + " [" + f.material + ", " + f.color + "]";
    };
    std::ostringstream out;
    out << "Extended AMS Lite - print swap guide\n";
    out << "A1 / PLA / four AMS Lite slots / LAN Developer Mode\n"
        << spec.plan.schedule.stops.size() << " visits, one pause per visit.\n\n"
        << "Load these first:\n";
    for (std::size_t s = 0; s < 4; ++s)
        out << "Slot " << s + 1 << ": " << display(spec.plan.schedule.initial_slots[s]) << '\n';
    out << "\nAt a swap pause, the active filament is already unloaded. Replace all spools listed for that visit. "
           "Feed each replacement to the AMS inlet. Keep other spools loaded. Press Resume only after every replacement is complete. "
           "Do not change the job's fixed slot mapping. An error or manual pause does not mean a swap visit.\n";
    for (double z : spec.magnet_pauses)
        out << "\nMagnet insertion at Z " << z << " mm: insert magnets; this is separate from the spool-swap visits. Keep the loaded spools.\n";
    for (std::size_t v = 0; v < spec.plan.schedule.stops.size(); ++v) {
        const auto& stop = spec.plan.schedule.stops[v];
        const auto& request = spec.plan.requests[stop.before_request];
        out << "\nVisit " << v + 1 << " of " << spec.plan.schedule.stops.size() << " - Layer " << request.layer << '\n';
        for (const auto& swap : stop.replacements)
            out << "Slot " << swap.slot + 1 << ": remove " << display(swap.remove)
                << " -> insert " << display(swap.insert) << '\n';
        out << "Next color: " << display(request.filament) << '\n';
    }
    if (!html) return out.str();
    return "<!doctype html><html><meta charset=utf-8><title>AMS Lite swap guide</title>"
           "<style>body{font:18px system-ui;max-width:900px;margin:32px auto;padding:16px}pre{white-space:pre-wrap;line-height:1.6}</style>"
           "<h1>Extended AMS Lite</h1><pre>" + escape_html(out.str()) + "</pre></html>";
}

ExecutionSession::ExecutionSession(ExecutionSpec spec) : m_spec(std::move(spec)), m_loaded(m_spec.plan.schedule.initial_slots)
{ verify_execution(m_spec); }

ExecutionStep ExecutionSession::begin(int filament)
{
    if (m_started || m_spec.plan.requests.front().filament != filament)
        throw std::runtime_error("The initial color changed. Disable swaps, slice, and prepare the AMS Lite plan again.");
    m_started = true;
    return {0, filament, m_spec.plan.requests.front().layer, std::nullopt};
}

std::optional<ExecutionStep> ExecutionSession::change(int filament, unsigned int layer)
{
    if (!m_started) throw std::runtime_error("Extended AMS Lite was not initialized before a tool change.");
    if (m_spec.plan.requests[m_cursor].filament == filament) return std::nullopt;
    std::size_t next = m_cursor + 1;
    while (next < m_spec.plan.requests.size() && m_spec.plan.requests[next].filament == m_spec.plan.requests[m_cursor].filament) ++next;
    if (next == m_spec.plan.requests.size() || m_spec.plan.requests[next].filament != filament || m_spec.plan.requests[next].layer != layer)
        throw std::runtime_error("The sliced color order or layer positions changed. Disable swaps, slice, and prepare the AMS Lite plan again.");
    ExecutionStep step{next, filament, layer, std::nullopt};
    const auto& stops = m_spec.plan.schedule.stops;
    if (m_next_visit < stops.size() && stops[m_next_visit].before_request == next) {
        step.visit = m_next_visit;
        for (const auto& r : stops[m_next_visit].replacements) {
            if (m_loaded[r.slot] != r.remove) throw std::runtime_error("The AMS Lite loading state does not match its plan.");
            m_loaded[r.slot] = r.insert;
        }
        ++m_next_visit;
    }
    const int slot = m_spec.plan.filament_slots[filament];
    if (m_loaded[slot] != filament) throw std::runtime_error("The next AMS Lite color is unavailable. Prepare the plan again.");
    m_cursor = next;
    return step;
}

void ExecutionSession::finish() const
{
    if (!m_started || m_next_visit != m_spec.plan.schedule.stops.size())
        throw std::runtime_error("The sliced job did not execute every planned AMS Lite visit.");
    for (std::size_t i = m_cursor + 1; i < m_spec.plan.requests.size(); ++i)
        if (m_spec.plan.requests[i].filament != m_spec.plan.requests[m_cursor].filament)
            throw std::runtime_error("The sliced job ended before its planned AMS Lite colors. Prepare again.");
}

std::string execution_selection_marker(const ExecutionStep& step)
{
    return "; EAMS_SELECT " + std::to_string(step.request) + " " + std::to_string(step.filament) + " " + std::to_string(step.layer) + "\n";
}

std::string execution_gcode_error(std::istream& input, const ExecutionSpec& spec)
{
    try { verify_execution(spec); } catch (const std::exception& e) { return e.what(); }
    std::vector<std::size_t> events{0};
    for (std::size_t i = 1; i < spec.plan.requests.size(); ++i)
        if (spec.plan.requests[i].filament != spec.plan.requests[i - 1].filament) events.push_back(i);
    auto loaded = spec.plan.schedule.initial_slots;
    std::size_t selects = 0, visits = 0, pauses = 0, magnets = 0;
    int pending = -1, current = -1;
    bool header = false, end = false, visit_open = false, scope = false, unloaded = false, closed = false;
    bool magnet_open = false, magnet_paused = false, absolute_extrusion_risk = false;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string magnet_begin = "; MAGNET_INSERT_BEGIN ";
        if (line.rfind(magnet_begin, 0) == 0) {
            double z = 0.; std::istringstream row(line.substr(magnet_begin.size()));
            if (!(row >> z) || magnets >= spec.magnet_pauses.size() || std::abs(z - spec.magnet_pauses[magnets]) > 0.001 ||
                magnet_open || visit_open || pending >= 0 || unloaded || !header)
                return "An unplanned magnet insertion pause was found. Prepare the print again.";
            magnet_open = true; magnet_paused = false;
        } else if (line == "; MAGNET_INSERT_END") {
            if (!magnet_open || !magnet_paused) return "The magnet insertion pause is incomplete.";
            magnet_open = false; ++magnets;
        }
        if (line.rfind("; EAMS_JOB ", 0) == 0) {
            if (header || line.substr(11) != spec.job_id) return "The slice belongs to a different AMS Lite job.";
            header = true;
        } else if (line.rfind("; EAMS_END ", 0) == 0) {
            if (end || line.substr(11) != spec.job_id) return "The AMS Lite slice has an invalid end marker.";
            end = true;
        } else if (line.rfind("; EAMS_VISIT ", 0) == 0) {
            std::istringstream row(line.substr(13));
            std::size_t visit = 0, request = 0;
            if (!(row >> visit >> request) || visit != visits || visit >= spec.plan.schedule.stops.size() ||
                request != spec.plan.schedule.stops[visit].before_request || visit_open || pending >= 0)
                return "An AMS Lite visit does not match its schedule.";
            visit_open = true; scope = false; closed = false;
            ++visits;
        } else if (line.rfind("; EAMS_SELECT ", 0) == 0) {
            std::istringstream row(line.substr(14));
            std::size_t index = 0; int id = -1; unsigned int layer = 0;
            if (!(row >> index >> id >> layer) || !header || selects >= events.size() || index != events[selects] ||
                spec.plan.requests[index].filament != id || spec.plan.requests[index].layer != layer || pending >= 0 || visit_open)
                return "The sliced filament selections do not match the AMS Lite plan.";
            pending = id; ++selects;
        }
        std::istringstream words(line.substr(0, line.find(';')));
        std::string command, parameter;
        words >> command >> parameter;
        if (visit_open && command == "M620" && parameter == "S255") scope = true;
        if (command == "T255") {
            if (visit_open && !scope) return "An AMS Lite unload has no native command scope.";
            unloaded = true;
        }
        if (visit_open && command == "M621" && parameter == "S255" && unloaded) closed = true;
        if (command == "M400" && parameter == "U1") {
            if (magnet_open) {
                if (magnet_paused || unloaded || visit_open) return "Invalid magnet insertion pause.";
                magnet_paused = true; continue;
            }
            if (!visit_open || !unloaded || !closed) return "A swap pause is missing its completed active unload.";
            for (const auto& r : spec.plan.schedule.stops[pauses].replacements) {
                if (loaded[r.slot] != r.remove) return "A swap instruction has a stale loading state.";
                loaded[r.slot] = r.insert;
            }
            visit_open = false; ++pauses;
        }
        if (command.size() > 1 && command.front() == 'T' && std::isdigit(static_cast<unsigned char>(command[1]))) {
            int id = -1;
            try { id = std::stoi(command.substr(1)); } catch (...) { return "Unreadable tool selection in AMS Lite job."; }
            if (id >= 0 && id < 254) {
                if (pending >= 0) {
                    if (id != pending || loaded[spec.plan.filament_slots[id]] != id)
                        return "A tool selection would use an unavailable AMS Lite spool.";
                    current = id; pending = -1; unloaded = false;
                } else if (id != current) return "An unplanned tool selection was found in the AMS Lite job.";
            }
        }
        if (command == "M83") absolute_extrusion_risk = false;
        else if (visit_open && (command == "G90" || command == "G91" || command == "M82")) absolute_extrusion_risk = true;
        if (command == "M400" && parameter == "U1") absolute_extrusion_risk = true; // restored only by an explicit M83 after the pause
        if (command == "G0" || command == "G1" || command == "G2" || command == "G3") {
            std::istringstream axes(line.substr(0, line.find(';')));
            while (axes >> parameter) if (parameter.size() > 1 && parameter.front() == 'E') {
                double amount = 0.;
                try { amount = std::stod(parameter.substr(1)); }
                catch (...) { return "Unreadable extrusion in AMS Lite job."; }
                if (amount > 0. && unloaded) return "The job would extrude while the AMS filament is unloaded.";
                if (amount != 0. && absolute_extrusion_risk)
                    return "Relative extrusion (M83) is not restored after an AMS Lite swap pause.";
            }
        }
    }
    if (input.bad() || !header || !end || visit_open || magnet_open || magnets != spec.magnet_pauses.size() || pending >= 0 || selects != events.size() ||
        visits != spec.plan.schedule.stops.size() || pauses != visits)
        return "The AMS Lite slice is incomplete or its pause count changed. Prepare again.";
    return {};
}

}} // namespace Slic3r::ExtendedAMSLite
