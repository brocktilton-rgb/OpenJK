#!/usr/bin/env python3
"""Decode OpenJK single-player bridge UDP packets.

Layout matches docs/BRIDGE.md (version 1, little-endian). No third-party packages.

Examples:
    python tools/bridge_receiver.py
    python tools/bridge_receiver.py --port 27420 --summary
    python tools/bridge_receiver.py --self-test
"""

from __future__ import annotations

import argparse
import socket
import struct
import sys
import time

MAGIC = b"OJB1"
VERSION = 1
PACKET_POSE = 1
PACKET_MAP = 2
PACKET_SABER = 3
PACKET_RENDER = 4
POSE_SIZE = 160
MAP_SIZE = 84
SABER_SIZE = 672
RENDER_SIZE = 228
MAP_NAME_LEN = 64
HILT_NAME_LEN = 64
SABER_BLADE_SLOTS = 16
SABER_BLADE_BYTES = 32
SABER_FLAG_DUAL = 0x01
SABER_FLAG_IN_FLIGHT = 0x02
RENDER_HAND_R = 0x01
RENDER_HAND_L = 0x02
SABER_COLORS = ("red", "orange", "yellow", "green", "blue", "purple")
SABER_TYPES = (
    "none", "single", "staff", "dagger", "broad", "prong", "arc",
    "sai", "claw", "lance", "star", "trident", "sith_sword",
)

FLAG_THIRDPERSON = 0x00000001
FLAG_CINEMATIC = 0x00000002
FLAG_ZOOM = 0x00000004
FLAG_INTERMISSION = 0x00000008
FLAG_PLAYER_VALID = 0x00000010


class PacketError(Exception):
    pass


def _u16(buf: bytes, offset: int) -> int:
    return struct.unpack_from("<H", buf, offset)[0]


def _u32(buf: bytes, offset: int) -> int:
    return struct.unpack_from("<I", buf, offset)[0]


def _i32(buf: bytes, offset: int) -> int:
    return struct.unpack_from("<i", buf, offset)[0]


def _f32(buf: bytes, offset: int) -> float:
    return struct.unpack_from("<f", buf, offset)[0]


def _vec3(buf: bytes, offset: int) -> tuple[float, float, float]:
    return struct.unpack_from("<3f", buf, offset)


def _header(buf: bytes) -> dict:
    if len(buf) < 20:
        raise PacketError("datagram is shorter than the 20-byte header (%d)" % len(buf))
    magic = buf[0:4]
    if magic != MAGIC:
        raise PacketError("bad magic %r" % (magic,))
    version = _u16(buf, 4)
    if version != VERSION:
        raise PacketError("unsupported version %d" % version)
    return {
        "version": version,
        "type": _u16(buf, 6),
        "sequence": _u32(buf, 8),
        "frame": _u32(buf, 12),
        "server_time": _i32(buf, 16),
    }


