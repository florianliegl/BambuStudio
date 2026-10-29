#include "MagnetPockets.hpp"
#include "Model.hpp"
#include "Print.hpp"
#include "Layer.hpp"
#include "Slicing.hpp"
#include "TriangleMeshSlicer.hpp"
#include "ClipperUtils.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace Slic3r::MagnetPockets {
namespace {
constexpr const char* pocket_prefix = "MAGNET_POCKET_V1 ";
constexpr double tolerance = 0.001;
void require(bool condition, const char* error) { if (!condition) throw std::invalid_argument(error); }
void validate(const Options& o)
{
    for (double value : {o.diameter, o.thickness, o.diameter_allowance, o.depth_allowance, o.edge_wall, o.bottom_wall})
        require(std::isfinite(value), "Magnet dimensions must be finite.");
    require(o.diameter > 0. && o.diameter <= 200. && o.thickness > 0. && o.thickness <= 100., "Enter a positive magnet diameter and thickness.");
    require(o.diameter_allowance >= 0. && o.diameter_allowance <= 5. && o.depth_allowance >= 0. && o.depth_allowance <= 5., "Pocket allowances must be between 0 and 5 mm.");
    require(o.edge_wall >= 0.2 && o.bottom_wall >= 0.1, "Leave at least 0.2 mm at the sides and 0.1 mm below the pockets.");
    require(o.count >= 1 && o.count <= 1000 && o.cover_layers >= 1 && o.cover_layers <= 100, "Choose 1 to 1000 magnets and at least one cover layer.");
}
struct Body {
    std::vector<TriangleMesh> positive, negative;
    BoundingBoxf3 bounds;
};
Body body(const ModelObject& object, std::size_t instance)
{
    require(instance < object.instances.size(), "Select a model instance first.");
    Body result;
    const auto matrix = object.instances[instance]->get_matrix();
    require(std::abs(matrix.linear()(2, 0)) < 1e-6 && std::abs(matrix.linear()(2, 1)) < 1e-6 && std::abs(matrix.linear()(2, 2)) > 1e-6,
        "Lay the keychain flat on the bed before adding magnet pockets.");
    for (const auto* volume : object.volumes) {
        if (is_pocket(*volume) || (!volume->is_model_part() && !volume->is_negative_volume())) continue;
        auto mesh = volume->mesh();
        mesh.transform(matrix * volume->get_matrix(), true);
        if (volume->is_model_part()) {
            result.bounds.merge(mesh.bounding_box());
            result.positive.push_back(std::move(mesh));
        } else result.negative.push_back(std::move(mesh));
    }
    require(!result.positive.empty() && result.bounds.size().z() > 0., "The selected object has no printable solid geometry.");
    require(std::abs(result.bounds.min.z()) < tolerance, "Place the keychain flat on the bed (Z bottom = 0) before adding pockets.");
    return result;
}
ExPolygons section(const Body& b, double z)
{
    ExPolygons positive, negative;
    for (const auto& mesh : b.positive) {
        auto slices = slice_mesh_ex(mesh.its, {float(z)});
        append(positive, std::move(slices.front()));
    }
    for (const auto& mesh : b.negative) {
        auto slices = slice_mesh_ex(mesh.its, {float(z)});
        append(negative, std::move(slices.front()));
    }
    return diff_ex(union_ex(positive), union_ex(negative));
}
ExPolygons usable_centers(const Body& b, const Plan& p)
{
    // Include intermediate sections and both sides of every mesh Z event. This
    // catches holes, stepped outlines and necks hidden by a bounding box alone.
    std::vector<double> zs;
    for (int i = 0; i <= 16; ++i) zs.push_back(p.bottom_z + 0.0002 + (p.top_z - p.bottom_z - 0.0004) * i / 16.);
    // The complete roof footprint must exist above each pocket too. A raised
    // logo elsewhere on a keychain must not be mistaken for a local cover.
    for (int i = 1; i <= 16; ++i) zs.push_back(p.top_z + (b.bounds.max.z() - p.top_z) * i / 16. - 0.0001);
    const double floor_bottom = p.bottom_z - p.options.bottom_wall;
    for (int i = 0; i <= 8; ++i) zs.push_back(floor_bottom + 0.0001 + (p.options.bottom_wall - 0.0002) * i / 8.);
    for (const auto& meshes : {&b.positive, &b.negative}) for (const auto& mesh : *meshes)
        for (const auto& v : mesh.its.vertices) if (v.z() > floor_bottom && v.z() < b.bounds.max.z()) {
            zs.push_back(std::max(floor_bottom + 0.0001, double(v.z()) - 0.0001));
            zs.push_back(std::min(b.bounds.max.z() - 0.0001, double(v.z()) + 0.0001));
        }
    std::sort(zs.begin(), zs.end());
    zs.erase(std::unique(zs.begin(), zs.end(), [](double a, double b) { return std::abs(a - b) < 0.00001; }), zs.end());
    require(zs.size() <= 4096, "This shape is too complex for automatic magnet placement. Simplify the model or use manual negative parts.");
    ExPolygons safe;
    bool first = true;
    for (double z : zs) {
        auto slice = offset_ex(section(b, z), float(-scale_(p.diameter / 2. + p.options.edge_wall)));
        safe = first ? std::move(slice) : intersection_ex(safe, slice);
        first = false;
        if (safe.empty()) break;
    }
    require(!safe.empty(), "There is no solid region wide enough for this pocket and its side walls. Use a smaller magnet or a larger part.");
    return safe;
}
bool inside(const ExPolygons& regions, const Vec2d& p)
{
    const Point point(scale_(p.x()), scale_(p.y()));
    return std::any_of(regions.begin(), regions.end(), [&](const ExPolygon& region) { return region.contains(point); });
}
std::vector<Vec2d> arrange(const ExPolygons& safe, const BoundingBoxf3& bounds, const Plan& p)
{
    const Vec2d middle = bounds.center().head<2>();
    const Vec2d size = bounds.size().head<2>();
    const double gap = p.diameter + p.options.edge_wall;
    require(double(p.options.count) * PI * std::pow(p.diameter / 2., 2) <= size.x() * size.y(), "The requested magnet count cannot fit in this part.");
    // Candidate centers form mirrored pairs, so every accepted layout has
    // exact 180-degree symmetry. Odd counts also contain the central magnet.
    const double step = std::max(0.15, std::min(gap / 5., std::max(size.x(), size.y()) / 90.));
    std::vector<Vec2d> anchors{middle};
    for (int iy = -3; iy <= 3; ++iy) for (int ix = -3; ix <= 3; ++ix)
        if (ix || iy) anchors.push_back(middle + Vec2d(size.x() * ix / 20., size.y() * iy / 20.));
    std::stable_sort(anchors.begin(), anchors.end(), [&](const Vec2d& a, const Vec2d& b) { return (a - middle).squaredNorm() < (b - middle).squaredNorm(); });
    for (const auto& center : anchors) {
        if ((p.options.count & 1) && !inside(safe, center)) continue;
        if (p.options.count == 1) return {center};
        if (p.options.count > 9) {
            const double pitch = std::max(gap, std::sqrt(size.x() * size.y() / (double(p.options.count) * 3.)));
            for (double x_phase : {0., 0.5}) for (double y_phase : {0., 0.5}) {
                std::vector<Vec2d> pairs;
                for (int iy = -int(size.y() / pitch) - 1; iy <= int(size.y() / pitch) + 1; ++iy)
                    for (int ix = -int(size.x() / pitch) - 1; ix <= int(size.x() / pitch) + 1; ++ix) {
                        Vec2d a = center + Vec2d((ix + x_phase) * pitch, (iy + y_phase) * pitch), b = 2. * center - a;
                        if (a.y() < center.y() - tolerance || (std::abs(a.y() - center.y()) < tolerance && a.x() <= center.x())) continue;
                        if ((a - b).norm() < gap || ((p.options.count & 1) && (a - center).norm() < gap)) continue;
                        if (inside(safe, a) && inside(safe, b)) pairs.push_back(a);
                    }
                if (pairs.size() < p.options.count / 2) continue;
                std::stable_sort(pairs.begin(), pairs.end(), [&](const Vec2d& a, const Vec2d& b) { return (a-center).squaredNorm() > (b-center).squaredNorm(); });
                std::vector<Vec2d> placed;
                if (p.options.count & 1) placed.push_back(center);
                for (std::size_t i = 0; placed.size() < p.options.count; ++i) { placed.push_back(pairs[i]); placed.push_back(2. * center - pairs[i]); }
                return placed;
            }
            continue;
        }
        std::vector<Vec2d> candidates;
        for (double y = bounds.min.y(); y <= bounds.max.y(); y += step)
            for (double x = bounds.min.x(); x <= bounds.max.x(); x += step) {
                Vec2d a(x, y), b = 2. * center - a;
                if (a.y() < center.y() - tolerance || (std::abs(a.y() - center.y()) < tolerance && a.x() <= center.x())) continue;
                if ((a - b).norm() + tolerance < gap || !inside(safe, a) || !inside(safe, b)) continue;
                candidates.push_back(a);
            }
        // Farthest-point pair selection spreads magnets over the available
        // body. Several deterministic seeds avoid a corner pair blocking a
        // denser symmetric layout for odd/prime requested counts.
        for (int strategy = 0; strategy < 3; ++strategy) {
            std::vector<Vec2d> placed;
            if (p.options.count & 1) placed.push_back(center);
            while (placed.size() < p.options.count) {
                double best = -1.;
                Vec2d chosen;
                for (const auto& a : candidates) {
                    const Vec2d b = 2. * center - a;
                    double distance = (a - b).norm();
                    for (const auto& existing : placed) distance = std::min({distance, (a - existing).norm(), (b - existing).norm()});
                    if (distance + tolerance < gap) continue;
                    const double score = p.options.count == 2 ? distance - 4. * (size.x() >= size.y() ? std::abs(a.y()-center.y()) : std::abs(a.x()-center.x())) :
                        placed.empty() && strategy == 1 ? 1. / (1. + std::abs((a - center).norm() - gap)) :
                        placed.empty() && strategy == 2 ? 1. / (1. + std::abs(a.y() - center.y())) : distance;
                    if (score > best) { best = score; chosen = a; }
                }
                if (best < 0.) break;
                placed.push_back(chosen); placed.push_back(2. * center - chosen);
            }
            if (placed.size() == p.options.count) return placed;
        }
    }
    throw std::invalid_argument("Could not fit that many magnets symmetrically inside the actual outline. Reduce the count/size or side-wall spacing.");
}
std::vector<double> layers(const ModelObject& object, const DynamicPrintConfig& config, double height)
{
    const auto parameters = PrintObject::slicing_parameters(config, object, float(height));
    require(parameters.valid && !parameters.has_raft(), "Magnet pockets require a valid FDM profile without a raft.");
    std::vector<double> profile;
    bool reset = false;
    PrintObject::update_layer_height_profile(object, parameters, profile, reset);
    const bool precise = object.config.has("precise_z_height") ? object.config.get().opt_bool("precise_z_height") : config.opt_bool("precise_z_height");
    return generate_object_layers(parameters, profile, precise);
}
std::string volume_settings(const Options& o)
{
    std::ostringstream s;
    s << std::setprecision(10) << pocket_prefix << o.diameter << ' ' << o.thickness << ' ' << o.diameter_allowance << ' '
      << o.depth_allowance << ' ' << o.edge_wall << ' ' << o.bottom_wall << ' ' << o.cover_layers;
    return s.str();
}
}

