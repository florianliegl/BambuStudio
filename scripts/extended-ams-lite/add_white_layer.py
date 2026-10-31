#!/usr/bin/env python3
"""Turn an Image-to-Keychain 3mf into a "N color layers + M white layers" keychain.

The MakerLab keychain is a 3 mm body plus a 0.4 mm color face made of flat prisms that
overlap the top of the body (printed face down). This tool thins the color face to N layers
and adds a white copy of the body outline, M layers thick, directly behind it:

    face (bed) -> N color layers -> M white layers -> black body

It never changes the input file; the result is written next to it. Standard library only.
"""
import argparse
import re
import sys
import uuid
import zipfile
from pathlib import Path

LAYER = 0.1
EPS = 1e-3
VERTEX = re.compile(r'<vertex x="([^"]+)" y="([^"]+)" z="([^"]+)"/>')


def fail(message):
    sys.exit("ERROR: " + message)


def fmt(value):
    return ("%.9g" % value)


def object_z_range(body):
    zs = [float(m.group(3)) for m in VERTEX.finditer(body)]
    return (min(zs), max(zs)) if zs else (0.0, 0.0)


def remap_z(body, mapping):
    """Rewrite every vertex z using mapping(z)."""
    return VERTEX.sub(lambda m: '<vertex x="%s" y="%s" z="%s"/>' % (m.group(1), m.group(2), fmt(mapping(float(m.group(3))))), body)


def find_white(project, override):
    import json
    colours = json.loads(project)["filament_colour"]
    if override:
        if not 1 <= override <= len(colours):
            fail("--white %d is not one of the %d filaments in this project" % (override, len(colours)))
        return override
    best, best_dist = None, 1e9
    for index, colour in enumerate(colours, 1):
        r, g, b = (int(colour[i:i + 2], 16) for i in (1, 3, 5))
        dist = (255 - r) + (255 - g) + (255 - b)
        if dist < best_dist:
            best, best_dist = index, dist
    if best_dist > 60:
        fail("no white filament in this project (colours: %s). Add a white filament in Studio "
             "or pass --white <filament number>." % ", ".join(colours))
    return best


