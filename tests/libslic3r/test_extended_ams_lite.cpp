#include <catch2/catch.hpp>
#include "libslic3r/ExtendedAMSLite/Planner.hpp"
#include "libslic3r/ExtendedAMSLite/Probe.hpp"
#include "libslic3r/ExtendedAMSLite/Execution.hpp"
#include "libslic3r/ExtendedAMSLite/PauseTimeline.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/ParameterUtils.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/WhiteLayer.hpp"
#include <boost/filesystem.hpp>
#include <cstdlib>
#include <fstream>
#include <array>
#include <map>
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

TEST_CASE("A1 eight-color execution preserves normal macros and inserts batch pauses", "[ExtendedAMSLite][Execution]")
{
    namespace fs = boost::filesystem;
    const auto resources = fs::path(TEST_DATA_DIR).parent_path().parent_path() / "resources";
    set_resources_dir(resources.string());
    PresetBundle presets;
    presets.load_vendor_configs_from_json((resources / "profiles").string(), "BBL",
        PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent);
    REQUIRE(presets.printers.select_preset_by_name("Bambu Lab A1 0.2 nozzle", true, true));
    REQUIRE(presets.prints.select_preset_by_name("0.10mm Standard @BBL A1 0.2 nozzle", true, true));
    REQUIRE(presets.filaments.select_preset_by_name("Generic PLA @BBL A1 0.2 nozzle", true, true));
    presets.filament_presets.assign(8, "Generic PLA @BBL A1 0.2 nozzle");
    presets.set_num_filaments(8);
    const std::vector<std::string> colors{"#202020", "#2463EB", "#E34B4B", "#F5F5F5", "#22A86A", "#F2C438", "#9755CE", "#ED8736"};
    presets.project_config.set_key_value("filament_colour", new ConfigOptionStrings(colors));
    auto config = presets.full_config();
    config.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(btPTE));
    std::vector<double> flush(64, 160.);
    for (int i = 0; i < 8; ++i) flush[i * 8 + i] = 0.;
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats(flush));
    std::vector<AMS::Request> nominal;
    for (unsigned layer = 1; layer <= 4; ++layer)
        for (int id = 0; id < 8; ++id) nominal.push_back({id, layer, nominal.size(), 0});
    const auto proposal = AMS::plan_batches(nominal, 8, std::vector<AMS::Slot>(4), {3, 1, 0, 2, 4, 5, 6, 7}, true);
    std::vector<LayerPrintSequence> other;
    for (const auto& layer : AMS::complete_layer_orders(proposal)) {
        std::vector<int> ids;
        for (int id : layer.filaments) ids.push_back(id + 1);
        if (layer.layer == 1) config.set_key_value("first_layer_print_sequence", new ConfigOptionInts(ids));
        else other.push_back({{int(layer.layer), int(layer.layer)}, ids});
    }
    int count = 0;
    std::vector<int> serialized;
    get_other_layers_print_sequence(other, count, serialized);
    config.set_key_value("other_layers_print_sequence_nums", new ConfigOptionInt(count));
    config.set_key_value("other_layers_print_sequence", new ConfigOptionInts(serialized));
    Model model;
    auto* object = model.add_object();
    object->name = "Eight-color four-layer execution coupon";
    for (int id = 0; id < 8; ++id) {
        auto mesh = make_cube(4, 4, 0.4);
        mesh.translate(116.f + float(id % 4) * 5.f, 123.f + float(id / 4) * 5.f, 0.f);
        auto* volume = object->add_volume(std::move(mesh));
        volume->config.set("extruder", id + 1);
    }
    object->add_instance();
    object->ensure_on_bed();
    Print print;
    print.set_status_silent(); print.set_BBL_Printer(true); print.set_plate_index(0); print.set_plate_origin(Vec3d::Zero());
    print.set_extruder_filament_info(presets.get_extruder_filament_info());
    print.apply(model, config);
    const auto validation = print.validate();
    INFO(validation.string);
    REQUIRE(validation.string.empty());
    print.process();
    struct Files {
        fs::path plain = fs::temp_directory_path() / fs::unique_path("ams-eight-%%%%-%%%%.gcode");
        fs::path execution = fs::temp_directory_path() / fs::unique_path("ams-eight-ready-%%%%-%%%%.gcode");
        ~Files() { boost::system::error_code ignored; fs::remove(plain, ignored); fs::remove(execution, ignored); }
    } files;
    GCodeProcessorResult baseline;
    print.export_gcode(files.plain.string(), &baseline, nullptr);
    REQUIRE(baseline.pause_printing.empty());
    REQUIRE_FALSE(baseline.filament_change_sequence.empty());
    auto requests = AMS::normalize_execution_requests(
        AMS::collect_requests(baseline.moves, EMoveType::Tool_change, EMoveType::Extrude, true), baseline.filament_change_sequence.front());
    REQUIRE(requests.front().filament == 3); // manual White-first choice
    AMS::ExecutionSpec spec;
    spec.job_id = "a100020000000008";
    spec.slots.resize(4);
    for (int i = 0; i < 8; ++i) spec.filaments.push_back({"PLA " + std::to_string(i + 1), "PLA", colors[i]});
    spec.plan = AMS::plan_fixed_batches(requests, spec.slots, proposal.filament_slots);
    REQUIRE(spec.plan.schedule.stops.size() == 4);
    for (const auto& stop : spec.plan.schedule.stops) REQUIRE(stop.replacements.size() == 4);
    config.set_key_value("process_notes", new ConfigOptionString(AMS::notes_with_execution("", spec)));
    print.apply(model, config);
    print.process();
    GCodeProcessorResult ready;
    print.export_gcode(files.execution.string(), &ready, nullptr);
    REQUIRE(ready.pause_printing.size() == spec.plan.schedule.stops.size());
    std::ifstream generated(files.execution.string(), std::ios::binary);
    const auto error = AMS::execution_gcode_error(generated, spec);
    INFO(error);
    REQUIRE(error.empty());
    generated.close();
    std::ifstream overview_source(files.execution.string(), std::ios::binary);
    std::vector<AMS::PauseTiming> timings;
    for (const auto& pause : ready.pause_printing) timings.push_back({pause.layer_id, pause.remaining_time * 60});
    const auto overview = AMS::read_pause_timeline(overview_source, spec, timings);
    overview_source.close(); // Later checks re-export this same path on Windows.
    REQUIRE(overview.events.size() == 4);
    CHECK(overview.complete);
    for (std::size_t i = 0; i < overview.events.size(); ++i) {
        CHECK(overview.events[i].kind == AMS::PauseKind::SpoolSwap);
        CHECK(overview.events[i].visit == i);
        CHECK(overview.events[i].layer == i + 1);
    }
    for (std::size_t i = 0; i < ready.pause_printing.size(); ++i)
        REQUIRE(ready.pause_printing[i].layer_id == spec.plan.requests[spec.plan.schedule.stops[i].before_request].layer);
    std::map<unsigned, std::set<int>> extruded;
    for (const auto& move : ready.moves) if (move.type == EMoveType::Extrude && move.layer_id > 0)
        extruded[move.layer_id].insert(move.extruder_id);
    REQUIRE(extruded.size() == 4);
    for (const auto& layer : extruded) REQUIRE(layer.second == std::set<int>({0,1,2,3,4,5,6,7}));

    // Use the same edited-process metadata as the GUI. Raw full-config changes
    // alone are not enough to survive native preset reconciliation on import.
    auto& process = presets.prints.get_edited_preset().config;
    for (const auto& key : config.keys())
        if (process.has(key)) process.set_key_value(key, config.option(key)->clone());
    presets.prints.update_dirty();
    const auto native_export = presets.full_config();
    config.set_key_value("different_settings_to_system", native_export.option("different_settings_to_system")->clone());
    PlateData plate;
    plate.plate_index = 0; plate.objects_and_instances.emplace_back(0, 0);
    plate.plate_name = "Eight colors - four batch visits";
    for (const auto* key : {"first_layer_print_sequence", "other_layers_print_sequence_nums", "other_layers_print_sequence"})
        plate.config.set_key_value(key, config.option(key)->clone());
    const auto project = files.plain.string() + ".3mf";
    const auto sliced = files.execution.string() + ".3mf";
    struct Archives {
        std::vector<std::string> paths;
        ~Archives() { for (const auto& path : paths) { boost::system::error_code ignored; fs::remove(path, ignored); } }
    } archives{{project, sliced}};
    StoreParams params;
    params.path = project.c_str(); params.model = &model; params.config = &config;
    params.plate_data_list.push_back(&plate);
    params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
    REQUIRE(store_bbs_3mf(params));
    Model loaded_model;
    DynamicPrintConfig loaded_config;
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::EnableSilent);
    PlateDataPtrs plates;
    std::vector<Preset*> embedded;
    bool is_bambu = false;
    Semver version;
    REQUIRE(load_bbs_3mf(project.c_str(), &loaded_config, &substitutions, &loaded_model, &plates,
        &embedded, &is_bambu, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence));
    PresetBundle restored;
    restored.load_vendor_configs_from_json((resources / "profiles").string(), "BBL",
        PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent);
    if (!embedded.empty()) restored.load_project_embedded_presets(embedded, ForwardCompatibilitySubstitutionRule::EnableSilent);
    Preset::normalize(loaded_config);
    restored.load_config_model(project, std::move(loaded_config), version);
    auto imported = restored.full_config();
    REQUIRE(AMS::serialize_execution(*AMS::execution_from_notes(imported.opt_string("process_notes"))) == AMS::serialize_execution(spec));
    REQUIRE(loaded_model.objects.size() == 1);
    REQUIRE(loaded_model.objects.front()->volumes.size() == 8);
    REQUIRE(plates.size() == 1);
    imported.apply(plates.front()->config);
    Print reopened;
    reopened.set_status_silent(); reopened.set_BBL_Printer(true); reopened.set_plate_index(0); reopened.set_plate_origin(Vec3d::Zero());
    reopened.set_extruder_filament_info(restored.get_extruder_filament_info());
    reopened.apply(loaded_model, imported); reopened.process();
    GCodeProcessorResult repeated;
    reopened.export_gcode(files.execution.string(), &repeated, nullptr);
    REQUIRE(repeated.pause_printing.size() == 4);
    std::ifstream reloaded(files.execution.string(), std::ios::binary);
    REQUIRE(AMS::execution_gcode_error(reloaded, spec).empty());
    reloaded.close();
    // Changing a normal process setting preserves geometry and the executable
    // schedule when the actual native color order/layers are still the same.
    restored.prints.get_edited_preset().config.set_key_value("prime_tower_width", new ConfigOptionFloat(45.));
    restored.prints.update_dirty();
    imported = restored.full_config(); imported.apply(plates.front()->config);
    reopened.apply(loaded_model, imported); reopened.process();
    reopened.export_gcode(files.execution.string(), &repeated, nullptr);
    REQUIRE(repeated.pause_printing.size() == 4);
    std::ifstream edited(files.execution.string(), std::ios::binary);
    REQUIRE(AMS::execution_gcode_error(edited, spec).empty());
    edited.close();
    // Export the original, checked slice as a companion. The editable project
    // is always the desktop launch target.
    plate.gcode_file = files.execution.string(); plate.is_sliced_valid = true;
    plate.printer_model_id = "A1"; plate.nozzle_diameters = "0.2";
    plate.pause_printing = repeated.pause_printing; plate.parse_filament_info(&repeated);
    for (auto& filament : plate.slice_filaments_info) {
        std::string display_type;
        filament.type = config.get_filament_type(display_type, filament.id);
        filament.color = colors[filament.id];
        filament.filament_id = config.option<ConfigOptionStrings>("filament_ids")->get_at(filament.id);
    }
    REQUIRE(plate.slice_filaments_info.size() == 8);
    params.path = sliced.c_str();
    params.strategy = SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::WithGcode | SaveStrategy::SkipModel;
    REQUIRE(store_bbs_3mf(params));
    release_PlateData_list(plates);
    for (auto* preset : embedded) delete preset;
    if (const char* destination = std::getenv("EXTENDED_AMS_EXECUTION_OUTPUT")) {
        const auto folder = fs::absolute(destination);
        fs::create_directories(folder);
        const auto output = folder / "A1 eight-color batch test.3mf";
        const auto fixed_output = folder / "A1 eight-color batch test.gcode.3mf";
        REQUIRE_FALSE(fs::exists(output)); REQUIRE_FALSE(fs::exists(fixed_output));
        fs::copy_file(project, output); fs::copy_file(sliced, fixed_output);
        std::ofstream guide((folder / "SWAP GUIDE.html").string(), std::ios::binary);
        guide << AMS::format_execution_guide(spec, true);
        REQUIRE(bool(guide));
    }
}

