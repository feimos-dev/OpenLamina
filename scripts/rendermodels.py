#!/usr/bin/env python3
"""Converts the controller models of the WebXR Input Profiles (assets/controllers) into SteamVR's render-model layout.

    scripts/rendermodels.py --assets assets/controllers --out DIR

Each controller's GLB (a profile's left.glb and right.glb, or none.glb for both hands) becomes a folder
DIR/<profile>-<hand>/ of SteamVR's layout: <profile>-<hand>.json, whose "components" name the parts and how each moves,
an OBJ of each part, a material library and the base-colour textures. The positions are in the controller's grip space,
in metres; OpenLamina puts them in the device's space as it loads them.

Every node's transform is baked into the parts' vertices, with the moving parts at rest (buttons up, sticks centred).
A part's motion comes from the WebXR profile's min and max nodes: a button's or trigger's as a rotation about a pivot or a
movement along an axis, a thumbstick's as SteamVR's "joystick", a touchpad's as "trackpad" with a "trackpad_touch" dot
shown while it's touched. Each motion is checked against the GLB's own poses half and fully pressed: no vertex may be
more than 0.5 mm off, or the conversion fails.

Only the standard library is used: CMake runs it at build time.
"""

import argparse
import json
import math
import struct
import sys
import zlib
from pathlib import Path

TOLERANCE_M = 0.0005

# SteamVR's legacy button ids (EVRButtonId) and the axis of the legacy controller state each input is on
SYSTEM, MENU, GRIP, BUTTON_A = 0, 1, 2, 7

# WebXR component -> (SteamVR component, legacy button, legacy axis). The legacy inputs are those OpenLamina's built-in
# legacy bindings give the controller type (Misc/Input/Bindings/LegacyBindings.cpp).
OCULUS = {
    "xr-standard-trigger": ("trigger", None, 1),
    "xr-standard-squeeze": ("grip", GRIP, 2),
    "xr-standard-thumbstick": ("thumbstick", None, 0),
    "x-button": ("x_button", BUTTON_A, None),
    "y-button": ("y_button", MENU, None),
    "a-button": ("a_button", BUTTON_A, None),
    "b-button": ("b_button", MENU, None),
    "menu": ("system", SYSTEM, None),
    "thumbrest": ("thumbrest", None, None),
}
KNUCKLES = {
    "xr-standard-trigger": ("trigger", None, 1),
    "xr-standard-squeeze": ("grip", GRIP, None),
    # The trackpad shares axis 0 with the stick in the legacy state: it's left still, its touch dot hidden
    "xr-standard-touchpad": ("trackpad", None, None),
    "xr-standard-thumbstick": ("thumbstick", None, 0),
    "a-button": ("button_a", GRIP, None),
    "b-button": ("button_b", MENU, None),
}
VIVE = {
    "xr-standard-trigger": ("trigger", None, 1),
    "xr-standard-squeeze": ("grip", GRIP, None),
    "xr-standard-touchpad": ("trackpad", None, 0),
    "menu": ("button", MENU, None),
}
# The other controllers' legacy bindings: trackpad axis 0, trigger axis 1, stick axis 2, no A button
GENERIC = {
    "xr-standard-trigger": ("trigger", None, 1),
    "xr-standard-squeeze": ("grip", GRIP, None),
    "xr-standard-touchpad": ("trackpad", None, 0),
    "xr-standard-thumbstick": ("thumbstick", None, 2),
    "x-button": ("x_button", None, None),
    "y-button": ("y_button", None, None),
    "a-button": ("a_button", None, None),
    "b-button": ("b_button", None, None),
    "menu": ("button", MENU, None),
    "bumper": ("bumper", None, None),
}

# The profiles converted, each with its component table
PROFILES = {
    "oculus-touch-v3": OCULUS,
    "meta-quest-touch-plus": OCULUS,
    "meta-quest-touch-pro": OCULUS,
    "valve-index": KNUCKLES,
    "htc-vive": VIVE,
    "microsoft-mixed-reality": GENERIC,
    "hp-mixed-reality": GENERIC,
    "htc-vive-cosmos": GENERIC,
    "htc-vive-focus-3": GENERIC,
    "pico-neo3": GENERIC,
    "pico-4": GENERIC,
    "generic-trigger-squeeze-thumbstick": GENERIC,
}


class ConversionError(Exception):
    pass


# --- Maths: 4x4 matrices as row-major lists of 16, quaternions as (x, y, z, w) like glTF's ---


def mat_mul(a, b):
    return [sum(a[r * 4 + k] * b[k * 4 + c] for k in range(4)) for r in range(4) for c in range(4)]


def mat_identity():
    return [1.0 if r == c else 0.0 for r in range(4) for c in range(4)]