bool is_pocket(const ModelVolume& v) {
    return v.is_negative_volume() && v.config.has("process_notes") && v.config.get().opt_string("process_notes").rfind(pocket_prefix, 0) == 0;
}
bool is_insertion_pause(const CustomGCode::Item& item) { return item.type == CustomGCode::PausePrint && item.extra.rfind(pause_prefix, 0) == 0; }
Options options_from_pocket(const ModelVolume& v)
{
    require(is_pocket(v), "This volume is not a managed magnet pocket.");
    Options o;
    std::istringstream in(v.config.get().opt_string("process_notes").substr(std::string(pocket_prefix).size()));
    require(bool(in >> o.diameter >> o.thickness >> o.diameter_allowance >> o.depth_allowance >> o.edge_wall >> o.bottom_wall >> o.cover_layers), "Magnet pocket settings are incomplete. Recreate it with Magnet pockets.");
    validate(o); return o;
}
std::vector<Vec2d> pocket_centers(const ModelObject& object, std::size_t instance)
{
    require(instance < object.instances.size(), "Select a model instance.");
    std::vector<Vec2d> out;
    for (const auto* v : object.volumes) if (is_pocket(*v)) {
        const auto box = v->mesh().transformed_bounding_box(object.instances[instance]->get_matrix() * v->get_matrix());
        out.push_back(box.center().head<2>());
    }
    return out;
}
Plan plan(const ModelObject& object, std::size_t instance, const DynamicPrintConfig& config, const Options& options, const std::vector<Vec2d>& keep_centers)
{
    validate(options);
    require(config.opt_enum<PrintSequence>("print_sequence") != PrintSequence::ByObject, "Magnet insertion pauses require printing by layer.");
    const auto b = body(object, instance);
    const auto stack = layers(object, config, b.bounds.size().z());
    require(stack.size() / 2 > options.cover_layers, "The part has too few layers for this cover.");
    Plan p; p.options = options;
    p.diameter = options.diameter + options.diameter_allowance;
    p.depth = options.thickness + options.depth_allowance;
    p.total_layers = unsigned(stack.size() / 2);
    p.cover_layer = p.total_layers - options.cover_layers + 1;
    p.top_z = stack[2 * (p.cover_layer - 1)];
    p.pause_z = stack[2 * (p.cover_layer - 1) + 1];
    p.bottom_z = p.top_z - p.depth;
    require(p.bottom_z >= options.bottom_wall - tolerance, "This magnet plus its cover would leave too little plastic below the pocket. Use a thinner magnet or a thicker part.");
    const auto safe = usable_centers(b, p);
    p.footprint = section(b, (p.bottom_z + p.top_z) / 2.);
    if (!keep_centers.empty()) {
        require(keep_centers.size() == options.count, "The requested count differs from the existing pockets. Turn off Keep positions to arrange them again.");
        p.centers = keep_centers;
    } else p.centers = arrange(safe, b.bounds, p);
    for (std::size_t i = 0; i < p.centers.size(); ++i) {
        require(inside(safe, p.centers[i]), "A magnet pocket is outside the solid body or too close to an edge/hole. Move it inward or arrange the pockets again.");
        for (std::size_t j = 0; j < i; ++j)
            require((p.centers[i] - p.centers[j]).norm() >= p.diameter + options.edge_wall - tolerance, "Magnet pockets overlap or leave too little plastic between them.");
    }
    return p;
}
void remove(ModelObject& object)
{
    for (std::size_t i = object.volumes.size(); i-- > 0;) if (is_pocket(*object.volumes[i])) object.delete_volume(i);
    object.invalidate_bounding_box();
}
void apply(ModelObject& object, std::size_t instance, const Plan& p)
{
    validate(p.options);
    require(instance < object.instances.size() && p.centers.size() == p.options.count, "Invalid magnet placement plan.");
    std::vector<TriangleMesh> meshes;
    for (std::size_t i = 0; i < p.centers.size(); ++i) {
        auto mesh = make_cylinder(p.diameter / 2., p.depth);
        mesh.translate(0., 0., -p.depth / 2.);
        meshes.push_back(std::move(mesh));
    }
    remove(object);
    for (std::size_t i = 0; i < meshes.size(); ++i) {
        auto* v = object.add_volume(std::move(meshes[i]), ModelVolumeType::NEGATIVE_VOLUME);
        Transform3d position = Transform3d::Identity();
        position.translation() = Vec3d(p.centers[i].x(), p.centers[i].y(), (p.bottom_z + p.top_z) / 2.);
        v->set_transformation(object.instances[instance]->get_matrix().inverse() * position);
        std::ostringstream name;
        name << "Magnet pocket " << i + 1 << " (" << p.options.diameter << " x " << p.options.thickness << " mm)";
        v->name = name.str();
        v->config.set_key_value("process_notes", new ConfigOptionString(volume_settings(p.options)));
        v->source.is_from_builtin_objects = true;
        v->config.set_key_value("extruder", new ConfigOptionInt(0));
    }
    object.invalidate_bounding_box();
}
std::vector<double> insertion_heights(const CustomGCode::Info& info)
{
    std::vector<double> out;
    for (const auto& tick : info.gcodes) if (is_insertion_pause(tick)) out.push_back(tick.print_z);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end(), [](double a, double b) { return std::abs(a - b) < tolerance; }), out.end());
    return out;
}
void sync_pauses(Model& model, int plate, const std::vector<int>& objects, const DynamicPrintConfig& config)
{
    auto info = model.plates_custom_gcodes[plate];
    const std::string adopted = std::string(pause_prefix) + "USER ";
    info.gcodes.erase(std::remove_if(info.gcodes.begin(), info.gcodes.end(), [&](CustomGCode::Item& item) {
        if (!is_insertion_pause(item)) return false;
        if (item.extra.rfind(adopted, 0) == 0) {
            std::istringstream saved(item.extra.substr(adopted.size())); std::string original;
            require(bool(saved >> std::quoted(original)), "The saved user-pause message is damaged.");
            item.extra = original; return false;
        }
        return true;
    }), info.gcodes.end());
    std::vector<double> zs;
    for (int id : objects) {
        require(id >= 0 && std::size_t(id) < model.objects.size(), "Invalid plate object.");
        const auto& object = *model.objects[id];
        const auto it = std::find_if(object.volumes.begin(), object.volumes.end(), [](const ModelVolume* v) { return is_pocket(*v); });
        if (it == object.volumes.end()) continue;
        auto o = options_from_pocket(**it);
        const auto centers = pocket_centers(object, 0); o.count = unsigned(centers.size());
        const auto p = plan(object, 0, config, o, centers);
        zs.push_back(p.pause_z);
    }
    std::sort(zs.begin(), zs.end());
    zs.erase(std::unique(zs.begin(), zs.end(), [](double a, double b) { return std::abs(a - b) < tolerance; }), zs.end());
    for (double z : zs) {
        auto tick = std::find_if(info.gcodes.begin(), info.gcodes.end(), [&](const CustomGCode::Item& i) { return std::abs(i.print_z - z) < tolerance; });
        if (tick != info.gcodes.end()) {
            require(tick->type == CustomGCode::PausePrint, "A custom command already occupies the magnet insertion layer. Move/remove that tick first.");
            // Reuse a user pause at this exact height; never create two pauses.
            std::ostringstream message; message << adopted << std::quoted(tick->extra); tick->extra = message.str();
        } else info.gcodes.push_back({z, CustomGCode::PausePrint, 0, "", std::string(pause_prefix) + "Insert magnets before the closing layers."});
    }
    std::stable_sort(info.gcodes.begin(), info.gcodes.end());
    model.plates_custom_gcodes[plate] = std::move(info);
}
void validate_print(const Print& print)
{
    const auto info = print.model().get_curr_plate_custom_gcodes();
    const auto ticks = insertion_heights(info);
    std::vector<double> required;
    for (const auto* object : print.objects()) {
        const auto& model = *object->model_object();
        const auto it = std::find_if(model.volumes.begin(), model.volumes.end(), [](const ModelVolume* v) { return is_pocket(*v); });
        if (it == model.volumes.end()) continue;
        require(print.config().print_sequence == PrintSequence::ByLayer, "Magnet insertion requires printing by layer.");
        auto o = options_from_pocket(**it);
        const auto centers = pocket_centers(model, 0); o.count = unsigned(centers.size());
        const auto p = plan(model, 0, print.full_print_config(), o, centers);
        require(!object->layers().empty(), "Slice the magnet pocket object before exporting.");
        for (const auto* v : model.volumes) if (is_pocket(*v)) for (const auto* instance : model.instances) {
            const auto bb = v->mesh().transformed_bounding_box(instance->get_matrix() * v->get_matrix());
            require(std::abs(bb.max.z() - p.top_z) < tolerance && std::abs(bb.min.z() - p.bottom_z) < tolerance &&
                std::abs(bb.size().x() - p.diameter) < tolerance && std::abs(bb.size().y() - p.diameter) < tolerance,
                "Magnet height, scaling or layer settings changed. Open Magnet pockets and Apply with Keep positions to update the pocket height and pause.");
        }
        auto roof = std::find_if(object->layers().begin(), object->layers().end(), [&](const Layer* layer) { return layer->slice_z > p.top_z + tolerance; });
        require(roof != object->layers().end() && std::size_t(object->layers().end() - roof) == o.cover_layers &&
            std::abs((*roof)->print_z - p.pause_z) < tolerance, "The actual sliced closing layers differ from the magnet plan. Update the pockets for this layer profile.");
        // Internal support can make a pocket impossible to fill. Do not export
        // a job that silently fills the intended insertion cavities.
        require(!object->has_support(), "Disable supports for this flat magnet-pocket object before printing, so the cavities remain empty.");
        required.push_back(p.pause_z);
    }
    std::sort(required.begin(), required.end());
    required.erase(std::unique(required.begin(), required.end(), [](double a, double b) { return std::abs(a - b) < tolerance; }), required.end());
    require(required.size() == ticks.size(), "A magnet insertion pause is missing or orphaned. Open Magnet pockets and Apply to update this plate's pauses.");
    for (std::size_t i = 0; i < required.size(); ++i)
        require(std::abs(required[i] - ticks[i]) < tolerance, "The magnet pause moved away from the first closing layer. Update it with Magnet pockets before printing.");
}
} // namespace Slic3r::MagnetPockets
