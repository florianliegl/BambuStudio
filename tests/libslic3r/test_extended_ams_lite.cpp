#include <catch2/catch.hpp>
#include "libslic3r/ExtendedAMSLite/Planner.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

using namespace Slic3r;
namespace AMS = Slic3r::ExtendedAMSLite;

// Requires the normal Bambu Studio dependencies. This exercises the real
// processor (including virtual filament vs physical extruder semantics),
// rather than only the standalone trace-contract fixture.
TEST_CASE("AMS Lite planner consumes real processed virtual selections", "[ExtendedAMSLite]")
{
    PrintConfig config;
    config.gcode_flavor.value = gcfMarlinLegacy;
    config.filament_diameter.values.assign(8, 1.75);
    config.filament_type.values.assign(8, "PLA");
    config.filament_map.values.assign(8, 1); // every virtual color uses one extruder
    config.filament_colour.values.assign(8, "#008080");

    GCodeProcessor processor;
    processor.apply_config(config);
    processor.initialize("extended-ams-lite-test.gcode");
    const std::string gcode =
        "G90\nM83\n; CHANGE_LAYER\n; Z_HEIGHT: 0.2\n; TYPE:Outer wall\n"
        "T7\nG1 X10 Y10 Z0.2 F1200\nG1 X20 Y10 E1\n"
        "T2\nG1 X20 Y20 E1\nT6\nG1 X10 Y20 E1\n"
        "T0\nG1 X10 Y10 E1\nT1\nG1 X20 Y10 E1\n"
        "T3\nG1 X20 Y20 E1\nT4\nG1 X10 Y20 E1\n"
        "T5\nG1 X10 Y10 E1\nT7\nG1 X20 Y10 E1\n";
    processor.process_buffer(gcode);
    processor.finalize(false); // never write a file or start a print
    const auto& result = processor.get_result();
    const auto requests = AMS::collect_requests(result.moves, EMoveType::Tool_change, EMoveType::Extrude);
    const std::vector<int> expected{7, 2, 6, 0, 1, 3, 4, 5, 7};
    REQUIRE(requests.size() == expected.size());
    for (std::size_t i = 0; i < requests.size(); ++i) {
        CHECK(requests[i].filament == expected[i]);
        CHECK(requests[i].layer == 1);
        CHECK(result.moves[requests[i].move].type == EMoveType::Tool_change);
    }
    const auto schedule = AMS::plan(requests, 8, std::vector<AMS::Slot>(4));
    REQUIRE(schedule.stops.size() == 4);
    CHECK(result.pause_printing.empty());
    CHECK(result.filament_types == std::vector<std::string>(8, "PLA"));

    GCodeProcessorResult copied;
    copied = result;
    CHECK(copied.moves[requests.back().move].layer_id == 1);
    CHECK(copied.filament_types == result.filament_types);
    copied.reset();
    CHECK(copied.moves.empty());
    CHECK(copied.filament_types.empty());
}