def quat_matrix(q):
    x, y, z, w = q
    return [
        1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
        2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
        2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y),
    ]


def trs_matrix(t, q, s):
    r = quat_matrix(q)
    return [
        r[0] * s[0], r[1] * s[1], r[2] * s[2], t[0],
        r[3] * s[0], r[4] * s[1], r[5] * s[2], t[1],
        r[6] * s[0], r[7] * s[1], r[8] * s[2], t[2],
        0.0, 0.0, 0.0, 1.0,
    ]


def apply(m, p):
    return tuple(m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3] for r in range(3))


def apply_dir(m, d):
    return tuple(m[r * 4] * d[0] + m[r * 4 + 1] * d[1] + m[r * 4 + 2] * d[2] for r in range(3))


def det3(m):
    return (m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8])
            + m[2] * (m[4] * m[9] - m[5] * m[8]))


def normal_matrix(m):
    """The inverse transpose of m's 3x3 part, as rows"""
    a = [[m[r * 4 + c] for c in range(3)] for r in range(3)]
    d = det3(m)
    if abs(d) < 1e-20:
        raise ConversionError("a node's transform collapses its mesh")
    cof = [[(a[(r + 1) % 3][(c + 1) % 3] * a[(r + 2) % 3][(c + 2) % 3]
             - a[(r + 1) % 3][(c + 2) % 3] * a[(r + 2) % 3][(c + 1) % 3]) / d for c in range(3)] for r in range(3)]
    return cof  # cofactor / det = inverse transpose


def mat_inverse_rigid(m):
    """The inverse of a rotation and translation (scale divided out per column)"""
    r = [[m[i * 4 + j] for j in range(3)] for i in range(3)]
    rt = [[r[j][i] for j in range(3)] for i in range(3)]
    t = [m[3], m[7], m[11]]
    out = mat_identity()
    for i in range(3):
        for j in range(3):
            out[i * 4 + j] = rt[i][j]
        out[i * 4 + 3] = -sum(rt[i][k] * t[k] for k in range(3))
    return out


def rotation_part(m):
    """m's rotation, its columns' scale divided out"""
    cols = [[m[r * 4 + c] for r in range(3)] for c in range(3)]
    lengths = [math.sqrt(sum(v * v for v in col)) for col in cols]
    out = mat_identity()
    for c in range(3):
        for r in range(3):
            out[r * 4 + c] = cols[c][r] / lengths[c]
    return out


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def add(a, b):
    return tuple(x + y for x, y in zip(a, b))


def scale(a, s):
    return tuple(x * s for x in a)


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def length(a):
    return math.sqrt(dot(a, a))


def normalize(a):
    n = length(a)
    return scale(a, 1 / n) if n > 1e-12 else (0.0, 0.0, 0.0)


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def quat_conj(q):
    return (-q[0], -q[1], -q[2], q[3])


def quat_normalize(q):
    n = math.sqrt(sum(v * v for v in q))
    return tuple(v / n for v in q)


def slerp(a, b, t):
    a, b = quat_normalize(a), quat_normalize(b)
    d = sum(x * y for x, y in zip(a, b))
    if d < 0:
        b, d = tuple(-v for v in b), -d
    if d > 0.9995:
        return quat_normalize(tuple(x + (y - x) * t for x, y in zip(a, b)))
    theta = math.acos(min(1.0, d))
    s = math.sin(theta)
    wa, wb = math.sin((1 - t) * theta) / s, math.sin(t * theta) / s
    return tuple(wa * x + wb * y for x, y in zip(a, b))


def axis_angle(q):
    """A rotation as (unit axis, degrees from 0 to 180)"""
    q = quat_normalize(q)
    if q[3] < 0:
        q = tuple(-v for v in q)
    s = math.sqrt(max(0.0, 1 - q[3] * q[3]))
    angle = math.degrees(2 * math.acos(min(1.0, q[3])))
    if s < 1e-9:
        return (1.0, 0.0, 0.0), 0.0
    return (q[0] / s, q[1] / s, q[2] / s), angle


