#include "WhiteLayer.hpp"
#include "Model.hpp"
#include "Print.hpp"
#include "Layer.hpp"
#include "Slicing.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace Slic3r::WhiteLayer {
namespace {
constexpr const char* note_prefix = "WHITE_LAYER_V1 ";
constexpr double tolerance = 0.001;
void require(bool condition, const char* error) { if (!condition) throw std::invalid_argument(error); }
bool is_managed_helper(const ModelVolume& v) { return is_white_layer(v) || (v.is_negative_volume() && v.config.has("process_notes") && v.config.get().opt_string("process_notes").rfind("MAGNET_POCKET_V1 ", 0) == 0); }
// Bottom and top Z of every layer, as the slicer will generate them: [bottom0, top0, bottom1, ...].
std::vector<double> layer_bounds(const ModelObject& object, const DynamicPrintConfig& config, double height)
{
    const auto parameters = PrintObject::slicing_parameters(config, object, float(height));
    require(parameters.valid && !parameters.has_raft(), "A white layer needs a valid FDM profile without a raft.");
    std::vector<double> profile;
    bool reset = false;
    PrintObject::update_layer_height_profile(object, parameters, profile, reset);
    const bool precise = object.config.has("precise_z_height") ? object.config.get().opt_bool("precise_z_height") : config.opt_bool("precise_z_height");
    return generate_object_layers(parameters, profile, precise);
}
}

bool is_white_layer(const ModelVolume& v)
{
    return v.is_modifier() && v.config.has("process_notes") && v.config.get().opt_string("process_notes").rfind(note_prefix, 0) == 0;
}

bool has_white_layer(const ModelObject& object)
{
    return std::any_of(object.volumes.begin(), object.volumes.end(), [](const ModelVolume* v) { return is_white_layer(*v); });
}

Options options_from_volume(const ModelVolume& v)
{
    require(is_white_layer(v), "This volume is not a managed white layer.");
    Options o;
    std::istringstream in(v.config.get().opt_string("process_notes").substr(std::string(note_prefix).size()));
    require(bool(in >> o.color_layers >> o.white_layers >> o.filament), "White layer settings are incomplete. Recreate it with White layer.");
    return o;
}

Plan plan(const ModelObject& object, std::size_t instance, const DynamicPrintConfig& config, const Options& options)
{
    require(options.white_layers >= 1 && options.white_layers <= 100 && options.color_layers <= 1000, "Choose at least one white layer.");
    require(options.filament >= 1, "Choose the white filament.");
    require(instance < object.instances.size(), "Select a model instance first.");
    const Transform3d matrix = object.instances[instance]->get_matrix();
    require(std::abs(matrix.linear()(2, 0)) < 1e-6 && std::abs(matrix.linear()(2, 1)) < 1e-6 && std::abs(matrix.linear()(2, 2)) > 1e-6,
            "Lay the model flat on the bed (face down) before adding a white layer.");
    BoundingBoxf3 bounds;
    for (const auto* volume : object.volumes) {
        if (!volume->is_model_part()) continue;
        bounds.merge(volume->mesh().transformed_bounding_box(matrix * volume->get_matrix()));
    }
    require(bounds.defined && bounds.size().z() > 0., "The selected object has no printable solid geometry.");
    require(std::abs(bounds.min.z()) < tolerance, "Place the model flat on the bed (Z bottom = 0) before adding a white layer.");
    const auto stack = layer_bounds(object, config, bounds.size().z());
    Plan p;
    p.options = options;
    p.total_layers = unsigned(stack.size() / 2);
    require(options.color_layers + options.white_layers <= p.total_layers, "The model has too few layers for that many color and white layers.");
    p.bottom_z = stack[2 * std::size_t(options.color_layers)];
    p.top_z = stack[2 * std::size_t(options.color_layers + options.white_layers - 1) + 1];
    p.min_x = bounds.min.x(); p.max_x = bounds.max.x();
    p.min_y = bounds.min.y(); p.max_y = bounds.max.y();
    return p;
}

void remove(ModelObject& object)
{
    for (std::size_t i = object.volumes.size(); i-- > 0;)
        if (is_white_layer(*object.volumes[i])) object.delete_volume(i);
    object.invalidate_bounding_box();
}

void apply(ModelObject& object, std::size_t instance, const Plan& p)
{
    require(instance < object.instances.size() && p.top_z > p.bottom_z && p.options.filament >= 1, "Invalid white layer plan.");
    remove(object);
    // A slab slightly wider than the model, aligned exactly to layer boundaries, in world space.
    const double margin = 1.;
    const double width = p.max_x - p.min_x + 2. * margin, depth = p.max_y - p.min_y + 2. * margin, height = p.top_z - p.bottom_z;
    auto mesh = make_cube(width, depth, height);
    mesh.translate(float(-width / 2.), float(-depth / 2.), float(-height / 2.));
    auto* v = object.add_volume(std::move(mesh), ModelVolumeType::PARAMETER_MODIFIER);
    Transform3d position = Transform3d::Identity();
    position.translation() = Vec3d((p.min_x + p.max_x) / 2., (p.min_y + p.max_y) / 2., (p.bottom_z + p.top_z) / 2.);
    v->set_transformation(object.instances[instance]->get_matrix().inverse() * position);
    std::ostringstream name;
    name << "White layer (layers " << p.options.color_layers + 1 << " to " << p.options.color_layers + p.options.white_layers << ")";
    v->name = name.str();
    std::ostringstream notes;
    notes << note_prefix << p.options.color_layers << ' ' << p.options.white_layers << ' ' << p.options.filament;
    v->config.set_key_value("process_notes", new ConfigOptionString(notes.str()));
    v->config.set_key_value("extruder", new ConfigOptionInt(p.options.filament));
    v->source.is_from_builtin_objects = true;
    object.invalidate_bounding_box();
}

} // namespace Slic3r::WhiteLayer
