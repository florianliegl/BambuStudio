#pragma once
#include "Point.hpp"
#include "ExPolygon.hpp"
#include "CustomGCode.hpp"
#include <string>
#include <vector>
#include <functional>

namespace Slic3r {
class Model;
class ModelObject;
class ModelVolume;
class DynamicPrintConfig;
class Print;
namespace MagnetPockets {

struct Options {
    double diameter = 8., thickness = 2.;
    double diameter_allowance = 0.20, depth_allowance = 0.10;
    double edge_wall = 1., bottom_wall = 0.4;
    unsigned count = 2, cover_layers = 2;
};
struct Plan {
    Options options;
    double diameter = 0., depth = 0., bottom_z = 0., top_z = 0., pause_z = 0.;
    unsigned cover_layer = 0, total_layers = 0;
    std::vector<Vec2d> centers; // world coordinates, millimetres
    ExPolygons footprint; // actual mid-pocket outline, for the placement preview
};
inline constexpr const char* pause_prefix = "MAGNET_POCKETS_V1: ";
bool is_pocket(const ModelVolume& volume);
bool is_insertion_pause(const CustomGCode::Item& item);
Options options_from_pocket(const ModelVolume& volume);
std::vector<Vec2d> pocket_centers(const ModelObject& object, std::size_t instance);
Plan plan(const ModelObject& object, std::size_t instance, const DynamicPrintConfig& config,
          const Options& options, const std::vector<Vec2d>& keep_centers = {},
          const std::function<void()>& cancel = []{});
// Validates the complete plan before changing the object. Native negative
// volumes remain individually movable and are preserved by normal 3MF/undo.
void apply(ModelObject& object, std::size_t instance, const Plan& plan);
void remove(ModelObject& object);
// Rebuild only this feature's native pauses; retain all other user ticks.
void sync_pauses(Model& model, int plate, const std::vector<int>& objects,
                 const DynamicPrintConfig& config);
std::vector<double> insertion_heights(const CustomGCode::Info& info);
// Check geometry, actual roof layers and native tick positions before export.
void validate_print(const Print& print);
} // namespace MagnetPockets
} // namespace Slic3r
