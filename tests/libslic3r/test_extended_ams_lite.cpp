#include <catch2/catch.hpp>
#include "libslic3r/ExtendedAMSLite/Planner.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/ParameterUtils.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include <boost/filesystem.hpp>
#include <sstream>
#include <set>

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
    CHECK(result.extruder_colors == config.filament_colour.values);
    const auto batched = AMS::plan_batches(requests, 8, std::vector<AMS::Slot>(4));
    CHECK(batched.schedule.stops.size() < schedule.stops.size());
    AMS::verify_batches(std::vector<AMS::Slot>(4), batched);

    GCodeProcessorResult copied;
    copied = result;
    CHECK(copied.moves[requests.back().move].layer_id == 1);
    CHECK(copied.filament_types == result.filament_types);
    copied.reset();
    CHECK(copied.moves.empty());
    CHECK(copied.filament_types.empty());
}

TEST_CASE("Batch sequences survive native Studio serialization", "[ExtendedAMSLite]")
{
    const std::vector<AMS::Request> requests{{3, 1, 0, 1}, {1, 1, 1, 2}, {0, 1, 2, 3},
        {4, 2, 3, 4}, {5, 2, 4, 5}, {1, 3, 5, 6}};
    const auto proposed = AMS::plan_batches(requests, 8, std::vector<AMS::Slot>(4), {1, 3, 0}, true);
    const auto layers = AMS::complete_layer_orders(proposed);
    REQUIRE(layers.size() == 3);
    CHECK(layers.front().filaments[0] == 1);
    std::vector<LayerPrintSequence> sequences;
    std::vector<unsigned> first;
    for (const auto& layer : layers) {
        REQUIRE(layer.filaments.size() == 8);
        std::vector<int> one_based;
        for (int id : layer.filaments) one_based.push_back(id + 1);
        if (layer.layer == 1) for (int id : layer.filaments) first.push_back(unsigned(id));
        else sequences.push_back({{int(layer.layer), int(layer.layer)}, one_based});
    }
    PrintConfig config;
    get_other_layers_print_sequence(sequences, config.other_layers_print_sequence_nums.value,
                                    config.other_layers_print_sequence.values);
    CHECK(get_other_layers_print_sequence(config.other_layers_print_sequence_nums.value,
                                         config.other_layers_print_sequence.values) == sequences);
    const auto callback = GroupReorder::create_custom_seq_function(config, true, first);
    for (const auto& layer : layers) {
        std::vector<int> actual;
        REQUIRE(callback(int(layer.layer) - 1, actual));
        std::vector<int> expected;
        for (int id : layer.filaments) expected.push_back(id + 1);
        CHECK(actual == expected);
    }
    std::vector<int> after;
    CHECK_FALSE(callback(3, after));
}

