#pragma once

#include <istream>
#include <sstream>
#include <string>

namespace Slic3r::ExtendedAMSLite {

inline constexpr const char* active_unload_probe_name =
    "EXPERIMENTAL - unload slot 1, replace all four, then print slots 2, 3, 4, 1";

// Validate real command lines, not escaped macro text in a config header.
// This guard applies only to the named attended probe, not arbitrary prints.
inline std::string active_unload_probe_error(std::istream& input)
{
    unsigned int markers = 0, pauses = 0;
    bool opened = false, unloaded = false, closed = false, paused = false, loaded = false;
    std::string line;
    while (std::getline(input, line)) {
        const auto begin = line.find_first_not_of(" \t\r");
        if (begin == std::string::npos) continue;
        line = line.substr(begin, line.find_last_not_of(" \t\r") - begin + 1);
        if (line == "; AMS_LITE_ACTIVE_UNLOAD_PROBE_BEGIN") ++markers;
        std::istringstream words(line.substr(0, line.find(';')));
        std::string command, parameter;
        words >> command >> parameter;
        if (command == "M400" && parameter == "U1") {
            ++pauses;
            if (markers != 1 || !unloaded || !closed || loaded)
                return "The test pause is missing its preceding AMS unload sequence.";
            paused = true;
        }
        if (markers == 1 && !loaded) {
            if (command == "M620" && parameter == "S255" && !paused) opened = true;
            if (command == "T255" && !paused) {
                if (!opened) return "The test unload is missing its AMS command scope.";
                unloaded = true;
            }
            if (command == "M621" && parameter == "S255" && unloaded) closed = true;
            if (paused && !command.empty() && command.front() == 'T') {
                if (command != "T1") return "The test selects an unexpected spool after its pause.";
                loaded = true;
            }
            if (unloaded && !loaded && (command == "G0" || command == "G1" || command == "G2" || command == "G3")) {
                std::istringstream axes(line.substr(0, line.find(';')));
                while (axes >> parameter) {
                    if (parameter.size() > 1 && parameter.front() == 'E') {
                        try {
                            if (std::stod(parameter.substr(1)) > 0.)
                                return "The test would extrude before a replacement spool is selected.";
                        } catch (...) { return "The test contains an unreadable extrusion command."; }
                    }
                }
            }
        }
    }
    if (input.bad()) return "The sliced test file could not be read.";
    if (markers != 1 || pauses != 1 || !paused || !loaded)
        return "The sliced test is missing its single unload-and-pause sequence or its next spool selection.";
    return {};
}

} // namespace Slic3r::ExtendedAMSLite
