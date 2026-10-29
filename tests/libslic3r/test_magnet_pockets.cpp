#include <catch2/catch.hpp>
#include "libslic3r/MagnetPockets.hpp"
#include "libslic3r/ExtendedAMSLite/Execution.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Utils.hpp"
#include <boost/filesystem.hpp>
#include <fstream>
#include <sstream>

using namespace Slic3r;
namespace MP = Slic3r::MagnetPockets;
namespace AMS = Slic3r::ExtendedAMSLite;
namespace fs = boost::filesystem;
namespace {
void load_magnet_presets(PresetBundle& presets, unsigned colors = 1)
{
    const auto resources = fs::path(TEST_DATA_DIR).parent_path().parent_path() / "resources";
    set_resources_dir(resources.string());
    presets.load_vendor_configs_from_json((resources / "profiles").string(), "BBL", PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent);
    REQUIRE(presets.printers.select_preset_by_name("Bambu Lab A1 0.2 nozzle", true, true));
    REQUIRE(presets.prints.select_preset_by_name("0.10mm Standard @BBL A1 0.2 nozzle", true, true));
    REQUIRE(presets.filaments.select_preset_by_name("Generic PLA @BBL A1 0.2 nozzle", true, true));
    presets.filament_presets.assign(colors, "Generic PLA @BBL A1 0.2 nozzle"); presets.set_num_filaments(colors);
}
ModelObject* keychain(Model& model, unsigned colors = 1)
{
    auto* object = model.add_object(); object->name = "Image keychain magnet regression";
    for (unsigned i = 0; i < colors; ++i) {
        auto mesh = make_cube(60. / colors, 40., 3.);
        mesh.translate(float(i * 60. / colors + 90.), 100.f, 0.f);
        auto* volume = object->add_volume(std::move(mesh)); volume->config.set("extruder", int(i + 1));
    }
    object->add_instance(); object->ensure_on_bed(); return object;
}
struct Files {
    fs::path gcode = fs::temp_directory_path() / fs::unique_path("magnet-%%%%-%%%%.gcode");
    fs::path project = fs::temp_directory_path() / fs::unique_path("magnet-%%%%-%%%%.3mf");
    ~Files() { boost::system::error_code ignored; fs::remove(gcode, ignored); fs::remove(project, ignored); }
};
void setup_print(Print& print, const PresetBundle& presets)
{
    print.set_status_silent(); print.set_BBL_Printer(true); print.set_plate_index(0); print.set_plate_origin(Vec3d::Zero());
    print.set_extruder_filament_info(presets.get_extruder_filament_info());
}
}