def decode_packet(buf: bytes) -> dict:
    header = _header(buf)
    kind = header["type"]
    if kind == PACKET_POSE:
        if len(buf) < POSE_SIZE:
            raise PacketError("short pose packet (%d < %d)" % (len(buf), POSE_SIZE))
        header.update({
            "cam_origin": _vec3(buf, 20),
            "cam_angles": _vec3(buf, 32),
            "cam_forward": _vec3(buf, 44),
            "cam_right": _vec3(buf, 56),
            "cam_up": _vec3(buf, 68),
            "fov_x": _f32(buf, 80),
            "fov_y": _f32(buf, 84),
            "player_origin": _vec3(buf, 88),
            "view_angles": _vec3(buf, 100),
            "velocity": _vec3(buf, 112),
            "legs_anim": _i32(buf, 124),
            "legs_anim_timer": _i32(buf, 128),
            "torso_anim": _i32(buf, 132),
            "torso_anim_timer": _i32(buf, 136),
            "legs_frame": _f32(buf, 140),
            "torso_frame": _f32(buf, 144),
            "flags": _u32(buf, 148),
            "weapon": _i32(buf, 152),
            "pm_type": _i32(buf, 156),
        })
        return header
    if kind == PACKET_MAP:
        if len(buf) < MAP_SIZE:
            raise PacketError("short map packet (%d < %d)" % (len(buf), MAP_SIZE))
        raw = buf[20:20 + MAP_NAME_LEN]
        header["map_name"] = raw.split(b"\x00", 1)[0].decode("ascii", "replace")
        return header
    if kind == PACKET_SABER:
        if len(buf) < SABER_SIZE:
            raise PacketError("short saber packet (%d < %d)" % (len(buf), SABER_SIZE))
        header["blade_count"] = _u16(buf, 20)
        header["saber_count"] = buf[22]
        header["saber_flags"] = buf[23]
        header["hilt0"] = buf[24:24 + HILT_NAME_LEN].split(b"\x00", 1)[0].decode("ascii", "replace")
        header["saber_type0"] = _i32(buf, 88)
        header["hilt1"] = buf[92:92 + HILT_NAME_LEN].split(b"\x00", 1)[0].decode("ascii", "replace")
        header["saber_type1"] = _i32(buf, 156)
        blades = []
        base = 160
        count = header["blade_count"]
        if count > SABER_BLADE_SLOTS:
            count = SABER_BLADE_SLOTS
        for index in range(count):
            off = base + index * SABER_BLADE_BYTES
            blades.append({
                "saber_index": buf[off],
                "blade_index": buf[off + 1],
                "active": buf[off + 2],
                "color_index": buf[off + 3],
                "length": _f32(buf, off + 4),
                "base": _vec3(buf, off + 8),
                "tip": _vec3(buf, off + 20),
            })
        header["blades"] = blades
        return header
    # One datagram is one packet. Unknown types are ignored so older readers
    # of type 1 and type 2 keep working when new types show up.
    if kind == PACKET_RENDER:
        if len(buf) < RENDER_SIZE:
            raise PacketError("short render packet (%d < %d)" % (len(buf), RENDER_SIZE))
        header["client_time"] = _i32(buf, 20)
        header["player_origin"] = _vec3(buf, 24)
        header["player_angles"] = _vec3(buf, 36)
        header["cam_origin"] = _vec3(buf, 48)
        header["cam_angles"] = _vec3(buf, 60)
        header["cam_forward"] = _vec3(buf, 72)
        header["cam_right"] = _vec3(buf, 84)
        header["cam_up"] = _vec3(buf, 96)
        header["fov_x"] = _f32(buf, 108)
        header["fov_y"] = _f32(buf, 112)
        header["legs_frame"] = _f32(buf, 116)
        header["torso_frame"] = _f32(buf, 120)
        header["origin_lerp"] = _f32(buf, 124)
        header["hand_flags"] = buf[128]
        header["hand_r"] = _read_hand(buf, 132)
        header["hand_l"] = _read_hand(buf, 180)
        return header
    header["skipped"] = True
    return header


def _read_hand(buf: bytes, offset: int) -> dict:
    return {
        "origin": _vec3(buf, offset),
        "axis_x": _vec3(buf, offset + 12),
        "axis_y": _vec3(buf, offset + 24),
        "axis_z": _vec3(buf, offset + 36),
    }


def _fmt_vec(value: tuple[float, float, float]) -> str:
    return "(%.2f, %.2f, %.2f)" % value


def _flag_names(flags: int) -> str:
    names = []
    if flags & FLAG_THIRDPERSON:
        names.append("thirdperson")
    if flags & FLAG_CINEMATIC:
        names.append("cinematic")
    if flags & FLAG_ZOOM:
        names.append("zoom")
    if flags & FLAG_INTERMISSION:
        names.append("intermission")
    if flags & FLAG_PLAYER_VALID:
        names.append("player")
    if not names:
        return "-"
    return ",".join(names)


def _saber_color(index: int) -> str:
    if 0 <= index < len(SABER_COLORS):
        return SABER_COLORS[index]
    return "color%d" % index


def _saber_type(index: int) -> str:
    if 0 <= index < len(SABER_TYPES):
        return SABER_TYPES[index]
    return "type%d" % index


def _saber_flag_names(flags: int) -> str:
    names = []
    if flags & SABER_FLAG_DUAL:
        names.append("dual")
    if flags & SABER_FLAG_IN_FLIGHT:
        names.append("inflight")
    if not names:
        return "-"
    return ",".join(names)