def matrix_quat(m):
    """The rotation of a matrix (scale-free) as a quaternion"""
    m00, m01, m02 = m[0], m[1], m[2]
    m10, m11, m12 = m[4], m[5], m[6]
    m20, m21, m22 = m[8], m[9], m[10]
    tr = m00 + m11 + m22
    if tr > 0:
        s = math.sqrt(tr + 1) * 2
        q = ((m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, s / 4)
    elif m00 > m11 and m00 > m22:
        s = math.sqrt(1 + m00 - m11 - m22) * 2
        q = (s / 4, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s)
    elif m11 > m22:
        s = math.sqrt(1 + m11 - m00 - m22) * 2
        q = ((m01 + m10) / s, s / 4, (m12 + m21) / s, (m02 - m20) / s)
    else:
        s = math.sqrt(1 + m22 - m00 - m11) * 2
        q = ((m02 + m20) / s, (m12 + m21) / s, s / 4, (m10 - m01) / s)
    return quat_normalize(q)


def angle_axis_quat(axis, degrees):
    h = math.radians(degrees) / 2
    s = math.sin(h)
    a = normalize(axis)
    return (a[0] * s, a[1] * s, a[2] * s, math.cos(h))


def quat_rotate(q, v):
    p = quat_mul(quat_mul(q, (v[0], v[1], v[2], 0.0)), quat_conj(q))
    return p[:3]


def euler_xyz(degrees):
    """SteamVR's rotate_xyz: about X, then Y, then Z"""
    x, y, z = degrees
    return quat_mul(quat_mul(angle_axis_quat((0, 0, 1), z), angle_axis_quat((0, 1, 0), y)), angle_axis_quat((1, 0, 0), x))


def frame_euler(x_axis, y_axis, z_axis):
    """rotate_xyz (degrees) of the frame whose axes are these (orthonormal, right-handed)"""
    # R = Rz(c) Ry(b) Rx(a); its columns are the axes
    r20, r21, r22 = x_axis[2], y_axis[2], z_axis[2]
    r10, r00 = x_axis[1], x_axis[0]
    b = math.asin(max(-1.0, min(1.0, -r20)))
    a = math.atan2(r21, r22)
    c = math.atan2(r10, r00)
    return [math.degrees(a), math.degrees(b), math.degrees(c)]


# --- SteamVR's component motion (Misc/SteamVRRenderModels.cpp ComponentPose), for the self-check ---


def lerp(r, t):
    return r[0] + (r[1] - r[0]) * t


def rigid_then(a, b):
    """a after b; each (quat, translation)"""
    return quat_mul(a[0], b[0]), add(quat_rotate(a[0], b[1]), a[1])


def rotate_about(point, q):
    return q, sub(point, quat_rotate(q, point))


def component_pose(motion, value, axis):
    kind = motion.get("type", "")
    x, y = axis
    ident = ((0.0, 0.0, 0.0, 1.0), (0.0, 0.0, 0.0))
    if kind == "rotate":
        degrees = lerp(motion.get("value_mapping", [0, 0]), value)
        return rotate_about(tuple(motion.get("pivot", [0, 0, 0])), angle_axis_quat(motion["axis"], degrees))
    if kind == "translate":
        return (0.0, 0.0, 0.0, 1.0), scale(tuple(motion["axis"]), lerp(motion.get("value_mapping", [0, 0]), value))
    if kind in ("trackpad", "joystick"):
        pad = kind == "trackpad"
        frame = euler_xyz(motion.get("rotate_xyz", [0, 0, 0]))
        about_z = lerp(motion.get("press_rotation_x" if pad else "joystick_rotation_x", [0, 0]), (x + 1) / 2)
        about_x = lerp(motion.get("press_rotation_y" if pad else "joystick_rotation_y", [0, 0]), (y + 1) / 2)
        if pad:
            about_z *= value
            about_x *= value
        tilt = quat_mul(quat_mul(quat_mul(frame, angle_axis_quat((0, 0, 1), about_z)), angle_axis_quat((1, 0, 0), about_x)),
                        quat_conj(frame))
        r = rotate_about(tuple(motion.get("center", [0, 0, 0])), tilt)
        move = scale(tuple(motion.get("press_translate", [0, 0, 0])), value)
        if pad:
            touch = (lerp(motion.get("touch_translate_x", [0, 0]), (x + 1) / 2), 0.0,
                     lerp(motion.get("touch_translate_y", [0, 0]), (y + 1) / 2))
            move = add(move, quat_rotate(frame, touch))
        return rigid_then(((0.0, 0.0, 0.0, 1.0), move), r)
    return ident


# --- GLB reading ---


class Glb:
    def __init__(self, path: Path):
        data = path.read_bytes()
        if len(data) < 20 or data[:4] != b"glTF":
            raise ConversionError(f"{path} isn't a GLB file")
        json_len, json_type = struct.unpack_from("<II", data, 12)
        if json_type != 0x4E4F534A:
            raise ConversionError(f"{path}: its first chunk isn't JSON")
        self.json = json.loads(data[20:20 + json_len])
        offset = 20 + json_len
        self.bin = b""
        if offset + 8 <= len(data):
            bin_len, bin_type = struct.unpack_from("<II", data, offset)
            if bin_type == 0x004E4942:
                self.bin = data[offset + 8:offset + 8 + bin_len]
        self.nodes = self.json["nodes"]
        self.parent = {}
        for i, n in enumerate(self.nodes):
            for c in n.get("children", []):
                self.parent[c] = i
        self.by_name = {}
        for i, n in enumerate(self.nodes):
            self.by_name.setdefault(n.get("name", ""), i)

    def view(self, index):
        bv = self.json["bufferViews"][index]
        start = bv.get("byteOffset", 0)
        return self.bin[start:start + bv["byteLength"]], bv.get("byteStride")

    def accessor(self, index):
        acc = self.json["accessors"][index]
        if "sparse" in acc:
            raise ConversionError("sparse accessors aren't read")
        count = acc["count"]
        width = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[acc["type"]]
        fmt, size = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2), 5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4)}[acc["componentType"]]
        data, stride = self.view(acc["bufferView"])
        base = acc.get("byteOffset", 0)
        stride = stride or size * width
        norm = acc.get("normalized", False)
        maxv = {"b": 127, "B": 255, "h": 32767, "H": 65535}.get(fmt)
        out = []
        for i in range(count):
            vals = struct.unpack_from("<" + fmt * width, data, base + i * stride)
            if norm and maxv:
                vals = tuple(max(v / maxv, -1.0) for v in vals)
            out.append(vals if width > 1 else vals[0])
        return out

    def image(self, index):
        img = self.json["images"][index]
        if "bufferView" not in img:
            raise ConversionError("an image outside the GLB isn't read")
        data, _ = self.view(img["bufferView"])
        if data[:8] == b"\x89PNG\r\n\x1a\n":
            return data, "png"
        if data[:3] == b"\xff\xd8\xff":
            return data, "jpg"
        raise ConversionError("an image that's neither PNG nor JPEG")

    def local(self, i, override=None):
        n = self.nodes[i]
        if override is not None:
            t, q = override
            return trs_matrix(t, q, n.get("scale", [1, 1, 1]))
        if "matrix" in n:
            m = n["matrix"]  # column-major
            return [m[c * 4 + r] for r in range(4) for c in range(4)]
        return trs_matrix(n.get("translation", [0, 0, 0]), n.get("rotation", [0, 0, 0, 1]), n.get("scale", [1, 1, 1]))

    def trs(self, i):
        n = self.nodes[i]
        if "matrix" in n:
            m = self.local(i)
            return (m[3], m[7], m[11]), matrix_quat(rotation_part(m))
        return tuple(n.get("translation", [0, 0, 0])), tuple(n.get("rotation", [0, 0, 0, 1]))

    def ancestors(self, i):
        while i in self.parent:
            i = self.parent[i]
            yield i