TEST_CASE("A1 attended replacement probes preserve native tool-change recovery", "[ExtendedAMSLite][HardwareProbe]")
{
    const bool active_unload = GENERATE(false, true);
    INFO((active_unload ? "Experimental active unload before one pause" : "Previously tested inactive-slot replacement"));
    namespace fs = boost::filesystem;
    const auto resources = fs::path(TEST_DATA_DIR).parent_path().parent_path() / "resources";
    REQUIRE(fs::exists(resources / "profiles" / "BBL.json"));
    set_resources_dir(resources.string());
    PresetBundle presets;
    presets.load_vendor_configs_from_json((resources / "profiles").string(), "BBL",
        PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent);
    REQUIRE(presets.printers.select_preset_by_name("Bambu Lab A1 0.2 nozzle", true, true));
    REQUIRE(presets.prints.select_preset_by_name("0.10mm Standard @BBL A1 0.2 nozzle", true, true));
    REQUIRE(presets.filaments.select_preset_by_name("Generic PLA @BBL A1 0.2 nozzle", true, true));
    const int filament_count = active_unload ? 4 : 2;
    presets.filament_presets.assign(filament_count, "Generic PLA @BBL A1 0.2 nozzle");
    presets.set_num_filaments(filament_count);
    presets.project_config.set_key_value("filament_colour", new ConfigOptionStrings(active_unload ?
        std::vector<std::string>{"#E34B4B", "#2463EB", "#22A86A", "#F2C438"} :
        std::vector<std::string>{"#E34B4B", "#2463EB"}));
    auto config = presets.full_config();
    config.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(btPTE));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.1));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.1));
    std::vector<double> flush_matrix(filament_count * filament_count, 160.);
    for (int i = 0; i < filament_count; ++i) flush_matrix[i * filament_count + i] = 0.;
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats(flush_matrix));
    REQUIRE(config.opt_string("printer_settings_id") == "Bambu Lab A1 0.2 nozzle");
    REQUIRE(config.option<ConfigOptionFloats>("nozzle_diameter")->values.front() == Approx(0.2));
    REQUIRE(config.opt_string("machine_pause_gcode") == "M400 U1");
    REQUIRE(config.opt_string("change_filament_gcode").find("M620 S[next_extruder]A") != std::string::npos);
    if (active_unload) {
        // An attended hardware probe, not an enabled batch execution backend.
        // Reuse only the exact A1 profile unload block; never insert its heater
        // shutdown, motor-current changes, timelapse shutdown or final parking.
        const auto end = config.opt_string("machine_end_gcode");
        const auto start = end.find("M620 S255\n");
        const auto finish = end.find("M621 S255", start);
        REQUIRE(start != std::string::npos);
        REQUIRE(finish != std::string::npos);
        const auto unload = end.substr(start, finish + std::string("M621 S255").size() - start);
        REQUIRE(unload.find("\nT255\n") != std::string::npos);
        REQUIRE(unload.find("M104") == std::string::npos);
        REQUIRE(unload.find("M140") == std::string::npos);
        REQUIRE(unload.find("M17") == std::string::npos);
        // At a layer pause the prime tower may still extrude the old color.
        // Insert at the actual 1->2 tool-change boundary instead, after that
        // old-color tower wall. The native macro then loads/purges normally.
        const auto ordinary_change = config.opt_string("change_filament_gcode");
        config.set_key_value("change_filament_gcode", new ConfigOptionString(
            "{if layer_z > 0.35 && layer_z < 0.45 && next_extruder == 1}\n;" +
            GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Pause_Print) +
            "\n; AMS_LITE_ACTIVE_UNLOAD_PROBE_BEGIN\nM400\nG90\nG1 Z{max_layer_z + 3.0} F1200\n" +
            unload + "\nM400\n; AMS_LITE_ACTIVE_UNLOAD_PROBE_END\nM400 U1\n{endif}\n" + ordinary_change));
        // Full config alone is insufficient: GUI import updates keys not listed
        // as overrides from the system profile. Generate the normal export
        // metadata from an edited printer preset so the unload macro survives.
        presets.printers.get_edited_preset().config.set_key_value("change_filament_gcode",
            config.option("change_filament_gcode")->clone());
        presets.printers.update_dirty();
        const auto native_export = presets.full_config();
        const auto* differences = native_export.option<ConfigOptionStrings>("different_settings_to_system");
        REQUIRE(differences != nullptr);
        REQUIRE(differences->values.back().find("change_filament_gcode") != std::string::npos);
        config.set_key_value("different_settings_to_system", differences->clone());
    }

    Model model;
    auto* object = model.add_object();
    object->name = active_unload ? AMS::active_unload_probe_name :
        "AMS Lite one-swap test - slot 1, slot 2, replacement in slot 1";
    for (int band = 0; band < (active_unload ? 5 : 3); ++band) {
        TriangleMesh mesh = make_cube(12, 8, 0.3);
        mesh.translate(122.f, 124.f, float(band) * 0.3f);
        auto* volume = object->add_volume(std::move(mesh));
        volume->name = active_unload ? (band == 0 ? "Bottom - slot 1 original" :
            "Replacement - slot " + std::to_string(band % 4 + 1)) :
            (band == 0 ? "Bottom - slot 1 original" : band == 1 ? "Middle - slot 2 unchanged" : "Top - slot 1 replacement");
        volume->config.set("extruder", active_unload ? band % 4 + 1 : band == 1 ? 2 : 1);
    }
    object->add_instance();
    object->ensure_on_bed();
    auto& custom = model.plates_custom_gcodes[0];
    custom.mode = CustomGCode::MultiExtruder;
    if (!active_unload)
        custom.gcodes.push_back({0.7, CustomGCode::PausePrint, 2, "", "Replace unloaded slot 1 only. Leave slot 2 loaded."});

    Print print;
    print.set_status_silent();
    print.set_BBL_Printer(true);
    print.set_plate_index(0);
    print.set_plate_origin(Vec3d::Zero());
    print.set_extruder_filament_info(presets.get_extruder_filament_info());
    print.apply(model, config);
    const auto validation = print.validate();
    INFO(validation.string);
    REQUIRE(validation.string.empty());
    print.process();
    struct ProbeFiles {
        fs::path gcode = fs::temp_directory_path() / fs::unique_path("ams-slot-probe-%%%%-%%%%.gcode");
        fs::path project = fs::temp_directory_path() / fs::unique_path("ams-slot-probe-%%%%-%%%%.3mf");
        fs::path sliced = fs::temp_directory_path() / fs::unique_path("ams-slot-probe-%%%%-%%%%.gcode.3mf");
        fs::path edited = fs::temp_directory_path() / fs::unique_path("ams-slot-probe-edited-%%%%-%%%%.gcode");
        ~ProbeFiles() { boost::system::error_code ignored; fs::remove(gcode, ignored); fs::remove(project, ignored); fs::remove(sliced, ignored); fs::remove(edited, ignored); }
    } files;
    GCodeProcessorResult result;
    print.export_gcode(files.gcode.string(), &result, nullptr);
    REQUIRE(result.pause_printing.size() == 1);
    REQUIRE(result.pause_printing.front().layer_id == (active_unload ? 4 : 7));
    const auto pause = std::find_if(result.moves.begin(), result.moves.end(), [](const auto& move) { return move.type == EMoveType::Pause_Print; });
    REQUIRE(pause != result.moves.end());
    // The processor ignores special T255; this is the last selected virtual ID,
    // not proof that firmware actually retracted the filament.
    REQUIRE(pause->extruder_id == (active_unload ? 0 : 1));
    const auto next_extrusion = std::find_if(pause + 1, result.moves.end(), [](const auto& move) { return move.type == EMoveType::Extrude; });
    REQUIRE(next_extrusion != result.moves.end());
    REQUIRE(next_extrusion->extruder_id == (active_unload ? 1 : 0));
    std::ifstream generated(files.gcode.string(), std::ios::binary);
    const std::string gcode((std::istreambuf_iterator<char>(generated)), std::istreambuf_iterator<char>());
    generated.close(); // Windows must release the file before the round-trip export replaces it.
    if (active_unload)
        if (const char* debug = std::getenv("EXTENDED_AMS_PROBE_DEBUG"))
            std::ofstream(debug, std::ios::binary).write(gcode.data(), gcode.size());
    // Ignore escaped macro text in the configuration header: match real lines.
    const auto pause_tag = gcode.find("\n;" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Pause_Print) + "\n");
    REQUIRE(pause_tag != std::string::npos);
    const auto native_pause = gcode.find("\nM400 U1\n", pause_tag);
    REQUIRE(native_pause != std::string::npos);
    REQUIRE(gcode.find(active_unload ? "\nT1\n" : "\nT0\n", native_pause) != std::string::npos);
    if (active_unload) {
        std::istringstream guarded(gcode);
        REQUIRE(AMS::active_unload_probe_error(guarded).empty());
        const auto unload = gcode.find("\nT255\n", pause_tag);
        REQUIRE(unload != std::string::npos);
        REQUIRE(unload < native_pause);
        REQUIRE(gcode.find("M621 S255", unload) < native_pause);
        auto previous = native_pause;
        for (const auto* selection : {"\nT1\n", "\nT2\n", "\nT3\n", "\nT0\n"}) {
            previous = gcode.find(selection, previous + 1);
            REQUIRE(previous != std::string::npos);
        }
    }

    PlateData plate;
    plate.plate_index = 0;
    plate.objects_and_instances.emplace_back(0, 0);
    plate.plate_name = active_unload ? "EXPERIMENTAL v2 - unload then one pause" : "One swap - firmware 01.08.01.00";
    const std::string project_path = files.project.string();
    StoreParams params;
    params.path = project_path.c_str();
    params.model = &model;
    params.config = &config;
    params.plate_data_list.push_back(&plate);
    params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
    REQUIRE(store_bbs_3mf(params));

    DynamicPrintConfig loaded_config;
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::EnableSilent);
    Model loaded_model;
    PlateDataPtrs plates;
    std::vector<Preset*> embedded;
    bool is_bambu = false;
    Semver version;
    REQUIRE(load_bbs_3mf(project_path.c_str(), &loaded_config, &substitutions, &loaded_model, &plates,
        &embedded, &is_bambu, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence));
    REQUIRE(loaded_model.objects.size() == 1);
    CHECK(loaded_model.objects.front()->volumes.size() == (active_unload ? 5 : 3));
    REQUIRE(loaded_model.plates_custom_gcodes[0].gcodes.size() == (active_unload ? 0 : 1));
    if (!active_unload)
        CHECK(loaded_model.plates_custom_gcodes[0].gcodes.front().type == CustomGCode::PausePrint);
    CHECK(loaded_config.opt_string("printer_settings_id") == "Bambu Lab A1 0.2 nozzle");
    CHECK(loaded_config.opt_string("machine_pause_gcode") == config.opt_string("machine_pause_gcode"));
    CHECK(loaded_config.opt_string("change_filament_gcode") == config.opt_string("change_filament_gcode"));
    // Match the GUI preset-reconciliation step, not just raw 3MF decoding.
    // Native different_settings_to_system metadata decides which printer
    // overrides survive replacement by the bundled system preset.
    PresetBundle restored_presets;
    restored_presets.load_vendor_configs_from_json((resources / "profiles").string(), "BBL",
        PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent);
    if (!embedded.empty())
        restored_presets.load_project_embedded_presets(embedded, ForwardCompatibilitySubstitutionRule::EnableSilent);
    auto gui_config = loaded_config;
    Preset::normalize(gui_config);
    restored_presets.load_config_model(project_path, std::move(gui_config), version);
    auto reconciled_config = restored_presets.full_config();
    REQUIRE(reconciled_config.opt_string("change_filament_gcode") == config.opt_string("change_filament_gcode"));
    if (active_unload) {
        const auto* differences = reconciled_config.option<ConfigOptionStrings>("different_settings_to_system");
        REQUIRE(differences != nullptr);
        REQUIRE(differences->values.back().find("change_filament_gcode") != std::string::npos);
    }
    Print reopened;
    reopened.set_status_silent();
    reopened.set_BBL_Printer(true);
    reopened.set_plate_index(0);
    reopened.set_plate_origin(Vec3d::Zero());
    reopened.set_extruder_filament_info(presets.get_extruder_filament_info());
    reopened.apply(loaded_model, reconciled_config);
    reopened.process();
    GCodeProcessorResult resliced;
    reopened.export_gcode(files.gcode.string(), &resliced, nullptr);
    REQUIRE(resliced.pause_printing.size() == 1);
    REQUIRE(resliced.pause_printing.front().layer_id == (active_unload ? 4 : 7));
    const auto repeated_pause = std::find_if(resliced.moves.begin(), resliced.moves.end(), [](const auto& move) { return move.type == EMoveType::Pause_Print; });
    REQUIRE(repeated_pause != resliced.moves.end());
    REQUIRE(repeated_pause->extruder_id == (active_unload ? 0 : 1));
    // The same native sliced archive used by the GUI's Bambu Connect handoff.
    plate.printer_model_id = "A1";
    plate.nozzle_diameters = "0.2";
    plate.gcode_file = files.gcode.string();
    plate.is_sliced_valid = true;
    plate.pause_printing = resliced.pause_printing;
    plate.parse_filament_info(&resliced);
    REQUIRE(plate.slice_filaments_info.size() == filament_count);
    for (auto& filament : plate.slice_filaments_info) {
        std::string display_type;
        filament.type = config.get_filament_type(display_type, filament.id);
        filament.color = config.option<ConfigOptionStrings>("filament_colour")->get_at(filament.id);
        filament.filament_id = config.option<ConfigOptionStrings>("filament_ids")->get_at(filament.id);
    }
    plate.gcode_prediction = std::to_string(int(resliced.print_statistics.modes[0].time));
    plate.gcode_weight = std::to_string(reopened.print_statistics().total_weight);
    const std::string sliced_path = files.sliced.string();
    params.path = sliced_path.c_str();
    params.strategy = SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::WithGcode | SaveStrategy::SkipModel;
    REQUIRE(store_bbs_3mf(params));
    REQUIRE(fs::file_size(files.sliced) > 0);
    if (active_unload) {
        // Regression: the editable project must survive a prime-tower setting
        // change and reslice. A SkipModel sliced archive cannot provide this.
        auto& edited_process = restored_presets.prints.get_edited_preset().config;
        const double previous_width = edited_process.option<ConfigOptionFloat>("prime_tower_width")->value;
        edited_process.set_key_value("prime_tower_width", new ConfigOptionFloat(previous_width + 5.));
        restored_presets.prints.update_dirty();
        auto edited_config = restored_presets.full_config();
        REQUIRE(edited_config.opt_string("change_filament_gcode") == config.opt_string("change_filament_gcode"));
        reopened.apply(loaded_model, edited_config);
        REQUIRE(loaded_model.objects.size() == 1);
        REQUIRE(loaded_model.objects.front()->volumes.size() == 5);
        for (const auto* volume : loaded_model.objects.front()->volumes)
            REQUIRE_FALSE(volume->mesh().empty());
        const auto edited_validation = reopened.validate();
        INFO(edited_validation.string);
        REQUIRE(edited_validation.string.empty());
        reopened.process();
        GCodeProcessorResult edited_result;
        reopened.export_gcode(files.edited.string(), &edited_result, nullptr);
        REQUIRE(edited_result.pause_printing.size() == 1);
        REQUIRE(edited_result.pause_printing.front().layer_id == 4);
        const auto edited_pause = std::find_if(edited_result.moves.begin(), edited_result.moves.end(),
            [](const auto& move) { return move.type == EMoveType::Pause_Print; });
        REQUIRE(edited_pause != edited_result.moves.end());
        const auto edited_extrusion = std::find_if(edited_pause + 1, edited_result.moves.end(),
            [](const auto& move) { return move.type == EMoveType::Extrude; });
        REQUIRE(edited_extrusion != edited_result.moves.end());
        REQUIRE(edited_extrusion->extruder_id == 1);
    }
    release_PlateData_list(plates);
    for (auto* preset : embedded) delete preset;
    // An explicit output directory is needed to retain a human-run hardware
    // probe. Routine integration checks create only temporary local files.
    if (const char* destination = std::getenv("EXTENDED_AMS_PROBE_OUTPUT")) {
        fs::path folder = fs::absolute(destination);
        fs::create_directories(folder);
        const std::string stem = active_unload ? "A1 EXPERIMENTAL active-unload v2" : "A1 AMS Lite one-swap test";
        const auto output = folder / (stem + ".3mf");
        const auto sliced_output = folder / (stem + ".gcode.3mf");
        REQUIRE_FALSE(fs::exists(output));
        REQUIRE_FALSE(fs::exists(sliced_output));
        fs::copy_file(files.project, output);
        fs::copy_file(files.sliced, sliced_output);
    }
}