def format_packet(packet: dict, verbose: bool) -> str:
    if packet.get("skipped"):
        return "skip type=%d seq=%d frame=%d" % (packet["type"], packet["sequence"], packet["frame"])
    if packet["type"] == PACKET_SABER:
        lines = [
            "saber seq=%d frame=%d t=%d blades=%d sabers=%d flags=0x%x(%s) hilt0=%s(%s) hilt1=%s(%s)"
            % (
                packet["sequence"],
                packet["frame"],
                packet["server_time"],
                packet["blade_count"],
                packet["saber_count"],
                packet["saber_flags"],
                _saber_flag_names(packet["saber_flags"]),
                packet["hilt0"] or '""',
                _saber_type(packet["saber_type0"]),
                packet["hilt1"] or '""',
                _saber_type(packet["saber_type1"]),
            )
        ]
        for blade in packet["blades"]:
            lines.append(
                "  blade s%d.b%d %s color=%d(%s) len=%.2f base=%s tip=%s"
                % (
                    blade["saber_index"],
                    blade["blade_index"],
                    "on" if blade["active"] else "off",
                    blade["color_index"],
                    _saber_color(blade["color_index"]),
                    blade["length"],
                    _fmt_vec(blade["base"]),
                    _fmt_vec(blade["tip"]),
                )
            )
        return "\n".join(lines)
    if packet["type"] == PACKET_RENDER:
        hands = []
        if packet["hand_flags"] & RENDER_HAND_R:
            hands.append("R")
        if packet["hand_flags"] & RENDER_HAND_L:
            hands.append("L")
        line = (
            "render seq=%d frame=%d t=%d client=%d lerp=%.2f org=%s ang=%s "
            "cam=%s fov=%.2fx%.2f frames=%.2f/%.2f hands=%s"
            % (
                packet["sequence"],
                packet["frame"],
                packet["server_time"],
                packet["client_time"],
                packet["origin_lerp"],
                _fmt_vec(packet["player_origin"]),
                _fmt_vec(packet["player_angles"]),
                _fmt_vec(packet["cam_origin"]),
                packet["fov_x"],
                packet["fov_y"],
                packet["legs_frame"],
                packet["torso_frame"],
                "".join(hands) or "-",
            )
        )
        if verbose:
            line += " axisF=%s handR=%s handL=%s" % (
                _fmt_vec(packet["cam_forward"]),
                _fmt_vec(packet["hand_r"]["origin"]),
                _fmt_vec(packet["hand_l"]["origin"]),
            )
        return line
    if packet["type"] == PACKET_MAP:
        return "map  seq=%d frame=%d t=%d name=%s" % (
            packet["sequence"], packet["frame"], packet["server_time"], packet["map_name"] or '""'
        )
    line = (
        "pose seq=%d frame=%d t=%d cam=%s ang=%s fov=%.2fx%.2f "
        "org=%s view=%s vel=%s legs=%d/%d torso=%d/%d frames=%.2f/%.2f "
        "flags=0x%x(%s) weapon=%d pm=%d"
        % (
            packet["sequence"],
            packet["frame"],
            packet["server_time"],
            _fmt_vec(packet["cam_origin"]),
            _fmt_vec(packet["cam_angles"]),
            packet["fov_x"],
            packet["fov_y"],
            _fmt_vec(packet["player_origin"]),
            _fmt_vec(packet["view_angles"]),
            _fmt_vec(packet["velocity"]),
            packet["legs_anim"],
            packet["legs_anim_timer"],
            packet["torso_anim"],
            packet["torso_anim_timer"],
            packet["legs_frame"],
            packet["torso_frame"],
            packet["flags"],
            _flag_names(packet["flags"]),
            packet["weapon"],
            packet["pm_type"],
        )
    )
    if verbose:
        line += " axisF=%s axisR=%s axisU=%s" % (
            _fmt_vec(packet["cam_forward"]),
            _fmt_vec(packet["cam_right"]),
            _fmt_vec(packet["cam_up"]),
        )
    return line