TEST_CASE("Magnet pockets use physical clearances, native layer heights and symmetric positions", "[MagnetPockets]")
{
    PresetBundle presets; load_magnet_presets(presets);
    auto config = presets.full_config();
    Model model; auto* object = keychain(model);
    MP::Options options;
    auto p = MP::plan(*object, 0, config, options);
    CHECK(p.diameter == Approx(8.2)); CHECK(p.depth == Approx(2.1));
    CHECK(p.top_z == Approx(2.8).margin(0.0001)); CHECK(p.bottom_z == Approx(0.7).margin(0.0001));
    CHECK((p.top_z + p.bottom_z) / 2. == Approx(1.75).margin(0.0001));
    CHECK(p.pause_z == Approx(2.9).margin(0.0001)); CHECK(p.total_layers == 30); CHECK(p.cover_layer == 29);
    REQUIRE(p.centers.size() == 2);
    CHECK((p.centers[0] + p.centers[1]).x() / 2. == Approx(120.));
    CHECK((p.centers[0] + p.centers[1]).y() / 2. == Approx(120.));
    CHECK(std::abs(p.centers[0].y() - p.centers[1].y()) < 0.5);
    for (unsigned count : {1u, 3u, 4u, 5u, 6u, 9u, 12u}) {
        options.count = count;
        auto layout = MP::plan(*object, 0, config, options);
        REQUIRE(layout.centers.size() == count);
        Vec2d sum = Vec2d::Zero(); for (const auto& c : layout.centers) sum += c;
        CHECK(sum.x() / count == Approx(120.)); CHECK(sum.y() / count == Approx(120.));
    }
    options.count = 2;
    MP::apply(*object, 0, p);
    CHECK(std::count_if(object->volumes.begin(), object->volumes.end(), [](const ModelVolume* v) { return MP::is_pocket(*v); }) == 2);
    for (auto* v : object->volumes) if (MP::is_pocket(*v)) {
        const auto bb = v->mesh().transformed_bounding_box(object->instances[0]->get_matrix() * v->get_matrix());
        CHECK(bb.size().x() == Approx(8.2).margin(0.001)); CHECK(bb.size().z() == Approx(2.1).margin(0.001));
        CHECK(bb.center().z() == Approx(1.75).margin(0.001));
        v->name = "My movable magnet";
        CHECK(MP::is_pocket(*v)); // renaming cannot silently remove pause metadata
    }
    MP::sync_pauses(model, 0, {0}, config);
    REQUIRE(model.plates_custom_gcodes[0].gcodes.size() == 1);
    CHECK(MP::insertion_heights(model.plates_custom_gcodes[0]).front() == Approx(2.9).margin(0.001));
    MP::sync_pauses(model, 0, {0}, config); CHECK(model.plates_custom_gcodes[0].gcodes.size() == 1);
    auto changed = config; changed.set_key_value("layer_height", new ConfigOptionFloat(0.15));
    auto updated = MP::plan(*object, 0, changed, options, MP::pocket_centers(*object, 0));
    CHECK(updated.top_z < p.top_z - 0.01); CHECK(updated.centers == p.centers);
    MP::remove(*object); MP::sync_pauses(model, 0, {0}, config);
    CHECK(object->volumes.size() == 1); CHECK(model.plates_custom_gcodes[0].gcodes.empty());
    // Existing user pauses at this height are adopted once, then restored when
    // removing the feature. Other ticks and messages are never discarded.
    model.plates_custom_gcodes[0].gcodes.push_back({p.pause_z, CustomGCode::PausePrint, 0, "", "My original pause"});
    model.plates_custom_gcodes[0].gcodes.push_back({0.5, CustomGCode::PausePrint, 0, "", "Other pause"});
    MP::apply(*object, 0, p); MP::sync_pauses(model, 0, {0}, config);
    CHECK(model.plates_custom_gcodes[0].gcodes.size() == 2);
    MP::remove(*object); MP::sync_pauses(model, 0, {0}, config);
    REQUIRE(model.plates_custom_gcodes[0].gcodes.size() == 2);
    CHECK(model.plates_custom_gcodes[0].gcodes.back().extra == "My original pause");
    options.thickness = 3.; CHECK_THROWS(MP::plan(*object, 0, config, options));
    options.thickness = 2.; options.count = 1000; CHECK_THROWS(MP::plan(*object, 0, config, options));
    options.count = 2;
    CHECK_THROWS(MP::plan(*object, 0, config, options, {Vec2d(90,100), Vec2d(95,105)}));
    // A pre-existing center hole must remain a hole in placement calculations.
    auto hole = make_cylinder(6., 3.); hole.translate(120.,120.,0.);
    auto* negative = object->add_volume(std::move(hole), ModelVolumeType::NEGATIVE_VOLUME);
    (void)negative;
    auto around_hole = MP::plan(*object, 0, config, options);
    for (const auto& c : around_hole.centers) CHECK((c-Vec2d(120.,120.)).norm() >= 11.);
    auto raised_logo = make_cube(2.,2.,0.2); raised_logo.translate(119.,119.,3.);
    object->add_volume(std::move(raised_logo));
    CHECK_THROWS(MP::plan(*object, 0, config, options)); // no full roof over any magnet
    Model flipped;
    auto* face_down = keychain(flipped);
    face_down->instances[0]->set_rotation(Vec3d(PI,0.,0.));
    face_down->ensure_on_bed();
    const auto face_down_plan = MP::plan(*face_down, 0, config, options);
    MP::apply(*face_down, 0, face_down_plan);
    for (const auto* v : face_down->volumes) if (MP::is_pocket(*v)) {
        const auto bb = v->mesh().transformed_bounding_box(face_down->instances[0]->get_matrix() * v->get_matrix());
        CHECK(bb.center().z() == Approx(1.75).margin(0.001));
        CHECK(bb.size().x() == Approx(8.2).margin(0.001));
    }
}