# --- Conversion ---


class Response:
    """A visual response: a value node and what drives it"""

    def __init__(self, glb, component, prop, resp):
        self.component = component
        self.prop = prop  # button, xAxis, yAxis, state
        self.kind = resp.get("valueNodeProperty", "transform")
        self.value = node(glb, resp["valueNodeName"])
        self.states = resp.get("states", [])
        self.min = node(glb, resp["minNodeName"]) if self.kind == "transform" else None
        self.max = node(glb, resp["maxNodeName"]) if self.kind == "transform" else None
        self.touched = "touched" in resp["valueNodeName"]

    def pose(self, glb, t):
        (t0, q0), (t1, q1) = glb.trs(self.min), glb.trs(self.max)
        return tuple(a + (b - a) * t for a, b in zip(t0, t1)), slerp(q0, q1, t)


def node(glb, name):
    if name not in glb.by_name:
        raise ConversionError(f"no node {name}")
    return glb.by_name[name]


def rest_value(r):
    return 0.0 if r.prop == "button" else 0.5


def world_matrices(glb, overrides):
    """Every node's world matrix, with the value nodes posed as overrides says"""
    out = {}

    def world(i):
        if i not in out:
            local = glb.local(i, overrides.get(i))
            out[i] = mat_mul(world(glb.parent[i]), local) if i in glb.parent else local
        return out[i]

    for i in range(len(glb.nodes)):
        world(i)
    return out


def poses(glb, responses, value_of):
    return {r.value: r.pose(glb, value_of(r)) for r in responses if r.kind == "transform"}


def vec(v):
    return [round(x, 6) + 0.0 for x in v]


def fit_button(glb, r, rest, maximum):
    """A button's or trigger's one transform: a rotation about a pivot, or a movement along an axis"""
    w0, w1 = rest[r.value], maximum[r.value]
    d = mat_mul(rotation_part(w1), mat_inverse_rigid(rotation_part(w0)))
    q = matrix_quat(d)
    axis, angle = axis_angle(q)
    o0, o1 = (w0[3], w0[7], w0[11]), (w1[3], w1[7], w1[11])
    if angle < 0.05:
        return {"type": "translate", "axis": vec(sub(o1, o0)), "value_mapping": [0, 1]}
    # The pivot: the point the change of pose keeps (nearest to it), on the plane through o0 across the axis
    t = sub(o1, quat_rotate(q, o0))
    t_perp = sub(t, scale(axis, dot(t, axis)))
    half = math.radians(angle) / 2
    pivot = scale(add(t_perp, scale(cross(axis, t_perp), 1 / math.tan(half))), 0.5)
    return {"type": "rotate", "axis": vec(axis), "pivot": vec(pivot), "value_mapping": [0, round(angle, 4)]}