TEST_CASE("Real slicing honors eight-color batch layer orders", "[ExtendedAMSLite]")
{
    std::vector<AMS::Request> requests;
    for (unsigned layer = 1; layer <= 4; ++layer)
        for (int id = 0; id < 8; ++id) requests.push_back({id, layer, requests.size(), 0});
    const auto proposal = AMS::plan_batches(requests, 8, std::vector<AMS::Slot>(4), {3, 1, 0, 2, 4, 5, 6, 7}, true);
    const auto layers = AMS::complete_layer_orders(proposal);
    // Allocate dynamic options through their schema so enum serialization has
    // its key maps, as it does when Studio loads real presets.
    DynamicPrintConfig config;
    config.apply(FullPrintConfig::defaults());
    config.set_num_filaments(8);
    // FullPrintConfig defaults describe one profile. Mirror PresetBundle's
    // eight-profile expansion, including the newer per-filament variant IDs.
    for (const auto& key : Preset::filament_options()) {
        auto* option = config.option(key);
        if (option && option->is_vector())
            static_cast<ConfigOptionVectorBase*>(option)->resize(8, FullPrintConfig::defaults().option(key));
    }
    config.set_key_value("filament_self_index", new ConfigOptionInts({1, 2, 3, 4, 5, 6, 7, 8}));
    config.set_key_value("filament_extruder_variant", new ConfigOptionStrings(std::vector<std::string>(8, "Direct Drive Standard")));
    config.set_key_value("filament_diameter", new ConfigOptionFloats(std::vector<double>(8, 1.75)));
    config.set_key_value("filament_colour", new ConfigOptionStrings({"#202020", "#2463EB", "#E34B4B", "#F5F5F5", "#22A86A", "#F2C438", "#9755CE", "#ED8736"}));
    config.set_key_value("filament_map", new ConfigOptionInts(std::vector<int>(8, 1)));
    config.set_key_value("filament_volume_map", new ConfigOptionInts(std::vector<int>(8, 0)));
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats(std::vector<double>(64, 0.0)));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
    // Real printer start macros select the initial filament. The generic
    // default macro is empty and would label its first extrusion as tool 0.
    config.set_key_value("machine_start_gcode", new ConfigOptionString("T[initial_extruder]"));
    std::vector<LayerPrintSequence> sequences;
    for (const auto& layer : layers) {
        std::vector<int> ids;
        for (int id : layer.filaments) ids.push_back(id + 1);
        if (layer.layer == 1) config.set_key_value("first_layer_print_sequence", new ConfigOptionInts(ids));
        else sequences.push_back({{int(layer.layer), int(layer.layer)}, ids});
    }
    int count;
    std::vector<int> serialized;
    get_other_layers_print_sequence(sequences, count, serialized);
    config.set_key_value("other_layers_print_sequence_nums", new ConfigOptionInt(count));
    config.set_key_value("other_layers_print_sequence", new ConfigOptionInts(serialized));
    Model model;
    auto* object = model.add_object();
    object->name = "eight-color-batch-coupon";
    for (int id = 0; id < 8; ++id) {
        TriangleMesh mesh = make_cube(5, 5, 0.8);
        mesh.translate(float((id % 4) * 8 + 30), float((id / 4) * 8 + 30), 0);
        auto* volume = object->add_volume(std::move(mesh));
        volume->config.set("extruder", id + 1);
    }
    object->add_instance();
    object->ensure_on_bed();
    Print print;
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.config().filament_diameter.size() == 8);
    REQUIRE(print.config().filament_colour.size() == 8);
    CHECK(print.config().filament_volume_map.values == std::vector<int>(8, 0));
    CHECK(print.config().filament_map.values == std::vector<int>(8, 1));
    print.process();
    CHECK(print.config().filament_volume_map.values == std::vector<int>(8, 0));
    CHECK(print.config().filament_map.values == std::vector<int>(8, 1));
    REQUIRE(print.get_layered_nozzle_group_result());
    for (int id = 0; id < 8; ++id) {
        const auto nozzle = print.get_layered_nozzle_group_result()->get_nozzle_for_filament(id);
        REQUIRE(nozzle);
        CHECK(nozzle->volume_type == nvtStandard);
    }
    const auto& actual = print.tool_ordering().layer_tools();
    REQUIRE(actual.size() == layers.size());
    for (std::size_t layer = 0; layer < layers.size(); ++layer) {
        std::vector<unsigned> expected;
        for (int id : layers[layer].filaments) expected.push_back(unsigned(id));
        CHECK(actual[layer].extruders == expected);
    }
    // Export through the normal generator and inspect its real processed trace.
    // This file is a test artifact only; no printer connection is involved.
    struct TemporaryGCode {
        boost::filesystem::path path = boost::filesystem::temp_directory_path() /
            boost::filesystem::unique_path("extended-ams-%%%%-%%%%-%%%%.gcode");
        ~TemporaryGCode() { boost::system::error_code ignored; boost::filesystem::remove(path, ignored); }
    } output;
    GCodeProcessorResult processed;
    print.export_gcode(output.path.string(), &processed, nullptr);
    const auto trace = AMS::collect_requests(processed.moves, EMoveType::Tool_change, EMoveType::Extrude, true);
    REQUIRE_FALSE(trace.empty());
    const auto exported = AMS::plan_batches(trace, 8, std::vector<AMS::Slot>(4));
    std::ostringstream trace_info;
    for (const auto& r : trace) trace_info << "L" << r.layer << ":V" << r.filament + 1 << ' ';
    INFO(trace_info.str());
    CHECK(exported.schedule.stops.size() == 4);
    CHECK(exported.spool_replacements == 16);
    std::map<unsigned, std::set<int>> extruded;
    for (const auto& move : processed.moves)
        if (move.type == EMoveType::Extrude && move.layer_id > 0) extruded[move.layer_id].insert(move.extruder_id);
    for (unsigned layer = 1; layer <= 4; ++layer)
        CHECK(extruded[layer] == std::set<int>({0, 1, 2, 3, 4, 5, 6, 7}));
    CHECK(processed.pause_printing.empty());
}
