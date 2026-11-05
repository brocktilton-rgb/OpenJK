# OpenJK single-player camera bridge

Proof of concept for a Jedi Academy remaster-as-mod. OpenJK stays the gameplay authority. This bridge streams the camera and the player pose out of the single-player game, once per rendered frame, as a compact UDP datagram. A later Unreal Engine client (or `tools/bridge_receiver.py` today) listens on localhost and follows along. Nothing is sent unless the bridge is turned on.

The stream is single-player only. It is compiled into the SP game module (`jagame*.dll` / `jagame*.so`), which is where both the game and the client game live.

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
| `bridge_rate` | `0` | Maximum samples per second, using client realtime. `0` sends every rendered frame. Clamped to 480. The interval is `1000 / rate` milliseconds. A sample is the map packet (when one is due), the pose packet, and the saber packet when a lightsaber is out. |

Changing host or port reopens the socket on the next frame. A rejected host stops the stream until the value is a numeric IPv4 address again.

Console, once a level is running:

```
set bridge_enable 1
set bridge_rate 30
```

Turn it back off with `set bridge_enable 0`. Because the cvars are archived, that value is remembered in the OpenJK config.

## Packet layout

Every datagram is little-endian. Multi-byte fields are little-endian. Floats are IEEE-754 binary32, little-endian. The first four bytes are the ASCII magic `OJB1` (`4F 4A 42 31`). Version is `1`.

World space is id Tech 3 / Jedi Academy: **+X forward, +Y left, +Z up**, in game units. Angles are degrees in the same order as `playerState_t::viewangles`: **pitch, yaw, roll**. Positive pitch looks down. Positive yaw turns left (yaw 0 faces +X, yaw 90 faces +Y).

`camAxis` is `cg.refdef.viewaxis`, three vectors, each XYZ:

1. forward (`viewaxis[0]`)
2. right (`viewaxis[1]`, which is `(0, -1, 0)` at the identity view, because +Y is left)
3. up (`viewaxis[2]`)

`serverTime` is `cg.time` in milliseconds (pauses and timescale affect it). `frame` is `cg.clientFrame`. `sequence` increments by one for every datagram the game attempts to send (map, pose, and saber), and resets to 1 on shutdown. A gap in `sequence` is a dropped datagram. A gap in `frame` while `sequence` stays contiguous is the rate limit (or a skipped stereo eye).

Each datagram is one packet. Readers that only know type 1 and type 2 drop any other type. Those two layouts are unchanged, so an older `OJB1` reader keeps working. Type 3 is an additional datagram, not a change to the pose packet.

### Common header

| Offset | Size | Type | Field |
| ---: | ---: | --- | --- |
| 0 | 4 | char[4] | magic `OJB1` |
| 4 | 2 | uint16 | version = 1 |
| 6 | 2 | uint16 | type: `1` pose, `2` map, `3` saber |
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

## Test on Windows with retail GameData

Use the 64-bit Release Non-Portable artifact from this pull request's GitHub Actions run. The job name is **Windows x86_64 Release (Non-Portable)**. The artifact name is **`OpenJK-windows-x86_64-Release-Non-Portable`**. Non-Portable uses the same `Documents\My Games\OpenJK` config and saves as the official build. A 32-bit retail install wants `OpenJK-windows-x86-Release-Non-Portable` instead (`openjk_sp.x86.exe`, `rdsp-vanilla_x86.dll`, `OpenJK\jagamex86.dll`).

1. On the PR, open **Checks**, then the Windows x86_64 Release (Non-Portable) job, and download the artifact (or use the run's **Artifacts** list). The zip root is the `JediAcademy` install tree.
2. In the retail `GameData` folder (Steam: `steamapps\common\Jedi Academy\GameData`), back up and then replace these files from the zip:
   - `openjk_sp.x86_64.exe`
   - `rdsp-vanilla_x86_64.dll`
   - `OpenJK\jagamex86_64.dll`

   Leave `base\` and the `.pk3` assets alone. The bridge itself is inside `jagamex86_64.dll`. The matching exe and renderer come from the same build so the module interfaces stay paired. `OpenAL32.dll` already present from the official OpenJK build can stay.
3. Install Python 3 if `py -3` is not already available. No packages to install.
4. From a copy of this repo (the script is `tools/bridge_receiver.py`), start the listener first:

   ```
   py -3 tools\bridge_receiver.py --port 27420
   ```

   Add `--summary` to print one pose per second instead of every frame, or `--verbose` to include the camera axis. `--self-test` checks the decoder without the game.
5. Launch the SP game from `GameData`:

   ```
   openjk_sp.x86_64.exe +set bridge_enable 1 +set bridge_host 127.0.0.1 +set bridge_port 27420
   ```

   Optional cap, for example 30 packets per second:

   ```
   openjk_sp.x86_64.exe +set bridge_enable 1 +set bridge_host 127.0.0.1 +set bridge_port 27420 +set bridge_rate 30
   ```

6. Start a new game or load a save. The console prints `Bridge: UDP pose stream -> 127.0.0.1:27420` when the first gameplay frame is drawn. The receiver prints `map` lines (`maps/<level>.bsp`) and `pose` lines, plus a `rate ... pkt/s` line every second. With the lightsaber equipped it also prints a `saber` line (hilt name, per-blade on/off, color, length, base, and tip) on the same frames.
7. Stop streaming with `set bridge_enable 0` in the console, or quit. Quitting and level changes close the socket.

Localhost UDP usually does not raise a firewall prompt. If Windows asks, allow it for private networks, or keep both sides on `127.0.0.1`.

To point the stream at another machine, set `bridge_host` to that machine's numeric IPv4 address and run the receiver with `--host 0.0.0.0` so it accepts more than loopback.
