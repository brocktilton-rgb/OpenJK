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
POSE_SIZE = 160
MAP_SIZE = 84
MAP_NAME_LEN = 64

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
    raise PacketError("unknown packet type %d" % kind)


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


def format_packet(packet: dict, verbose: bool) -> str:
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
        for payload in (_pack_map(), _pack_pose()):
            sent = sock.sendto(payload, dest)
            if sent != len(payload):
                raise AssertionError("short UDP send")
            data, _addr = sock.recvfrom(2048)
            decoded = decode_packet(data)
            if payload[6:8] == struct.pack("<H", PACKET_MAP):
                assert decoded["map_name"] == "maps/kejim_post.bsp"
            else:
                assert decoded["cam_origin"] == (10.0, 20.0, 30.0)
    finally:
        sock.close()

    print("bridge_receiver self-test ok (pose %d bytes, map %d bytes)" % (POSE_SIZE, MAP_SIZE))
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
        "OpenJK bridge receiver listening on %s:%d (pose %d bytes, map %d bytes, version %d)"
        % (host, port, POSE_SIZE, MAP_SIZE, VERSION),
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
                if packet["type"] == PACKET_POSE:
                    poses += 1
                    last_pose = packet
                elif packet["type"] == PACKET_MAP:
                    maps += 1
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
                    "rate %.1f pkt/s  window=%d total=%d pose=%d map=%d bad=%d seq_gaps=%d"
                    % (rate, window_count, total, poses, maps, bad, tracker.gaps),
                    flush=True,
                )
                if summary and last_pose is not None:
                    print(format_packet(last_pose, verbose), flush=True)
                window_start = now
                window_count = 0
    except KeyboardInterrupt:
        print("\nstopped. total=%d pose=%d map=%d bad=%d seq_gaps=%d" % (total, poses, maps, bad, tracker.gaps))
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