def convert(source, target, color_layers, white_layers, white_override, keep=()):
    with zipfile.ZipFile(source) as archive:
        order = [info.filename for info in archive.infolist()]
        files = {name: archive.read(name) for name in order}
    for needed in ("3D/3dmodel.model", "Metadata/model_settings.config", "Metadata/project_settings.config"):
        if needed not in files:
            fail("%s is missing; this is not a Studio keychain project" % needed)

    top_model = files["3D/3dmodel.model"].decode("utf-8")
    # The part meshes live in one file whose name differs between projects (object_1.model, object-3770.model, ...).
    paths = set(re.findall(r'<component [^>]*?p:path="([^"]+)"', top_model))
    if len(paths) != 1:
        fail("the parts are not stored in a single object file; unsupported file layout")
    objects_path = paths.pop().lstrip("/")
    if objects_path not in files:
        fail("%s is missing from the project" % objects_path)
    objects_xml = files[objects_path].decode("utf-8")
    settings = files["Metadata/model_settings.config"].decode("utf-8")
    white = find_white(files["Metadata/project_settings.config"].decode("utf-8"), white_override)

    # A component's transform is optional (identity when absent, as in projects re-saved by Studio).
    components = []
    for tag in re.findall(r'<component [^>]*/>', top_model):
        transform = re.search(r'transform="([^"]*)"', tag)
        components.append((re.search(r'objectid="(\d+)"', tag).group(1), transform.group(1) if transform else "1 0 0 0 1 0 0 0 1 0 0 0"))
    objects = {m.group(1): m for m in re.finditer(r'<object id="(\d+)"[^>]*>.*?</object>', objects_xml, re.S)}
    parts = list(re.finditer(r'<part id="(\d+)"[^>]*?subtype="(\w+)".*?</part>', settings, re.S))
    if len(parts) != len(components) or any(p.group(1) != c[0] for p, c in zip(parts, components)):
        fail("the part list and the component list do not match; unsupported file layout")

    # Absolute z extent of every normal part (component translation + local mesh). Negative
    # parts such as magnet pockets are left alone.
    normal_ids = {p.group(1) for p in parts if p.group(2) == "normal_part"}
    extents = {}
    for object_id, transform in components:
        if object_id not in normal_ids:
            continue
        body = objects[object_id].group(0)
        low, high = object_z_range(body)
        shift = float(transform.split()[11])
        extents[object_id] = (low + shift, high + shift, low, high, shift)
    thickness = {k: v[1] - v[0] for k, v in extents.items()}
    body_id = max(thickness, key=thickness.get)
    body_low, body_top = extents[body_id][0], extents[body_id][1]
    color_ids = [k for k in thickness if k != body_id]
    if not color_ids:
        fail("no color face found")
    face = {round(thickness[k], 3) for k in color_ids}
    tops = {round(extents[k][1], 3) for k in color_ids}
    if len(face) != 1 or tops != {round(body_top, 3)}:
        fail("the color face is not a single flat 0.4 mm slab on top of the body; unsupported file")
    face_thickness = face.pop()
    have_layers = round(face_thickness / LAYER)
    if color_layers > have_layers:
        fail("the project's color face is only %d layers thick; asked for %d" % (have_layers, color_layers))
    new_face = color_layers * LAYER
    white_top = body_top - new_face
    white_low = white_top - white_layers * LAYER
    if white_low < body_low + LAYER:
        fail("not enough body left behind the face for the white layers")

    # 1. Thin the color face: keep the face side (top in model space), move the back up.
    thinned = objects_xml
    for object_id in color_ids:
        if object_id in keep:
            continue
        original = objects[object_id].group(0)
        _, _, local_low, local_high, shift = extents[object_id]
        back = local_high - new_face
        edited = remap_z(original, lambda z: back if abs(z - local_low) < EPS else z)
        thinned = thinned.replace(original, edited, 1)

    # 2. A white copy of the body outline, stacked right behind the thinned face.
    body_xml = objects[body_id].group(0)
    _, _, b_low, b_high, b_shift = extents[body_id]
    new_id = str(max(int(k) for k in objects) + 1)
    white_xml = re.sub(r'(<object id=")\d+(")', r"\g<1>%s\g<2>" % new_id, body_xml, count=1)
    white_xml = re.sub(r'p:UUID="[^"]*"', 'p:UUID="%s"' % uuid.uuid4(), white_xml, count=1)
    white_xml = remap_z(white_xml, lambda z: (white_low - b_shift) if abs(z - b_low) < EPS else (white_top - b_shift))
    thinned = thinned.replace("</resources>", white_xml + "\n </resources>", 1) if white_layers else thinned

    if white_layers:
        body_component = re.search(r'<component [^>]*?objectid="%s"[^>]*/>' % body_id, top_model).group(0)
        new_component = re.sub(r'objectid="\d+"', 'objectid="%s"' % new_id, body_component, count=1)
        new_component = re.sub(r'p:UUID="[^"]*"', 'p:UUID="%s"' % uuid.uuid4(), new_component, count=1)
        # Insert before the first negative part so magnet pockets still cut the white layer.
        first_negative = next((p for p in parts if p.group(2) != "normal_part"), None)
        if first_negative is None:
            top_model = top_model.replace("</components>", "  " + new_component + "\n   </components>", 1)
        else:
            anchor = re.search(r'<component [^>]*?objectid="%s"[^>]*/>' % first_negative.group(1), top_model).group(0)
            top_model = top_model.replace(anchor, new_component + "\n    " + anchor, 1)

        body_part = next(p for p in parts if p.group(1) == body_id).group(0)
        new_part = re.sub(r'<part id="\d+"', '<part id="%s"' % new_id, body_part, count=1)
        new_part = re.sub(r'uuid="[^"]*"', 'uuid="%s"' % uuid.uuid4(), new_part, count=1)
        new_part = re.sub(r'(<metadata key="name" value=")[^"]*(")', r"\g<1>White backing layer\g<2>", new_part, count=1)
        new_part = re.sub(r'(<metadata key="extruder" value=")\d+(")', r"\g<1>%d\g<2>" % white, new_part)
        if first_negative is None:
            anchor = parts[-1].end()
            settings = settings[:anchor] + "\n    " + new_part + settings[anchor:]
        else:
            anchor = first_negative.start()
            settings = settings[:anchor] + new_part + "\n    " + settings[anchor:]

    # A saved AMS Lite swap plan is tied to the old layer positions; keeping it makes the slicer
    # refuse ("sliced color order or layer positions changed"), so drop it. The plan has to be
    # prepared again in Studio.
    project = files["Metadata/project_settings.config"].decode("utf-8")
    project, dropped = re.subn(r'(?:\\n)*\[\[EXTENDED_AMS_LITE_V1\]\].*?\[\[/EXTENDED_AMS_LITE\]\]', "", project, flags=re.S)
    files["Metadata/project_settings.config"] = project.encode("utf-8")

    files[objects_path] = thinned.encode("utf-8")
    files["3D/3dmodel.model"] = top_model.encode("utf-8")
    files["Metadata/model_settings.config"] = settings.encode("utf-8")
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as out:
        for name in order:
            out.writestr(name, files[name])
    return have_layers, white, bool(dropped)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("project", type=Path, help="keychain .3mf")
    parser.add_argument("--color-layers", type=int, default=3, help="color layers to keep (default 3)")
    parser.add_argument("--white-layers", type=int, default=1, help="white layers behind them (default 1)")
    parser.add_argument("--white", type=int, default=0, help="filament number of the white (default: auto-detect)")
    parser.add_argument("--keep", default="", help=argparse.SUPPRESS)  # debugging: part ids left at full thickness
    args = parser.parse_args()
    if not args.project.is_file():
        fail("file not found: %s" % args.project)
    if args.color_layers < 1 or args.white_layers < 0:
        fail("need at least 1 color layer")
    label = "%dC+%dW" % (args.color_layers, args.white_layers)
    target = args.project.with_name("%s %s.3mf" % (args.project.stem, label))
    if target.exists():
        fail("%s already exists; move or rename it first" % target.name)
    have, white, dropped = convert(args.project, target, args.color_layers, args.white_layers, args.white, set(args.keep.split(',')) if args.keep else ())
    print("Made %s" % target)
    print("  color face: %d -> %d layers; %d white layer(s) of filament %d behind it" % (have, args.color_layers, args.white_layers, white))
    if dropped:
        print("  The old AMS Lite swap plan was removed: slice, then prepare the swap plan again.")
    print("  Open it in Studio and slice as usual.")


if __name__ == "__main__":
    main()
