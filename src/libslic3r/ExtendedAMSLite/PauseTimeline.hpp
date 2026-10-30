#pragma once
#include "Execution.hpp"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace Slic3r::ExtendedAMSLite {

enum class PauseKind { SpoolSwap, Magnets };
struct PauseEvent {
    PauseKind kind = PauseKind::SpoolSwap;
    std::size_t visit = 0;
    unsigned layer = 0;
    double z = 0.;
    int remaining_seconds = -1; // slicer estimate; -1 means unavailable
};
struct PauseTiming { unsigned layer = 0; int remaining_seconds = -1; };
struct PauseTimeline {
    std::string job_id;
    std::vector<PauseEvent> events;
    bool complete = true;
};

inline std::string pause_job_id(const std::string& name)
{
    if (name.rfind("EAMS-", 0) != 0 || name.size() < 21) return {};
    const auto id = name.substr(5, 16);
    if (!std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) return {};
    if (name.size() > 21 && name[21] != '_' && name[21] != '.' && name[21] != ' ' && name[21] != '-') return {};
    return id;
}
inline void verify_pause_timeline(const PauseTimeline& timeline, const ExecutionSpec& spec)
{
    verify_execution(spec);
    if (timeline.job_id != spec.job_id || timeline.events.size() > 201000)
        throw std::invalid_argument("Pause overview belongs to a different or invalid job.");
    std::size_t swaps = 0, magnets = 0;
    unsigned previous_layer = 0;
    for (const auto& e : timeline.events) {
        if (e.remaining_seconds < -1 || e.layer > 1000000 || (e.layer && e.layer < previous_layer))
            throw std::invalid_argument("Invalid pause overview timing or layer order.");
        if (e.layer) previous_layer = e.layer;
        if (e.kind == PauseKind::SpoolSwap) {
            if (e.visit != swaps || swaps >= spec.plan.schedule.stops.size() ||
                e.layer != spec.plan.requests[spec.plan.schedule.stops[swaps].before_request].layer)
                throw std::invalid_argument("Pause overview does not match the spool-swap plan.");
            ++swaps;
        } else {
            if (!timeline.complete || magnets >= spec.magnet_pauses.size() || !std::isfinite(e.z) ||
                std::abs(e.z - spec.magnet_pauses[magnets]) > 0.001)
                throw std::invalid_argument("Pause overview does not match the magnet insertion plan.");
            ++magnets;
        }
    }
    if (swaps != spec.plan.schedule.stops.size() || (timeline.complete && magnets != spec.magnet_pauses.size()))
        throw std::invalid_argument("The pause overview is incomplete.");
}
inline PauseTimeline legacy_pause_timeline(const ExecutionSpec& spec)
{
    PauseTimeline out; out.job_id = spec.job_id; out.complete = spec.magnet_pauses.empty();
    for (std::size_t v = 0; v < spec.plan.schedule.stops.size(); ++v)
        out.events.push_back({PauseKind::SpoolSwap, v, spec.plan.requests[spec.plan.schedule.stops[v].before_request].layer, 0., -1});
    verify_pause_timeline(out, spec); return out;
}
// Read actual generated audit markers, preserving the order of multiple stops
// within one layer. The caller supplies the real processor's native pause list.
inline PauseTimeline read_pause_timeline(std::istream& gcode, const ExecutionSpec& spec,
                                         const std::vector<PauseTiming>& timings = {})
{
    PauseTimeline out; out.job_id = spec.job_id;
    std::optional<PauseEvent> pending;
    bool header = false, end = false;
    std::string line;
    const std::string swap_tag = "; EAMS_VISIT ", magnet_tag = "; MAGNET_INSERT_BEGIN ";
    while (std::getline(gcode, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("; EAMS_JOB ", 0) == 0) {
            if (header || line.substr(11) != spec.job_id) throw std::invalid_argument("Wrong job in pause overview.");
            header = true;
        } else if (line.rfind("; EAMS_END ", 0) == 0) end = line.substr(11) == spec.job_id;
        else if (line.rfind(swap_tag, 0) == 0) {
            PauseEvent e; std::size_t request = 0;
            std::istringstream row(line.substr(swap_tag.size()));
            if (pending || !(row >> e.visit >> request) || e.visit >= spec.plan.schedule.stops.size() ||
                request != spec.plan.schedule.stops[e.visit].before_request)
                throw std::invalid_argument("Invalid swap marker in pause overview.");
            e.layer = spec.plan.requests[request].layer; pending = e;
        } else if (line.rfind(magnet_tag, 0) == 0) {
            PauseEvent e; e.kind = PauseKind::Magnets;
            std::istringstream row(line.substr(magnet_tag.size()));
            if (pending || !(row >> e.z)) throw std::invalid_argument("Invalid magnet marker in pause overview.");
            pending = e;
        }
        std::istringstream words(line.substr(0, line.find(';')));
        std::string command, arg; words >> command >> arg;
        if (command == "M400" && arg == "U1") {
            if (!header || !pending) throw std::invalid_argument("Unplanned pause in overview.");
            out.events.push_back(*pending); pending.reset();
        }
    }
    if (!header || !end || pending || gcode.bad()) throw std::invalid_argument("Incomplete pause overview.");
    if (!timings.empty()) {
        if (timings.size() != out.events.size()) throw std::invalid_argument("Native pause count does not match the overview.");
        for (std::size_t i = 0; i < timings.size(); ++i) {
            if (out.events[i].layer && out.events[i].layer != timings[i].layer)
                throw std::invalid_argument("Native pause layer does not match the overview.");
            out.events[i].layer = timings[i].layer;
            out.events[i].remaining_seconds = timings[i].remaining_seconds;
        }
    }
    verify_pause_timeline(out, spec); return out;
}
inline std::string serialize_pause_timeline(const PauseTimeline& timeline, const ExecutionSpec& spec)
{
    verify_pause_timeline(timeline, spec);
    std::ostringstream out;
    out << "EAMST1 " << timeline.job_id << ' ' << timeline.complete << ' ' << timeline.events.size() << '\n' << std::setprecision(12);
    for (const auto& e : timeline.events)
        out << (e.kind == PauseKind::SpoolSwap ? 'S' : 'M') << ' ' << e.visit << ' ' << e.layer << ' ' << e.z << ' ' << e.remaining_seconds << '\n';
    return out.str() + "END\n";
}
inline PauseTimeline deserialize_pause_timeline(const std::string& text, const ExecutionSpec& spec)
{
    if (text.size() > 10000000) throw std::invalid_argument("Pause overview is too large.");
    std::istringstream in(text); std::string tag; std::size_t count = 0; PauseTimeline out;
    if (!(in >> tag >> out.job_id >> out.complete >> count) || tag != "EAMST1" || count > 201000)
        throw std::invalid_argument("Invalid pause overview header.");
    for (std::size_t i = 0; i < count; ++i) {
        PauseEvent e; char kind = 0;
        if (!(in >> kind >> e.visit >> e.layer >> e.z >> e.remaining_seconds) || (kind != 'S' && kind != 'M'))
            throw std::invalid_argument("Invalid pause overview entry.");
        e.kind = kind == 'S' ? PauseKind::SpoolSwap : PauseKind::Magnets; out.events.push_back(e);
    }
    if (!(in >> tag) || tag != "END" || (in >> tag)) throw std::invalid_argument("Incomplete pause overview.");
    verify_pause_timeline(out, spec); return out;
}

