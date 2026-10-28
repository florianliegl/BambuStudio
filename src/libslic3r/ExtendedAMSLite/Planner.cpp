#include "Planner.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
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
    return "Simulation only. Automatic printing is unavailable: A1 AMS Lite spool replacement, "
           "mapping changes during a paused print, safe unload/reload, and resume have not been verified. "
           "No pauses or physical slot mappings have been written to G-code. Do not print this schedule as a four-slot job.";
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
