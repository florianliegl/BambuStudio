# White layer

In **Prepare**, select a model that is lying flat with its colored face on the plate,
right-click it and choose **White layer...** (custom Studio desktop build).

Choose how many layers keep their own colors (counted from the bed), how many white layers
follow them, and which filament is the white (the one closest to white is preselected).
For a 3 color + 1 white face with 0.1 mm layers, layers 1-3 keep their colors and layer 4
(0.30-0.40 mm) prints entirely in white, then the body continues in its own filament.

How it works: one native **modifier** volume covers exactly those layers, with its filament
set to the white. A modifier overrides the filament of every part it overlaps, so parts that
carry their own color are recolored too, and no plastic is added. The slab is aligned to
Studio's real layer boundaries (first layer and variable layer height included). Select the
modifier in the Objects list to see or move it, or reopen **White layer...** to change the
counts or **Remove white layer**. Add and remove have an ordinary Undo snapshot.

A model generated with a thicker color face (for example 0.4 mm) needs no editing: choose
3 color layers + 1 white layer and the fourth color layer becomes the white layer.

A saved AMS Lite swap plan describes the old layer colors, so Studio marks it stale. Slice,
then Prepare print again.

Verified by `White layer recolors the layers behind the color face` in the integration
suite, which slices a real model: layers 1-3 orange and blue, layer 4 white only, the rest
black. It has not been printed.

The command-line tool `scripts/extended-ams-lite/add_white_layer.py` does the same job on a
project file outside Studio, and also thins the color face.
