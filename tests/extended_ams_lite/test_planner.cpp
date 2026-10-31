#include "Planner.hpp"
#include "Connect.hpp"
#include "Probe.hpp"
#include "Execution.hpp"
#include "PauseTimeline.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
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

void execution_cases()
{
    std::vector<Request> requests;
    for (unsigned layer = 1; layer <= 2; ++layer)
        for (int i = 0; i < 8; ++i)
            requests.push_back({layer == 1 ? i : 7 - i, layer, requests.size(), 0});
    ExecutionSpec spec;
    spec.job_id = "0123456789abcdef";
    spec.slots.resize(4);
    for (int i = 0; i < 8; ++i) spec.filaments.push_back({"PLA \"color\" " + std::to_string(i), "PLA", "#2463EB"});
    spec.plan = plan_batches(requests, 8, spec.slots);
    check(format_execution_guide(spec).find("Slot 1: V1 Blue [PLA, #2463EB]") != std::string::npos,
        "Execution guides show a readable color name instead of the generic filament profile name");
    check(spec.plan.schedule.stops.size() == 2, "Carried banks need one visit per full eight-color layer");
    const auto text = serialize_execution(spec);
    check(serialize_execution(deserialize_execution(text)) == text, "Execution settings round trip exactly");
    const auto notes = notes_with_execution("User notes", spec);
    check(execution_from_notes(notes)->job_id == spec.job_id, "Project notes retain the execution plan");
    check(!execution_from_notes(notes_with_execution(notes, std::nullopt)), "Disabling removes the execution plan");
    rejects([] { execution_from_notes("[[EXTENDED_AMS_LITE_V1]]broken"); });
    auto invalid = spec;
    invalid.filaments[4].material = "PETG";
    rejects([&] { verify_execution(invalid); });
    invalid = spec;
    invalid.plan.filament_slots[4] = 1;
    rejects([&] { verify_execution(invalid); });
    ExecutionSession session(spec);
    std::string gcode = "; EAMS_JOB " + spec.job_id + "\n" + execution_selection_marker(session.begin(0)) + "T0\nG1 E2\n";
    int previous = 0;
    for (const auto& r : requests) {
        const auto step = session.change(r.filament, r.layer);
        if (!step) continue;
        if (step->visit)
            gcode += "; EAMS_VISIT " + std::to_string(*step->visit) + " " + std::to_string(step->request) +
                "\nG90\nM83\nM620 S255\nT255\nM621 S255\n; PAUSE_PRINTING\nM400 U1\nM83\n";
        gcode += execution_selection_marker(*step) + "T" + std::to_string(r.filament) + "\nG1 E2\n";
        previous = r.filament;
    }
    (void)previous;
    session.finish();
    gcode += "T255\n; EAMS_END " + spec.job_id + "\n";
    auto error = [&](const std::string& value) { std::istringstream input(value); return execution_gcode_error(input, spec); };
    check(error(gcode).empty(), "Replay verifies the complete scoped unload/pause/select stream");
    std::istringstream source(gcode);
    auto timeline = read_pause_timeline(source, spec, {{1, 300}, {2, 120}});
    check(timeline.events.size() == 2 && timeline.events[1].visit == 1, "Overview preserves swap visit identity and actual order");
    const auto timeline_text = serialize_pause_timeline(timeline, spec);
    check(serialize_pause_timeline(deserialize_pause_timeline(timeline_text, spec), spec) == timeline_text, "Pause overview round trips");
    PauseObservation state{"printer-a", execution_job_prefix(spec) + "_rsubmission-1_plate_1", "RUNNING", true, true, 1, 0, 360};
    PauseProgress progress{"submission-1", 0, false};
    check(matches_pause_job(spec, "printer-a", state), "Overview matches exact job and target printer");
    check(!matches_pause_job(spec, "printer-b", state), "Different printer never uses a saved guide");
    check(pause_job_id("other-" + spec.job_id).empty() && pause_job_id(execution_job_prefix(spec) + "f").empty(), "Job prefixes reject partial matches");
    check(estimated_seconds_to_pause(timeline.events[0], state) == 60, "ETA uses actual pause remaining time");
    check(!observe_pause_progress(progress, spec, "printer-a", state) && progress.completed == 0, "RUNNING never advances a visit");
    check(!complete_pause(progress, timeline, spec, "printer-a", state), "Cannot mark a running print's pause complete");
    state.status = "PAUSE";
    check(estimated_seconds_to_pause(timeline.events[0], state) == -1, "No countdown while paused");
    state.error = 123;
    check(!complete_pause(progress, timeline, spec, "printer-a", state), "Printer errors cannot complete a planned stop");
    state.error = 0; state.layer = 2;
    check(!complete_pause(progress, timeline, spec, "printer-a", state), "Wrong layer requires explicit progress adjustment");
    state.layer = 1; state.connected = false;
    check(!complete_pause(progress, timeline, spec, "printer-a", state), "Disconnected status cannot acknowledge a pause");
    state.connected = true; state.ready = false;
    check(!complete_pause(progress, timeline, spec, "printer-a", state), "Stale not-ready state cannot acknowledge a pause");
    state.ready = true;
    // The A1 reports 03008013 for every pause command in the file, so a planned stop always carries it.
    state.error = planned_pause_error_code;
    check(!is_printer_fault(state.error) && is_printer_fault(123) && !is_printer_fault(0), "Only the pause-command code is treated as a planned pause");
    state.status = "RUNNING";
    check(pause_completion_blocker(progress, timeline, spec, "printer-a", state).find("not paused yet") != std::string::npos, "Running print explains why confirm is unavailable");
    state.status = "PAUSE"; state.layer = 2;
    check(pause_completion_blocker(progress, timeline, spec, "printer-a", state).find("layer 2") != std::string::npos, "Layer mismatch is explained with both layers");
    state.layer = 1; state.error = 123;
    check(pause_completion_blocker(progress, timeline, spec, "printer-a", state).find("fault") != std::string::npos, "Real printer faults are explained");
    state.error = planned_pause_error_code;
    check(pause_completion_blocker(progress, timeline, spec, "printer-a", state).empty(), "A planned pause with the A1 pause code can be confirmed");
    check(complete_pause(progress, timeline, spec, "printer-a", state) && progress.completed == 1 && progress.awaiting_resume, "Explicit confirmation advances exactly one pause");
    check(!complete_pause(progress, timeline, spec, "printer-a", state), "Repeated clicks at the same pause cannot advance twice");
    const auto checkpoint = serialize_pause_progress(progress, timeline.events.size());
    const auto restored = deserialize_pause_progress(checkpoint, "submission-1", timeline.events.size());
    check(restored.completed == 1 && restored.awaiting_resume, "Restart preserves the acknowledgement latch");
    rejects([&] { deserialize_pause_progress(checkpoint, "submission-2", timeline.events.size()); });
    rejects([&] { deserialize_pause_progress(checkpoint, "submission-1", 0); });
    state.status = "RUNNING"; state.job_name = "Other print";
    check(!observe_pause_progress(progress, spec, "printer-a", state) && progress.awaiting_resume, "Wrong job cannot clear the pause latch");
    state.job_name = execution_job_prefix(spec) + "_rsubmission-2";
    check(!observe_pause_progress(progress, spec, "printer-a", state) && progress.awaiting_resume, "Another run of the same project cannot clear the pause latch");
    state.job_name = execution_job_prefix(spec) + "_rsubmission-1";
    check(observe_pause_progress(progress, spec, "printer-a", state) && !progress.awaiting_resume && progress.completed == 1, "Resume clears only the latch, not the next visit");
    state.layer = 2; state.status = "PAUSE";
    check(complete_pause(progress, timeline, spec, "printer-a", state), "Second planned pause can be acknowledged independently");
    state.status = "RUNNING"; observe_pause_progress(progress, spec, "printer-a", state); state.status = "PAUSE";
    check(!complete_pause(progress, timeline, spec, "printer-a", state), "No acknowledgement beyond end of schedule");
    rejects([&] { deserialize_pause_timeline(timeline_text + "junk", spec); });
    auto bad_timeline = timeline; bad_timeline.events[0].visit = 1;
    rejects([&] { serialize_pause_timeline(bad_timeline, spec); });
    std::istringstream extra_pause(gcode + "M400 U1\n");
    rejects([&] { read_pause_timeline(extra_pause, spec); });
    std::istringstream missing_timing(gcode);
    rejects([&] { read_pause_timeline(missing_timing, spec, {{1, 1}}); });
    std::istringstream wrong_layer(gcode);
    rejects([&] { read_pause_timeline(wrong_layer, spec, {{2, 300}, {2, 120}}); });
    auto corrupt = gcode;
    corrupt.erase(corrupt.find("M400 U1\n"), 8);
    check(!error(corrupt).empty(), "A missing swap pause blocks submission");
    corrupt = gcode;
    corrupt.insert(corrupt.find("M400 U1\n"), "G1 E2\n");
    check(!error(corrupt).empty(), "Extrusion while unloaded blocks submission");
    corrupt = gcode;
    corrupt.erase(corrupt.find("M400 U1\nM83\n") + 8, 4);
    check(!error(corrupt).empty(), "Extrusion after a swap pause without restored relative mode blocks submission");
    corrupt = gcode;
    corrupt.erase(corrupt.find("G90\nM83\n") + 4, 4);
    corrupt.erase(corrupt.find("M400 U1\nM83\n") + 8, 4);
    check(!error(corrupt).empty(), "A swap that never restores relative extrusion blocks submission");
    check(!error("; process_notes = " + text).empty(), "Plan text in a config header cannot stand in for actual execution");
    ExecutionSession wrong(spec);
    wrong.begin(0);
    rejects([&] { wrong.change(2, 1); });
    rejects([&] { wrong.change(1, 2); });
    rejects([&] { wrong.finish(); });
    auto fixed = plan_fixed_batches(requests, spec.slots, spec.plan.filament_slots);
    check(fixed.schedule.request_slots == spec.plan.schedule.request_slots, "Final native order retains the chosen physical assignments");
    spec.magnet_pauses = {2.9};
    check(deserialize_execution(serialize_execution(spec)).magnet_pauses == spec.magnet_pauses, "Magnet insertion pauses persist independently of spool visits");
    auto with_magnet = gcode;
    with_magnet.insert(with_magnet.find("G1 E2\n") + 6, "; MAGNET_INSERT_BEGIN 2.900000\n; PAUSE_PRINTING\nM400 U1\n; MAGNET_INSERT_END\n");
    check(error(with_magnet).empty(), "A planned magnet insertion keeps the current spool loaded");
    std::istringstream magnet_source(with_magnet);
    const auto with_magnets = read_pause_timeline(magnet_source, spec, {{1, 400}, {1, 300}, {2, 120}});
    check(with_magnets.events.size() == 3 && with_magnets.events[0].kind == PauseKind::Magnets &&
          with_magnets.events[1].visit == 0, "Magnet pauses retain their position without shifting swap visit identity");
    check(deserialize_pause_timeline(serialize_pause_timeline(with_magnets, spec), spec).events[0].layer == 1,
          "Magnet layer derives from actual generated processor output");
    check(!legacy_pause_timeline(spec).complete && legacy_pause_timeline(spec).events.size() == 2,
          "Legacy guides never invent magnet layers or include magnets in spool visit numbering");
    auto same_layer_spec = spec;
    for (auto& r : same_layer_spec.plan.requests) r.layer = 1;
    // Only event grouping changes here; execution mapping and chronological requests are unchanged.
    auto same_layer_timeline = with_magnets; same_layer_timeline.events[2].layer = 1;
    verify_pause_timeline(same_layer_timeline, same_layer_spec);
    PauseProgress same_layer_progress{"submission-1", 1, false};
    state.layer = 1;
    check(complete_pause(same_layer_progress, same_layer_timeline, same_layer_spec, "printer-a", state), "Multiple visits in one layer remain individually confirmable");
    check(!complete_pause(same_layer_progress, same_layer_timeline, same_layer_spec, "printer-a", state), "Same-layer visits still require a resume between confirmations");
    check(!error(gcode).empty(), "Removing a planned magnet insertion blocks submission");
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

PurgeModel random_purge(unsigned seed, std::size_t count, double flow = 5.)
{
    std::mt19937 random(seed);
    PurgeModel model;
    model.count = count; model.flow = flow;
    model.volume.assign(count * count, 0.);
    for (std::size_t from = 0; from < count; ++from)
        for (std::size_t to = 0; to < count; ++to)
            if (from != to) model.volume[from * count + to] = 100. + double(random() % 500);
    return model;
}

void purge_optimization()
{
    const std::vector<Slot> slots(4);
    std::vector<Request> input;
    for (unsigned layer = 1; layer <= 4; ++layer)
        for (int id = 0; id < 8; ++id) input.push_back({id, layer, input.size(), unsigned(input.size() + 1)});
    // A color-aware table: leaving black (0) for white (7) is very expensive, the reverse is cheap.
    PurgeModel table = random_purge(1, 8);
    table.volume[0 * 8 + 7] = 600.; table.volume[7 * 8 + 0] = 100.;
    for (unsigned seed = 1; seed <= 12; ++seed) {
        const auto purge = random_purge(seed, 8);
        const auto result = optimize_batches(input, 8, slots, {}, purge);
        check(result.baseline.schedule.stops.size() == 4, "Baseline for eight colors over four layers is four visits");
        check(result.score.visits <= result.baseline_score.visits, "Optimization never adds visits");
        check(result.score.purge_mm3 <= result.baseline_score.purge_mm3 + 1e-9, "Optimization never adds purge at equal visits");
        verify_batches(slots, result.plan);
        check(std::abs(result.score.purge_mm3 - purge_volume(result.plan.requests, purge)) < 1e-9, "Score matches the plan's own purge");
        if (result.faster) {
            check(result.faster->schedule.stops.size() == result.plan.schedule.stops.size() + 1, "A suggestion has exactly one more visit");
            check(result.score.purge_seconds - result.faster_score.purge_seconds >= 600., "A suggestion must save at least ten minutes");
            verify_batches(slots, *result.faster);
        }
        // The user's first-layer order wins over any purge saving.
        const std::vector<int> order{3, 1, 0, 2, 7, 6, 5, 4};
        const auto pinned = optimize_batches(input, 8, slots, order, purge);
        check(pinned.plan.layers.front().filaments == order, "Manual first-layer order is never reordered for purge");
        const auto partial = optimize_batches(input, 8, slots, {7, 0}, purge);
        check(partial.plan.layers.front().filaments[0] == 7 && partial.plan.layers.front().filaments[1] == 0, "A manual prefix keeps its first colors");
        check(pinned.score.visits <= pinned.baseline_score.visits, "Pinned plans do not add visits either");
        verify_batches(slots, pinned.plan);
    }
    const auto tabled = optimize_batches(input, 8, slots, {}, table);
    check(tabled.score.purge_mm3 <= tabled.baseline_score.purge_mm3, "A color-aware table is never worse than the standard order");
    // Without a usable table the result is exactly the standard plan.
    const auto none = optimize_batches(input, 8, slots, {}, PurgeModel{});
    check(none.plan.schedule.stops.size() == none.baseline.schedule.stops.size() && !none.faster, "No purge table keeps the standard plan");
    // Locks and fixed slots survive optimization.
    const auto locked = optimize_batches(input, 8, {{0, true}, {7, false}, {-1, false}, {-1, false}}, {}, random_purge(5, 8));
    for (const auto& stop : locked.plan.schedule.stops)
        for (const auto& change : stop.replacements) check(change.slot != 0, "Optimized plan never replaces a locked spool");
    // A small suggestion threshold can only ever add exactly one visit.
    for (unsigned seed = 20; seed < 28; ++seed) {
        const auto eager = optimize_batches(input, 8, slots, {}, random_purge(seed, 8, 1.), 1.);
        if (eager.faster) check(eager.faster->schedule.stops.size() == eager.plan.schedule.stops.size() + 1, "Eager suggestions stay one visit away");
    }
    // Hand-supplied layer orders must cover exactly each layer's colors.
    const std::vector<std::vector<int>> wrong{{0}};
    rejects([&] { plan_batches_custom(input, 8, slots, {}, true, nullptr, &wrong); });
    rejects([&] { plan_batches_custom(input, 8, slots, {}, false, nullptr, &wrong); });
}

void batch_scenarios()
{
    const std::vector<Slot> slots(4);
    std::vector<Request> input;
    for (unsigned layer = 1; layer <= 4; ++layer)
        for (int id = 0; id < 8; ++id) input.push_back({id, layer, input.size(), unsigned(input.size() + 1)});
    const auto current = plan_batches(input, 8, slots);
    const auto optimized = plan_batches(input, 8, slots, {}, true);
    check(current.schedule.stops.size() == 7, "Repeated banks need seven visits without reordering");
    check(optimized.schedule.stops.size() == 4, "Eight colors over four layers need only four visits with carry-over");
    check(optimized.spool_replacements == 16, "Four complete bank changes replace sixteen spools");
    check(optimized.color_changes == 28, "Keep boundary color to attain seven changes per eight-color layer");
    for (const auto& stop : optimized.schedule.stops)
        check(stop.replacements.size() == 4, "All four replacements belong to one visit");
    for (std::size_t layer = 1; layer < optimized.layers.size(); ++layer)
        check(optimized.layers[layer - 1].filaments.back() == optimized.layers[layer].filaments.front(), "Next layer starts with the loaded color");
    check(input[0].gcode_line == 1 && input.back().layer == 4, "Input positions remain untouched");
    check(optimized.reordered && optimized.requests.front().gcode_line == 0, "Proposed positions are never presented as actual G-code positions");
    auto manual = plan_batches(input, 8, slots, {3, 1, 0, 2, 7, 6, 5, 4}, true);
    check(manual.layers.front().filaments == std::vector<int>({3, 1, 0, 2, 7, 6, 5, 4}), "Manual first-layer order is exact");
    check(manual.schedule.initial_slots == std::vector<int>({3, 1, 0, 2}), "Initial load follows the manual first batch");
    const auto locked = plan_batches(input, 8, {{0, true}, {7, false}, {-1, false}, {-1, false}}, {}, true);
    check(locked.schedule.initial_slots[0] == 0 && locked.schedule.initial_slots[1] == 7, "Batch plan respects initial and locked slots");
    for (const auto& stop : locked.schedule.stops)
        for (const auto& change : stop.replacements) check(change.slot != 0, "Batch plan never replaces locked spool");
    check(plan_batches(trace({0, 1, 0}), 2, {{0, true}, {1, true}}).schedule.stops.empty(), "All-locked resident job succeeds");
    check(plan_batches(trace({0, 1, 2}), 3, slots, {}, true).schedule.stops.empty(), "Small palette needs no visits");

    // Representative four-layer snake pattern from the user's schedule. The
    // demand planner makes 16 visits; fixed-slot batches group these into four.
    input.clear();
    const std::vector<int> a{7, 1, 3, 6, 4, 5, 2, 0}, b{0, 2, 5, 4, 6, 3, 1, 7};
    for (unsigned layer = 1; layer <= 4; ++layer)
        for (int id : layer % 2 ? a : b) {
            if (!input.empty() && input.back().filament == id) continue;
            input.push_back({id, layer, input.size(), unsigned(input.size() * 10 + 1)});
        }
    check(input.size() == 29, "Shared layer-boundary colors are compressed in fixture");
    const auto legacy = plan(input, 8, slots);
    const auto grouped = plan_batches(input, 8, slots);
    check(legacy.stops.size() == 16 && grouped.schedule.stops.size() == 4, "Existing snake order groups sixteen visits into four");
    check(grouped.spool_replacements == 16, "Grouping reduces visits while preserving actual spool work");
    const std::vector<Filament> metadata(8, {"<profile>", "PLA", "#FFFFFF"});
    const auto report = format_batch_schedule(metadata, optimized);
    const auto html = format_batch_schedule_html(metadata, optimized);
    check(report.find("Visits: 4") != std::string::npos && report.find("PROPOSED") != std::string::npos, "Batch report identifies proposed visits");
    check(report.find("G-code line ID") == std::string::npos, "Proposed report omits fictitious G-code positions");
    check(html.find("&lt;profile&gt;") != std::string::npos && html.find("<profile>") == std::string::npos, "Batch HTML escapes metadata");
    check(color_name("#FFFFFF") == "White" && color_name("#2463EB") == "Blue" && color_name("") == "Unknown color", "Readable colors and explicit unknown metadata");
    check(color_name("#A1A3A5") == "Light gray" && color_name("#636567") == "Dark gray", "Initial loading distinguishes the user's two gray spools");
    check(color_name("#FEFEFE") == "White" && color_name("#F28C1D") == "Orange", "Initial loading names the user's white and orange colors");
    check(color_name("#E34B4B") == "Red" && color_name("#ED8736") == "Orange" && color_name("#FF6A13") == "Orange", "A strong red is not named orange");
    rejects([&] { plan_batches(input, 8, slots, {1, 1}, true); });
    rejects([&] { plan_batches(input, 8, slots, {8}, true); });
    rejects([&] { plan_batches(trace({0, 1}, 0), 8, slots, {}, true); });
    rejects([&] { auto bad = input; bad.back().layer = 1; plan_batches(bad, 8, slots, {}, true); });
    rejects([&] { plan_batches(input, 8, {{0, true}, {1, true}, {2, true}, {3, true}}); });
    rejects([&] { auto bad = optimized; bad.filament_slots[0] = 3; verify_batches(slots, bad); });
    rejects([&] { auto bad = optimized; ++bad.spool_replacements; verify_batches(slots, bad); });
}

// Independent dynamic program permits every legal simultaneous slot load at
// every position, including proactive changes. It does not use greedy windows.
int optimum_visits(const std::vector<Request>& input, const BatchPlan& plan)
{
    using Key = std::pair<std::size_t, std::vector<int>>;
    std::map<Key, int> memo;
    std::function<int(std::size_t, const std::vector<int>&)> solve;
    solve = [&](std::size_t i, const std::vector<int>& loaded) {
        if (i == input.size()) return 0;
        const Key key{i, loaded};
        const auto cached = memo.find(key);
        if (cached != memo.end()) return cached->second;
        const int required = input[i].filament;
        int best = 10000;
        if (loaded[plan.filament_slots[required]] == required) best = solve(i + 1, loaded);
        auto candidate = loaded;
        std::function<void(std::size_t)> enumerate = [&](std::size_t s) {
            if (s == loaded.size()) {
                if (candidate != loaded && candidate[plan.filament_slots[required]] == required)
                    best = std::min(best, 1 + solve(i + 1, candidate));
                return;
            }
            candidate[s] = loaded[s];
            enumerate(s + 1);
            for (std::size_t id = 0; id < plan.filament_slots.size(); ++id)
                if (plan.filament_slots[id] == int(s) && int(id) != loaded[s]) {
                    candidate[s] = int(id);
                    enumerate(s + 1);
                }
        };
        enumerate(0);
        memo.emplace(key, best);
        return best;
    };
    return solve(0, plan.schedule.initial_slots);
}

void batch_exhaustive_and_random()
{
    for (unsigned encoded = 0; encoded < 4096; ++encoded) {
        unsigned n = encoded;
        std::vector<int> ids;
        for (int i = 0; i < 6; ++i) { ids.push_back(n % 4); n /= 4; }
        const auto input = trace(ids);
        const auto p = plan_batches(input, 4, std::vector<Slot>(2));
        check(int(p.schedule.stops.size()) == optimum_visits(input, p), "Greedy batch visits attain exhaustive fixed-mapping optimum");
    }
    std::mt19937 random(143);
    for (int trial = 0; trial < 300; ++trial) {
        std::vector<Request> input;
        std::map<unsigned, std::set<int>> required;
        for (unsigned layer = 1; layer <= 10; ++layer) for (int i = 0; i < 10; ++i) {
            int id = int(random() % 12);
            input.push_back({id, layer, input.size(), unsigned(input.size() + 1)});
            required[layer].insert(id);
        }
        auto slots = std::vector<Slot>(4);
        if (trial % 2) slots[0] = {0, true};
        const auto p = plan_batches(input, 12, slots, {}, true);
        verify_batches(slots, p);
        std::vector<Request> resliced;
        for (const auto& layer : p.layers)
            for (int id : layer.filaments) resliced.push_back({id, layer.layer, resliced.size(), 0});
        const auto repeated = plan_batches(resliced, 12, slots, {}, true);
        for (std::size_t layer = 0; layer < p.layers.size(); ++layer)
            check(repeated.layers[layer].filaments == p.layers[layer].filaments, "Reapplying optimization retains the same layer order");
        check(p.layers.size() == required.size(), "Reordering retains every source layer");
        for (const auto& layer : p.layers)
            check(std::set<int>(layer.filaments.begin(), layer.filaments.end()) == required[layer.layer], "Reordering preserves each layer's required identities");
        for (const auto& layer : complete_layer_orders(p)) {
            check(layer.filaments.size() == 12, "Native sequence records have equal widths on sparse layers");
            check(std::equal(p.layers[layer.layer - 1].filaments.begin(), p.layers[layer.layer - 1].filaments.end(), layer.filaments.begin()), "Native padding retains each required prefix");
        }
        check(p.schedule.request_slots == plan_batches(input, 12, slots, {}, true).schedule.request_slots, "Batch output is deterministic");
    }
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
    const auto layered = collect_requests(moves, Type::Tool, Type::Extrude, true);
    check(layered.size() == 5 && layered[1].filament == 7 && layered[1].layer == 1, "Layer-aware trace retains extrusion after startup with the same color");
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
        execution_cases();
        const std::string probe = "; AMS_LITE_ACTIVE_UNLOAD_PROBE_BEGIN\nM620 S255\nT255\nM621 S255\nM400 U1\nT1\nG1 E1\n";
        auto probe_error = [](const std::string& text) { std::istringstream input(text); return active_unload_probe_error(input); };
        check(probe_error(probe).empty(), "Accept the expected unload-pause-load sequence");
        check(!probe_error("T1\nG1 E1\n").empty(), "Reject the actual failure: system macro has no probe pause");
        check(!probe_error("; change_filament_gcode = ; AMS_LITE_ACTIVE_UNLOAD_PROBE_BEGIN\\nT255\\nM621 S255\\nM400 U1\\nT1\n").empty(), "Escaped config text is not execution");
        auto bad_probe = probe;
        bad_probe.erase(bad_probe.find("T255\n"), 5);
        check(!probe_error(bad_probe).empty(), "Reject pause without active unload");
        bad_probe = probe;
        bad_probe.insert(bad_probe.find("T1\n"), "G1 E1\n");
        check(!probe_error(bad_probe).empty(), "Reject extrusion while unloaded");
        check(!probe_error(probe + "M400 U1\n").empty(), "Reject an extra pause");
        check(!probe_error(probe.substr(0, probe.find("T1\n"))).empty(), "Reject a missing replacement selection");
        bad_probe = probe;
        bad_probe.erase(bad_probe.find("M620 S255\n"), 10);
        check(!probe_error(bad_probe).empty(), "Reject an unload outside the native AMS scope");
        bad_probe = probe;
        bad_probe.replace(bad_probe.find("T1\n"), 2, "T2");
        check(!probe_error(bad_probe).empty(), "Reject the wrong first replacement spool");
        check(connect_import_uri("/tmp/cube.gcode.3mf", "Cube") ==
            "bambu-connect://import-file?path=%2Ftmp%2Fcube.gcode.3mf&name=Cube&version=1.0.0", "Official Connect example");
        check(connect_uri_component("C:\\Prints\\a b&c#1%.gcode.3mf") ==
            "C%3A%5CPrints%5Ca%20b%26c%231%25.gcode.3mf", "Windows path and query delimiters stay in path parameter");
        check(connect_uri_component(u8"Grün") == "Gr%C3%BCn", "UTF-8 file names");
        rejects([] { connect_import_uri("", "Cube"); });
        rejects([] { connect_import_uri("/tmp/a.3mf", std::string("a\0b", 3)); });
        check(connect_job_supported("A1", 2), "Ordinary A1 probe can be handed off");
        check(!connect_job_supported("A1", 8), "Connect does not implement eight-color A1 execution");
        check(!connect_job_supported("Bambu Lab A1", 8), "Native A1 profile name retains the four-slot Connect boundary");
        check(!connect_job_supported("Bambu Lab A1 mini", 8), "Native A1 mini profile name retains the four-slot Connect boundary");
        check(!connect_job_supported("A1mini", 5), "A1 mini has the same AMS Lite limit");
        check(!connect_job_supported("A1", 0), "Empty jobs cannot be handed off");
        check(connect_job_supported("X1C", 8), "Other models keep their normal multi-AMS support");
        scenarios();
        invalid_inputs_and_corruption();
        trace_contract();
        exhaustive_and_random();
        batch_scenarios();
        purge_optimization();
        batch_exhaustive_and_random();
        std::cout << "Passed " << checks << " checks, including 20,480 cache and 4,096 batch optimum comparisons, plus 1,300 randomized replays.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
