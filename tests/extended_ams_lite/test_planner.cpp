#include "Planner.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <utility>

using namespace Slic3r::ExtendedAMSLite;
namespace {
int checks = 0;
void check(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects(F function)
{
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    check(rejected, "Expected actionable rejection");
}

std::vector<Request> trace(const std::vector<int>& ids, unsigned int layer = 1)
{
    std::vector<Request> requests;
    for (std::size_t i = 0; i < ids.size(); ++i)
        requests.push_back({ids[i], layer, i, static_cast<unsigned int>(i * 10 + 1)});
    return requests;
}

// Exhaustive reference solver explores every legal victim. It shares no
// replacement policy or next-use computation with the production planner.
int optimum(const std::vector<Request>& requests, const std::vector<Slot>& slots, const std::vector<int>& initial)
{
    using Key = std::pair<std::size_t, std::vector<int>>;
    std::map<Key, int> memo;
    std::function<int(std::size_t, std::vector<int>)> solve = [&](std::size_t i, std::vector<int> loaded) {
        if (i == requests.size()) return 0;
        const Key key{i, loaded};
        auto cached = memo.find(key);
        if (cached != memo.end()) return cached->second;
        const int required = requests[i].filament;
        int cost = 100000;
        if (std::find(loaded.begin(), loaded.end(), required) != loaded.end()) {
            cost = solve(i + 1, loaded);
        } else {
            for (std::size_t s = 0; s < loaded.size(); ++s) {
                if (slots[s].locked) continue;
                auto candidate = loaded;
                candidate[s] = required;
                cost = std::min(cost, 1 + solve(i + 1, candidate));
            }
        }
        memo.emplace(key, cost);
        return cost;
    };
    return solve(0, initial);
}

void scenarios()
{
    const std::vector<Slot> four(4);
    auto requests = trace({0, 1, 2, 3, 0, 2, 1, 3});
    check(plan(requests, 4, four).stops.empty(), "Four colors must need no swaps");
    requests = trace({0, 1, 2, 3, 4, 5, 6, 7});
    auto result = plan(requests, 8, four);
    check(result.initial_slots == std::vector<int>({0, 1, 2, 3}), "Initial loading in first-use order");
    check(result.stops.size() == 4, "Eight distinct colors need four replacements");
    check(result.stops.front().before_request == 4, "Swap before first missing filament");
    check(result.stops.front().replacements.front().slot == 0, "Stable lowest-slot tie break");
    check(result.request_slots == plan(requests, 8, four).request_slots, "Deterministic assignments");

    requests = trace({0, 1, 2, 3, 4, 0, 5, 1, 6, 0, 7, 1, 8, 9, 10, 11});
    result = plan(requests, 12, four);
    verify(requests, 12, four, result);
    check(!result.stops.empty(), "More than eight colors supported");
    check(result.stops.front().replacements.front().remove == 2, "Keep soon-needed filaments");
    check(result.stops[0].before_request < result.stops[1].before_request, "Multiple swaps within one layer retain order");
    check(requests[result.stops[0].before_request].layer == requests[result.stops[1].before_request].layer, "Same layer is not collapsed");

    check(plan(trace({7, 7, 7, 7}), 8, four).stops.empty(), "Repeated virtual filament does not swap");
    check(plan(trace({0, 1, 0, 1, 0, 1, 0, 1}), 8, four).stops.empty(), "Frequent alternating resident colors do not swap");
    auto locked = four;
    locked[0] = {0, true};
    result = plan(requests, 12, locked);
    for (const auto& stop : result.stops)
        check(stop.replacements.front().slot != 0, "Locked filament never evicted");
    locked = {{7, false}, {2, true}, {-1, false}, {-1, false}};
    result = plan(requests, 12, locked);
    check(result.initial_slots[0] == 7 && result.initial_slots[1] == 2, "Initial user overrides preserved");
    verify(requests, 12, locked, result);

    // Batched instructions can be verified atomically even though the demand
    // optimizer currently emits one replacement per missing color.
    requests = trace({0, 2, 3});
    Plan batched{{0, 1}, {0, 0, 1}, {{1, {{0, 0, 2}, {1, 1, 3}}}}};
    verify(requests, 4, std::vector<Slot>(2), batched);
    check(batched.stops[0].replacements.size() == 2, "Multiple changes at one stop replay correctly");

    const std::vector<Filament> metadata = {{"Blue", "PLA", "#0000FF"}, {"Blue", "PETG", "#0000FF"}, {"Green", "PLA", "#00FF00"}, {"White", "PLA", "#FFFFFF"}};
    const auto report = format_schedule(metadata, requests, batched);
    check(report.find("PETG") != std::string::npos && report.find("#00FF00") != std::string::npos, "Materials and colors retain identity in export");
    check(report.find("DRY RUN") != std::string::npos && report.find("No pauses") != std::string::npos, "Report cannot be mistaken for executable instructions");
    check(std::string(execution_blocker()).find("resume") != std::string::npos, "Unverified hardware execution is gated");
    auto hostile = metadata;
    hostile[0].name = "<script>alert('name')</script>";
    hostile[0].color = "red; background:url(https://example.com)";
    const auto html = format_schedule_html(hostile, requests, batched);
    check(html.find("<script>") == std::string::npos && html.find("&lt;script&gt;") != std::string::npos, "HTML export escapes profile names");
    check(html.find("style=\"background:#808080\"") != std::string::npos, "Invalid CSS colors fall back to gray");
    check(html.find("style=\"background:red;") == std::string::npos, "Untrusted color cannot inject CSS");
}

void invalid_inputs_and_corruption()
{
    const auto requests = trace({0, 1, 2, 3, 4, 5, 6, 7});
    const std::vector<Slot> slots(4);
    rejects([&] { plan(requests, 8, {}); });
    rejects([&] { plan(requests, 8, std::vector<Slot>(33)); });
    rejects([&] { plan({}, 8, slots); });
    rejects([&] { plan(requests, 0, slots); });
    rejects([&] { plan(trace({254}), 255, slots); });
    rejects([&] { plan(trace({-1}), 8, slots); });
    rejects([&] { plan(trace({8}), 8, slots); });
    rejects([&] { plan(requests, 8, {{8, false}}); });
    rejects([&] { plan(requests, 8, {{-2, false}}); });
    rejects([&] { plan(requests, 8, {{-1, true}}); });
    rejects([&] { plan(requests, 8, {{1, false}, {1, false}}); });
    rejects([&] { plan(requests, 8, {{0, true}, {1, true}, {2, true}, {3, true}}); });
    rejects([&] { auto bad = requests; bad.back().move = 0; plan(bad, 8, slots); });
    const auto valid = plan(requests, 8, slots);
    const auto bad_plan = [&](const std::function<void(Plan&)>& change) {
        auto bad = valid;
        change(bad);
        rejects([&] { verify(requests, 8, slots, bad); });
    };
    bad_plan([](Plan& p) { p.request_slots.pop_back(); });
    bad_plan([](Plan& p) { p.request_slots[0] = 5; });
    bad_plan([](Plan& p) { p.initial_slots[0] = 9; });
    bad_plan([](Plan& p) { p.initial_slots[0] = p.initial_slots[1]; });
    bad_plan([](Plan& p) { p.stops.clear(); });
    bad_plan([](Plan& p) { p.stops[0].before_request++; });
    bad_plan([](Plan& p) { p.stops[0].replacements[0].remove = 7; });
    bad_plan([](Plan& p) { p.stops[0].replacements[0].insert = 1; });
    bad_plan([](Plan& p) { p.stops[0].replacements[0].slot = 999; });
    bad_plan([](Plan& p) { p.stops.push_back(p.stops[0]); });
    bad_plan([](Plan& p) { p.stops[0].replacements.clear(); });
}

void trace_contract()
{
    enum class Type { Noop, Travel, Tool, Purge, Extrude, Pause };
    struct Move {
        Type type;
        unsigned char extruder_id;
        unsigned int layer_id;
        unsigned int gcode_id;
    };
    const std::vector<Move> moves = {{Type::Noop, 0, 0, 0}, {Type::Tool, 7, 0, 10},
        {Type::Purge, 7, 0, 20}, {Type::Extrude, 7, 1, 30}, {Type::Travel, 7, 1, 40},
        {Type::Tool, 2, 1, 50}, {Type::Extrude, 2, 1, 60}, {Type::Pause, 2, 1, 70},
        {Type::Tool, 7, 1, 80}, {Type::Extrude, 7, 1, 90}, {Type::Extrude, 1, 2, 100}};
    const auto requests = collect_requests(moves, Type::Tool, Type::Extrude);
    check(requests.size() == 4, "Trace retains actual filament selections and implicit extrusion");
    check(requests[0].filament == 7 && requests[1].filament == 2 && requests[2].filament == 7, "Trace is not palette order");
    check(requests[0].layer == 0 && requests[0].gcode_line == 10, "Startup selection preserved before purge");
    check(requests[2].layer == 1 && requests[2].move == 8, "Within-layer selection position preserved");
    check(requests[3].filament == 1 && requests[3].layer == 2, "Extrusion-only initial tool can be discovered");
    auto copy = moves;
    const auto result = plan(requests, 8, std::vector<Slot>(2));
    (void)result;
    check(copy[5].gcode_id == moves[5].gcode_id && copy[5].extruder_id == moves[5].extruder_id, "Planning leaves source moves intact");
}

void exhaustive_and_random()
{
    // Every six-request trace over four colors, with 1..3 slots and one
    // optional locked initial color. Compare with the independent solver.
    for (int encoded = 0; encoded < 4096; ++encoded) {
        int value = encoded;
        std::vector<int> ids;
        for (int i = 0; i < 6; ++i) { ids.push_back(value % 4); value /= 4; }
        const auto requests = trace(ids);
        for (int capacity = 1; capacity <= 3; ++capacity) {
            for (int with_lock = 0; with_lock <= (capacity > 1 ? 1 : 0); ++with_lock) {
                std::vector<Slot> slots(static_cast<std::size_t>(capacity));
                if (with_lock) slots[0] = {3, true};
                const auto result = plan(requests, 4, slots);
                check(static_cast<int>(result.stops.size()) == optimum(requests, slots, result.initial_slots), "Belady result differs from exhaustive optimum");
            }
        }
    }
    std::mt19937 generator(20261028);
    for (int trial = 0; trial < 1000; ++trial) {
        std::vector<int> ids;
        for (int i = 0; i < 100; ++i) ids.push_back(static_cast<int>(generator() % 12));
        auto slots = std::vector<Slot>(4);
        if (trial % 2) slots[0] = {0, true};
        auto requests = trace(ids, 42);
        auto result = plan(requests, 12, slots);
        verify(requests, 12, slots, result);
        check(result.request_slots == plan(requests, 12, slots).request_slots, "Random traces must remain deterministic");
    }
}
}

int main()
{
    try {
        scenarios();
        invalid_inputs_and_corruption();
        trace_contract();
        exhaustive_and_random();
        std::cout << "Passed " << checks << " checks, including 20,480 exhaustive optimum comparisons and 1,000 randomized replays.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
