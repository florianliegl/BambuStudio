#pragma once
#include "libslic3r.h"
#include <string>

namespace Slic3r {
class ModelObject;
class ModelVolume;
class DynamicPrintConfig;
namespace WhiteLayer {

// Layers are counted from the bed. The first `color_layers` keep their own colors, the next
// `white_layers` print entirely in `filament` (1-based), whatever the model's parts specify.
struct Options {
    unsigned color_layers = 3, white_layers = 1;
    int filament = 1;
};
struct Plan {
    Options options;
    double bottom_z = 0., top_z = 0.; // millimetres from the bed
    unsigned total_layers = 0;
    double min_x = 0., max_x = 0., min_y = 0., max_y = 0.; // world footprint of the printable parts
};
bool is_white_layer(const ModelVolume& volume);
Options options_from_volume(const ModelVolume& volume);
// Uses Studio's own layer generation (first layer, variable layer height) to find the slab.
// Throws std::invalid_argument with a plain-language reason when the model cannot take it.
Plan plan(const ModelObject& object, std::size_t instance, const DynamicPrintConfig& config, const Options& options);
// One native modifier volume covering the slab. It overrides the filament of everything it
// overlaps and adds no plastic, so existing parts, pockets and color volumes stay untouched.
void apply(ModelObject& object, std::size_t instance, const Plan& plan);
void remove(ModelObject& object);
bool has_white_layer(const ModelObject& object);

} // namespace WhiteLayer
} // namespace Slic3r