class SequenceTracker:
    def __init__(self) -> None:
        self.last = None
        self.gaps = 0

    def note(self, sequence: int) -> int:
        if self.last is None:
            self.last = sequence
            return 0
        expected = (self.last + 1) & 0xFFFFFFFF
        self.last = sequence
        if sequence == expected:
            return 0
        # Level changes reset the counter to 1.
        if sequence < expected and sequence <= 2:
            return 0
        missed = (sequence - expected) & 0xFFFFFFFF
        if missed > 100000:
            return 0
        self.gaps += missed
        return missed


def _pack_pose() -> bytes:
    # Identity-ish camera: forward +X, right -Y, up +Z (id Tech 3).
    axis = (1.0, 0.0, 0.0, 0.0, -1.0, 0.0, 0.0, 0.0, 1.0)
    payload = struct.pack(
        "<4sHHIIi3f3f9f2f3f3f3f4i2fIii",
        MAGIC,
        VERSION,
        PACKET_POSE,
        7,
        42,
        1500,
        10.0, 20.0, 30.0,
        -5.0, 90.0, 0.0,
        *axis,
        80.0, 64.0,
        1.0, 2.0, 3.0,
        4.0, 5.0, 6.0,
        7.0, 8.0, 9.0,
        100, 250, 110, 80,
        12.5, 4.25,
        FLAG_THIRDPERSON | FLAG_PLAYER_VALID,
        6,
        0,
    )
    if len(payload) != POSE_SIZE:
        raise AssertionError("pose pack size %d" % len(payload))
    return payload


def _pack_map() -> bytes:
    name = b"maps/kejim_post.bsp"
    payload = struct.pack(
        "<4sHHIIi64s",
        MAGIC,
        VERSION,
        PACKET_MAP,
        8,
        42,
        1500,
        name + b"\x00" * (MAP_NAME_LEN - len(name)),
    )
    if len(payload) != MAP_SIZE:
        raise AssertionError("map pack size %d" % len(payload))
    return payload


def _pack_saber() -> bytes:
    buf = bytearray(SABER_SIZE)
    struct.pack_into("<4sHHIIi", buf, 0, MAGIC, VERSION, PACKET_SABER, 9, 42, 1500)
    struct.pack_into("<HBB", buf, 20, 2, 1, 0)
    buf[24:28] = b"Kyle"
    struct.pack_into("<i", buf, 88, 2)  # staff
    struct.pack_into(
        "<BBBBf3f3f", buf, 160,
        0, 0, 1, 4, 32.0,
        1.0, 2.0, 3.0,
        33.0, 2.0, 3.0,
    )
    struct.pack_into(
        "<BBBBf3f3f", buf, 160 + SABER_BLADE_BYTES,
        0, 1, 0, 4, 0.0,
        1.0, 2.0, 40.0,
        1.0, 2.0, 40.0,
    )
    if len(buf) != SABER_SIZE:
        raise AssertionError("saber pack size %d" % len(buf))
    return bytes(buf)


def _pack_render() -> bytes:
    buf = bytearray(RENDER_SIZE)
    struct.pack_into("<4sHHIIi", buf, 0, MAGIC, VERSION, PACKET_RENDER, 11, 42, 1600)
    struct.pack_into("<i", buf, 20, 5000)
    struct.pack_into("<3f", buf, 24, 1.0, 2.0, 3.0)
    struct.pack_into("<3f", buf, 36, 0.0, 90.0, 0.0)
    struct.pack_into("<3f", buf, 48, 4.0, 5.0, 6.0)
    struct.pack_into("<3f", buf, 60, -5.0, 90.0, 0.0)
    struct.pack_into("<9f", buf, 72, 1, 0, 0, 0, -1, 0, 0, 0, 1)
    struct.pack_into("<2f", buf, 108, 80.0, 64.0)
    struct.pack_into("<2f", buf, 116, 12.5, 4.25)
    struct.pack_into("<f", buf, 124, 0.5)
    buf[128] = RENDER_HAND_R | RENDER_HAND_L
    struct.pack_into("<3f", buf, 132, 10, 0, 40)
    struct.pack_into("<3f", buf, 144, 1, 0, 0)
    struct.pack_into("<3f", buf, 156, 0, -1, 0)
    struct.pack_into("<3f", buf, 168, 0, 0, 1)
    struct.pack_into("<3f", buf, 180, -10, 0, 40)
    if len(buf) != RENDER_SIZE:
        raise AssertionError("render pack size %d" % len(buf))
    return bytes(buf)


