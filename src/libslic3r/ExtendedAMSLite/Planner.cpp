#include "Planner.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
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
    validate_inputs(input, count, slots);
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
    for (std::size_t offset = 0; offset < mutable_colors.size(); offset += free_slots.size()) {
        const std::size_t end = std::min(offset + free_slots.size(), mutable_colors.size());
        std::vector<int> bank(mutable_colors.begin() + offset, mutable_colors.begin() + end);
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
        result.banks.push_back(std::move(bank));
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
            if (l == 0 && !first_order.empty()) {
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
    if (hi - lo < 28) return "Gray";
    if (r > g * 1.3 && g > b * 1.25 && hi < 175) return "Brown";
    if (r > 190 && g > 155 && b < 115) return "Yellow";
    if (r > 180 && g > 65 && g < 170 && b < 110) return "Orange";
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
    return "Planning only. Automatic printing is unavailable: safe mid-print A1 AMS Lite "
           "unload, spool replacement, reload, and resume have not been verified. "
           "No pauses or physical slot mappings have been written to G-code. Do not print this schedule as a four-slot job.";
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

}} // namespace Slic3r::ExtendedAMSLite