TEST_CASE("Real A1 slicing leaves magnet cavities open until one insertion pause and two roof layers", "[MagnetPockets][MagnetIntegration]")
{
    const bool extended_ams = GENERATE(false, true);
    INFO((extended_ams ? "Eight colors with independent spool visits" : "Ordinary one-color keychain"));
    PresetBundle presets; load_magnet_presets(presets, extended_ams ? 8 : 1);
    auto config = presets.full_config();
    config.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(btPTE));
    config.set_key_value("enable_support", new ConfigOptionBool(false));
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(extended_ams));
    if (extended_ams) {
        config.set_key_value("filament_colour", new ConfigOptionStrings({"#202020", "#2463EB", "#E34B4B", "#F5F5F5", "#22A86A", "#F2C438", "#9755CE", "#ED8736"}));
        std::vector<double> flush(64, 40.); for (int i=0;i<8;++i) flush[8*i+i]=0.;
        config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats(flush));
    }
    Model model; auto* object = keychain(model, extended_ams ? 8 : 1);
    MP::Options options;
    auto p = MP::plan(*object, 0, config, options);
    MP::apply(*object, 0, p); MP::sync_pauses(model, 0, {0}, config);
    Print print; setup_print(print, presets); print.apply(model, config);
    const auto validation = print.validate(); INFO(validation.string); REQUIRE(validation.string.empty());
    print.process(); CHECK_NOTHROW(MP::validate_print(print));
    REQUIRE(print.objects().size() == 1); REQUIRE(print.objects().front()->layers().size() == 30);
    auto hole_count = [](const Layer* layer) { std::size_t count=0; for (const auto& island : layer->lslices) count += island.holes.size(); return count; };
    CHECK(hole_count(print.objects().front()->layers()[27]) == 2);
    CHECK(hole_count(print.objects().front()->layers()[28]) == 0);
    CHECK(hole_count(print.objects().front()->layers()[29]) == 0);
    Files files;
    GCodeProcessorResult baseline; print.export_gcode(files.gcode.string(), &baseline, nullptr);
    REQUIRE(baseline.pause_printing.size() == 1);
    CHECK(baseline.pause_printing.front().layer_id == 29);
    AMS::ExecutionSpec spec;
    if (extended_ams) {
        auto requests = AMS::normalize_execution_requests(AMS::collect_requests(baseline.moves, EMoveType::Tool_change, EMoveType::Extrude, true), baseline.filament_change_sequence.front());
        spec.job_id = "a10002000000abcd"; spec.slots.resize(4);
        for (int i=0;i<8;++i) spec.filaments.push_back({"Color " + std::to_string(i+1), "PLA", config.option<ConfigOptionStrings>("filament_colour")->get_at(i)});
        spec.plan = AMS::plan_batches(requests, 8, spec.slots);
        spec.magnet_pauses = MP::insertion_heights(model.plates_custom_gcodes[0]);
        REQUIRE_FALSE(spec.plan.schedule.stops.empty());
        config.set_key_value("process_notes", new ConfigOptionString(AMS::notes_with_execution("", spec)));
        print.apply(model, config); print.process(); print.export_gcode(files.gcode.string(), &baseline, nullptr);
        CHECK(baseline.pause_printing.size() == spec.plan.schedule.stops.size() + 1);
        std::ifstream guarded(files.gcode.string(), std::ios::binary);
        const auto error = AMS::execution_gcode_error(guarded, spec); INFO(error); REQUIRE(error.empty());
    }
    std::ifstream generated(files.gcode.string(), std::ios::binary);
    const std::string gcode{std::istreambuf_iterator<char>(generated), {}};
    CHECK(gcode.find("; MAGNET_INSERT_BEGIN 2.900000") != std::string::npos);
    CHECK(gcode.find("; MAGNET_INSERT_END") != std::string::npos);
    generated.close();
    // The negative-volume metadata and native pause must survive normal 3MF.
    PlateData plate; plate.plate_index = 0; plate.objects_and_instances.emplace_back(0,0);
    StoreParams params; const auto path = files.project.string(); params.path = path.c_str(); params.model = &model; params.config = &config;
    params.plate_data_list.push_back(&plate); params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
    REQUIRE(store_bbs_3mf(params));
    if (!extended_ams) if (const char* destination = std::getenv("MAGNET_POCKET_OUTPUT")) {
        const auto folder = fs::absolute(destination); fs::create_directories(folder);
        const auto output = folder / "Magnet pockets demo.3mf";
        REQUIRE_FALSE(fs::exists(output)); fs::copy_file(files.project, output);
    }
    Model loaded; DynamicPrintConfig loaded_config; PlateDataPtrs plates; std::vector<Preset*> embedded;
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::EnableSilent); bool is_bambu=false; Semver version;
    REQUIRE(load_bbs_3mf(path.c_str(), &loaded_config, &substitutions, &loaded, &plates, &embedded, &is_bambu, &version, nullptr,
        LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::Silence));
    REQUIRE(loaded.objects.size() == 1);
    CHECK(MP::pocket_centers(*loaded.objects[0], 0).size() == 2);
    CHECK(MP::insertion_heights(loaded.plates_custom_gcodes[0]).size() == 1);
    Print reopened; setup_print(reopened, presets); reopened.apply(loaded, loaded_config); reopened.process();
    CHECK_NOTHROW(MP::validate_print(reopened));
    GCodeProcessorResult repeated; reopened.export_gcode(files.gcode.string(), &repeated, nullptr);
    CHECK(repeated.pause_printing.size() == baseline.pause_printing.size());
    // A manually moved pocket retains metadata and still slices correctly.
    for (auto* v : loaded.objects[0]->volumes) if (MP::is_pocket(*v)) { v->translate(Vec3d(0.,0.1,0.)); break; }
    reopened.apply(loaded, loaded_config); reopened.process(); CHECK_NOTHROW(MP::validate_print(reopened));
    // Moving the pause or changing the layer profile cannot silently seal a
    // cavity before the user has inserted the magnet.
    loaded.plates_custom_gcodes[0].gcodes.front().print_z = 3.;
    reopened.apply(loaded, loaded_config); reopened.process(); CHECK_THROWS(MP::validate_print(reopened));
    release_PlateData_list(plates); for (auto* preset : embedded) delete preset;
}