def self_test() -> int:
    pose = decode_packet(_pack_pose())
    assert pose["sequence"] == 7
    assert pose["frame"] == 42
    assert pose["server_time"] == 1500
    assert pose["cam_origin"] == (10.0, 20.0, 30.0)
    assert pose["cam_angles"] == (-5.0, 90.0, 0.0)
    assert pose["cam_forward"] == (1.0, 0.0, 0.0)
    assert pose["cam_right"] == (0.0, -1.0, 0.0)
    assert pose["cam_up"] == (0.0, 0.0, 1.0)
    assert pose["fov_x"] == 80.0 and pose["fov_y"] == 64.0
    assert pose["player_origin"] == (1.0, 2.0, 3.0)
    assert pose["view_angles"] == (4.0, 5.0, 6.0)
    assert pose["velocity"] == (7.0, 8.0, 9.0)
    assert pose["legs_anim"] == 100 and pose["legs_anim_timer"] == 250
    assert pose["torso_anim"] == 110 and pose["torso_anim_timer"] == 80
    assert pose["legs_frame"] == 12.5 and pose["torso_frame"] == 4.25
    assert pose["flags"] == (FLAG_THIRDPERSON | FLAG_PLAYER_VALID)
    assert pose["weapon"] == 6 and pose["pm_type"] == 0

    map_packet = decode_packet(_pack_map())
    assert map_packet["map_name"] == "maps/kejim_post.bsp"
    assert map_packet["sequence"] == 8

    saber = decode_packet(_pack_saber())
    assert saber["sequence"] == 9
    assert saber["blade_count"] == 2 and saber["saber_count"] == 1
    assert saber["hilt0"] == "Kyle" and saber["saber_type0"] == 2
    assert saber["hilt1"] == "" and saber["saber_type1"] == 0
    assert saber["blades"][0]["active"] == 1 and saber["blades"][0]["color_index"] == 4
    assert saber["blades"][0]["length"] == 32.0
    assert saber["blades"][0]["base"] == (1.0, 2.0, 3.0)
    assert saber["blades"][0]["tip"] == (33.0, 2.0, 3.0)
    assert saber["blades"][1]["active"] == 0 and saber["blades"][1]["blade_index"] == 1

    render = decode_packet(_pack_render())
    assert render["client_time"] == 5000
    assert render["player_origin"] == (1.0, 2.0, 3.0)
    assert render["player_angles"] == (0.0, 90.0, 0.0)
    assert render["cam_origin"] == (4.0, 5.0, 6.0)
    assert render["cam_forward"] == (1.0, 0.0, 0.0)
    assert render["cam_right"] == (0.0, -1.0, 0.0)
    assert render["origin_lerp"] == 0.5
    assert render["legs_frame"] == 12.5 and render["torso_frame"] == 4.25
    assert render["hand_flags"] == (RENDER_HAND_R | RENDER_HAND_L)
    assert render["hand_r"]["origin"] == (10.0, 0.0, 40.0)
    assert render["hand_r"]["axis_y"] == (0.0, -1.0, 0.0)
    assert render["hand_l"]["origin"] == (-10.0, 0.0, 40.0)
    # Type 1 and type 2 still decode after a type 3 packet, and unknown types are skipped.
    assert decode_packet(_pack_pose())["type"] == PACKET_POSE
    unknown = bytearray(24)
    struct.pack_into("<4sHHIIi", unknown, 0, MAGIC, VERSION, 99, 10, 1, 0)
    skipped = decode_packet(bytes(unknown))
    assert skipped.get("skipped") and skipped["type"] == 99

    try:
        decode_packet(b"NOPE" + b"\x00" * 20)
        raise AssertionError("bad magic was accepted")
    except PacketError:
        pass

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind(("127.0.0.1", 0))
        sock.settimeout(2.0)
        dest = sock.getsockname()
        for payload in (_pack_map(), _pack_pose(), _pack_saber(), _pack_render()):
            sent = sock.sendto(payload, dest)
            if sent != len(payload):
                raise AssertionError("short UDP send")
            data, _addr = sock.recvfrom(2048)
            decoded = decode_packet(data)
            kind = struct.unpack_from("<H", payload, 6)[0]
            if kind == PACKET_MAP:
                assert decoded["map_name"] == "maps/kejim_post.bsp"
            elif kind == PACKET_POSE:
                assert decoded["cam_origin"] == (10.0, 20.0, 30.0)
            elif kind == PACKET_SABER:
                assert decoded["hilt0"] == "Kyle" and len(decoded["blades"]) == 2
            else:
                assert decoded["hand_r"]["origin"] == (10.0, 0.0, 40.0)
    finally:
        sock.close()

    print(
        "bridge_receiver self-test ok (pose %d, map %d, saber %d, render %d bytes)"
        % (POSE_SIZE, MAP_SIZE, SABER_SIZE, RENDER_SIZE)
    )
    return 0