// A deliberately tiny hardware coupon: one 0.1 mm layer in four colors, one
// executable spool-swap visit that replaces all four spools, then one more 0.1 mm
// layer in four other colors. It uses the same eight virtual identities, fixed
// slot aliases, native unload block and normal A1 change macro as a full job.
TEST_CASE("A1 quick one-swap coupon uses the native eight-identity execution path", "[ExtendedAMSLite][Execution][HardwareProbe]")
{
    namespace fs = boost::filesystem;
    const auto resources = fs::path(TEST_DATA_DIR).parent_path().parent_path() / "resources";
    set_resources_dir(resources.string());
    PresetBundle presets;
    presets.load_vendor_configs_from_json((resources / "profiles").string(), "BBL",
        PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent);
    REQUIRE(presets.printers.select_preset_by_name("Bambu Lab A1 0.2 nozzle", true, true));
    REQUIRE(presets.prints.select_preset_by_name("0.10mm Standard @BBL A1 0.2 nozzle", true, true));
    REQUIRE(presets.filaments.select_preset_by_name("Generic PLA @BBL A1 0.2 nozzle", true, true));
    presets.filament_presets.assign(8, "Generic PLA @BBL A1 0.2 nozzle");
    presets.set_num_filaments(8);
    const std::vector<std::string> colors{"#F5F5F5", "#2463EB", "#202020", "#E34B4B", "#22A86A", "#F2C438", "#9755CE", "#ED8736"};
    presets.project_config.set_key_value("filament_colour", new ConfigOptionStrings(colors));
    auto config = presets.full_config();
    config.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(btPEI));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.1));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.1));
    // Keep the normal purge for the colors loaded at the swap; shorten the purges
    // between colors within one group so the coupon finishes quickly.
    std::vector<double> flush(64, 160.);
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j)
            if (i == j) flush[i * 8 + j] = 0.;
            else if (i / 4 == j / 4) flush[i * 8 + j] = 50.;
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats(flush));
    // Layer 1 prints identities 0-3, layer 2 prints identities 4-7.
    std::vector<AMS::Request> nominal;
    for (unsigned layer = 1; layer <= 2; ++layer)
        for (int id = 0; id < 4; ++id) nominal.push_back({int(layer - 1) * 4 + id, layer, nominal.size(), 0});
    const auto proposal = AMS::plan_batches(nominal, 8, std::vector<AMS::Slot>(4), {0, 1, 2, 3, 4, 5, 6, 7}, true);
    std::vector<LayerPrintSequence> other;
    for (const auto& layer : AMS::complete_layer_orders(proposal)) {
        std::vector<int> ids;
        for (int id : layer.filaments) ids.push_back(id + 1);
        if (layer.layer == 1) config.set_key_value("first_layer_print_sequence", new ConfigOptionInts(ids));
        else other.push_back({{int(layer.layer), int(layer.layer)}, ids});
    }
    int count = 0;
    std::vector<int> serialized;
    get_other_layers_print_sequence(other, count, serialized);
    config.set_key_value("other_layers_print_sequence_nums", new ConfigOptionInt(count));
    config.set_key_value("other_layers_print_sequence", new ConfigOptionInts(serialized));
    Model model;
    auto* object = model.add_object();
    object->name = "Quick one-swap coupon";
    for (int id = 0; id < 8; ++id) {
        auto mesh = make_cube(6, 6, 0.1);
        mesh.translate(110.f + float(id % 4) * 8.f, 124.f, id < 4 ? 0.f : 0.1f);
        auto* volume = object->add_volume(std::move(mesh));
        volume->config.set("extruder", id + 1);
    }
    object->add_instance();
    object->ensure_on_bed();
    Print print;
    print.set_status_silent(); print.set_BBL_Printer(true); print.set_plate_index(0); print.set_plate_origin(Vec3d::Zero());
    print.set_extruder_filament_info(presets.get_extruder_filament_info());
    print.apply(model, config);
    const auto validation = print.validate();
    INFO(validation.string);
    REQUIRE(validation.string.empty());
    print.process();
    struct Files {
        fs::path plain = fs::temp_directory_path() / fs::unique_path("ams-quick-%%%%-%%%%.gcode");
        fs::path execution = fs::temp_directory_path() / fs::unique_path("ams-quick-ready-%%%%-%%%%.gcode");
        ~Files() { boost::system::error_code ignored; fs::remove(plain, ignored); fs::remove(execution, ignored); }
    } files;
    GCodeProcessorResult baseline;
    print.export_gcode(files.plain.string(), &baseline, nullptr);
    REQUIRE(baseline.pause_printing.empty());
    REQUIRE_FALSE(baseline.filament_change_sequence.empty());
    auto requests = AMS::normalize_execution_requests(
        AMS::collect_requests(baseline.moves, EMoveType::Tool_change, EMoveType::Extrude, true), baseline.filament_change_sequence.front());
    {
        std::string sequence;
        for (const auto& request : requests) sequence += std::to_string(request.filament) + "@" + std::to_string(request.layer) + " ";
        INFO("normalized requests: " << sequence);
        // Initial selection, eight colors, plus the prime tower's carry-over of the
        // last layer-1 color at the start of layer 2.
        CHECK(requests.size() == 10);
    }
    AMS::ExecutionSpec spec;
    spec.job_id = "a100010000000002";
    spec.slots.resize(4);
    for (int i = 0; i < 8; ++i) spec.filaments.push_back({"PLA " + std::to_string(i + 1), "PLA", colors[i]});
    spec.plan = AMS::plan_fixed_batches(requests, spec.slots, proposal.filament_slots);
    REQUIRE(spec.plan.schedule.stops.size() == 1);
    REQUIRE(spec.plan.schedule.stops.front().replacements.size() == 4);
    config.set_key_value("process_notes", new ConfigOptionString(AMS::notes_with_execution("", spec)));
    print.apply(model, config);
    print.process();
    GCodeProcessorResult ready;
    print.export_gcode(files.execution.string(), &ready, nullptr);
    REQUIRE(ready.pause_printing.size() == 1);
    REQUIRE(ready.pause_printing.front().layer_id == 2);
    {
        std::ifstream generated(files.execution.string(), std::ios::binary);
        const auto error = AMS::execution_gcode_error(generated, spec);
        INFO(error);
        REQUIRE(error.empty());
    }
    std::map<unsigned, std::set<int>> extruded;
    for (const auto& move : ready.moves) if (move.type == EMoveType::Extrude && move.layer_id > 0)
        extruded[move.layer_id].insert(move.extruder_id);
    REQUIRE(extruded.size() == 2);
    CHECK(extruded[1] == std::set<int>({0, 1, 2, 3}));
    CHECK(extruded[2] == std::set<int>({3, 4, 5, 6, 7})); // 3 is the tower carry-over before the swap
    // After the pause the next extrusion must come from a newly loaded identity.
    const auto pause = std::find_if(ready.moves.begin(), ready.moves.end(), [](const auto& move) { return move.type == EMoveType::Pause_Print; });
    REQUIRE(pause != ready.moves.end());
    const auto after = std::find_if(pause + 1, ready.moves.end(), [](const auto& move) { return move.type == EMoveType::Extrude; });
    REQUIRE(after != ready.moves.end());
    CHECK(after->extruder_id >= 4);

    // Match the GUI's edited-process metadata so the schedule survives project import.
    auto& process = presets.prints.get_edited_preset().config;
    for (const auto& key : config.keys())
        if (process.has(key)) process.set_key_value(key, config.option(key)->clone());
    presets.prints.update_dirty();
    const auto native_export = presets.full_config();
    config.set_key_value("different_settings_to_system", native_export.option("different_settings_to_system")->clone());
    PlateData plate;
    plate.plate_index = 0; plate.objects_and_instances.emplace_back(0, 0);
    plate.plate_name = "Quick test - one layer, swap all four, one layer";
    for (const auto* key : {"first_layer_print_sequence", "other_layers_print_sequence_nums", "other_layers_print_sequence"})
        plate.config.set_key_value(key, config.option(key)->clone());
    const auto project = files.plain.string() + ".3mf";
    const auto sliced = files.execution.string() + ".3mf";
    struct Archives {
        std::vector<std::string> paths;
        ~Archives() { for (const auto& path : paths) { boost::system::error_code ignored; fs::remove(path, ignored); } }
    } archives{{project, sliced}};
    StoreParams params;
    params.path = project.c_str(); params.model = &model; params.config = &config;
    params.plate_data_list.push_back(&plate);
    params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
    REQUIRE(store_bbs_3mf(params));
    Model loaded_model;
    DynamicPrintConfig loaded_config;
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::EnableSilent);
    PlateDataPtrs plates;
    std::vector<Preset*> embedded;
    bool is_bambu = false;
    Semver version;
    REQUIRE(load_bbs_3mf(project.c_str(), &loaded_config, &substitutions, &loaded_model, &plates,
        &embedded, &is_bambu, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence));
    PresetBundle restored;
    restored.load_vendor_configs_from_json((resources / "profiles").string(), "BBL",
        PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent);
    if (!embedded.empty()) restored.load_project_embedded_presets(embedded, ForwardCompatibilitySubstitutionRule::EnableSilent);
    Preset::normalize(loaded_config);
    restored.load_config_model(project, std::move(loaded_config), version);
    auto imported = restored.full_config();
    REQUIRE(AMS::serialize_execution(*AMS::execution_from_notes(imported.opt_string("process_notes"))) == AMS::serialize_execution(spec));
    REQUIRE(loaded_model.objects.size() == 1);
    REQUIRE(loaded_model.objects.front()->volumes.size() == 8);
    REQUIRE(plates.size() == 1);
    imported.apply(plates.front()->config);
    Print reopened;
    reopened.set_status_silent(); reopened.set_BBL_Printer(true); reopened.set_plate_index(0); reopened.set_plate_origin(Vec3d::Zero());
    reopened.set_extruder_filament_info(restored.get_extruder_filament_info());
    reopened.apply(loaded_model, imported); reopened.process();
    GCodeProcessorResult repeated;
    reopened.export_gcode(files.execution.string(), &repeated, nullptr);
    REQUIRE(repeated.pause_printing.size() == 1);
    {
        std::ifstream reloaded(files.execution.string(), std::ios::binary);
        REQUIRE(AMS::execution_gcode_error(reloaded, spec).empty());
    }
    plate.gcode_file = files.execution.string(); plate.is_sliced_valid = true;
    plate.printer_model_id = "A1"; plate.nozzle_diameters = "0.2";
    plate.pause_printing = repeated.pause_printing; plate.parse_filament_info(&repeated);
    for (auto& filament : plate.slice_filaments_info) {
        std::string display_type;
        filament.type = config.get_filament_type(display_type, filament.id);
        filament.color = colors[filament.id];
        filament.filament_id = config.option<ConfigOptionStrings>("filament_ids")->get_at(filament.id);
    }
    REQUIRE(plate.slice_filaments_info.size() == 8);
    params.path = sliced.c_str();
    params.strategy = SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::WithGcode | SaveStrategy::SkipModel;
    REQUIRE(store_bbs_3mf(params));
    release_PlateData_list(plates);
    for (auto* preset : embedded) delete preset;
    if (const char* destination = std::getenv("EXTENDED_AMS_QUICK_OUTPUT")) {
        const auto folder = fs::absolute(destination);
        fs::create_directories(folder);
        const auto output = folder / "A1 quick spool-swap test.3mf";
        const auto fixed_output = folder / "A1 quick spool-swap test.gcode.3mf";
        REQUIRE_FALSE(fs::exists(output)); REQUIRE_FALSE(fs::exists(fixed_output));
        fs::copy_file(project, output); fs::copy_file(sliced, fixed_output);
        std::ofstream guide((folder / "SWAP GUIDE.html").string(), std::ios::binary);
        guide << AMS::format_execution_guide(spec, true);
        REQUIRE(bool(guide));
    }
}

