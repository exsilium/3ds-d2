"""The HOME-menu banner (CGFX) from 256x128 frames, built the way the
hardware-tested ClouDS Music FA banner is (github.com/Epic0522/ClouDS-Music-FA,
MIT: tools/banner/build_banner_scene.py and convert_banner_cgfx.py, and the
GBAtemp guide "How to Make a 3D Banner Like an Official Nintendo 3DS Game"):

- a glTF scene converted by pycgfx's own converter (main.convert_gltf, at the
  commit both pin; fetched into the packaging image, not in this repository),
  so the model has everything pycgfx writes: the visibility, skeletal and
  material animation groups, the lookup tables, lit materials, RGBA4
  textures;
- every mesh bound to its node (mesh_node_name), no skinning, no bone
  streams. The layout: COMMON -> name -> frameNN (the flipbook's planes)
  and name -> subtitle;
- facing (the default): each visible plane has Y-axis billboarding (mode 5),
  as the reference's screen-facing logo "name", so the HOME menu's turning
  of the model leaves the logo facing the viewer. Billboarding only the
  parent, as without facing, does not carry over to the children: the logo
  turns. (Moving holders above still billboarded planes crashed the HOME
  menu: a null pointer read, likely the lookup of an animated node's mesh;
  every animated node here has its mesh, as in the reference.);
- animation only as ordinary Transform members on the frame planes:
  translation, rotation and scale, linearly interpolated. (Banners an
  emulator shows can still freeze the real HOME menu: ours did.) At most
  MAX_ANIMATED of them: on hardware 3 worked, 5 or more froze the HOME menu;
- matte materials (pycgfx's PBR specular stage bypassed), alpha-blended:
  the logo has no background.

The flipbook: the current frame's plane is in place and the others are far
above the view. The keys are sampled at 30 fps on every channel, as Blender
exports the reference's; a frame is in place for SAMPLES_PER_FRAME samples
and one more, while the next frame arrives, so a change never shows a blank
(mid-way, a moving plane is still far out of view). With one frame there is
no animation: a still banner in the same layout.

The quads have the scale of bannertool's own still banner (the quad in its
CGFX template, source/3ds/data.h: 256x128 texels on 26 x 13 units at z=0,
facing the HOME menu's camera on +Z). The optional subtitle plane (not
animated) goes under the logo, and the visible pixels of the two together
are centred on CENTRE_Y: the middle of the top screen between the HOME
menu's top bar and its bottom hints (10 pixels a unit, y = 1 at the
screen's centre).
"""
import base64
import io
import itertools
import json
import os
import struct
import sys
import tempfile

if not hasattr(itertools, "batched"):   # pycgfx's main.py uses Python 3.12's
    def _batched(iterable, n):
        it = iter(iterable)
        while chunk := tuple(itertools.islice(it, n)):
            yield chunk
    itertools.batched = _batched

MAX_SIZE = 0x80000   # the HOME menu rejects a larger CGFX
WIDTH, HEIGHT = 26.0, 13.0
TEXEL = WIDTH / 256
CENTRE_Y = 1.2       # where the banner's visible pixels are centred
SUBTITLE_GAP = 0.3   # units between the logo's last visible row and the subtitle
HIDDEN_Y = 1000.0
FPS = 30               # the key sampling rate
SAMPLES_PER_FRAME = 2  # 15 frames a second
MAX_ANIMATED = 3


def _png_bytes(width, height, rows):
    from make_art import png   # the packaging's own PNG writer
    fd, path = tempfile.mkstemp(suffix=".png")
    os.close(fd)
    try:
        png(path, width, height, [p if len(p) == 4 else p + (255,) for row in rows for p in row])
        with open(path, "rb") as f:
            return f.read()
    finally:
        os.unlink(path)