def listen(host: str, port: int, verbose: bool, summary: bool) -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((host, port))
    except OSError as exc:
        print("could not bind %s:%d: %s" % (host, port, exc), file=sys.stderr)
        return 1

    print(
        "OpenJK bridge receiver listening on %s:%d (pose %d, map %d, saber %d, render %d bytes, version %d)"
        % (host, port, POSE_SIZE, MAP_SIZE, SABER_SIZE, RENDER_SIZE, VERSION),
        flush=True,
    )
    print("Ctrl+C to stop. Start a level in OpenJK; the main menu does not send poses.", flush=True)

    tracker = SequenceTracker()
    window_start = time.monotonic()
    window_count = 0
    total = 0
    bad = 0
    poses = 0
    maps = 0
    sabers = 0
    renders = 0
    skipped = 0
    last_pose = None

    try:
        while True:
            try:
                data, _addr = sock.recvfrom(2048)
            except socket.timeout:
                continue
            total += 1
            window_count += 1
            try:
                packet = decode_packet(data)
            except PacketError as exc:
                bad += 1
                print("bad  %s (%d bytes)" % (exc, len(data)), flush=True)
                packet = None

            if packet is not None:
                missed = tracker.note(packet["sequence"])
                if packet.get("skipped"):
                    skipped += 1
                elif packet["type"] == PACKET_POSE:
                    poses += 1
                    last_pose = packet
                elif packet["type"] == PACKET_MAP:
                    maps += 1
                elif packet["type"] == PACKET_SABER:
                    sabers += 1
                elif packet["type"] == PACKET_RENDER:
                    renders += 1
                if not summary:
                    suffix = "" if missed == 0 else "  gap=%d" % missed
                    print(format_packet(packet, verbose) + suffix, flush=True)
                elif packet["type"] == PACKET_MAP:
                    print(format_packet(packet, verbose), flush=True)

            now = time.monotonic()
            elapsed = now - window_start
            if elapsed >= 1.0:
                rate = window_count / elapsed
                print(
                    "rate %.1f pkt/s  window=%d total=%d pose=%d map=%d saber=%d render=%d skip=%d bad=%d seq_gaps=%d"
                    % (rate, window_count, total, poses, maps, sabers, renders, skipped, bad, tracker.gaps),
                    flush=True,
                )
                if summary and last_pose is not None:
                    print(format_packet(last_pose, verbose), flush=True)
                window_start = now
                window_count = 0
    except KeyboardInterrupt:
        print(
            "\nstopped. total=%d pose=%d map=%d saber=%d render=%d skip=%d bad=%d seq_gaps=%d"
            % (total, poses, maps, sabers, renders, skipped, bad, tracker.gaps)
        )
        return 0
    finally:
        sock.close()


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Print OpenJK bridge UDP packets and the packet rate.")
    parser.add_argument("--host", default="127.0.0.1", help="bind address (default 127.0.0.1)")
    parser.add_argument("--port", type=int, default=27420, help="UDP port (default 27420)")
    parser.add_argument("--verbose", action="store_true", help="also print the camera axis vectors")
    parser.add_argument("--summary", action="store_true", help="print a pose once per second instead of every packet")
    parser.add_argument("--self-test", action="store_true", help="check the decoder and a localhost UDP loopback, then exit")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()
    if not (0 < args.port < 65536):
        print("port must be 1..65535", file=sys.stderr)
        return 1
    return listen(args.host, args.port, args.verbose, args.summary)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