struct PauseObservation {
    std::string printer, job_name, status;
    bool connected = false, ready = false;
    unsigned layer = 0;
    int error = 0, remaining_seconds = -1;
};
struct PauseProgress {
    std::string run;
    std::size_t completed = 0;
    bool awaiting_resume = false;
};
inline bool matches_pause_run(const PauseProgress& progress, const PauseObservation& state)
{
    // New sends put the nonce in the printer-reported job name as well as the
    // local sidecar. Re-sending a plan must not attach new progress to an old run.
    const auto suffix = state.job_name.size() > 21 ? state.job_name.substr(21) : std::string();
    if (progress.run.empty()) return suffix.rfind("_r", 0) != 0;
    const auto expected = "_r" + progress.run;
    if (suffix.rfind(expected, 0) != 0) return false;
    return suffix.size() == expected.size() || suffix[expected.size()] == '_' ||
        suffix[expected.size()] == '.' || suffix[expected.size()] == ' ' || suffix[expected.size()] == '-';
}
inline bool matches_pause_job(const ExecutionSpec& spec, const std::string& printer, const PauseObservation& state)
{
    return !printer.empty() && printer == state.printer && pause_job_id(state.job_name) == spec.job_id;
}
inline bool observe_pause_progress(PauseProgress& progress, const ExecutionSpec& spec, const std::string& printer, const PauseObservation& state)
{
    // Neither layer advancement nor a generic PAUSE advances the guide.
    if (matches_pause_job(spec, printer, state) && matches_pause_run(progress, state) && state.connected && state.ready && state.status == "RUNNING" && progress.awaiting_resume) {
        progress.awaiting_resume = false; return true;
    }
    return false;
}
inline bool can_complete_pause(const PauseProgress& progress, const PauseTimeline& timeline, const ExecutionSpec& spec,
                               const std::string& printer, const PauseObservation& state)
{
    return matches_pause_job(spec, printer, state) && matches_pause_run(progress, state) && state.connected && state.ready && state.status == "PAUSE" && state.error == 0 &&
        !progress.awaiting_resume && progress.completed < timeline.events.size() && timeline.events[progress.completed].layer > 0 &&
        timeline.events[progress.completed].layer == state.layer;
}
inline bool complete_pause(PauseProgress& progress, const PauseTimeline& timeline, const ExecutionSpec& spec,
                           const std::string& printer, const PauseObservation& state)
{
    if (!can_complete_pause(progress, timeline, spec, printer, state)) return false;
    ++progress.completed; progress.awaiting_resume = true; return true;
}
inline std::string serialize_pause_progress(const PauseProgress& progress, std::size_t count)
{
    if (progress.run.empty() || progress.run.size() > 512 || progress.completed > count || (progress.awaiting_resume && progress.completed == 0))
        throw std::invalid_argument("Invalid local pause progress.");
    std::ostringstream out; out << "EAMSP1 " << std::quoted(progress.run) << ' ' << progress.completed << ' ' << progress.awaiting_resume << "\nEND\n";
    return out.str();
}
inline PauseProgress deserialize_pause_progress(const std::string& text, const std::string& run, std::size_t count)
{
    if (text.size() > 2048) throw std::invalid_argument("Invalid local pause progress.");
    PauseProgress out; std::string tag; std::istringstream in(text);
    if (!(in >> tag >> std::quoted(out.run) >> out.completed >> out.awaiting_resume) || tag != "EAMSP1" ||
        !(in >> tag) || tag != "END" || (in >> tag) || out.run != run)
        throw std::invalid_argument("Saved pause progress belongs to a different run.");
    serialize_pause_progress(out, count); return out;
}
inline int estimated_seconds_to_pause(const PauseEvent& event, const PauseObservation& state)
{
    if (!state.connected || !state.ready || state.status != "RUNNING" || event.remaining_seconds < 0 || state.remaining_seconds < 0)
        return -1;
    const auto seconds = static_cast<long long>(state.remaining_seconds) - event.remaining_seconds;
    return seconds < 0 || seconds > 2147483647 ? -1 : int(seconds);
}
} // namespace Slic3r::ExtendedAMSLite
