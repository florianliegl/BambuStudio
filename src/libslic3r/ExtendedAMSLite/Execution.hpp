#pragma once
#include "Planner.hpp"
#include <istream>
#include <optional>
#include <string>

namespace Slic3r::ExtendedAMSLite {
inline constexpr const char* execution_preparing_tag = "[[EXTENDED_AMS_LITE_PREPARING]]";

struct ExecutionSpec {
    std::string job_id;
    std::vector<Slot> slots;
    std::vector<Filament> filaments;
    BatchPlan plan;
    std::vector<double> magnet_pauses; // native insertion pauses, separate from spool visits
};

BatchPlan plan_fixed_batches(const std::vector<Request>& requests, const std::vector<Slot>& slots,
                             const std::vector<int>& filament_slots);
std::vector<Request> normalize_execution_requests(std::vector<Request> requests, int initial_filament);
void verify_execution(const ExecutionSpec& spec);
std::string serialize_execution(const ExecutionSpec& spec);
ExecutionSpec deserialize_execution(const std::string& text);
std::optional<ExecutionSpec> execution_from_notes(const std::string& notes);
std::string notes_with_execution(const std::string& notes, const std::optional<ExecutionSpec>& spec);
std::string execution_job_prefix(const ExecutionSpec& spec);
std::string format_execution_guide(const ExecutionSpec& spec, bool html = false);

struct ExecutionStep {
    std::size_t request = 0;
    int filament = -1;
    unsigned int layer = 0;
    std::optional<std::size_t> visit;
};

// Consumes actual native tool changes. It never selects a tool or resumes a
// printer; GCode expands the existing A1 unload/pause and normal change macros.
class ExecutionSession {
public:
    explicit ExecutionSession(ExecutionSpec spec);
    ExecutionStep begin(int filament);
    std::optional<ExecutionStep> change(int filament, unsigned int layer);
    void finish() const;
    const ExecutionSpec& spec() const { return m_spec; }
private:
    ExecutionSpec m_spec;
    std::vector<int> m_loaded;
    std::size_t m_cursor = 0, m_next_visit = 0;
    bool m_started = false;
};

std::string execution_selection_marker(const ExecutionStep& step);
// Returns an actionable error if the real command stream differs from its plan.
std::string execution_gcode_error(std::istream& input, const ExecutionSpec& spec);

} // namespace Slic3r::ExtendedAMSLite
