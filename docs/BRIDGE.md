# OpenJK single-player camera bridge

Proof of concept for a Jedi Academy remaster-as-mod. OpenJK stays the gameplay authority. This bridge streams the camera and the player pose out of the single-player game, once per rendered frame, as a compact UDP datagram. A later Unreal Engine client (or `tools/bridge_receiver.py` today) listens on localhost and follows along. Nothing is sent unless the bridge is turned on.

The stream is single-player only. It is compiled into the SP game module (`jagame*.dll` / `jagame*.so`), which is where both the game and the client game live.

## Install

Use the CI build from this pull request, not the normal OpenJK download. The zip named like `OpenJK-windows-x86_64.zip` on the OpenJK releases page has no bridge. With that zip the console never prints a Bridge line and no packets are sent.

1. On the pull request, open Checks. Open the job **Windows x86_64 Release (Non-Portable)**. Download the artifact **`OpenJK-windows-x86_64-Release-Non-Portable`**.
2. Unzip it. You should see `openjk_sp.x86_64.exe` and a folder named `OpenJK`.
3. Go to your retail GameData folder. Steam's copy is `steamapps\common\Jedi Academy\GameData`.
4. Back up these three files (copy them somewhere else) before you replace them:
   - `openjk_sp.x86_64.exe`
   - `rdsp-vanilla_x86_64.dll`
   - `OpenJK\jagamex86_64.dll`