// The smallest swap coupon: one 0.1 mm layer with two chips. Color 1 and color 2
// are forced onto the same physical slot, so the job prints color 1, unloads,
// pauses once for a single spool replacement, then prints color 2 on that layer.
TEST_CASE("A1 minimal coupon swaps one spool inside a single layer", "[ExtendedAMSLite][Execution][HardwareProbe]")
{
    namespace fs = boost::filesystem;
    const auto resources = fs::path(TEST_DATA_DIR).parent_path().parent_path() / "resources";
    set_resources_dir(resources.string());
    PresetBundle presets;
    presets.load_vendor_configs_from_json((resources / "profiles").string(), "BBL",
        PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent);
    REQUIRE(presets.printers.select_preset_by_name("Bambu Lab A1 0.2 nozzle", true, true));
    REQUIRE(presets.prints.select_preset_by_name("0.10mm Standard @BBL A1 0.2 nozzle", true, true));
    REQUIRE(presets.filaments.select_preset_by_name("Generic PLA @BBL A1 0.2 nozzle", true, true));
    presets.filament_presets.assign(2, "Generic PLA @BBL A1 0.2 nozzle");
    presets.set_num_filaments(2);
    const std::vector<std::string> colors{"#F5F5F5", "#22A86A"};
    presets.project_config.set_key_value("filament_colour", new ConfigOptionStrings(colors));
    auto config = presets.full_config();
    config.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(btPEI));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.1));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.1));
    // The failed eight-color job used 280 for every pair; keep that purge amount.
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats(std::vector<double>{0., 280., 280., 0.}));
    config.set_key_value("first_layer_print_sequence", new ConfigOptionInts(std::vector<int>{1, 2}));
    config.set_key_value("other_layers_print_sequence_nums", new ConfigOptionInt(0));
    config.set_key_value("other_layers_print_sequence", new ConfigOptionInts(std::vector<int>{}));
    Model model;
    auto* object = model.add_object();
    object->name = "Minimal one-swap coupon";
    for (int id = 0; id < 2; ++id) {
        auto mesh = make_cube(8, 8, 0.1);
        mesh.translate(112.f + float(id) * 12.f, 124.f, 0.f);
        auto* volume = object->add_volume(std::move(mesh));
        volume->config.set("extruder", id + 1);
    }
    object->add_instance();
    object->ensure_on_bed();
    Print print;
    print.set_status_silent(); print.set_BBL_Printer(true); print.set_plate_index(0); print.set_plate_origin(Vec3d::Zero());
    print.set_extruder_filament_info(presets.get_extruder_filament_info());
    print.apply(model, config);
    const auto validation = print.validate();
    INFO(validation.string);
    REQUIRE(validation.string.empty());
    print.process();
    struct Files {
        fs::path plain = fs::temp_directory_path() / fs::unique_path("ams-minimal-%%%%-%%%%.gcode");
        fs::path execution = fs::temp_directory_path() / fs::unique_path("ams-minimal-ready-%%%%-%%%%.gcode");
        ~Files() { boost::system::error_code ignored; fs::remove(plain, ignored); fs::remove(execution, ignored); }
    } files;
    GCodeProcessorResult baseline;
    print.export_gcode(files.plain.string(), &baseline, nullptr);
    REQUIRE(baseline.pause_printing.empty());
    REQUIRE_FALSE(baseline.filament_change_sequence.empty());
    auto requests = AMS::normalize_execution_requests(
        AMS::collect_requests(baseline.moves, EMoveType::Tool_change, EMoveType::Extrude, true), baseline.filament_change_sequence.front());
    {
        std::string sequence;
        for (const auto& request : requests) sequence += std::to_string(request.filament) + "@" + std::to_string(request.layer) + " ";
        INFO("normalized requests: " << sequence);
        REQUIRE(requests.size() >= 2);
        CHECK(requests.front().filament == 0);
        CHECK(requests.back().filament == 1);
    }
    AMS::ExecutionSpec spec;
    spec.job_id = "a100010000000003";
    spec.slots.resize(4);
    for (int i = 0; i < 2; ++i) spec.filaments.push_back({"PLA " + std::to_string(i + 1), "PLA", colors[i]});
    // Both identities share physical slot 1: that forces exactly one replacement.
    spec.plan = AMS::plan_fixed_batches(requests, spec.slots, std::vector<int>{0, 0});
    REQUIRE(spec.plan.schedule.stops.size() == 1);
    REQUIRE(spec.plan.schedule.stops.front().replacements.size() == 1);
    config.set_key_value("process_notes", new ConfigOptionString(AMS::notes_with_execution("", spec)));
    print.apply(model, config);
    print.process();
    GCodeProcessorResult ready;
    print.export_gcode(files.execution.string(), &ready, nullptr);
    REQUIRE(ready.pause_printing.size() == 1);
    REQUIRE(ready.pause_printing.front().layer_id == 1);
    {
        std::ifstream generated(files.execution.string(), std::ios::binary);
        const auto error = AMS::execution_gcode_error(generated, spec);
        INFO(error);
        REQUIRE(error.empty());
    }
    std::map<unsigned, std::set<int>> extruded;
    for (const auto& move : ready.moves) if (move.type == EMoveType::Extrude && move.layer_id > 0)
        extruded[move.layer_id].insert(move.extruder_id);
    REQUIRE(extruded.size() == 1);
    CHECK(extruded[1] == std::set<int>({0, 1}));
    const auto pause = std::find_if(ready.moves.begin(), ready.moves.end(), [](const auto& move) { return move.type == EMoveType::Pause_Print; });
    REQUIRE(pause != ready.moves.end());
    const auto after = std::find_if(pause + 1, ready.moves.end(), [](const auto& move) { return move.type == EMoveType::Extrude; });
    REQUIRE(after != ready.moves.end());
    CHECK(after->extruder_id == 1);

    // Match the GUI's edited-process metadata so the schedule survives project import.
    auto& process = presets.prints.get_edited_preset().config;
    for (const auto& key : config.keys())
        if (process.has(key)) process.set_key_value(key, config.option(key)->clone());
    presets.prints.update_dirty();
    const auto native_export = presets.full_config();
    config.set_key_value("different_settings_to_system", native_export.option("different_settings_to_system")->clone());
    PlateData plate;
    plate.plate_index = 0; plate.objects_and_instances.emplace_back(0, 0);
    plate.plate_name = "Minimal test - one layer, one spool swap";
    for (const auto* key : {"first_layer_print_sequence", "other_layers_print_sequence_nums", "other_layers_print_sequence"})
        plate.config.set_key_value(key, config.option(key)->clone());
    const auto project = files.plain.string() + ".3mf";
    const auto sliced = files.execution.string() + ".3mf";
    struct Archives {
        std::vector<std::string> paths;
        ~Archives() { for (const auto& path : paths) { boost::system::error_code ignored; fs::remove(path, ignored); } }
    } archives{{project, sliced}};
    StoreParams params;
    params.path = project.c_str(); params.model = &model; params.config = &config;
    params.plate_data_list.push_back(&plate);
    params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
    REQUIRE(store_bbs_3mf(params));
    Model loaded_model;
    DynamicPrintConfig loaded_config;
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::EnableSilent);
    PlateDataPtrs plates;
    std::vector<Preset*> embedded;
    bool is_bambu = false;
    Semver version;
    REQUIRE(load_bbs_3mf(project.c_str(), &loaded_config, &substitutions, &loaded_model, &plates,
        &embedded, &is_bambu, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence));
    PresetBundle restored;
    restored.load_vendor_configs_from_json((resources / "profiles").string(), "BBL",
        PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent);
    if (!embedded.empty()) restored.load_project_embedded_presets(embedded, ForwardCompatibilitySubstitutionRule::EnableSilent);
    Preset::normalize(loaded_config);
    restored.load_config_model(project, std::move(loaded_config), version);
    auto imported = restored.full_config();
    REQUIRE(AMS::serialize_execution(*AMS::execution_from_notes(imported.opt_string("process_notes"))) == AMS::serialize_execution(spec));
    REQUIRE(loaded_model.objects.size() == 1);
    REQUIRE(loaded_model.objects.front()->volumes.size() == 2);
    REQUIRE(plates.size() == 1);
    imported.apply(plates.front()->config);
    Print reopened;
    reopened.set_status_silent(); reopened.set_BBL_Printer(true); reopened.set_plate_index(0); reopened.set_plate_origin(Vec3d::Zero());
    reopened.set_extruder_filament_info(restored.get_extruder_filament_info());
    reopened.apply(loaded_model, imported); reopened.process();
    GCodeProcessorResult repeated;
    reopened.export_gcode(files.execution.string(), &repeated, nullptr);
    REQUIRE(repeated.pause_printing.size() == 1);
    {
        std::ifstream reloaded(files.execution.string(), std::ios::binary);
        REQUIRE(AMS::execution_gcode_error(reloaded, spec).empty());
    }
    plate.gcode_file = files.execution.string(); plate.is_sliced_valid = true;
    plate.printer_model_id = "A1"; plate.nozzle_diameters = "0.2";
    plate.pause_printing = repeated.pause_printing; plate.parse_filament_info(&repeated);
    for (auto& filament : plate.slice_filaments_info) {
        std::string display_type;
        filament.type = config.get_filament_type(display_type, filament.id);
        filament.color = colors[filament.id];
        filament.filament_id = config.option<ConfigOptionStrings>("filament_ids")->get_at(filament.id);
    }
    REQUIRE(plate.slice_filaments_info.size() == 2);
    params.path = sliced.c_str();
    params.strategy = SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::WithGcode | SaveStrategy::SkipModel;
    REQUIRE(store_bbs_3mf(params));
    release_PlateData_list(plates);
    for (auto* preset : embedded) delete preset;
    if (const char* destination = std::getenv("EXTENDED_AMS_MINIMAL_OUTPUT")) {
        const auto folder = fs::absolute(destination);
        fs::create_directories(folder);
        const auto output = folder / "A1 minimal spool-swap test.3mf";
        const auto fixed_output = folder / "A1 minimal spool-swap test.gcode.3mf";
        REQUIRE_FALSE(fs::exists(output)); REQUIRE_FALSE(fs::exists(fixed_output));
        fs::copy_file(project, output); fs::copy_file(sliced, fixed_output);
        std::ofstream guide((folder / "SWAP GUIDE.html").string(), std::ios::binary);
        guide << AMS::format_execution_guide(spec, true);
        REQUIRE(bool(guide));
    }
}