def stick_fit(glb, active, rest, parents_rest, kind, touch=None):
    """A thumbstick's or touchpad's motion: its press, its tilts (x about the frame's Z, y about its X), and for a
    touchpad's touch dot the touch point's movement across the frame's X and Z"""
    by = {}
    for r in active:
        by.setdefault((r.prop, r.touched), r)
    motion = {"type": kind}
    press = by.get(("button", False))
    if press:
        p = parents_rest[press.value]
        (t0, q0), (t1, q1) = press.pose(glb, 0), press.pose(glb, 1)
        _, angle = axis_angle(quat_mul(q1, quat_conj(q0)))
        if angle > 0.05:
            raise ConversionError(f"{press.component}: its press turns it, which SteamVR's {kind} can't")
        motion["press_translate"] = vec(apply_dir(p, sub(t1, t0)))
    tilts = {}
    for prop in ("xAxis", "yAxis"):
        r = by.get((prop, False))
        if not r:
            continue
        (_, q0), (_, q1) = r.pose(glb, 0), r.pose(glb, 1)
        axis, angle = axis_angle(quat_mul(q1, quat_conj(q0)))
        if angle < 0.05:
            continue
        p = parents_rest[r.value]
        # An axis is turned as a rotation's, not a point's: through a mirroring parent it points the other way
        world_axis = normalize(apply_dir(rotation_part(p), axis))
        if det3(p) < 0:
            world_axis = scale(world_axis, -1)
        centre = apply(rest[r.value], (0, 0, 0))
        tilts[prop] = (world_axis, angle, centre)
    frame = None
    if touch:
        # The touch frame: the touched X node's parent, its X across the pad and its Z along it
        p = rotation_part(parents_rest[touch[0].value])
        frame = (apply_dir(p, (1, 0, 0)), apply_dir(p, (0, 1, 0)), apply_dir(p, (0, 0, 1)))
    elif tilts:
        z = tilts["xAxis"][0] if "xAxis" in tilts else None
        x = tilts["yAxis"][0] if "yAxis" in tilts else None
        if z is None:
            z = normalize(cross(x, (0, 1, 0) if abs(x[1]) < 0.9 else (1, 0, 0)))
        if x is None:
            x = normalize(cross((0, 1, 0) if abs(z[1]) < 0.9 else (1, 0, 0), z))
        x = normalize(sub(x, scale(z, dot(x, z))))
        frame = (x, cross(z, x), z)
    if frame:
        motion["rotate_xyz"] = [round(a, 4) for a in frame_euler(*frame)]
    if tilts:
        centre = tilts.get("xAxis", tilts.get("yAxis"))[2]
        motion["center"] = vec(centre)
        names = ("press_rotation_x", "press_rotation_y") if kind == "trackpad" else ("joystick_rotation_x", "joystick_rotation_y")
        for prop, name, sign in (("xAxis", names[0], 1), ("yAxis", names[1], -1)):
            if prop in tilts:
                world_axis, angle, _ = tilts[prop]
                about = frame[2] if prop == "xAxis" else frame[0]
                s = 1 if dot(world_axis, about) >= 0 else -1
                # WebXR's x runs left to right as OpenVR's; its y runs forward to back, OpenVR's back to forward
                h = round(s * angle / 2, 4)
                motion[name] = [-h, h] if sign > 0 else [h, -h]
    if touch:
        tx, ty = touch
        for r, name, along, flip in ((tx, "touch_translate_x", 0, False), (ty, "touch_translate_y", 2, True)):
            if not r:
                continue
            (t0, _), (t1, _) = r.pose(glb, 0), r.pose(glb, 1)
            mid = [(a + b) / 2 for a, b in zip(t0, t1)]
            lo, hi = round(t0[along] - mid[along], 6), round(t1[along] - mid[along], 6)
            motion[name] = [hi, lo] if flip else [lo, hi]
    return motion