5. Copy those same three files from the zip over the ones in GameData. The dll goes in the `OpenJK` folder, not in `base`.
6. Do not replace anything in `base\`. Do not add `+set fs_game` and do not create a mod folder. A second `jagamex86_64.dll` in `base\` or in `Documents\My Games\OpenJK\base\` is loaded instead of the one you just copied.

A 32-bit install uses the artifact `OpenJK-windows-x86-Release-Non-Portable` and the names `openjk_sp.x86.exe`, `rdsp-vanilla_x86.dll`, and `OpenJK\jagamex86.dll`.

## Launch

Start the game from the GameData folder. `bridge_host` must be numbers and dots, such as `127.0.0.1`. A computer name is rejected. `bridge_port` stays `27420` unless you change both the game and the listener. `bridge_rate 0` sends every frame. A higher number is a cap, for example 30 packets a second.

1. Open a command prompt in GameData.
2. Run:

```
openjk_sp.x86_64.exe +set bridge_enable 1 +set bridge_host 127.0.0.1 +set bridge_port 27420 +set logfile 2 +devmap t1_rail
```

`+devmap t1_rail` starts the Pit level so you do not have to click through the menus. `+set logfile 2` writes every console line to `Documents\My Games\OpenJK\base\qconsole.log` as it happens.

To cap the stream:

```
openjk_sp.x86_64.exe +set bridge_enable 1 +set bridge_host 127.0.0.1 +set bridge_port 27420 +set bridge_rate 30 +set logfile 2 +devmap t1_rail
```

After the level is up, the console should show:

```
Bridge: OJB1 v1 UDP pose stream -> 127.0.0.1:27420 (rate 0)
```

`OJB1 v1` is this protocol. An older dll will not print that line. In the console you can also type `bridge_status`. It prints whether the bridge is on, the address, how many pose, map, and saber packets were sent, and the last send error.

Turn it off with `set bridge_enable 0`. These settings are saved, so the next launch keeps them until you set `bridge_enable` back to `0`.

## How to check it

1. The console line above must appear after the level starts. The main menu does not send packets.
2. Open `Documents\My Games\OpenJK\base\qconsole.log` and search for `Bridge: OJB1 v1`. That file exists only if you launched with `+set logfile 2`.
3. In PowerShell, check that the dll you installed is the bridge build. `True` means the string is there. `False` means this is the vanilla dll:

```
$dll = "C:\Program Files (x86)\Steam\steamapps\common\Jedi Academy\GameData\OpenJK\jagamex86_64.dll"
[Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($dll)).Contains('bridge_enable')
```

Change the path if your GameData folder is somewhere else.

4. From a copy of this repo, with Python 3 installed and no extra packages:

```
py -3 tools\bridge_receiver.py --self-test
```

That only checks the decoder. Then, before or while the game is running:

```
py -3 tools\bridge_receiver.py --port 27420 --summary
```

You should see a `map` line with `maps/t1_rail.bsp`, `pose` lines, and a `rate` line about once a second. `--summary` prints one pose a second instead of every frame. Drop `--summary` to print every packet. With the lightsaber out, a `saber` line is sent on the same frames. The Unreal saber picture is not hooked up yet, so you will not see the blade in the editor even though the packet is on the wire.

## If nothing arrives

1. You installed the vanilla `OpenJK-windows-x86_64.zip` from the OpenJK site. That build has no bridge. Use the CI artifact named `OpenJK-windows-x86_64-Release-Non-Portable` and replace all three files again.
2. The PowerShell check printed `False`. Same problem: the dll on disk is not the bridge dll.
3. Another `jagamex86_64.dll` is sitting in `GameData\base\` or in `Documents\My Games\OpenJK\base\`. The game loads that copy first. Remove it. Do not set `fs_game`.
4. The game and the listener are not on the same port. Both must use `27420` unless you changed both.
5. `bridge_host` is a name instead of numbers. The console warns once: `bridge_host "..." is not a numeric IPv4 address`. Set it to `127.0.0.1`.
6. Windows Firewall asked and you blocked the game. Localhost usually does not ask. If it does, allow it on private networks, or keep both sides on `127.0.0.1`.
7. You are still on the main menu. Start a level. `+devmap t1_rail` does that for you.
8. Type `bridge_status` in the game console. `streaming: no` and a line under `not streaming` tell you why.

## What you should see today

The bridge sends the camera and the player pose, the map name, the saber blades when a lightsaber is the weapon, and mover motion (type 7). It does not send the level mesh. Rooms come from `tools/bsp2gltf`. Until those meshes are in Unreal, a ground-only view is still the right picture.

A client that only reads type 1 will still see the body step about 20 times a second. That packet is sent every render frame, but the origin inside it only changes on the server tick. Type 4 is the smooth pose. Point the new Unreal reader at type 4.

## Not done yet

1. Level meshes are exported by `tools/bsp2gltf`, not by the UDP stream. Unreal still has to load that glTF.
2. NPCs are type 5 and movers are type 7. Other entity kinds are not sent.
3. Saber packets are in this build, but the Unreal saber picture was not checked, because that part of the editor is not hooked up yet.
4. A list of hilt names (`saberInfo_t::name` values such as `Kyle`) is planned as `tools/list_saber_hilts.py`. That script is not in this branch.
5. On GitHub, the macOS arm64 jobs and the Ubuntu Debug job have failed or been cancelled because the runner never started or the job hit the time limit. That is the build service, not this code. The job that matters for the Windows test is **Windows x86_64 Release (Non-Portable)**.

## Hook point

The packet is built at the start of `CG_DrawActive` (`code/cgame/cg_draw.cpp`), after the snapshot check and before the stereo eye offset.

That function runs only when a real scene is about to be submitted. By then `CG_DrawActiveFrame` has already:

- predicted the local player (`cg.predicted_player_state`)
- resolved the camera, including third person, zoom, cinematic cameras (`in_camera` / `CGCam_RenderScene`), and emplaced guns (`CG_RunEmplacedWeapon`)
- written the final view into `cg.refdef` (origin, axis, `fov_x`, `fov_y`) and `cg.refdefViewAngles`
- evaluated the player model, including Ghoul2 bone frames

The renderer is the wrong side of this. It receives a `refdef` and a bag of draw entities, not the player-state anim numbers, timers, weapon, or movement type. Sampling any earlier (`CG_CalcViewValues` alone) misses cinematic cameras and emplaced-gun overrides, which replace the view after that function returns.

The skybox portal is a second, internal scene. It is not streamed. Stereo draws the same pose twice; the right eye is skipped so a frame produces one pose packet. The camera in the packet is the center camera, before the stereo eye offset.

Loading screens and the main menu do not emit poses. `CG_DrawActive` returns before the hook when there is no snapshot.

Sockets are non-blocking UDP. `sendto` either queues the datagram or returns immediately if the kernel buffer is full. The game never waits on the network, never resolves DNS, and closes the socket from `CG_Shutdown` (level change and quit).

## Cvars

All four are archived. The default is off.

| Cvar | Default | Meaning |
| --- | --- | --- |
| `bridge_enable` | `0` | `1` streams. `0` closes the socket. |
| `bridge_host` | `127.0.0.1` | Numeric IPv4 only. Hostnames are rejected so the game cannot block in DNS. |
| `bridge_port` | `27420` | UDP destination port. |
| `bridge_rate` | `0` | Maximum samples per second, using client realtime. `0` sends every rendered frame. Clamped to 480. The interval is `1000 / rate` milliseconds. A sample is the map packet (when one is due), the pose packet, the saber packet when a lightsaber is out, the render packet, the NPC packets, the HUD packet, and the mover packets. |
| `bridge_npc_radius` | `4096` | NPCs farther than this from the camera are left out of type 5. `0` means no limit. |

Changing host or port reopens the socket on the next frame. A rejected host stops the stream until the value is a numeric IPv4 address again.

Console, once a level is running:

```
set bridge_enable 1
set bridge_rate 30
```

Turn it back off with `set bridge_enable 0`. Because the cvars are archived, that value is remembered in the OpenJK config. `bridge_status` prints the live settings and packet counts. The startup line is `Bridge: OJB1 v1 UDP pose stream -> 127.0.0.1:27420 (rate 0)`.

## Packet layout

Every datagram is little-endian. Multi-byte fields are little-endian. Floats are IEEE-754 binary32, little-endian. The first four bytes are the ASCII magic `OJB1` (`4F 4A 42 31`). Version is `1`.

World space is id Tech 3 / Jedi Academy: **+X forward, +Y left, +Z up**, in game units. Angles are degrees in the same order as `playerState_t::viewangles`: **pitch, yaw, roll**. Positive pitch looks down. Positive yaw turns left (yaw 0 faces +X, yaw 90 faces +Y).

`camAxis` is `cg.refdef.viewaxis`, three vectors, each XYZ:

1. forward (`viewaxis[0]`)
2. right (`viewaxis[1]`, which is `(0, -1, 0)` at the identity view, because +Y is left)
3. up (`viewaxis[2]`)

`serverTime` is `cg.time` in milliseconds (pauses and timescale affect it). `frame` is `cg.clientFrame`. `sequence` increments by one for every datagram the game attempts to send, and resets to 1 on shutdown. The datagrams that split one NPC frame share a single `sequence`, and the datagrams that split one mover frame do the same. A gap in `sequence` is a dropped datagram. A gap in `frame` while `sequence` stays contiguous is the rate limit (or a skipped stereo eye).

Each datagram is one packet. Readers that only know type 1 and type 2 drop any other type. Those two layouts are unchanged, so an older `OJB1` reader keeps working. Type 3 is an additional datagram, not a change to the pose packet.

### Common header

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| 0 | 4 | char[4] | magic `OJB1` |
| 4 | 2 | uint16 | version = 1 |
| 6 | 2 | uint16 | type: `1` pose, `2` map, `3` saber, `4` render pose, `5` NPC, `6` HUD, `7` mover |
| 8 | 4 | uint32 | sequence |
| 12 | 4 | uint32 | frame (`cg.clientFrame`) |
| 16 | 4 | int32 | serverTime (`cg.time`, ms) |

Header size is 20 bytes.

### Pose packet (type 1, 160 bytes)

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| 20 | 12 | float[3] | camera origin (`cg.refdef.vieworg`) |
| 32 | 12 | float[3] | camera angles (`cg.refdefViewAngles`, degrees) |
| 44 | 36 | float[9] | camera axis, forward then right then up |
| 80 | 4 | float | `fovX` (degrees, horizontal, as submitted to the renderer) |
| 84 | 4 | float | `fovY` (degrees, vertical) |
| 88 | 12 | float[3] | player origin |
| 100 | 12 | float[3] | player view angles (degrees) |
| 112 | 12 | float[3] | player velocity (units per second) |
| 124 | 4 | int32 | `legsAnim` |
| 128 | 4 | int32 | `legsAnimTimer` (ms remaining) |
| 132 | 4 | int32 | `torsoAnim` |
| 136 | 4 | int32 | `torsoAnimTimer` (ms remaining) |
| 140 | 4 | float | legs frame (fractional when Ghoul2 reports it) |
| 144 | 4 | float | torso frame |
| 148 | 4 | uint32 | flags |
| 152 | 4 | int32 | weapon (`playerState_t::weapon`) |
| 156 | 4 | int32 | `pm_type` |

Player origin is the origin the player model is drawn at (`centity_t::lerpOrigin`) when that entity is in the snapshot. Before that, it falls back to the predicted player origin. View angles, velocity, anim numbers, anim timers, weapon, and `pm_type` come from `cg.predicted_player_state` (the SP local player).

Legs and torso frames are the rendered phase. On a Ghoul2 player (the normal case) they are the current root-bone and lower-lumbar bone frames, including the fraction. If Ghoul2 does not answer, they are the legacy lerp `oldFrame * backlerp + frame * (1 - backlerp)`.

Anim numbers are the raw player-state values. They are the gameplay animation ids, not Unreal asset names.

Flags:

| Bit | Mask | Meaning |
| ---: | --- | --- |
| 0 | `0x00000001` | third person |
| 1 | `0x00000002` | cinematic camera (`in_camera`) |
| 2 | `0x00000004` | zoom (`cg.zoomMode != 0`) |
| 3 | `0x00000008` | intermission (`pm_type == PM_INTERMISSION`) |
| 4 | `0x00000010` | player entity is in the current snapshot |

`pm_type` uses the game enum: `0` normal, `1` noclip, `2` spectator, `3` dead, `4` freeze, `5` intermission.

### Map packet (type 2, 84 bytes)

Sent when the map name changes, and again at most once every two seconds so a listener that starts late still learns the map. Not sent while the name is empty.

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| 0 | 20 | header | type = 2 |
| 20 | 64 | char[64] | map name, NUL-terminated, rest zeroed |

The name is `cgs.mapname`, for example `maps/kejim_post.bsp`.

### Saber packet (type 3, 672 bytes)

Sent on the same sample as the pose packet, under the same `bridge_rate` cap, while the live player weapon is `WP_SABER` or `saberInFlight` is set. A blaster or other weapon produces no type 3 datagram that frame, which means the lightsaber is not out. Ignition and shutdown still send the packet: each blade has its own active flag and a length that eases in and out.

The data is the single-player `playerState_t::saber[2]` (`saberInfo_t`). Blade positions are the world-space segment cgame stored while drawing the blade earlier in the frame (`CG_AddSaberBladeGo` for an in-hand saber, or the thrown saber entity). Same coordinates as the pose packet: **+X forward, +Y left, +Z up**, game units.

`hiltName` is `saberInfo_t::name`, the `sabers.cfg` key (for example `Kyle`), not the UI `fullName`. It is a stable id for the hilt. 64 bytes, NUL-terminated, truncated, and all zeros if the pointer is null. Saber 1's name is empty unless `dualSabers` is set.

`saberType` is `saberType_t`: `0` none, `1` single, `2` staff, `3` dagger, `4` broad, `5` prong, `6` arc, `7` sai, `8` claw, `9` lance, `10` star, `11` trident, `12` sith sword. A staff is one saber (`saber[0]`) with two blades. Dual sabers set `saberCount` to 2 and the dual flag.

There are always 16 blade slots (`MAX_SABERS * MAX_BLADES`). `bladeCount` is how many of those slots are filled, in order: every blade of saber 0, then every blade of saber 1. Slots at or past `bladeCount` are zero and must be ignored (a zero `colorIndex` would otherwise look like red). Each filled slot is 32 bytes:

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| 0 | 1 | uint8 | `saberIndex` (0 or 1) |
| 1 | 1 | uint8 | `bladeIndex` within that saber |
| 2 | 1 | uint8 | active (`bladeInfo_t::active`, 0 off, 1 on) |
| 3 | 1 | uint8 | `colorIndex` (`saber_colors_t`) |
| 4 | 4 | float | length, game units (`bladeInfo_t::length`) |
| 8 | 12 | float[3] | base, world (`muzzlePoint`) |
| 20 | 12 | float[3] | tip, world (`muzzlePoint + muzzleDir * length`) |

`colorIndex`: `0` red, `1` orange, `2` yellow, `3` green, `4` blue, `5` purple.

If a blade's length is 0, cgame may not refresh the muzzle that frame, so base and tip can be zero or a previous point. The active flag and length are still the gameplay state.

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| 0 | 20 | header | type = 3 |
| 20 | 2 | uint16 | `bladeCount` (0..16) |
| 22 | 1 | uint8 | `saberCount` (1, or 2 when dual) |
| 23 | 1 | uint8 | flags |
| 24 | 64 | char[64] | hilt name 0 |
| 88 | 4 | int32 | saber type 0 |
| 92 | 64 | char[64] | hilt name 1 |
| 156 | 4 | int32 | saber type 1 |
| 160 | 512 | 16 × 32 | blade slots |

Saber flags:

| Bit | Mask | Meaning |
| ---: | --- | --- |
| 0 | `0x01` | `dualSabers` |
| 1 | `0x02` | `saberInFlight` (thrown; saber 0's blade follows the saber entity) |

Type 3 base and tip are `blade.muzzlePoint` and `muzzlePoint + muzzleDir * length`. `CG_AddSaberBladeGo` writes those two vectors while it draws the blade, earlier in the same frame as this packet. A blade with length 0 is not redrawn, so its muzzle can be zero or left over from the last time it was on.

### Why the body steps, and the render packet (type 4, 228 bytes)

Type 1, 2, and 3 are sent from `CG_DrawActive`, once per client render frame (`com_maxfps`, default 125), still capped by `bridge_rate` when that is not 0. They are not sent once per snapshot.

The origin in type 1 is still `cent->lerpOrigin`. For the local player that is copied from `cg.predicted_player_state.origin`. In single-player, prediction does not run a client pmove. It copies the latest snapshot and only lerps when a future snapshot exists. The local server is usually caught up, so there is no future snapshot. The origin therefore changes when `sv_fps` runs (default 20). View angles from the latest command do update every render frame, but the body position does not. That is the 15–20 Hz step.

Type 4 does not change those packets. It is an extra datagram on the same sample. Its player origin is the type 1 origin played smoothly across the gap until the next server tick, so it moves every render frame and sits about one tick behind the game. The camera axis and angles are the real `cg.refdef` for this frame (mouse look is not delayed). The camera origin is `cg.refdef.vieworg` shifted by the same smoothing offset, so the third-person gap and view bob stay, without the 20 Hz jump. A jump bigger than 200 units (a teleport) is not smoothed.

Ghoul2 frames are the fractional frame `G2API_GetBoneAnimIndex` returns for this `cg.time`: legs from the root bone, torso from the lower lumbar bone. That is the frame the renderer is using, including the blend fraction.

Each hand is the player model's `*r_hand` or `*l_hand` bolt, from `G2API_GetBoltMatrix`, using the same yaw, origin, time, and model scale `CG_Player` uses when it reads `handRBolt` / `handLBolt` this frame. The matrix is Ghoul2's 3×4: three rows, four columns. Column 0 is the bone's +X axis, column 1 is +Y, column 2 is +Z, column 3 is the origin. The packet stores origin, then +X, +Y, +Z, each as XYZ in world space. Axes are columns, not rows. World space is the same as the other packets: **+X forward, +Y left, +Z up**, game units. The right slot is always `*r_hand`. The left slot is `*l_hand`, including when only one saber is equipped, so the empty hand is still there. A cleared flag means that matrix is zero.

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| 0 | 20 | header | type = 4. `serverTime` is `cg.time` |
| 20 | 4 | int32 | `clientTime`, `cgi_Milliseconds` |
| 24 | 12 | float[3] | smoothed player origin |
| 36 | 12 | float[3] | player angles this frame (`lerpAngles`, pitch, yaw, roll) |
| 48 | 12 | float[3] | smoothed camera origin |
| 60 | 12 | float[3] | camera angles (`refdefViewAngles`) |
| 72 | 36 | float[9] | camera axis, forward, then right, then up |
| 108 | 4 | float | fovX |
| 112 | 4 | float | fovY |
| 116 | 4 | float | legs frame (Ghoul2 root, fractional) |
| 120 | 4 | float | torso frame (Ghoul2 lower lumbar, fractional) |
| 124 | 4 | float | origin lerp, 0 at the previous tick, 1 at the current one |
| 128 | 1 | uint8 | hand flags |
| 129 | 3 | bytes | zero padding |
| 132 | 48 | 12 floats | right hand: origin, +X, +Y, +Z |
| 180 | 48 | 12 floats | left hand: origin, +X, +Y, +Z |

Hand flags:

| Bit | Mask | Meaning |
| ---: | --- | --- |
| 0 | `0x01` | right hand (`*r_hand`) matrix is valid |
| 1 | `0x02` | left hand (`*l_hand`) matrix is valid |

With `bridge_rate 0`, type 4 goes out on every rendered frame, the same rate as type 1 (default `com_maxfps` 125). The smoothed origin changes on those frames. Type 1's origin field is unchanged and still steps with the server tick.

### NPC packet (type 5, header version 2)

The game always sends version 2 now. The 20-byte header and the 8-byte body header are unchanged. Version 1 entries were 100 bytes, at most 10 per datagram (1028 bytes). Version 2 entries are 248 bytes, at most 5 per datagram (28 + 1240 = 1268). Bytes 0–99 of each entry are the version 1 layout.

Sent on the same samples as type 4, under the same `bridge_rate` cap, to the same address. One frame can be several datagrams. They share one `sequence` and one `frame`. An empty frame is one datagram with `count` 0.

An NPC is an entity in `cg.snap` whose `gent` has `NPC` or `client`, and is not the player. Dead bodies stay in the list while that entity still exists. `bridge_npc_radius` (default 4096, `0` = no limit) measures distance from `cg.refdef.vieworg` to `cent->lerpOrigin`.

World space matches the other packets: **+X forward, +Y left, +Z up**, game units. Angles are pitch, yaw, roll. `legsFrame` and `torsoFrame` are the same Ghoul2 fractional frames as type 4 (root bone and lower lumbar). `trDelta` is `currentState.pos.trDelta`. `NPC_type` is `gent->NPC_type`, truncated to 31 characters plus a NUL. The hash is FNV-1a 32-bit of that name in lowercase (offset basis `2166136261`, prime `16777619`).

`state`: `0` alive (`health > 0`), `1` dying (`health <= 0` and the legs anim is still in `BOTH_DEATH1` .. `BOTH_DEAD1`), `2` dead. `flags`: `0x01` newly seen this frame or `EF_TELEPORT_BIT` flipped, `0x02` weapon is not `WP_NONE`, `0x04` saber is active. Anim numbers have `ANIM_TOGGLEBIT` (`2048`) cleared.

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| 0 | 20 | header | type = 5, version = 2 |
| 20 | 2 | uint16 | `count` (entries in this datagram) |
| 22 | 2 | uint16 | `total` (NPCs this frame, all datagrams) |
| 24 | 1 | uint8 | `packetIndex` (0-based) |
| 25 | 1 | uint8 | `packetCount` |
| 26 | 2 | uint16 | reserved, 0 |
| 28 | 248 × count | entries | at most 5; datagram is at most 1268 bytes |

Each entry:

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| 0 | 2 | uint16 | entity number |
| 2 | 1 | uint8 | state |
| 3 | 1 | uint8 | flags |
| 4 | 4 | uint32 | FNV-1a-32 of lowercase `NPC_type` |
| 8 | 12 | float[3] | `lerpOrigin` |
| 20 | 12 | float[3] | `lerpAngles` |
| 32 | 4 | int32 | legs anim |
| 36 | 4 | int32 | torso anim |
| 40 | 4 | float | legs frame |
| 44 | 4 | float | torso frame |
| 48 | 4 | int32 | weapon |
| 52 | 4 | int32 | health |
| 56 | 12 | float[3] | `pos.trDelta` |
| 68 | 32 | char[32] | `NPC_type` |
| 100 | 1 | uint8 | hand flags: `0x01` `*r_hand` valid, `0x02` `*l_hand` valid |
| 101 | 1 | uint8 | fire count since last type 5 send (`EV_FIRE_WEAPON` + `EV_ALT_FIRE`, saturates at 255, then reset) |
| 102 | 1 | uint8 | fire flags: `0x01` primary, `0x02` alt, `0x04` `EF_FIRING`, `0x08` `EF_ALT_FIRING` |
| 103 | 1 | uint8 | saber flags: `0x01` blade 1 on, `0x02` blade 2 on, `0x04` dual, `0x08` staff |
| 104 | 48 | float[12] | `*r_hand` world matrix: origin, +X, +Y, +Z (Ghoul2 columns) |
| 152 | 48 | float[12] | `*l_hand`, same layout (`handLBolt`) |
| 200 | 8 | float[2] | blade lengths: `saber[0].blade[0]`, then `saber[0].blade[1]` or `saber[1].blade[0]` when dual |
| 208 | 2 | uint8[2] | blade colors (`saber_colors_t`), same blade order |
| 210 | 2 | uint16 | reserved, 0 |
| 212 | 32 | char[32] | hilt name (`saber[0].name`, empty if none) |
| 244 | 4 | int32 | `cg.time` of the latest fire event, 0 if none |

### HUD packet (type 6, 56 bytes, header version 1)

Sent on the same samples as the other bridge packets, under `bridge_rate`. Skipped when `cg.snap` is null. Every value is an int32 from `cg.snap->ps`.

| Offset | Type | Field |
| ---: | --- | --- |
| 0 | header | type = 6, version = 1 |
| 20 | int32 | health, `stats[STAT_HEALTH]` |
| 24 | int32 | max health, `stats[STAT_MAX_HEALTH]` |
| 28 | int32 | armor, `stats[STAT_ARMOR]` |
| 32 | int32 | `forcePower` |
| 36 | int32 | `forcePowerMax` |
| 40 | int32 | `saberAnimLevel` |
| 44 | int32 | `weapon` |
| 48 | int32 | ammo, `ammo[weaponData[weapon].ammoIndex]`, or -1 when that index is `AMMO_NONE` or the weapon is out of range |
| 52 | int32 | `ammoData[ammoIndex].max`, or 0 for `AMMO_NONE` |

### Mover packet (type 7, header version 1)

Sent on the same samples as the other bridge packets, under the same `bridge_rate` cap. Header version stays 1. The 8-byte body header matches type 5. Entries are 44 bytes, at most 24 per datagram (28 + 1056 = 1084). A frame with more movers is several datagrams that share one `sequence` and one `frame`. An empty frame is one datagram with `count` 0.

A mover is an entity in `cg.snap` whose brush is inline: `currentState.solid == SOLID_BMODEL`, and the model string starts with `*`. World model `*0` is not sent. There is no distance cutoff.

`inlineModel` is N from that `*N` string. It is the same number as the `*N` node `tools/bsp2gltf` writes for that inline model. The game looks up `CS_MODELS + currentState.modelindex` first. Brush models usually do not live there: `SV_SetBrushModel` stores N in `modelindex` itself, and the `*N` text stays on the entity (`g_entities[num].model`), which is the string the map compiler wrote. That second string is used when the configstring is not `*N`.

The bsp2gltf vertices are the baked BSP draw verts, already in world space. `lerpOrigin` is the offset from that baked position, not a replacement for it. `lerpAngles` are pitch, yaw, roll. Place the mesh the way `CG_Mover` and `R_RotateForEntity` do: rotate the baked vertex by those angles, then add `lerpOrigin`. With angles at 0, which is a sliding door at rest or in motion, that is a plain translation by `lerpOrigin`. A rotating brush uses the same rotate-then-translate. Do not rebuild the mover in game code; these are the interpolated cgame values (`cent->lerpOrigin`, `cent->lerpAngles`, `currentState.pos.trDelta`).

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| 0 | 20 | header | type = 7, version = 1 |
| 20 | 2 | uint16 | `count` (entries in this datagram) |
| 22 | 2 | uint16 | `total` (movers this frame, all datagrams) |
| 24 | 1 | uint8 | `packetIndex` (0-based) |
| 25 | 1 | uint8 | `packetCount` |
| 26 | 2 | uint16 | reserved, 0 |
| 28 | 44 × count | entries | at most 24; datagram is at most 1084 bytes |

Each entry:

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| 0 | 2 | uint16 | entity number |
| 2 | 2 | uint16 | `inlineModel`, N from `*N` |
| 4 | 1 | uint8 | flags |
| 5 | 1 | uint8 | `eType` (`currentState.eType`, low 8 bits) |
| 6 | 2 | uint16 | reserved, 0 |
| 8 | 12 | float[3] | `lerpOrigin` |
| 20 | 12 | float[3] | `lerpAngles` |
| 32 | 12 | float[3] | `pos.trDelta` |

Flags:

| Bit | Mask | Meaning |
| ---: | --- | --- |
| 0 | `0x01` | newly seen this frame, or `EF_TELEPORT_BIT` flipped |
| 1 | `0x02` | moving: `pos.trType` or `apos.trType` is not `TR_STATIONARY` |
| 2 | `0x04` | `EF_NODRAW` (the renderer skips the brush) |

Hand matrices use the same `G2API_GetBoltMatrix` call as type 4: the NPC ghoul2 model, `handRBolt` / `handLBolt`, `lerpOrigin` (plus the model-scale Z nudge), and yaw from `renderAngles`. Axis order is the same columns: +X, +Y, +Z, in **+X forward, +Y left, +Z up** world units.

Windows install, launch, and checks are in the sections above. To point the stream at another machine, set `bridge_host` to that machine's numeric IPv4 address and run the receiver with `--host 0.0.0.0`.