// A color-layer swatch plate: six stack variants, one tile each, printed face down. The face is an
// orange | blue background with the stack label written in white, mirrored so it reads correctly
// when the printed face is looked at. Only four filaments are used (black, white, orange, light
// blue), so it needs no spool swap.
TEST_CASE("A1 color-layer swatch plate prints with four filaments and no swap", "[ExtendedAMSLite][HardwareProbe]")
{
    namespace fs = boost::filesystem;
    const auto resources = fs::path(TEST_DATA_DIR).parent_path().parent_path() / "resources";
    set_resources_dir(resources.string());
    PresetBundle presets;
    presets.load_vendor_configs_from_json((resources / "profiles").string(), "BBL",
        PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent);
    REQUIRE(presets.printers.select_preset_by_name("Bambu Lab A1 0.2 nozzle", true, true));
    REQUIRE(presets.prints.select_preset_by_name("0.10mm Standard @BBL A1 0.2 nozzle", true, true));
    REQUIRE(presets.filaments.select_preset_by_name("Bambu PLA Basic @BBL A1 0.2 nozzle", true, true));
    presets.filament_presets.assign(4, "Bambu PLA Basic @BBL A1 0.2 nozzle");
    presets.set_num_filaments(4);
    const std::vector<std::string> colors{"#000000", "#FEFEFE", "#F28C1D", "#4588C8"};
    presets.project_config.set_key_value("filament_colour", new ConfigOptionStrings(colors));
    auto config = presets.full_config();
    config.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(btPEI));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.1));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.1));
    std::vector<double> flush(16, 140.);
    for (int i = 0; i < 4; ++i) flush[i * 4 + i] = 0.;
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats(flush));

    struct Stack { int color_layers, white_layers; };
    const std::vector<Stack> rows{{4, 0}, {3, 0}, {3, 1}, {2, 0}, {2, 2}, {2, 1}, {1, 2}};
    const char* const labels[] = {"4C", "3C", "3C+1W", "2C", "2C+2W", "2C+1W", "1C+2W"};
    constexpr int black = 1, white = 2, orange = 3, blue = 4; // extruder numbers
    constexpr int columns = 36, pixel_rows = 15, text_rows = 7;
    const double layer = 0.1, total = 0.8, pixel = 0.8, x0 = 108., pitch = 15.;
    // 5x7 pixel glyphs for the stack labels.
    const std::map<char, std::array<const char*, 7>> font{
        {'1', {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
        {'2', {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}},
        {'3', {".###.", "#...#", "....#", "..##.", "....#", "#...#", ".###."}},
        {'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
        {'C', {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}},
        {'W', {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"}},
        {'+', {".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."}}};
    Model model;
    auto* object = model.add_object();
    object->name = "Color layer swatch plate";
    const auto add = [&](double x, double y, double z0, double z1, double w, double d, int extruder, const std::string& name) {
        auto mesh = make_cube(w, d, z1 - z0);
        mesh.translate(float(x), float(y), float(z0));
        auto* volume = object->add_volume(std::move(mesh));
        volume->name = name;
        volume->config.set("extruder", extruder);
    };
    struct Run { int begin, end, extruder; bool operator==(const Run& o) const { return begin == o.begin && end == o.end && extruder == o.extruder; } };
    for (std::size_t r = 0; r < rows.size(); ++r) {
        const std::string text = labels[r], name = "Stack " + text;
        const double y = 70. + double(r) * pitch;
        const double color_top = rows[r].color_layers * layer;
        // White text pixels. The face is looked at from below, so the label is mirrored in X.
        const int text_columns = int(text.size()) * 6 - 1, text_start = (columns - text_columns) / 2, text_row0 = (pixel_rows - text_rows) / 2;
        std::vector<std::vector<bool>> is_text(std::size_t(pixel_rows), std::vector<bool>(std::size_t(columns), false));
        for (std::size_t k = 0; k < text.size(); ++k)
            for (int gr = 0; gr < text_rows; ++gr)
                for (int gc = 0; gc < 5; ++gc)
                    if (font.at(text[k])[std::size_t(gr)][gc] == '#') {
                        const int read_column = int(k) * 6 + gc;
                        is_text[std::size_t(text_row0 + (text_rows - 1 - gr))][std::size_t(text_start + text_columns - 1 - read_column)] = true;
                    }
        // Face background: orange on the left half, blue on the right half, white where the text is.
        // Identical neighbouring pixel rows are merged so no volumes overlap and the count stays small.
        std::vector<std::vector<Run>> patterns;
        for (int ry = 0; ry < pixel_rows; ++ry) {
            std::vector<Run> runs;
            for (int col = 0; col < columns;) {
                const auto extruder_at = [&](int c) { return is_text[std::size_t(ry)][std::size_t(c)] ? white : (c < columns / 2 ? orange : blue); };
                int end = col;
                while (end < columns && extruder_at(end) == extruder_at(col)) ++end;
                runs.push_back({col, end, extruder_at(col)});
                col = end;
            }
            patterns.push_back(std::move(runs));
        }
        for (int ry = 0; ry < pixel_rows;) {
            int end = ry;
            while (end < pixel_rows && patterns[std::size_t(end)] == patterns[std::size_t(ry)]) ++end;
            for (const auto& run : patterns[std::size_t(ry)])
                add(x0 + double(run.begin) * pixel, y + double(ry) * pixel, 0., color_top,
                    double(run.end - run.begin) * pixel, double(end - ry) * pixel, run.extruder, name + " face");
            ry = end;
        }
        const double tile_width = double(columns) * pixel, tile_depth = double(pixel_rows) * pixel;
        double z = color_top;
        if (rows[r].white_layers > 0) {
            const double white_top = z + rows[r].white_layers * layer;
            add(x0, y, z, white_top, tile_width, tile_depth, white, name + " white backing");
            z = white_top;
        }
        add(x0, y, z, total, tile_width, tile_depth, black, name + " black body");
    }
    object->add_instance();
    object->ensure_on_bed();
    Print print;
    print.set_status_silent(); print.set_BBL_Printer(true); print.set_plate_index(0); print.set_plate_origin(Vec3d::Zero());
    print.set_extruder_filament_info(presets.get_extruder_filament_info());
    print.apply(model, config);
    const auto validation = print.validate();
    INFO(validation.string);
    REQUIRE(validation.string.empty());
    print.process();
    const auto gcode_path = fs::temp_directory_path() / fs::unique_path("ams-swatch-%%%%-%%%%.gcode");
    struct Cleanup { fs::path path; ~Cleanup() { boost::system::error_code ignored; fs::remove(path, ignored); } } cleanup{gcode_path};
    GCodeProcessorResult result;
    print.export_gcode(gcode_path.string(), &result, nullptr);
    REQUIRE(result.pause_printing.empty());
    std::map<unsigned, std::set<int>> by_layer;
    for (const auto& move : result.moves) if (move.type == EMoveType::Extrude && move.layer_id > 0)
        by_layer[move.layer_id].insert(move.extruder_id);
    REQUIRE(by_layer.size() == 8);
    // Layer 1 is the three face colors; black first appears behind the shortest stacks and
    // the last layers are black only.
    CHECK(by_layer[1] == std::set<int>({1, 2, 3}));
    CHECK(by_layer[3].count(0) == 1);
    CHECK(by_layer[8] == std::set<int>({0}));
    std::string estimate;
    {
        std::ifstream in(gcode_path.string(), std::ios::binary);
        std::string line;
        while (std::getline(in, line)) if (line.find("total estimated time") != std::string::npos) { estimate = line; break; }
    }
    INFO(estimate);
    CHECK_FALSE(estimate.empty());

    auto& process = presets.prints.get_edited_preset().config;
    for (const auto& key : config.keys())
        if (process.has(key)) process.set_key_value(key, config.option(key)->clone());
    presets.prints.update_dirty();
    const auto native_export = presets.full_config();
    config.set_key_value("different_settings_to_system", native_export.option("different_settings_to_system")->clone());
    PlateData plate;
    plate.plate_index = 0; plate.objects_and_instances.emplace_back(0, 0);
    plate.plate_name = "Color layer swatch plate";
    const auto project = gcode_path.string() + ".3mf";
    struct Archive { std::string path; ~Archive() { boost::system::error_code ignored; fs::remove(path, ignored); } } archive{project};
    StoreParams params;
    params.path = project.c_str(); params.model = &model; params.config = &config;
    params.plate_data_list.push_back(&plate);
    params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
    REQUIRE(store_bbs_3mf(params));
    Model loaded_model;
    DynamicPrintConfig loaded_config;
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::EnableSilent);
    PlateDataPtrs plates;
    std::vector<Preset*> embedded;
    bool is_bambu = false;
    Semver version;
    REQUIRE(load_bbs_3mf(project.c_str(), &loaded_config, &substitutions, &loaded_model, &plates,
        &embedded, &is_bambu, &version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence));
    REQUIRE(loaded_model.objects.size() == 1);
    CHECK(loaded_model.objects.front()->volumes.size() == object->volumes.size());
    release_PlateData_list(plates);
    for (auto* preset : embedded) delete preset;
    if (const char* destination = std::getenv("EXTENDED_AMS_SWATCH_OUTPUT")) {
        const auto folder = fs::absolute(destination);
        fs::create_directories(folder);
        const auto output = folder / "Color layer swatch plate.3mf";
        REQUIRE_FALSE(fs::exists(output));
        fs::copy_file(project, output);
        std::ofstream((folder / "estimate.txt").string(), std::ios::binary) << estimate << " volumes=" << object->volumes.size() << "\n";
    }
}

// The "White layer" feature: layers behind the colored face are recolored by one modifier volume,
// even where parts carry their own filament, and the model, prime tower and ordering follow.
TEST_CASE("White layer recolors the layers behind the color face", "[ExtendedAMSLite][WhiteLayer]")
{
    namespace fs = boost::filesystem;
    const auto resources = fs::path(TEST_DATA_DIR).parent_path().parent_path() / "resources";
    set_resources_dir(resources.string());
    PresetBundle presets;
    presets.load_vendor_configs_from_json((resources / "profiles").string(), "BBL",
        PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent);
    REQUIRE(presets.printers.select_preset_by_name("Bambu Lab A1 0.2 nozzle", true, true));
    REQUIRE(presets.prints.select_preset_by_name("0.10mm Standard @BBL A1 0.2 nozzle", true, true));
    REQUIRE(presets.filaments.select_preset_by_name("Bambu PLA Basic @BBL A1 0.2 nozzle", true, true));
    presets.filament_presets.assign(4, "Bambu PLA Basic @BBL A1 0.2 nozzle");
    presets.set_num_filaments(4);
    const std::vector<std::string> colors{"#000000", "#FEFEFE", "#F28C1D", "#4588C8"};
    presets.project_config.set_key_value("filament_colour", new ConfigOptionStrings(colors));
    auto config = presets.full_config();
    config.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(btPEI));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.1));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.1));
    std::vector<double> flush(16, 100.);
    for (int i = 0; i < 4; ++i) flush[i * 4 + i] = 0.;
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats(flush));

    // A black 1.2 mm body whose 0.4 mm face is orange | blue, like a generated keychain.
    Model model;
    auto* object = model.add_object();
    object->name = "Face";
    const auto add = [&](double x, double w, double h, int extruder) {
        auto mesh = make_cube(w, 20., h);
        mesh.translate(float(x), 0.f, 0.f);
        auto* volume = object->add_volume(std::move(mesh));
        volume->config.set("extruder", extruder);
    };
    add(100., 30., 1.2, 1);
    add(100., 15., 0.4, 3);
    add(115., 15., 0.4, 4);
    object->add_instance();
    object->ensure_on_bed();

    WhiteLayer::Options options;
    options.color_layers = 3; options.white_layers = 1; options.filament = 2;
    const auto plan = WhiteLayer::plan(*object, 0, config, options);
    CHECK(plan.total_layers == 12);
    CHECK(plan.bottom_z == Approx(0.3).margin(0.001));
    CHECK(plan.top_z == Approx(0.4).margin(0.001));
    options.color_layers = 12;
    CHECK_THROWS(WhiteLayer::plan(*object, 0, config, options)); // no room left for a white layer
    WhiteLayer::apply(*object, 0, plan);
    REQUIRE(WhiteLayer::has_white_layer(*object));
    std::size_t modifiers = 0;
    for (const auto* v : object->volumes) if (v->is_modifier()) { ++modifiers; CHECK(WhiteLayer::options_from_volume(*v).filament == 2); }
    CHECK(modifiers == 1);

    Print print;
    print.set_status_silent(); print.set_BBL_Printer(true); print.set_plate_index(0); print.set_plate_origin(Vec3d::Zero());
    print.set_extruder_filament_info(presets.get_extruder_filament_info());
    print.apply(model, config);
    const auto validation = print.validate();
    INFO(validation.string);
    REQUIRE(validation.string.empty());
    print.process();
    const auto gcode_path = fs::temp_directory_path() / fs::unique_path("ams-white-%%%%-%%%%.gcode");
    struct Cleanup { fs::path path; ~Cleanup() { boost::system::error_code ignored; fs::remove(path, ignored); } } cleanup{gcode_path};
    GCodeProcessorResult result;
    print.export_gcode(gcode_path.string(), &result, nullptr);
    // The prime tower may keep the previous tool for a moment at a layer start, so only the model's
    // own extrusions say which filament each layer of the part uses.
    std::map<unsigned, std::set<int>> by_layer;
    for (const auto& move : result.moves) if (move.type == EMoveType::Extrude && move.layer_id > 0 && move.extrusion_role != erWipeTower)
        by_layer[move.layer_id].insert(move.extruder_id);
    REQUIRE(by_layer.size() == 12);
    for (unsigned layer = 1; layer <= 3; ++layer) CHECK(by_layer[layer] == std::set<int>({2, 3})); // orange and blue
    CHECK(by_layer[4] == std::set<int>({1})); // white only, over the whole footprint
    for (unsigned layer = 5; layer <= 12; ++layer) CHECK(by_layer[layer] == std::set<int>({0})); // black body

    WhiteLayer::remove(*object);
    CHECK_FALSE(WhiteLayer::has_white_layer(*object));
    CHECK(object->volumes.size() == 3);
}
