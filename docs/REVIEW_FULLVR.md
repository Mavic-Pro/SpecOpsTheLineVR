# SpecOpsVR: Full-VR Architecture Review

Scope: a read-only review of `C:\Users\Matti\Projects\SpecOpsVR` at version 0.9.9 plus the local commits (cinema/VR hysteresis, settle-black, special moves locked to the game). Six area reviews were each checked by an adversarial verifier, and I ran Ghidra (`tools/ghq.py`) myself to settle the open questions. I dropped refuted bugs and any proposal the verifiers found already implemented. All file:line references point at the tree as reviewed.

---

## 1. Executive summary

**Where the mod stands.** SpecOpsVR is already a real 6DOF VR conversion, not a "VR screen" mod:
- **Rendering and output:** synchronous stereo uses two `FViewport::Draw` calls per tick. Each eye gets an off-center projection patched only through `VR_CalcSceneView`, which Ghidra confirms. A frame tag carries the pose and the dynamic-resolution percent, and a separate output thread copies D3D9Ex shared surfaces to D3D11.
- **Head and body:** the head is sampled once per tick, uses the real standing height, has collision, and room-scale moves the Pawn.
- **Arms and weapons:** the arms are driven by Walker's own IK skeletal controls. Shots and projectiles follow the visible barrel. Reload is physical, and there is a haptics bridge.

What separates it from "natively VR" is three things:
1. **Interactions are still pad buttons.** Ropes, turrets, pick-ups, executions and weapon swaps are all a pressed A/B/Y/X, sometimes triggered by a gesture.
2. **The UI is still the 2D game canvas re-projected.** Nothing sits on the body, on the weapon or in the world, apart from the cinema screen.
3. **Several edge cases break presence or stability.** These are transitions: game-locked moves, falls, tracking loss, level changes and GPU stalls.

**Top 5 conclusions**

1. **Fix the transition bugs before adding features.** They hit exactly the moments 0.9.9 was meant to polish:
   - The gun floats at the controller during vault, rope and zipline (`UpdateGunInHand`, hooks.cpp:4341/4350).
   - Turning the body with the head slides the eyes sideways (hooks.cpp:3819).
   - A fall (PHYS_Falling) is treated as a mount (hooks.cpp:2367).
   - The neck-following view drop now also fires on every pick-up and vault (hooks.cpp:5899).

   All four are confirmed or plausible, each is cheap to fix, and each hurts comfort.