def touch_dot(centre, frame_y, frame_x):
    """A touch dot: a flat disc 4 mm across the pad, a little above it, both sides"""
    up = normalize(frame_y)
    ax = normalize(frame_x)
    az = cross(ax, up)
    c = add(centre, scale(up, 0.0005))
    ring = [add(c, add(scale(ax, 0.004 * math.cos(a)), scale(az, 0.004 * math.sin(a))))
            for a in (2 * math.pi * k / 16 for k in range(16))]
    verts, tris = [], []
    for side, n in ((1, up), (-1, scale(up, -1))):
        base = len(verts)
        verts.append((c, n, (0.5, 0.5)))
        for p in ring:
            verts.append((p, n, (0.5, 0.5)))
        for k in range(16):
            a, b = base + 1 + k, base + 1 + (k + 1) % 16
            tris.append((base, b, a) if side > 0 else (base, a, b))
    return verts, tris


def png_bytes(width, height, rgba_rows):
    raw = b"".join(b"\x00" + bytes(row) for row in rgba_rows)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 0)) + chunk(b"IEND", b""))


def srgb(c):
    c = max(0.0, min(1.0, c))
    return round(255 * (12.92 * c if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055))


def fmt(v, places=6):
    s = f"{v:.{places}f}"
    return "0" if s.strip("-0.") == "" else s.rstrip("0").rstrip(".")


def obj_text(mtllib, groups):
    """groups: [(material name, vertices [(pos, normal, uv)], triangles)]. Positions to 10 um, each distinct position,
    UV and normal written once."""
    lines = [f"mtllib {mtllib}"]
    seen = [{}, {}, {}]  # text -> its index, for v, vt, vn

    def index(kind, text):
        table = seen[kind]
        if text not in table:
            table[text] = len(table) + 1
            lines.append(("v ", "vt ", "vn ")[kind] + text)
        return table[text]

    for material, verts, tris in groups:
        corners = []
        for p, n, uv in verts:
            # OBJ's v counts from the texture's bottom row, glTF's from its top
            corners.append((index(0, " ".join(fmt(x, 5) for x in p)), index(1, f"{fmt(uv[0], 5)} {fmt(1 - uv[1], 5)}"),
                            index(2, " ".join(fmt(x, 3) for x in n))))
        lines.append(f"usemtl {material}")
        for tri in tris:
            lines.append("f " + " ".join("%d/%d/%d" % corners[i] for i in tri))
    return "\n".join(lines) + "\n"