def atlases(frames):
    """The frames packed into as few textures of at most 256x256 (pycgfx's
    limit) as fit: [(rows, [(u0, v0, u1, v1) of each frame in it])], glTF
    texture coordinates (v = 0 at the top)."""
    w, h = len(frames[0][0]), len(frames[0])
    cols, rows_per = max(1, 256 // w), max(1, 256 // h)
    per = cols * rows_per
    out = []
    for start in range(0, len(frames), per):
        chunk = frames[start:start + per]
        aw, ah = w * min(cols, len(chunk)), h * -(-len(chunk) // cols)
        canvas = [[(0, 0, 0, 0)] * aw for _ in range(ah)]
        cells = []
        for j, f in enumerate(chunk):
            cx, cy = (j % cols) * w, (j // cols) * h
            for y in range(h):
                canvas[cy + y][cx:cx + w] = f[y]
            # half a texel in from the edges: linear filtering stays inside the cell
            cells.append(((cx + 0.5) / aw, (cy + 0.5) / ah, (cx + w - 0.5) / aw, (cy + h - 0.5) / ah))
        out.append((canvas, cells))
    return out


def scene(frames, animate=True, subtitle=None):
    # (facing is applied after conversion, in build())
    """The flipbook as a glTF document (a dict, buffers inline); subtitle:
    RGBA rows 256 wide, drawn from its top row just under the logo."""
    buf = io.BytesIO()
    views, accessors = [], []

    def add(data, target=None, **acc):
        while buf.tell() % 4:
            buf.write(b"\0")
        view = dict(buffer=0, byteOffset=buf.tell(), byteLength=len(data))
        if target:
            view["target"] = target
        views.append(view)
        buf.write(data)
        accessors.append(dict(bufferView=len(views) - 1, **acc))
        return len(accessors) - 1

    def quad(y0, y1):
        x = WIDTH / 2
        return add(struct.pack("<12f", -x, y0, 0, x, y0, 0, x, y1, 0, -x, y1, 0), 34962,
                   componentType=5126, count=4, type="VEC3", min=[-x, y0, 0], max=[x, y1, 0])

    def visible_rows(images):
        rows = [y for img in images for y, row in enumerate(img) if any(p[3] for p in row)]
        return min(rows), max(rows) + 1

    # placement: the logo quad's top at `top`, everything relative to it
    ltop, lbottom = visible_rows(frames)
    extent = [ltop * TEXEL, lbottom * TEXEL]   # down from the logo quad's top
    if subtitle:
        sub_at = lbottom * TEXEL + SUBTITLE_GAP
        extent[1] = sub_at + visible_rows([subtitle])[1] * TEXEL
    top = CENTRE_Y + (extent[0] + extent[1]) / 2

    pos = quad(top - HEIGHT, top)
    nrm = add(struct.pack("<12f", *([0, 0, 1] * 4)), 34962, componentType=5126, count=4, type="VEC3")
    idx = add(struct.pack("<6H", 0, 1, 2, 0, 2, 3), 34963, componentType=5123, count=6, type="SCALAR")

    n = len(frames)
    doc = {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "COMMON", "children": [1]}, {"name": "name", "children": []}],
        "meshes": [], "materials": [], "textures": [], "images": [],
        "samplers": [{"magFilter": 9729, "minFilter": 9729, "wrapS": 33071, "wrapT": 33071}],
    }
    # One material and texture per atlas, not per frame: the HOME menu sets
    # up runtime state for every material (and its 16 animatable members),
    # and a 15-material banner froze it where the 4-material reference works.
    def node(name, parent, **fields):
        doc["nodes"].append(dict(name=name, **fields))
        doc["nodes"][parent].setdefault("children", []).append(len(doc["nodes"]) - 1)
        return len(doc["nodes"]) - 1

    planes = []
    i = 0
    for a, (canvas, cells) in enumerate(atlases(frames)):
        tex = "flames%d" % a
        img = _png_bytes(len(canvas[0]), len(canvas), canvas)
        doc["images"].append({"name": tex, "uri": "data:image/png;base64," + base64.b64encode(img).decode()})
        doc["textures"].append({"sampler": 0, "source": a})
        doc["materials"].append({"name": tex, "doubleSided": True, "alphaMode": "BLEND", "pbrMetallicRoughness": {
            "baseColorTexture": {"index": a}, "metallicFactor": 0.0, "roughnessFactor": 1.0}})
        for u0, v0, u1, v1 in cells:
            # corners: bottom left, bottom right, top right, top left
            uv = add(struct.pack("<8f", u0, v1, u1, v1, u1, v0, u0, v0), 34962,
                     componentType=5126, count=4, type="VEC2")
            doc["meshes"].append({"name": "frame%02d" % i, "primitives": [{
                "attributes": {"POSITION": pos, "NORMAL": nrm, "TEXCOORD_0": uv},
                "indices": idx, "material": a}]})
            # without an animation only frame 0 is in place; animated, every
            # plane rests at the origin, as all the reference's animated
            # nodes do, and the keys place it
            rest = {"translation": [0.0, HIDDEN_Y, 0.0]} if i and not animate else {}
            planes.append(node("frame%02d" % i, 1, mesh=len(doc["meshes"]) - 1, **rest))
            i += 1

    if subtitle:
        # a texture of power-of-two height, the text in its top rows
        th = 8
        while th < len(subtitle):
            th *= 2
        canvas = subtitle + [[(0, 0, 0, 0)] * 256 for _ in range(th - len(subtitle))]
        a = len(doc["materials"])
        img = _png_bytes(256, th, canvas)
        doc["images"].append({"name": "subtitle", "uri": "data:image/png;base64," + base64.b64encode(img).decode()})
        doc["textures"].append({"sampler": 0, "source": a})
        doc["materials"].append({"name": "subtitle", "doubleSided": True, "alphaMode": "BLEND", "pbrMetallicRoughness": {
            "baseColorTexture": {"index": a}, "metallicFactor": 0.0, "roughnessFactor": 1.0}})
        spos = quad(top - sub_at - th * TEXEL, top - sub_at)
        suv = add(struct.pack("<8f", 0, 1, 1, 1, 1, 0, 0, 0), 34962, componentType=5126, count=4, type="VEC2")
        doc["meshes"].append({"name": "subtitle", "primitives": [{
            "attributes": {"POSITION": spos, "NORMAL": nrm, "TEXCOORD_0": suv},
            "indices": idx, "material": a}]})
        node("subtitle", 1, mesh=len(doc["meshes"]) - 1)

    if n > 1 and animate:
        samples = n * SAMPLES_PER_FRAME
        samplers, channels = [], []
        for i in range(n):
            # in place from its first sample through the next frame's first
            # (sample `samples` closes the loop: frame 0 is back in place)
            first = i * SAMPLES_PER_FRAME
            keys = [(k / FPS, 0.0 if first <= k <= first + SAMPLES_PER_FRAME or (i == 0 and k == samples)
                     else HIDDEN_Y) for k in range(samples + 1)]
            k = len(keys)
            times = add(struct.pack("<%df" % k, *(t for t, _ in keys)),
                        componentType=5126, count=k, type="SCALAR", min=[keys[0][0]], max=[keys[-1][0]])
            outputs = {
                "translation": add(b"".join(struct.pack("<3f", 0, y, 0) for _, y in keys),
                                   componentType=5126, count=k, type="VEC3"),
                "rotation": add(struct.pack("<4f", 0, 0, 0, 1) * k, componentType=5126, count=k, type="VEC4"),
                "scale": add(struct.pack("<3f", 1, 1, 1) * k, componentType=5126, count=k, type="VEC3"),
            }
            for path, out in outputs.items():
                channels.append({"sampler": len(samplers), "target": {"node": planes[i], "path": path}})
                samplers.append({"input": times, "output": out, "interpolation": "LINEAR"})
        doc["animations"] = [{"name": "flames", "channels": channels, "samplers": samplers}]

    data = buf.getvalue()
    doc["buffers"] = [{"byteLength": len(data),
                       "uri": "data:application/octet-stream;base64," + base64.b64encode(data).decode()}]
    doc["bufferViews"] = views
    doc["accessors"] = accessors
    return doc


def build(frames, pycgfx_dir, animate=True, subtitle=None, facing=True):
    """The CGFX bytes, with the reference converter's fixes and checks.
    facing: billboard every visible plane (else only their parent, as in
    the first banner tested on hardware, which the HOME menu turns)."""
    sys.path.insert(0, pycgfx_dir)
    import gltflib
    import main as pycgfx
    from cgfx.canm import CANMBoneTransform, PrimitiveType
    from cgfx.mtob import ColorFloat, FragmentLightingFlags
    from cgfx.primitives import VertexAttributeUsage
    from cgfx.sobj import BillboardMode

    fd, path = tempfile.mkstemp(suffix=".gltf")
    os.close(fd)
    try:
        with open(path, "w") as f:
            json.dump(scene(frames, animate, subtitle), f)
        gltf = gltflib.GLTF.load(path, load_file_resources=True)
    finally:
        os.unlink(path)
    cgfx = pycgfx.convert_gltf(gltf)

    if animate and len(frames) > MAX_ANIMATED:
        raise SystemExit("banner: %d animated frames; more than %d froze the HOME menu"
                         % (len(frames), MAX_ANIMATED))
    frame_names = ["frame%02d" % i for i in range(len(frames))]
    names = frame_names + ["subtitle"] * bool(subtitle)
    if list(cgfx.data.models) != ["COMMON"]:
        raise SystemExit("banner: the CGFX's one model must be COMMON")
    model = cgfx.data.models["COMMON"]
    for nm in names if facing else ["name"]:
        bone = model.skeleton.bones[nm]
        if bone is None or bone.position.values() != (0, 0, 0) or bone.rotation.values() != (0, 0, 0):
            raise SystemExit("banner: no node %s at rest at the origin" % nm)
        bone.billboard_mode = BillboardMode.YAxial
    for mesh in model.meshes.data.contents:
        node = next((nm for nm in names if mesh.name == nm or mesh.name.startswith(nm + "_")), None)
        if node is None:
            raise SystemExit("banner: no node for mesh %s" % mesh.name)
        mesh.mesh_node_name = node   # pycgfx leaves it empty; official banners set it
    for shape in model.shapes.data.contents:
        for pset in shape.primitive_sets.data.contents:
            if pset.skinning_mode != 0:
                raise SystemExit("banner: %s is skinned" % shape.name)
        for attr in shape.vertex_attributes.data.contents:
            streams = attr.vertex_streams.data.contents if hasattr(attr, "vertex_streams") else (attr,)
            for s in streams:
                if s.usage in (VertexAttributeUsage.BoneIndex, VertexAttributeUsage.BoneWeight):
                    raise SystemExit("banner: %s has bone streams" % shape.name)
    for name in model.materials:
        # matte, as the reference: no specular stage, no distribution table
        m = model.materials[name]
        m.material_color.constant[0] = ColorFloat(0, 0, 0, 1)
        tev = m.fragment_shader.texture_combiners[2]
        tev.src_rgb, tev.combine_rgb = 0xFFF, 0
        m.fragment_shader.fragment_lighting.flags = FragmentLightingFlags(0)
        m.fragment_shader.fragment_lighting_table.distribution_0_sampler = None
    anims = list(cgfx.data.skeletal_animations)
    if len(frames) > 1 and animate:
        if anims != ["COMMON"]:
            raise SystemExit("banner: the animation must be COMMON, got %s" % anims)
        anim = cgfx.data.skeletal_animations["COMMON"]
        members = {nm: anim.member_animations_data[nm] for nm in anim.member_animations_data}
        if set(members) != set(frame_names):
            raise SystemExit("banner: animated nodes %s, expected the frames" % sorted(members))
        for nm, mem in members.items():
            if not isinstance(mem, CANMBoneTransform) or mem.primitive_type != PrimitiveType.Transform:
                raise SystemExit("banner: %s is not an ordinary Transform member" % nm)
    elif anims:
        raise SystemExit("banner: an animation in a banner meant to be still")

    data = pycgfx.write(cgfx)
    if len(data) > MAX_SIZE:
        raise SystemExit("banner CGFX is %d bytes, over the HOME menu's %d" % (len(data), MAX_SIZE))
    return data