2. **The barrel-aim hook throws away the game's own ballistics.** Ghidra shows the spread cone is applied in `FUN_0093f510` **before** `VR_CalcWeaponFire @004a3d50`. That function is a bare `VR_SingleLineCheck` inside a shoot-through loop. As a result:
   - the `spread` setting does nothing;
   - shotgun pellets probably collapse into a single line;
   - bullets no longer pass through penetrable actors.

   Fixing this (keep the game's angular deviation and apply it to the barrel axis) is the single best weapons improvement for its cost.
3. **Cached engine pointers are the main remaining crash class.** The PlayerController pointers, the camera pointer and the projectile list store bare addresses. They are written with no identity check after a level change or respawn, and SEH cannot catch writes into memory the engine has reused. Crashes on the game thread are also invisible to the mod, because UE3's filter `FUN_00827770` catches them first. Add generation or identity guards, and hook that filter.
4. **Physical interaction is reachable without touching animations.** Ghidra lists the natives needed:
   - `AYTrigger_*Interaction` getters: `GetEntryPoint @00b17a90`, `GetRopeStart/End`, `GetBounds`, `GetCurrentIconPosition @008812f0`;
   - `SetAimAtLocation @00af19b0`;
   - `IsExecutable @00c0e920`, `FindMeleeTarget @00517c00`;
   - the YContextCommands getters.

   A staged "scanner → affordances → grab-to-use" plan (section 4) turns ropes, turrets, pick-ups and executions into reach-and-grab. The game's own special moves still play.
5. **Comfort and UI wins with no reverse engineering:**
   - re-enable posed submit, which was likely turned off because of two bugs that have since been fixed;
   - add `Submit_FrameDiscontinuity` on cuts;
   - add a status/hint overlay in the headset, reusing the cinema-overlay code;
   - add wrist and controller hints;
   - make the aim marker a constant angular size;
   - drive the vignette only from motion the player did not cause.

---

## 2. Verified bugs to fix now

These are the confirmed and plausible bugs only. Duplicates found by several areas are merged. Wherever the verifier corrected the line or the fix, the corrected version is used.

### High

| # | Bug | Where | Verdict |
|---|---|---|---|
| H1 | Gun mesh snaps to the controller during game-locked moves, scripted scenes and death | hooks.cpp:4341, 4346-4350 | confirmed |
| H2 | Barrel-aim CalcWeaponFire discards the spread cone and breaks shoot-through | hooks.cpp:6908-6931 | confirmed |
| H3 | Turn-with-head rotates the eye base around Walker's centre (sideways slide) | hooks.cpp:3819-3820 | confirmed |
| H4 | Stick, head and turret turning write into stale PlayerController pointers | vr_input.cpp:1181/1406/1420; hooks.cpp:3673, 3818, 3999-4015 | plausible |

**H1. Gun in hand during locked scenes.**
- **Cause:** `UpdateArms` releases the arms for `g_gameLockedScene` or a dead pawn (5410-5424), and `UpdateGunInHand` runs right after it (6157-6158). Its only guards are `g_gunInHand && hand && fp && pawn` (4346-4350).
- **Scenario:** during a vault, rope or zipline (special-move lock, hooks.cpp:2799-2813), the arms follow the animation but the rifle stays pinned to the real controller (`SafeSetGunPose`, 4258-4266). The same happens with cinemaWalker3D and with a dead pawn. Turrets are unaffected, because `CurrentGunComponent` returns null for them (4325).
- **Fix:** run `UpdateGunInHand` only when `!vr::control::g_arms`, which is what the ini comment at deploy/specopsvr.ini:85-86 intends. At minimum, add `!g_gameLockedScene && !g_cinemaActive && PawnAlive(pawn)` to the condition at 4350.

**H2. Muzzle aim discards ballistics.**
- **Ghidra evidence:**
  - `VR_CalcWeaponFire @004a3d50` only calls `VR_SingleLineCheck` and fills the impact info.
  - Its caller `FUN_0093f510` calls `rand()` twice and `FUN_00f60300` (the cone) on End, and only then enters a `while(true)` loop.
  - The loop continues while the hit actor has `[0x7f] & 0x800`, restarting from `hit - normal*k`. It breaks on a null hit or on the same actor as the previous hit.
- **What goes wrong:** the hook rebuilds `end = MuzzleStart + bd*range` (6908-6912). So:
  - the cone is lost, and `spread on` (ini:69) does nothing;
  - zeroing the arsenal accuracy (6922-6931) is a no-op;
  - on a pass-through iteration the start is reset to the muzzle, the trace hits the same actor again and the loop breaks, so penetration is lost.
- **Fix:**
  - On each call, take the rotation from the game's nominal direction (game start toward `g_muzzleTarget`) to its actual `end - start`, and apply it to `bd`.
  - Use `MuzzleStart` only when the incoming start is the game's muzzle origin for this shot. For later loop iterations, keep the advanced start projected onto the corrected ray.
  - Remove the accuracy-zeroing block.
  - Correct docs/TECHNICAL.md:341-343, which says spread is applied inside CalcWeaponFire.
- **Open points:** check with `firelog` that player weapons and shotgun pellets go through `FUN_0093f510`, and what flag `0x800` means.

**H3. Turn-with-head eye slide.**
- **Cause:** `AddControllerYaw` (3462-3469) turns the PlayerController and `g_bodyYaw`. `ApplyFirstPerson` keeps the eye base 15 units ahead along the Pawn yaw `+0x64` (2266-2274), and nothing corrects `posRef` on this path. Only the stick path does (3629-3642).
- **Effect:** a 45° catch-up moves the view about 11.5 units (~23 cm) sideways over about 0.25 s, although the head did not move.
- **Fix:** see the "eye-pivot" helper in R-B1. Correct the fwd term by the change in Pawn yaw. The headOffset term should keep rotating with `g_bodyYaw`, as used at 6025.

**H4. Stale PlayerController pointers.**
- **Cause:**
  - `vr::control::g_playerController` is set only in `CameraHookCheck` (hooks.cpp:1630-1633).
  - `g_playerControllerPtr` is set only when script calls `GetViewTarget`. Ghidra: `APlayerController_execGetViewTarget @0057e900` has only native-table xrefs.
  - Neither pointer is ever cleared or revalidated before `SafeAddYaw`, the head turn or the turret writes.
- **Failure window:** after GC has freed the old PlayerController and before the new one is captured, with the freed block already reused. The window is narrow and no log has shown it. When it happens, it is silent corruption.
- **Fix:**
  - Store the class with each pointer and require `StillAlive(ptr, cls)` (4126) before each write.
  - In UpdateHeadTracking and the turret code, prefer `cam-0x77C` from a camera whose `g_cameraSeenSeq` advanced this tick. That counter is currently incremented but never read.
  - Have `SafeAddYaw` accumulate into an atomic that the game thread applies.

### Medium

| # | Bug | Where | Verdict | Fix (short) |
|---|---|---|---|---|
| M1 | Stick and snap turn correction assumes fwd = 15 and pivoting on the PlayerController: wrong on mounts and turrets (fwd = 0), in cover (only the Pawn turns), and during locked moves | hooks.cpp:3634; vr_input.cpp:1383-1427 | confirmed (cover part slightly less certain) | Use the same helper as H3 (correct by the actual change in fwd·dir(pawnYaw)). Block smooth and snap turn while `g_gameLocked`. |
| M2 | PHYS_Falling counts as "not on firm ground": fwd 15→0, Neck-bone base, no Z smoothing, mesh snaps back (5103-5107), all in one frame, then back on landing | hooks.cpp:2367-2369, 2272, 5796-5859, 5933-5936 | confirmed | Treat Falling as on foot (or allow a ~0.5 s airborne grace period). Blend fwd and base over ~0.2 s when the mode changes. |
| M3 | Gesture melee B reaches the game in the same tick, before ExecutionAssist turns the body | vr_input.cpp:1334-1348 (B OR-ed back in after `GateCombo`); hooks.cpp:3801 via 2725 | confirmed (Ghidra: `FUN_007849f0` ticks client and world before `VR_ViewportDraw @008f6a80`) | Hold the gesture's kB back for one poll (~30 ms) and record `g_meleeIntentMs` now. ExecutionAssist then runs in the next Draw, before the game sees B. |
| M4 | ExecutionAssist snaps the body toward the nearest downed enemy in any direction (up to 180°), overriding a melee on the enemy in front | hooks.cpp:6556-6590 | confirmed | Score candidates by angle to gaze or punch direction (±60°). Skip when a live hostile that is not downed (DBNO) is within ~150 u in front. Optionally validate with `IsExecutable` (`@00c0e920` → vtable+0x6A0; **takes one argument + bool return**). |
| M5 | Updater ignores `autoupdate off`, channel and repo when the ini never loads (unsupported exe, or first EndScene later than 120 s) | updater.cpp:174-186; dllmain.cpp:59-65; control.cpp:169, 1084-1087 | confirmed | If `WaitForConfig` times out, parse only `autoupdate`, `updatechannel` and `updaterepo` from the ini on the updater thread (same approach as `GpuShareRequested`, hooks.cpp:7571-7590), or do nothing. |
| M6 | `openvr_api.dll` is a static import: if it is missing or quarantined, the game does not start at all. The updater never refreshes it. | CMakeLists.txt:87-88; updater.cpp:234-247 | confirmed | `/DELAYLOAD:openvr_api.dll` + delayimp. Check that it loads in `session::Init` and stay inactive if not. Ship companion assets in the update manifest. |
| M7 | Output thread can use D3D11 and shared textures that the render thread freed after the 5 s quiesce timeout | vr_output.cpp:300-311, 466-472, 671-678, 720-726 | plausible (only if CopyResource or Submit blocks >5 s; WaitGetPoses is outside the busy window) | AddRef `tex[e]` and `pk[e].gpuSrc` under the lock, Release after Submit. Or defer the release to the worker once it is idle, checked by generation. |
| M8 | Neck-following view drop (written for bCinematicMode) now fires on every special move (vault 2-6, pick-ups 23-34, slide 41): up to 65 u of induced vertical motion. The 2 s `afterCinema` window also suppresses cover compensation. | hooks.cpp:2813, 5894-5919, 3712-3713 | plausible | Add a separate cinematic-only atomic and key the drop and `afterCinema` on it, or use an explicit move list (rope, zipline, knockdown). |
| M9 | Any non-zero `m_specialMove` counts as cover: during a sprint or stagger, movewithhead switches off (the stick frame changes from head to body), room-scale stops, and nearCover lowers the correction threshold | hooks.cpp:3684-3687; vr_input.cpp:1359; hooks.cpp:3852, 3725 | plausible (sprint and stagger being non-zero is known only from the comment at 219-220) | Split the flags: cover = face state or moves 19/20. Keep movewithhead during sprint. Confirm the values with the `specialmoves` dump (R-G6). |
| M10 | Room-scale uses `Actor.Move`: no step-up, no sliding along walls, and walking off a ledge triggers M2 | hooks.cpp:3861-3865 | plausible (Ghidra: `execMove @00922f40` is a single MoveActor; `execMoveSmooth @004d7560` → `0x0088fb70` with stepUp and SmoothHitWall) | Call `"MoveSmooth"` (same Delta parameter). Keep measuring the real displacement for posRef. |
| M11 | `g_leftYawValid` is never cleared, so `movedir hand` walks in a frozen direction after the left hand loses tracking | hooks.cpp:3972 (only write), 3981-3987; vr_input.cpp:1360 | confirmed | Set it to false in both else branches (3981, 3984). Better: compute the support-hand yaw outside the gun-valid block. |
| M12 | Two-hand aim state is not reset when the support pose goes invalid: the aim steps to the raw right-hand aim, then snaps back with a stale offset and no blend | hooks.cpp:3964-3983, statics 2074-2126 | plausible (the jump is only the drift since engagement, and the path needs a real invalid pose) | Else branch: reset `wasActive` and blend out from twoYaw/twoPitch. Reset `lastT` so the offsets are recaptured on re-engage. |
| M13 | Right-stick-click gate (120 ms) lets the tilt through as a snap or smooth turn before the D-pad modifier engages. A quick press-and-push loses the D-pad direction. | vr_input.cpp:1343-1349, 1271-1273, 1387-1424 | confirmed | Record the raw kRThumb before `GateCombo`. While it is physically held, zero RX/RY and keep the direction for `ApplyDpadModifier`. |
| M14 | Physical reload is wiped by any invalid pose sample: back to Mag::In, trigger unlocked mid-reload, the next magwell grip ejects and sends X again | vr_input.cpp:880-884 (no log) | plausible (frequency unproven; most runtimes extrapolate short occlusions) | Freeze the state machine while a pose is invalid. Reset only after more than 1 s of continuous loss (or rely on the 12 s timeout at 905), and log it. |
| M15 | Honest-reload trigger block applies outside gameplay: with the magazine out when a cinematic or locked section starts, `bRightTrigger` stays at zero and the 12 s timeout cannot run | vr_input.cpp:1177, 767-771, 905-908 | plausible (edge case) | Gate 1177 on `GestureDetector::Gameplay()`. Reset `m_mag` when `Gameplay()` goes false. |
| M16 | Pad 0 reported disconnected while the VR session is down: the engine latches it off | vr_input.cpp:1457-1473 | plausible (Ghidra `FUN_006d3340`: `piVar14[0x7b] = (XInputGetState(0)==0)`, so one failure stops polling. The third caller at `0x00c91be8` is still not disassembled; I re-checked and ghq reports "no function", so the re-enable path is unknown.) | For user 0 in HMD mode, always return ERROR_SUCCESS with a neutral pad and an incrementing packet number. First define a function at `0x00c91be8` in Ghidra and see whether it re-detects pads. |
| M17 | Left-handed mode: the lateral arm and gun offsets are not mirrored by physical hand | hooks.cpp:4588 (`mirror = side==0?1:-1`), 4377-4379; hooks.cpp never reads `g_leftHanded` | plausible | Mirror only the lateral terms (`g_armOffsetCm[1]`, `g_gunOffsetCm[1]`, the class offset) by physical hand. Keep `armrot` (the mesh is right-handed). Playtest. |
| M18 | Projectiles spawned at a previous projectile's address are never redirected (bare-pointer list, never pruned) | hooks.cpp:5238, 5248-5249, 5360, 5384-5385 | plausible | `StillAlive` **does not help** (same slot and class). At the end of each scan, prune entries that were not seen among the scanned projectiles. |
| M19 | Canvas.Project HUD items are transformed twice: already in the eye's off-center projection, then scaled and re-parallaxed by `AdjustCanvasMatrix` | hooks.cpp:1462-1497; Ghidra `UCanvas_execProject @0x00A15D90` | plausible (it is unknown which Spec Ops widgets call Project; only 2 script-table xrefs) | First hook `execProject` and count calls per tick. If used, return coordinates pre-mapped through the inverse of the per-eye affine remap, using the same FrameTag. |
| M20 | Vignette fires on motion the player caused (turn-with-head catch-up, wall pull, room-scale steps) | hooks.cpp:6100-6116, 3469, 6012-6015 | confirmed, but **opt-in** (vignette defaults to 0, control.cpp:131); room-scale only gives a faint pulse thanks to tau 0.1 s (963-965) | See R-C3: drive it from view motion minus HMD motion. |

### Low

| # | Bug | Where | Verdict / Fix |
|---|---|---|---|
| L1 | `headsample render` runs UnrealScript (`Move` 3865), ExecutionAssist (3801), `AddControllerYaw` (3818) and the turret writes (4001-4013) on the render thread; the `CallFunction` cache (script_call.cpp:442-445) has no lock. Reported by three areas. | hooks.cpp:341 | Confirmed path, non-default option. Remove the option, or make it sample the pose only. Add a mutex to the cache like `NameIndex`. |
| L2 | GPU fence give-ups: D3D9 query 8 ms then delivers anyway (1163-1167); D3D11 copy query 10 ms then returns the slots (750-766). No SetMaximumFrameLatency. | vr_output.cpp:1164, 757 | Plausible, never observed. First count timeouts in the VR-OUTPUT line. If they occur, keep the slot (retry next Present) and defer returning it until S_OK. A keyed mutex is **not feasible** with D3D9Ex shared resources. Test SetMaximumFrameLatency(1) separately with A/B. |
| L3 | Same-tick fast path can pair L(N-1) with R(N): no tick or sequence check | vr_output.cpp:1286-1296 | Plausible. Require `g_slotSeq[1][g1] - g_slotSeq[0][g0] <= 2` (or equal ticks) before the fast path. |
| L4 | Scope mask and HUD scope branch read the game-thread `g_scopeActive` on the render thread (one frame off on entry and exit). The forced VR FOV also reaches two aim functions (`FUN_00d24da0`, `FUN_00d28d30`). The claim that "the left eye is unzoomed for one tick" is **largely refuted** (those callers refresh the flag earlier in the tick). | hooks.cpp:901, 1431, 2967, 3111-3127 | Add `tag.scope` to FrameTag and read it in DrawScopeBars and AdjustCanvasMatrix. Return the forced FOV only for calls from `VR_CalcSceneView`. |
| L5 | Right-eye FSceneViewState reallocated without freeing the old one (C11) | hooks.cpp:2594-2606 | Real but probably rare (only if the LocalPlayer is recreated; the setres trigger is doubtful). Count the owner-change log lines before writing a destructor call. |
| L6 | Cover peek stuck (LT 255) when the HMD pose goes invalid while leaning out | hooks.cpp:3585-3588, 3794; vr_input.cpp:1329 | Confirmed. Clear `g_coverPeek` on the early returns, or time it out after ~200 ms without a pose. |
| L7 | Turn-with-head drift: the PlayerController gets truncated units, the reference gets exact radians | hooks.cpp:3424-3427, 3819-3820 | Confirmed, ≤0.25°/s. Round, and add back the rounded angle. |
| L8 | Possible one-frame backward pop after a room-scale step | hooks.cpp:2725 vs 2934 | Plausible, unverified. Check with camlog (raw vs pawn) while walking physically. |
| L9 | KeepGoodBones statics survive ReleaseArms and can restore a pre-release skeleton for one tick | hooks.cpp:7020-7076, 4463-4473 | Plausible. Add `ResetSkelFix()` and call it from Acquire and Release. |
| L10 | Arms never retried for a mesh whose first AcquireArms failed (until want = false) | hooks.cpp:5426-5431 | Plausible. Retry after 1 s and log once. |
| L11 | Game recoil zeroed on the weapon object and never restored; the original values are lost | hooks.cpp:5140-5144, 5175 | Confirmed. Save per-weapon bytes on first sight and restore them when arms release. This is a prerequisite for R-W4. |
| L12 | Full GObjects scans on the game thread: launcher held (every frame), throw (every other frame for 2 s), CheapShooters (every 2 s with RB held, plus a name scan every 60 s) | hooks.cpp:5295-5392, 5202-5257, 6638-6671, 6695 | Plausible, cost unmeasured. Walk WorldInfo or level actors, stop the throw scan when grenade ammo did not drop, drop the call from the diagnostic at 6695. |
| L13 | Squad pick traces every pawn before the tolerance test, in up to 2 passes, twice per tick | hooks.cpp:6767 vs 6777; 2733 and 3044 | Confirmed. Trace only when `off < 1.5*tol`; cache the per-tick result. |
| L14 | VisibilityCheck is an always-on diagnostic (420-char line per occluded enemy every 0.5 s) | hooks.cpp:6445-6489, 2734 | Confirmed. Put it behind a `vischeck` flag, default off. |
| L15 | Melee intent recorded in the pause menu and the B+Y combo; ExecutionAssist ignores `g_gamePaused` | vr_input.cpp:1336-1339; hooks.cpp:6546 | Plausible. Record intent only `!menu` and after the gate; add `!g_gamePaused`. |
| L16 | Execution detection box widened once per Pawn pointer, never retried | hooks.cpp:6528-6532 | Plausible. Decide each tick from the values (`cur[1] < 120`). |
| L17 | Turret P-gain applied per tick (depends on frame rate) | hooks.cpp:4008, 4013 | Confirmed, minor. `k = 1 - exp(-dt/0.03)`. |
| L18 | `turretaim stick` cannot pitch: RY is zeroed outside menus | vr_input.cpp:1382 | Plausible. Skip zeroing when `g_onTurret && !TurretAimActive()`. |
| L19 | bHaptics forearm positions ignore left-handed mode | haptics_ext.cpp:342-344, 388-399 | Confirmed. Choose via `PhysicalHand`. |
| L20 | Support-hand pitch adds aimpitch as an absolute pitch (roll-coupled error) | hooks.cpp:3973-3974 | Confirmed, only when aimpitch ≠ 0. Use the same `ca` matrix rotation, or drop aimpitch for the support hand. |
| L21 | Stale grenade throw stamp when the grip is held for more than 2 s after the gesture | hooks.cpp:5205; vr_input.cpp:775-792 | Plausible design inconsistency. Publish the direction and stamp on left-grip release. |
| L22 | VR output disabled for the whole session after one transient D3D11 failure | vr_output.cpp:161-164, 1025, 476, 498, 1050 | Confirmed. Retry with backoff, tied to Reset and session generation. |
| L23 | Dead "idle → Background" branch; vr_session.h:3-8 is stale | vr_session.cpp:398, 411-417 | Dead code confirmed; performance effect unproven. Fix the header. At least skip the 25 ms hand-off wait while `!HmdPresent()`. |
| L24 | Crashes on the game thread bypass the mod's crash report (UE3 filter `FUN_00827770` writes its own dump and calls ReportFault) | logger.cpp:176-191 | Plausible (Ghidra: 0 callers, returns 1). See R-R2. |
| L25 | `CallProcessEvent` swallows every exception without logging | script_call.cpp:76-84 | Plausible. Log code, address and UFunction name. Disable that call site after the first fault. |
| L26 | Link ASW restore always writes `asw.Auto`; a path with an apostrophe breaks the PowerShell | vr_session.cpp:139-148 | Confirmed. Read the previous value first; escape quotes. |
| L27 | Legacy auto-fallback reuses stale action handles after a SteamVR restart | vr_input.cpp:270, 540-555 | Plausible. Re-run `Ensure()` when `session::Generation() != act::g_gen`. |
| L28 | Vive wands: D-pad modifier hard to reach (pad north/south = B/A) | vr_input.cpp:124-125, 329-330 | Plausible, depends on SteamVR dpad semantics. Add explicit dpad actions. |

**Dropped (refuted):** "teleport black hold uses an 80 ms ramp". The settle hold is already black before the helicopter jump (hooks.cpp:2860-2863). Only a narrower polish point remains, at the first VR frames after the cinema screen ends; it is listed in R-C4.

---

## 3. Full-VR roadmap

Legend: Value is how much it improves the VR experience (T = transformative / H = high / M = medium). Effort: S ≤ 2 days, M ≤ 1-2 weeks, L = multi-week. Proposals the verifiers found already implemented are removed or reduced to their new part.

### A. Body and presence (view)

| ID | Item | Exists | Proposal | Value | Effort | Risk | RE needed |
|---|---|---|---|---|---|---|---|
| R-B1 | Eye-pivot helper | Three separate corrections (stick 3629-3642, cover/turret 3700-3760, turn-with-head 3806-3822) | Each tick, store {pawnYaw, fwdApplied, pcYaw} in `ApplyFirstPerson`. Correct posRef by the change in fwd·dir(pawnYaw). Keep rotating headOffset with the bodyYaw change. Fixes H3 and M1, and lets fwd blend 15↔0. | H | S | Low (camlog regressions) | none |
| R-B2 | Falling and mount transitions | Hard switch on `PawnOnFirmGround` | Falling counts as on foot (fixes M2). Blend base and fwd over 0.25 s. On vehicles, clamp horizontal head offset to a ~25 u "cabin box". Optional horizon lock (later). | M | M | Medium (fragile helicopter scenes) | Base/vehicle rotation; whether the helicopter seat is a Base or a Vehicle (`AVehicle_exec*`) |
| R-B3 | Body facing from hands and head | `turnwithhead` 45° threshold | Torso yaw = weighted head (0.5) + inter-controller vector (0.3) + gun hand (0.2), with a critically damped spring and a ~20° deadband. Keep `turnwithhead` as the fallback. Depends on R-B1. | H | S | Low | none |
| R-B4 | Room-scale v2 | `Move`, 15 cm threshold (3849-3879) | `MoveSmooth` (fixes M10), continuous catch-up capped at walk speed. The Velocity write for legs and footsteps is **speculative**. | H | S (MoveSmooth) / M (+legs) | Medium for the Velocity part | Pawn Velocity offset; what the AnimTree blends on |
| R-B5 | Realign after cutscenes under the black | No auto-realign (only manual, combo, session start: vr_session.cpp:179, 220) | While `HoldBlack` after `g_cinemaEndMs`/`g_teleportMs`: set `g_hmdYawReference = hmdYaw` and rebase posRef XZ. Option `cinemaalign`. | M | S | Low | none |
| R-B6 | Standing-height calibration | Max height with no decay when crouched >15 cm (3656-3668): **already in place** | New parts only: a percentile window and a "stand tall + both grips 1 s" calibration saved with `SaveIniValue`. | M | S | Low | none |
| R-B7 | Headless shadow and self mirror | Head hidden by bone scale (5001-5011) | **Research only**: first a screenshot to see whether the shadow is headless; then per-component `bOwnerNoSee`/`bCastHiddenShadow`. A mirror view for tuning would be a third Draw. | M | L | High | PrimitiveComponent flag offsets |

### B. Weapons

| ID | Item | Exists | Proposal | Value | Effort | Risk | RE |
|---|---|---|---|---|---|---|---|
| R-W1 | Barrel-true ballistics that keep the game's cone | Hook 6892-6953 (bug H2) | Fix H2, then scale the recovered deviation by stance: two-hand ×0.6, ADS ×0.5, stock braced ×0.8, one-handed rifle ×1.5. `spread` 0 = laser, 1 = game. | H | S | Low | Confirm pellets go through `FUN_0093f510`; meaning of flag 0x800 |
| R-W2 | Scope with eye relief | LT already synthesized on ADS (vr_input.cpp:1333). Barrel view lock (5746-5749). Head-locked lens (900-947). | New parts: the lens follows the barrel axis per eye (TanToNdc), shrinks and darkens off-axis, 80 ms fade; release the view lock as soon as the eye leaves the scope. Carry the lens centre in the FrameTag (also fixes L4). Step 2, a picture-in-picture third Draw, is ambitious. | T | M (step 1) / XL (PiP) | Step 1 low-medium (lens must stay small); PiP high (+30-40 % GPU) | Step 1 none |
| R-W3 | Manual actions (charging handle, pump/bolt) | Reload state machine 874-990, trigger gate 1177 | `Mag::Chamber` state; rack after each shot for bolt-action and pump classes; option `manualaction`. Needs the M14 fix first. | H | M | Low | none (offsets via gunlog) |
| R-W4 | Recoil and haptics from game data | GunKick hardcoded (5151-5172); ProTube `ShotChannel` (protube_bridge.cpp:205) never sent | Save `m_recoilDistortionRange` (L11) and derive kick and climb from it, damped by two-hand and stock. Send ShotChannel per class. | M | S | Low | Layout of the 3×8 bytes (`ue3.py props YWeapon`) |
| R-W5 | Body holsters | Shoulder gesture → Y (vr_input.cpp:806-821); belt magazine | Zones relative to the HMD: right hip, shoulders, chest pouch. Haptic tick on hover. Select by cycling Y while watching `g_weaponPtr` (slow, racy), or a direct script call if one is found. | H | M | Medium | Inventory switch function (script; `AInventoryManager_execInventoryActors @008bfbf0` lists weapons) |
| R-W6 | Grenade from the vest, hand-authored throw | Direction redirect (5202-5257), fixed 0.3 s LB | Grip at a chest slot = LB; on release publish direction and **speed** (fixes L21); scale velocity by `clamp(handSpeed/4, 0.4, 1.3)` in the existing redirect. | H | M | Low-medium (cap the speed) | none essential |
| R-W7 | Magazine visuals | Logical only | Step 1: `HideBoneByName` on the magazine bone (the same ucall is already used for the head at 5009). Step 2 (attach a copy to the hand) is speculative. | M | S / L | Low / High | Weapon bone names (LogBones) |

**Note on the grenade spawn hook.** I checked `FUN_00eae840` in Ghidra (`AYWeapon_execStaticSetGrenadeLocAndDir` → `FUN_00eae840`, also called from `FUN_007fadb0`):
- It spawns a projectile with `FUN_00881810` **only if the slot `*param_3` is empty or flagged**. Otherwise it repositions the existing one (`FUN_0083f210`).
- It then computes the toss direction, including the arc (`param_17`) and the speed through projectile vtable+0x3bc.

That pattern (a persistent projectile in a slot, re-placed each call) fits the **trajectory preview** better than the real throw. Do not build R-W6 on hooking it. Keep the existing redirect-after-spawn, made cheaper (L12/M18).

### C. Comfort and rendering

| ID | Item | Exists | Proposal | Value | Effort | Risk | RE |
|---|---|---|---|---|---|---|---|
| R-C1 | Posed submit re-test plus discontinuity | Posed path all-or-nothing per pair (vr_output.cpp:836) and `predict auto`: **already in place**. Off in ini:135-138, probably because of two since-fixed bugs (GetProjectionRaw flip 181-189; stale tag queue hooks.cpp:589-603). | Re-test `submitpose on`, log pose age, add `Submit_FrameDiscontinuity` after cinema switch, teleport/settle and snap turn. If it validates, make it the default. | H | S | Low | none |
| R-C2 | Aim marker and target box at constant angle | Fixed-pixel Clear rectangles (1119-1131, 1062-1070) | Size from m00 in degrees; draw an alpha quad using the DrawVignette state-block path (993-1040). | M | S | Low | none |
| R-C3 | Vignette from motion the player did not cause | Bug M20 | Forced motion = rendered view change minus HMD change. Blink 0.15 s on recenter and on large reference jumps. Helicopter "tunnel only when forced" preset (E9). | M | S | Low | none |
| R-C4 | Instant black at the start of the hold after the cinema screen | 0.08 s ramp (vr_output.cpp:631) | 0 s fade when the hold starts within a few hundred ms of `g_cinemaEndMs`; keep 0.08 s for death. | M | S | Low | none |
| R-C5 | Cinema on the GPU path | CPU readback in cinema (vr_output.cpp:1226, 1249-1261) | Use the shared GPU slots. Letterbox from the constrained aspect ratio (hooks.cpp:7190-7210); CPU scan only for menus and Bink, on a 64×64 downsample. | M | S | Low | none |
| R-C6 | Stereo cinema screen for in-engine cutscenes | Mono, eye 0 only | Keep the two-Draw loop with a reduced half-IPD (0.3-0.5×), a side-by-side overlay and slight curvature; Bink stays mono. | H | M | Medium-high (comfort, DOF and letterbox symmetry) | Whether Matinee cameras go through the cam+0x10 copy |
| R-C7 | Hidden-area mesh (E14) | None | Hook `Clear`, stamp the HAM at z = 0 on the scene depth clear. ~10-20 % pixel saving. | M | M | Medium-high | Clear sequence in FSceneRenderer (Ghidra) |

### D. UI in 3D

| ID | Item | Exists | Proposal | Value | Effort | Risk | RE |
|---|---|---|---|---|---|---|---|
| R-U1 | In-headset status/toast overlay | Log only; the cinema overlay pipeline (vr_output.cpp:400-458, 780-807) is a ready model | `specopsvr.status` overlay on the worker; `log::Notify()` queue: update installed, SteamVR reconnected, VR output stopped (L22), haptics lost, recentred. | H | M | Low | none |
| R-U2 | Controller hints and context prompts | README only (README.md:92-130); Xbox glyphs in the game | Overlays on GunRole and SupportRole via `SetOverlayTransformTrackedDeviceRelative`; labels per profile; shown on first runs or when looking at a controller; contextual (reload steps, turret, squad). `hints on|off|auto`. Shares code with R-U1. | H | M | Low | none |
| R-U3 | Wrist/weapon HUD (E3) | Single canvas remap; crosshair hidden via YHUD +0x480 bit 0x40 | Wrist overlay drawn with GDI from reflection data (ammo, grenades, objective). Then hide the matching game widgets by hooking `UCanvas_execDrawTile @00C5DDC0` and the others, filtered by the calling script function. | T | L | Medium | FFrame Node offset; YHUD widget function names |
| R-U4 | World-correct projected markers | Bug M19 | Fix the `execProject` remap, then draw a world-locked squad chevron at `g_squadTargetLoc`. | M | S-M | Medium | Call counts first |
| R-U5 | Laser pointer on the cinema screen and in-VR quick menu (E12) | Overlay with crop bounds; combos and ini only | `ComputeOverlayIntersection` + WM_MOUSE posts. Wrist quick menu (recenter, calibrate, comfort options) through `Execute`/`SaveIniValue`. Optional `recenter`/`calibrate` SteamVR actions. Once those exist, the 120 ms B/Y gate can be shortened. | H | M (laser) / L (menu) | Medium (Scaleform menus may ignore mouse messages; a GetCursorPos IAT hook may be needed) | How the viewport reads the mouse |

### E. Input and controllers

| ID | Item | Exists | Proposal | Value | Effort | Risk |
|---|---|---|---|---|---|---|
| R-I1 | Finish left-handed support | Roles swapped only | M17 and L19 fixes, hint row, optional "arms off" path for left-handers. | M | S | Low |
| R-I2 | SteamVR pose and skeletal actions, more profiles | TipOf/render-model hack (vr_input.cpp:382-439); 6 profiles | Pose actions bound to `/pose/tip`; finger curls for grip and pointing; Pico, Focus 3 and Pimax profiles. Needs a calibration migration (armoffset, gunoffset, magwell); keep `controllerpose raw`. | M | M | Low-medium |
| R-I3 | Gun-hand melee: butt jab and downward strike (A3) | Melee independent of grenade: **already done** (A3 [~]) | Pass the gun-hand velocity into GestureSample; add butt jab and downward strike; gesturesim cases. Merged with R-P4. | M | S | Low-medium |

### F. Robustness and performance

| ID | Item | Proposal | Value | Effort | Risk | RE |
|---|---|---|---|---|---|---|
| R-R1 | Identity guards and a world generation counter | Extend `StillAlive`/`ObjClass` (4121-4135) into a `WeakObj` with a `g_worldGen` counter and `g_cameraSeenSeq` consumption. Note: the class+index check cannot detect same-class address reuse, so generation and presence pruning are what actually help. Fixes H4 and M18. | H | M | Low | Real CollectGarbage/LoadMap (trace callers of `FUN_009524f0`: `FUN_00db3360`, `FUN_0082ecf0`, `FUN_00413530`) |
| R-R2 | Hook the engine crash filter | MinHook `FUN_00827770(EXCEPTION_POINTERS*)` (returns 1). Write the CRASH line, mod dump, ring-buffer log tail and last script call, then chain. `collect_logs` also gathers the engine dump and Launch.log. | M (enables everything else) | S | Low | Where the dump path `DAT_01503d78` is filled |
| R-R3 | Lifetime-safe output pipeline | AddRef ownership (M7), fence-gated slots (L2), backoff instead of `Disable()` (L22), Submit >100 ms watchdog line. No keyed mutex. | H | M | Medium (hot path) | none |
| R-R4 | Safe updates | Boot sentinel with automatic `.prev` rollback (`.prev` and revocation already exist, updater.cpp:158-171, 311-315), companion asset manifest, delay-loaded OpenVR (M6). Signed manifest optional. | M | M | Medium | none |
| R-R5 | Asynchronous ring-buffer logger | MPSC ring, drain thread, lock-free crash dump, per-category rate limits. | M | S | Low | none |
| R-R6 | Cheaper scans | Prune the projectile list by presence in each scan, walk actor lists, cache the squad pick (L12, L13, L14). ProcessEvent-level spawn hooks are not recommended (hot path). | M | S | Low | none |
| R-R7 | Resilient haptics | All ProTube calls through the queue thread; bridge restart with backoff; `haptics test` command. | L | S | Low | none |

### G. Tooling

| ID | Item | Proposal | Effort |
|---|---|---|---|
| R-T1 | regress.py robustness phases (run only in play sessions; this review touched no game tools) | transition (map reload while turning), death and respawn, `setres` Reset, SteamVR null-driver session cycle, perf thresholds from STATS and VR-OUTPUT. | M |
| R-T2 | Offline unit tests | `tests/` target for ParseVersion, IsRevoked, JsonString, MigrateConfig, Execute normalisation, ConfigKey; updater against a local stub. | S |
| R-T3 | `specialmoves` enum dump | Read the YE_SpecialMove UEnum names at runtime (UEnum Names offset ≈ +0x40, verify once). Turns the SPECIALMOVE logs and IsBodyAnimSpecialMove (221-230) into named, checkable data. Cross-check with `AYGamePawn_execGetSpecialMove @004a6a50`. | S |
| R-T4 | `interactlog` | Stage 0 of section 4. | S-M |
| R-T5 | Ghidra follow-ups | Define a function at `0x00c91be8` (pad re-detect, M16); FUN_0093f510 vtable users (pellets, H2); GuardedMain `__try` site (L24); FSceneViewState destructor (L5). | S |

### Milestones

**Milestone 1: next release (0.9.10 / 1.0-rc). Fixes plus cheap comfort.**
1. Bug fixes:
   - Locked-scene and state: H1, H3+M1 via R-B1, M2, M8, M9 (after R-T3 confirms the values), M3, M4, L15.
   - Tracking-loss robustness: M11, M12, M14, M15, L6.
   - Input: M13.
   - Install and update: M5, M6.
   - Diagnostics noise: L14 (vischeck off).
2. R-W1: H2 fix with stance-scaled spread (verify with `firelog` first).
3. R-C1 (posed submit re-test plus discontinuity), R-C4, R-C2.
4. R-B4 step 1 (MoveSmooth), R-B5 (cinema align).
5. R-R2 (crash filter hook), R-R5 (async logger), R-T3 (specialmoves dump), R-T2.

*Dependencies:* R-B1 before R-B3 and before the M1 fix. R-T3 before M9 and the per-move policy.

**Milestone 2: medium term. Physical interaction v1 and 3D UI.**
1. R-R1 (identity guards, fixes H4 and M18), R-R3 (output pipeline: M7, L2, L22), R-R6.
2. R-U1 status overlay, then R-U2 hints (same overlay code).
3. Section 4 stages 0-2: scanner, affordances, grab-to-use for rope, zipline and turret, plus R-P4 executions (with M3 and M4 fixed).
4. R-W2 step 1 (eye-relief scope; fixes L4), R-W6 (vest grenade), R-W4 (needs L11), R-W3 (needs M14), R-W7 step 1.
5. R-B3 body facing, R-C3 vignette, R-C5 cinema GPU path, R-I1 left-handed, R-I3, R-U4 (after M19 measurements).
6. R-R4 safe updates, R-T1 regress phases.

**Milestone 3: ambitious.**
R-U3 wrist HUD with game widgets hidden; R-W5 holsters with direct selection; R-C6 stereo cutscenes; R-U5 quick menu and laser; R-I2 skeletal input; section 4 stages 3-4 (pick-ups, QTE/moral-choice gestures, two-handed turret grip); R-C7 hidden-area mesh; R-B2 cabin box and horizon lock; R-W2 step 2 (picture-in-picture scope); R-B7.

---

## 4. Physical interaction plan (detailed)

Principle: **the game stays in charge.** The mod only decides *when* to send the input the game already expects, and *where* the player's hands are. Every UE3 special move (FastRope, Zipline, PickUp, Execution) still plays its own animation, which `IsBodyAnimSpecialMove` already locks the body to. Nothing in the current tree reads `YTrigger_*Interaction`. A grep for interactlog, SetAimAtLocation, IsExecutable, FindMeleeTarget, GetBoundContext and GetCurrentIconPosition finds nothing in src, so all of this is new.

### Stage 0: InteractionScanner (diagnostic, `interactlog`). Effort S-M, risk low.
- **Where:** game thread, every 250 ms, from `ViewportDrawThunk::Detour` next to `UpdateSquadCommandAim` (hooks.cpp:2733).
- **What to enumerate:** actors that IsA `YTrigger_TurretInteraction`, `YTrigger_FastRopeInteraction` or `YTrigger_ZiplineInteraction`. Resolve the class pointers once per world change. Walk the level actor list, **not** GObjects (see L12).
- **Natives to call** via `vr::script::CallFunction`, each found in Ghidra:
  - Turret: `GetEntryPoint @00b17a90`, `GetBounds @007a7110`.
  - FastRope: `GetBounds @00d08b90`, `GetCurrentIconPosition @008812f0` (→ vtable+0x3a0).
  - Zipline: `GetRopeStart @00b83a60`, `GetRopeEnd @00e9a080`.
  - Also `GetCurrentIconPosition @00e831a0` and `IsEnabledForTacView @00e66a90`.
- **First job: pin the parameter layouts with ucall on a live instance.** The exec stubs show one out-vector for `SetAimAtLocation` and none for GetBounds, so the parameter structs must be confirmed.
- **Also log** `UYContextCommands.GetBoundContextCommand @00aa14d0` (→ vtable+300) and `GetAvailableGenericCommand @00ecf6e0` (→ +0x134) while standing at each interaction. This gives the YE_ContextCommand value → pad button mapping, which is UnrealScript-side and can only be learned from logs.
- **Deliverable:** a log table per chapter of {kind, world position, enabled, context command, button}.

### Stage 1: affordances (read-only). Effort M, risk low.
- Publish the nearest enabled interaction as `{kind, pos, cmd}`, like `g_squadTargetLoc`.
- **Haptics:** a 40 ms tick when a hand enters a ~25 cm zone around the icon or entry point, and a rising tick rate as the hand gets closer (`Pulse`/`g_hapticUntil`, vr_input.cpp:991).
- **Visuals:** a world-locked ring at `GetCurrentIconPosition`, drawn through the per-eye NDC path that the aim marker already uses. Hiding the game's 2D icon needs one YHUD flag found by memscan diff in a play session; this is optional.
- **Hint overlay text** (R-U2): "grab".

### Stage 2: grab-to-use. Effort L, risk medium.
- **New gesture `grab-use` in `GestureDetector::Update` (vr_input.cpp:700-834).** It fires only when all of these hold:
  - the grip closes while the hand is inside the zone;
  - the context command query reports a command;
  - `Gameplay()` is true;
  - the hand is not reloading.

  The gesture then `Press()`es exactly the button that the context command maps to.
- **Held interactions** (rope, zipline): hold the button while the grip is held.
- **Logging:** log every grab-use; the prompt button stays as a fallback. Ship each kind behind an ini switch (`grabuse rope|zipline|turret|off`).
- **Main risk:** a wrong button in context (A also means cover and vault). Requiring a reported context command mitigates it.
- **Turrets:** after entry, `AYTrigger_TurretInteraction_execSetAimAtLocation @00af19b0` (→ vtable+0x3A0, one FVector). Try `SetAimAtLocation(eye + handRay*5000)`:
  - If the engine honours it for a player-controlled turret, it replaces the per-tick P controller at hooks.cpp:3996-4016.
  - If not, keep the P controller with a dt-based gain (L17) and the RY fix (L18).
  - Use `GetEntryPoint` within ~150 u while `g_onTurret` to find which turret interaction is in use.

### Stage 3: hand-directed melee and executions. Effort M, risk low-medium.
1. Ordering fix (M3): hold the gesture's B back for one poll, so selection and rotation happen first.
2. Candidate choice by gaze or strike direction (M4). Validate with `IsExecutable` (one argument + bool) and `!IsBeingExecutedOrRevived @0040aa70`. Optionally ask `FindMeleeTarget @00517c00` (→ vtable+0x724, ~14 parameters; pin the layout with ucall).
3. New gestures (R-I3): downward strike below the hips with a candidate in front (A3), and a rifle-butt jab.
4. Turn the body only by the needed angle, and only for an execution candidate.
5. Impact haptic when the SPECIALMOVE transition enters the execution value. Needs R-T3 to know which value in 23-34 that is.

### Stage 4: pick-ups, squad pointing, scripted choices. Effort L, risk medium-high.
- **Pick-ups (special moves 23-34):** grab at the dropped weapon or ammo actor location found by the scanner (class names from stage 0 logs).
- **Squad orders by pointing (E8):** add a `hand` value to the existing `commandaim` (control.cpp:882-885). Pointing the support hand for 150 ms acts as RB held; lowering it issues the order. 4° tolerance, reusing the candidate loop after fixing L13. Haptic tick per target change, with a world chevron.
- **QTE and moral choices (E9/E10):** a separate state for "cinematic with Walker + on-screen prompt", detected via `YPlayerController.GetSpecialMoveMessages @005c3d20` or the context commands while bCinematicMode. Add a whitelist of gestures mapped to the prompted button. **High risk:** every scene is scripted differently, and a choice cannot be undone. Use per-scene ini switches and a per-chapter playtest.
- **Per-move policy table** replacing the single `g_pawnSpecialAnim` bool, built on R-T3: {ArmsToAnim, BodyToAnim, HeadFollowsNeck, GesturesAllowed, Vignette, Haptic}.
  - Vault and pick-up: no neck drop (fixes M8).
  - Rope and zipline: vignette and rumble.
  - QTE: gestures allowed.

**Dependencies:** R-T3 → policy table → M8/M9 cleanup. Stage 0 → 1 → 2. M3 and M4 → stage 3. L13 → squad pointing. R-U2 hint overlay → stage 1 UX.

---

## 5. Gaps and required playtests

**Not covered or not verifiable in this review:**
- **UnrealScript behaviour** is not in the exe. Open questions:
  - which HUD widgets call `Canvas.Project` (M19);
  - YE_SpecialMove values beyond 19/20 (M9, M8, policy);
  - the context command → button mapping;
  - whether `SetAimAtLocation` is honoured for the player;
  - inventory switch and zoom entry points;
  - melee target selection parameters.
- **Engine paths still unresolved in Ghidra:**
  - `0x00c91be8` (third XInputGetState caller; still "no function" in my re-check), which decides whether M16 is permanent;
  - the GuardedMain `__try` site (L24);
  - the real CollectGarbage/LoadMap (R-R1);
  - the FSceneViewState destructor (L5);
  - the FUN_0093f510 vtable users (pellets, H2);
  - the meaning of hit flag `0x800`.
- **Grenade spawn:** I checked `FUN_00eae840` and it looks like a persistent preview projectile (spawn-or-reuse in a slot plus toss direction), not the throw spawn. Unconfirmed at runtime.
- **Areas not reviewed in depth:** display_spoof.cpp (window spoofing, needed for R-U5), the ProTube bridge protocol beyond ShotChannel, the bHaptics reconnect logic, tools/*.py correctness, and CMake/packaging beyond the OpenVR import.
- **Performance claims are unmeasured:** GObjects scan cost, squad trace cost, GPU fence timeouts, and the standby stall in Scene mode. None has a log line yet. Instrument first: VR-OUTPUT timeout counters, scan timings in STATS.
- **No runtime test was run.** Everything here is static analysis plus Ghidra, under the read-only rules.

**Playtests needed** (headset, real gameplay):
1. **Locked moves:** vault, rope, zipline, pick-up and death with arms on (H1, M8). Use camlog plus a screenshot of the gun position.
2. **Turning:** turn-with-head, snap turn on the helicopter and on a turret, stick turn in cover (H3, M1). camlog base drift should be ≈0 after R-B1.
3. **Falls:** ledge drops and stair walking in room-scale with Move vs MoveSmooth (M2, M10, L8).
4. **Firing:** `firelog` with shotgun, sniper and assault rifle against penetrable cover, spread on/off (H2, R-W1).
5. **Melee:** punch gesture next to a downed enemy, with and without a standing enemy in front (M3, M4).
6. **Tracking loss:** cover a controller during two-hand aim, during the belt reload and while peeking; cover the headset camera (M11, M12, M14, L6).
7. **Left-handed session** (M17, L19).
8. **Posed submit on vs off:** pose-age log, judder on head turns, on Quest Link, Index and WMR (R-C1).
9. **SteamVR restart mid-game and headset standby:** input recovery and frame pacing on the monitor (M16, L23, L27).
10. **Long session:** several checkpoint reloads and map changes, watching for random crashes (H4); this also validates R-R2 crash capture.
11. **Updater:** unsupported exe with `autoupdate off`; missing `openvr_api.dll` (M5, M6).
12. **Stage 0 interaction census:** walk each chapter with `interactlog` and `specialmoves` before starting stage 2.