def convert_layout(glb_path: Path, layout: dict, table: dict, name: str, files: dict):
    """One hand's model into files (name -> bytes); the worst distance off the max pose"""
    glb = Glb(glb_path)
    responses = []
    for cid, comp in layout["components"].items():
        if cid not in table:
            raise ConversionError(f"{name}: component {cid} isn't in its table")
        for resp in comp["visualResponses"].values():
            # A response whose nodes the model hasn't (a part that's in the body) is left out, as WebXR's library does
            names = [resp["valueNodeName"]] + ([resp["minNodeName"], resp["maxNodeName"]]
                                               if resp.get("valueNodeProperty", "transform") == "transform" else [])
            if all(n in glb.by_name for n in names):
                responses.append(Response(glb, cid, resp["componentProperty"], resp))
    value_nodes = {r.value: r for r in responses}

    rest_overrides = poses(glb, responses, rest_value)
    max_overrides = poses(glb, responses, lambda r: 1.0)
    rest = world_matrices(glb, rest_overrides)
    maximum = world_matrices(glb, max_overrides)
    parents_rest = {i: (rest[glb.parent[i]] if i in glb.parent else mat_identity()) for i in range(len(glb.nodes))}

    # Each mesh node's component, by its nearest value node; and whether it's in a touchpad's touch dot
    parts = {}  # SteamVR name -> {"webxr", "meshes": [node], "touch": bool}
    for i, n in enumerate(glb.nodes):
        if "mesh" not in n:
            continue
        owner = None
        touch = False
        for a in [i, *glb.ancestors(i)]:
            r = value_nodes.get(a)
            if r is None:
                continue
            if r.touched or r.kind == "visibility":
                touch = True
            owner = r.component
            break
        if owner is None:
            key, webxr = "body", None
        else:
            key, webxr = table[owner][0] + ("_touch" if touch else ""), owner
        parts.setdefault(key, {"webxr": webxr, "meshes": [], "touch": touch})["meshes"].append(i)

    # Touchpads without a touch dot of their own get one
    for cid, comp in layout["components"].items():
        if comp["type"] != "touchpad":
            continue
        key = table[cid][0] + "_touch"
        if key not in parts:
            parts[key] = {"webxr": cid, "meshes": [], "touch": True}

    # Textures: each material's base colour, else a 2x2 of its colour
    materials = glb.json.get("materials", [{}])
    textures = {}  # material index -> file name
    mtl_lines = []
    for mi, mat in enumerate(materials):
        pbr = mat.get("pbrMetallicRoughness", {})
        tex = pbr.get("baseColorTexture")
        if tex is not None:
            if tex.get("texCoord", 0) != 0:
                raise ConversionError(f"{name}: a material's texture uses a second set of UVs")
            data, ext = glb.image(glb.json["textures"][tex["index"]]["source"])
            file = f"texture_{mi}.{ext}"
        else:
            colour = pbr.get("baseColorFactor", [1, 1, 1, 1])
            px = [srgb(colour[0]), srgb(colour[1]), srgb(colour[2]), 255]
            data, file = png_bytes(2, 2, [px * 2, px * 2]), f"texture_{mi}.png"
        files[file] = data
        textures[mi] = file
        mtl_lines += [f"newmtl m{mi}", f"map_Kd {file}", ""]
    dot_file = "touch.png"
    needs_dot = any(not p["meshes"] for p in parts.values())
    if needs_dot:
        px = [200, 230, 255, 255]
        files[dot_file] = png_bytes(2, 2, [px * 2, px * 2])
        mtl_lines += ["newmtl touch", f"map_Kd {dot_file}", ""]
    mtl = "materials.mtl"
    files[mtl] = "\n".join(mtl_lines).encode()

    def mesh_groups(meshes, worlds):
        groups = []
        for i in meshes:
            m = worlds[i]
            nm = normal_matrix(m)
            flip = det3(m) < 0
            for prim in glb.json["meshes"][glb.nodes[i]["mesh"]]["primitives"]:
                if prim.get("mode", 4) != 4:
                    raise ConversionError(f"{name}: a mesh that isn't triangles")
                attrs = prim["attributes"]
                pos = glb.accessor(attrs["POSITION"])
                nor = glb.accessor(attrs["NORMAL"]) if "NORMAL" in attrs else [(0, 1, 0)] * len(pos)
                uvs = glb.accessor(attrs["TEXCOORD_0"]) if "TEXCOORD_0" in attrs else [(0, 0)] * len(pos)
                idx = glb.accessor(prim["indices"]) if "indices" in prim else list(range(len(pos)))
                verts = []
                for p, n, uv in zip(pos, nor, uvs):
                    wn = normalize(tuple(sum(nm[r][c] * n[c] for c in range(3)) for r in range(3)))
                    verts.append((apply(m, p), wn, uv))
                tris = [tuple(idx[k:k + 3]) for k in range(0, len(idx) - 2, 3)]
                if flip:
                    tris = [(a, c, b) for a, b, c in tris]
                groups.append((f"m{prim.get('material', 0)}", verts, tris))
        return groups

    components = {}
    worst = 0.0
    for key in sorted(parts):
        part = parts[key]
        webxr = part["webxr"]
        entry = {}
        groups = mesh_groups(part["meshes"], rest)
        dot_point = None  # the node a generated touch dot is at
        if webxr is not None:
            _, button, axis = table[webxr]
            ctype = layout["components"][webxr]["type"]
            mine = [r for r in responses if r.component == webxr]
            meshes = part["meshes"]
            active = [r for r in mine if r.kind == "transform"
                      and any(r.value == a for m in meshes for a in [m, *glb.ancestors(m)])]
            touch_x = next((r for r in mine if r.touched and r.prop == "xAxis" and r.kind == "transform"), None)
            touch_y = next((r for r in mine if r.touched and r.prop == "yAxis" and r.kind == "transform"), None)
            if part["touch"]:
                if not touch_x or not touch_y:
                    raise ConversionError(f"{name}: {webxr} has no touch point to move a dot with")
                # The dot moves with the pad's press and with the touch point
                pad = [r for r in mine if r.kind == "transform" and not r.touched
                       and any(r.value == a for a in glb.ancestors(touch_x.value))]
                active = pad + [touch_x, touch_y]
                motion = stick_fit(glb, pad, rest, parents_rest, "trackpad", (touch_x, touch_y))
                entry["visibility"] = {"default": False, "touch": True, "press": True}
                if not groups:
                    vis = next((r for r in mine if r.kind == "visibility"), None)
                    point = dot_point = vis.value if vis else touch_y.value
                    frame = rotation_part(parents_rest[touch_x.value])
                    verts, tris = touch_dot(apply(rest[point], (0, 0, 0)), apply_dir(frame, (0, 1, 0)),
                                            apply_dir(frame, (1, 0, 0)))
                    groups = [("touch", verts, tris)]
            elif ctype == "thumbstick":
                motion = stick_fit(glb, active, rest, parents_rest, "joystick")
            elif ctype == "touchpad":
                motion = stick_fit(glb, active, rest, parents_rest, "trackpad")
            else:
                if len(active) > 1:
                    raise ConversionError(f"{name}: {webxr} moves by more than one response")
                motion = fit_button(glb, active[0], rest, maximum) if active else None
            if motion:
                if axis is not None:
                    motion["controller_axis"] = axis
                if button is not None:
                    motion["controller_button"] = button
                entry["motion"] = motion
                # The self-check: SteamVR's motion half and fully pressed (x right, y back) against the GLB posed so
                for f in (0.5, 1.0) if part["meshes"] or dot_point is not None else ():
                    q, t = component_pose(motion, f, (f, -f))
                    full = dict(rest_overrides)
                    full.update({r.value: r.pose(glb, f if r.prop == "button" else (1 + f) / 2) for r in active})
                    posed = world_matrices(glb, full)
                    if dot_point is None:
                        want = mesh_groups(part["meshes"], posed)
                        pairs = [(p, target) for (_, v0, _), (_, v1, _) in zip(groups, want)
                                 for (p, _, _), (target, _, _) in zip(v0, v1)]
                    else:
                        # A generated dot: its centre against the touch point
                        pairs = [(apply(rest[dot_point], (0, 0, 0)), apply(posed[dot_point], (0, 0, 0)))]
                    for p, target in pairs:
                        off = length(sub(add(quat_rotate(q, p), t), target))
                        worst = max(worst, off)
                        if off > TOLERANCE_M:
                            raise ConversionError(
                                f"{name}: {key}'s motion is {off * 1000:.2f} mm off the model's pose at {f}")
        if not groups:
            continue
        if sum(len(v) for _, v, _ in groups) > 65535:
            raise ConversionError(f"{name}: {key} has more vertices than a render model's 65535")
        obj = f"{key}.obj"
        files[obj] = obj_text(mtl, groups).encode()
        entry["filename"] = obj
        components[key] = entry

    files[f"{name}.json"] = (json.dumps({"components": components}, indent=1, sort_keys=True) + "\n").encode()
    return worst


def merge_hands(hands: dict) -> dict:
    """Both hands' files as one folder's: a file the same for both keeps its name, one that differs gets the hand's
    "<hand>_" in front, and the names in the files that refer to it are changed to match"""
    out = {}
    renamed = {hand: {} for hand in hands}
    # Textures first, then the material libraries naming them, the OBJs naming those, the JSON naming the OBJs
    order = lambda f: (f.endswith(".json"), f.endswith(".obj"), f.endswith(".mtl"), f)
    names = sorted({f for files in hands.values() for f in files}, key=order)
    for f in names:
        contents = {}
        for hand, files in hands.items():
            if f not in files:
                continue
            data = files[f]
            for old, new in sorted(renamed[hand].items(), key=lambda kv: -len(kv[0])):
                if f.endswith((".mtl", ".obj")):
                    data = data.replace(f"map_Kd {old}\n".encode(), f"map_Kd {new}\n".encode())
                    data = data.replace(f"mtllib {old}\n".encode(), f"mtllib {new}\n".encode())
                elif f.endswith(".json"):
                    data = data.replace(f'"filename": "{old}"'.encode(), f'"filename": "{new}"'.encode())
            contents[hand] = data
        if len(set(contents.values())) == 1:
            out[f] = next(iter(contents.values()))
        else:
            for hand, data in contents.items():
                name = f if f.endswith(".json") else f"{hand}_{f}"
                renamed[hand][f] = name
                out[name] = data
    return out


def convert(assets: Path, out: Path) -> list[str]:
    """Every profile's models into out/<profile>/: <profile>-left.json and <profile>-right.json, and their files"""
    report = []
    for profile_id, table in PROFILES.items():
        folder = assets / profile_id
        profile = json.loads((folder / "profile.json").read_text(encoding="utf-8"))
        layouts = profile["layouts"]
        hands = {}
        for hand in ("left", "right"):
            layout = layouts.get(hand) or layouts.get("left-right") or layouts.get("none") or layouts.get("left-right-none")
            if layout is None:
                raise ConversionError(f"{profile_id}: no layout for the {hand} hand")
            name = f"{profile_id}-{hand}"
            hands[hand] = {}
            try:
                worst = convert_layout(folder / layout["assetPath"], layout, table, name, hands[hand])
            except ConversionError as e:
                raise ConversionError(f"{profile_id} ({hand}): {e}") from e
            report.append(f"{name}: within {worst * 1000:.3f} mm of the model's poses")
        target = out / profile_id
        target.mkdir(parents=True, exist_ok=True)
        files = merge_hands(hands)
        for old in target.iterdir():
            if old.name not in files:
                old.unlink()
        for f, data in files.items():
            path = target / f
            if not path.is_file() or path.read_bytes() != data:
                path.write_bytes(data)
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()
    try:
        report = convert(args.assets, args.out)
    except (ConversionError, OSError, KeyError, ValueError) as e:
        sys.exit(f"rendermodels.py: {e}")
    if not args.quiet:
        print("\n".join(report))


if __name__ == "__main__":
    main()
