# SpecOpsTheLineVR

An unofficial **full VR mod for Spec Ops: The Line** (2012, Unreal Engine 3,
Direct3D 9): stereoscopic 6DOF first person, motion-controller aiming with
Walker's own arms, VR cover, gestures, external haptics, and cutscenes on a
virtual cinema screen.

> Status: **beta**. The whole campaign loads and plays; please report bugs
> (see [Reporting bugs](#reporting-bugs)).

## Features

- **Stereoscopic 3D** at the headset's resolution, field of view and IPD,
  synchronous stereo (both eyes rendered from the same game tick), head pose
  predicted to the moment the image reaches your eyes (measured on your PC).
  The view uses the exact head rotation: the world no longer swings when you
  tilt your head while looking down.
- **6DOF head tracking**: look, lean, crouch and side-step; the head stops at
  walls, crates and squadmates instead of clipping through them (*Head in
  walls*: only the part that goes in is stopped, sliding along a wall stays
  1:1). **Room-scale**: when you step, Walker walks with you. Your real height
  sets the eye height, taken when you put the headset on or recenter; it never
  moves by itself while you play (put the headset on sitting and then stand up:
  after 3 s standing it is taken again, once, under a blink). Your eyes are
  never lower than Walker's own eyes when you stand straight (shorter players
  and players sitting with seated play off see from Walker's eye height, so his
  collar is never under your chin). **Seated play** is
  detected by itself when you put the headset on or recenter (*Seated play*
  auto/on/off): sitting, your eyes are at Walker's eye height and your real
  head movement is added; started sitting and then standing up for 3 s, it is
  measured again (a message tells you), and sitting back down for 3 s after
  that measures again too. Crouch for real and Walker crouches with you; the
  game's crouch button (and scripted crouches, and low cover when seated)
  lowers the view smoothly to Walker's crouch (*Crouch view*, `crouchtime`),
  and a real crouch is never counted twice. Hit reactions, stumbles and sprint
  slides never move the view (the game shrinks Walker for the animation; you
  did not crouch), and aiming out of low cover with the crouch button on keeps
  the view where it is. The game's recoil and camera
  shakes never move your view, and neither do the headset runtime's own recenters (your hands
  and gun stay put too). Squadmates walking past no longer pull the view; your
  head only stops at them if they come within reach, and one walking right
  through your head moves your view aside smoothly instead of jumping it from one
  side to the other (walls, edges and objects still stop your head at once). Cuts, checkpoints and death fade from black.
- **Audio in the headset**: the game's sound follows the Windows default
  playback device, so it reaches the headset even when Virtual Desktop or
  Link starts streaming after the game is already running.
- **Wrist HUD and laser**: ammo, weapon and health on your support-hand
  wrist like a watch (`wristhud`); on the virtual screen the gun controller
  is a laser pointer for the game's menus, trigger to click (`cinemalaser`).
- **First person** with Walker's body: his arms follow your controllers
  (inverse kinematics on the game's own skeleton), the gun stays in his hand,
  and the body turns with you when you turn around. His eyes are on yours and
  his body hangs from your neck: turning or bending your head does not drag
  his chest under your eyes, leaning (also in a chair) moves him with you.
  After a glance to the side, once your head stays still for a moment his body
  turns slowly back under it (Basic > Body under the head = `bodyalign`;
  `bodyalignspeed`; the view never moves), so looking down shows your chest,
  not his shoulder pad. The gun in your hand, its shots and the green marker
  stay where you hold them while he turns, also when he turns with your head
  and through long frames. It stops while you aim (two hands
  on the gun, gun at your eye, trigger pulled in the last second) and with a
  wall or low cover in front of your eyes (the view keeps its distance from
  walls along his body, so turning him there would move it). It needs
  `aim hand` (the default) with tracked controllers and `turnwithhead` above 0.
  Looking down you see his chest and legs; his neck and scarf stay whole under
  the hidden head (drawn only, `necksize` 0-1); when his gun arm would have to
  stretch to reach your hand, his body moves forward just enough, never past
  where it used to be (`reachslack`).
  *Walker's body* in the menu: `full`, or `arms` (only arms, hands and gun are
  drawn; hits and physics still use the whole body).
- **Aim with the gun**: shots, rockets and grenade-launcher rounds leave the
  barrel of the gun you hold, with the game's own spread scaled by your
  stance (sights at the eye = practically none, two hands = half, long gun in
  one hand = more). A green marker shows where the barrel points (on by
  default, `marker off` or the in-headset menu to use only the weapon's
  sights); `laser on` adds a visible beam. **Two hands**: put your left hand
  on the barrel of the drawn rifle and close the grip (or just keep the hand
  there a moment): Walker's left hand grips it and the gun turns about your
  right hand so the barrel goes through your left hand (two-handed spread and
  vibration); open the grip to let go (the gun eases back to your right hand
  as you move it), or move the hand away: the gun goes back to your right hand
  as your left hand leaves the barrel (after leaving with the grip closed,
  open and close it again to grab). While both hands hold it, turning or
  rolling your right wrist and sliding the left hand along the barrel do not
  move the barrel. With a pistol the left hand wraps the
  gun hand (direction unchanged). With *Controller hold* gunstock the gun always
  follows the stock. **Aim down the sights** by bringing the gun to your eye.
  Gamepad aim assist is disabled while aiming by hand.
- **Gun angle**: the gun points where the controller points, like the menu
  laser, on Virtual Desktop (VDXR) and SteamVR (headset check of both still
  pending). Tilt it with *Gun angle* in the in-headset menu.
- **Gunstock support**: *Controller hold* > gunstock and one-button
  calibration of the stock angle and of where your left hand sits on the
  stock.
- **VR cover**: the world does not spin when Walker takes cover; lean out
  with your body to pop out and aim.
- **Mounted guns and the helicopter minigun**: with *Turret grip* normal
  (the default with bare controllers), hold a mounted gun on the ground by its
  two handles with both hands (no need to close the grips): it turns about a
  pivot 35 cm in front of your hands (`turretpivot`), like a real mounted
  gun, so moving your hands left turns the barrel right and lowering them
  raises it; turning your wrists does not move it. On the helicopter minigun,
  grip both handles and turn them like handlebars. One hand alone also aims;
  *Turret grip* gunstock aims along your stock instead. The gun stops at its
  mount's limits with a short buzz on both hands (firing bursts never makes it
  stick: the recoil is not taken for a limit), the green marker shows
  where the rounds go, and the view stays still while the gun turns (no
  black flashes in the helicopter). On a turret with *Turret grip* normal the
  grips send no grenade or squad order, not even when you let go of the
  turret, pause or switch grip with the grips still closed: a grip closed on
  the handles stays a handle until you open it. The pivot distance, the
  handlebar thresholds and the limits of each kind of mount are *Advanced*
  settings (`turretpivot`, `turretbar*`, `turretground*`, `turretflight*`).
  In the first sessions the controller hints come on foot, where they apply;
  on a turret you get a turret hint instead. Headset check pending, also for
  the feel of the ground-turret pivot and the helicopter handlebars.
- **Squad orders** target the enemy you look at; the
  target stays locked until you release the button.
- **Executions**: press B next to a downed enemy and Walker turns to him.
- **Gestures**: throw grenades (harder throw = faster grenade), punch with the left hand (melee; a jab with the gun too with `gesture gunmelee on`, off by default), reload by touching the magazine with the left hand and pulling down (`reload physical` in the settings: pull the magazine, take a new one from your belt, push it in; `manualaction on` also asks you to rack the gun),
  duck into cover, grab the next weapon from your shoulder.
- **Interactive objects** (work in progress): pick up, hold and throw loose
  objects with the left hand (grip): bottles, cans, bags, suitcases, boxes,
  radios, books, chairs, barrels and the like, including the ones placed in
  the levels. Each object settles into the hand the same way every time and the
  fingers close around it. Thrown objects leave in the direction your hand
  moved, and heavy ones (over ~15 kg: suitcases, chairs, barrels) follow the
  hand and fly a little more slowly, but nothing is too heavy to lift, even
  while running. **Throw style** (settings: peak by default, classic): a
  throw leaves at the fastest speed of your swing, not the slower speed your
  hand has when it opens, and spins with your wrist (a bottle flicked by the
  neck flips end over end); heavy objects lob (a 33 kg barrel at 2.3 m/s at
  most). Letting go slowly just puts the object down, and walking or stopping
  while you let go does not fling it. **Object weight** (settings: light by default, full, off):
  heavy objects lag behind the hand, sag a little and turn more slowly (but
  always turn), and objects held off-centre tilt down in the hand; a 50 kg
  crate still comes up, slowly. An object stuck among others (a bottle wedged
  in a crate) comes free if you keep pulling: the pull grows until it gives,
  then the weight takes over again. If you hold still with your hand on a stuck
  object, your grip settles where the object is (as if your hand closed on it
  there) instead of yanking it or letting go; it only slips out of your hand
  when you pull far away. Snap turns carry the object in your hand with
  you. **Analog grip** (settings: off by default; needs Object weight): how
  hard you squeeze matters. A loose grip, just past the point where the hand
  closes, gives your hand less pull and twist (40% instead of 100%): a radio
  trails a few cm further and turns back more slowly, and objects held
  off-centre tilt a little more; a normal full squeeze (no need to crush the
  grip) holds them tight. The heaviest objects still come up as with a firm
  grip, and opening your hand as you throw does not weaken the throw (it uses
  your firmest grip of the last 0.3 s). Controllers with an on/off grip see no
  change.
  **Solid hands** (settings: held by default, full, off): your free hand is
  drawn where physics puts it, not on the controller. With held (and Object
  weight on) it stays on what you hold: it lags, sags and tilts with a heavy
  object instead of closing on air in front of it, and a radio pressed down
  into a crate keeps your hand on the radio instead of sinking into the
  crate; on a door you pull, the hand stays on the point you grabbed when the
  door stops. With full your empty hand also stops on objects that have
  physics, such as a crate lid (most walls and floors of the levels do not
  stop it yet). With off the drawn hand stays on the controller, so a heavy
  object trails behind your fingers (up to ~20 cm for a crate). The drawn hand
  is never more than 25 cm from your controller and follows it back within
  ~0.1 s; the gun hand, aiming and the left hand on the gun never change.
  **Impact
  haptics** (settings: full by default, low, off): the controller pulses once
  when what you hold hits something (a deep thud for a radio against a wall, a
  short tick for a bottle on a table), when a held object hits an enemy, or
  when your empty hand slaps something, stronger for harder hits (a radio
  swung gently gives a softer thud than one slammed down); grabbing gives a
  confirmation pulse, stronger for heavy objects or fast catches. The gun
  hand, and the left hand while it holds the gun or reloads, keep the fixed
  pulses of before. **Gravity gloves**: point your left hand at an object up
  to 8 m away, close the grip and flick the hand towards you to pull it into
  your hand (keep the grip closed to catch it), as in Half-Life: Alyx. Guns
  dropped on the ground come the same way (with Pick up enemy weapons on; not
  while your left hand holds your gun or reloads): caught, the gun is in your
  left hand as if you had picked it up, or, if you already carry that gun and
  your ammo is not full, you take its ammo instead. The object or gun you can
  pull is marked with bright corners. Your hands are solid: a slap knocks a bottle off a crate
  and pushes light objects around, but they never stop your own shots or the
  aim marker (a hand on the handguard or in front of the muzzle). Glass bottles break when thrown against
  something, dropped from high enough or smashed while you hold them, and
  shooting a loose object knocks it away. Hinged things such as car doors
  and the stop sign swing when you grab and pull them. The game notices
  when an object you throw or swing hits an enemy (never your squad, allies
  or civilians) or a glass pane, and which body part it hits (the "Objects
  as weapons" setting). **Thrown objects hurt** (settings: on by default):
  a bottle thrown at an enemy's body hurts him a little (about a tenth of
  his health); a heavy object (a radio, a bag, a suitcase) or a bottle on
  the head makes him stagger and fall like your own melee, and you can then
  finish him with B as after a melee. A thrown object never kills, and a
  throw spoiled by a tracking glitch of the controller never knocks anyone
  down. **Swing objects at enemies** (settings: on by default): hit an enemy
  with what you hold, swinging fast (2.5 m/s or more, measured from your
  body: walking into him with the object held still is not a swing): from a
  real swing (or on the head) he staggers and often falls like after your
  melee, then B finishes him; a slower hit or a light object only hurts him a
  little. Head hits count double, nothing you swing ever kills, and a bottle
  or a glass breaks on him. The controller gives one firm pulse (none on the
  hand that holds the gun), and the object stops on his body for an instant.
  A hit with the object does not land on top of your own melee (B), but B
  right after a stagger finishes the enemy, as after your melee. Swinging or
  throwing an object never throws a grenade. **Objects break glass** (settings: on by default): objects you throw
  or swing damage glass panes, breakable crates and the windows that hold
  back sand, through the game's own damage: a bottle, thrown or swung, only
  cracks the sand-filled bus windows of chapter 1, a thrown radio smashes them and the sand pours
  down on whoever is below, just as when you shoot them (once the game has
  made them breakable in that fight). A pane that holds bounces the object
  back. **Objects stop bullets** (settings: on by default): hold a suitcase,
  a box or a radio in front of you and enemy bullets hit it instead of you;
  each hit pushes it and you feel it in the hand, and after a few hits it is
  shot out of your hand (plates and books 2, suitcases and radios about 6,
  metal boxes and sandbags 15; a sniper or shotgun wears it out faster). Off
  = enemy bullets go through what you hold. **Explosive bottles** (settings:
  on by default): the propane and diving bottles in the levels can be picked
  up, pulled with the gravity gloves and carried (a propane bottle weighs
  about 14 kg, so it is a short toss, not a throw). It explodes where it is,
  with the game's own blast, when a bullet hits it hard enough (yours or an
  enemy's: a rifle needs two hits, as on the bottles left in place) or when it
  lands from 3 m or higher, say dropped off a roof onto enemies; putting it
  down never sets it off. Carry it to the enemies, step back at least 5 m and
  shoot it: the blast hurts you too. **Pick up enemy weapons** (settings: on
  by default): close your left (support) hand on a gun a fallen enemy dropped
  (or pull it from up to 8 m with the gravity gloves) and it
  comes into your hand by its grip; you can carry it, throw it, or bring it to
  your gun hand (within about 15 cm) to take it, with its ammo, the gun you
  held falling to the ground if you already carry two. Touching a gun of a
  type you already carry takes its ammo instead (with your ammo full you pick
  it up). A gun in your hand never vanishes, although the game clears away
  dropped guns after a while. Off = press A next to it, as in the game.
  **Fake grenades** (settings: on by default): toss a can, a bottle or a
  rock so it lands within 3 m of enemies and they take it for a grenade: the
  nearest one shouts "grenade!" and their AI gets the same warning as for a
  real frag grenade, which can make them leave their cover. It works once
  every 10 s per group, only with small objects (a radio is too big), not
  when the object hits or touches an enemy, and not when Adams or Lugo is
  about as close to where it lands as the enemy.
  **Throw back grenades** (settings: on by default): an enemy grenade that
  has come to rest on the ground within 8 m of you gets red corners (thicker
  when your left hand can reach it). Close your left hand on it to pick it up
  and throw it back. Its fuse keeps burning (enemy frag grenades go off 4 s
  after the throw, so you have a couple of seconds), and if you hold it too
  long it explodes in your hand and kills you. Thrown back it is your
  grenade: the enemies it kills count as your kills. Grenades still in the air
  cannot be caught yet, and a grenade with under 1 s of fuse left (stun and
  phosphor grenades always) is not offered. Your squad's grenades are left
  alone.
  **Objects make noise** (settings: off by default; experimental, not
  verified): an object you throw, drop or knock over that lands hard, or a
  bottle you smash in your hand, sends the enemies' AI the noise the game
  makes for a fist fight, at the spot where it lands: within 3 to 15 m
  depending on how heavy it is and how hard it lands (breaking glass is
  louder). In firefight tests it made no clear difference, because the
  enemies already knew where you were. It was not tested on enemies that are
  not fighting yet, and the game records the noise as made by you, so it may
  draw them to you rather than to the object.
  **Disarm enemies** (settings: on by default; needs Objects as weapons):
  hit an enemy's gun hard and he staggers as after your melee; harder (a
  slap with your free hand at about 4 m/s, a bottle swung at 2.5 m/s, a
  thrown radio) and the gun falls out of his hands, the game's own drop, so
  you can pick it up. A disarmed enemy stops shooting; once he is back on his
  feet (5-10 s) he grabs his gun again if it is still next to him (within
  3 m) and you did not take it.
  You feel the hit in the hand that struck (never in the gun hand).
  **Drag bodies** (settings: on by default): close your left hand on a dead
  enemy's arm, leg or body to grab it and drag it along the ground, say to
  clear a doorway or pull it out of sight; a fast swing of your free hand
  also shoves a body. A body is heavy: it follows a slow pull closely, lags
  more when you pull harder, and the pull grows while it does not come, so it
  always moves; yank it away fast and it slips out of your hand. Only dead
  enemies (never squad mates, story characters or civilians), never living
  ones or enemies still dying on the ground. A body
  the game has already laid to rest wakes up when you touch it, and when it
  stops again the game lays it to rest where it is (with a new blood pool).
  **Object hints**: the first time each situation comes up, a short hint in
  the headset tells you what you can do (an object near your left hand, the
  first object you hold, an enemy within reach while you hold something, an
  enemy grenade with red corners, a dropped gun at your left hand, an
  explosive bottle in your hand), at most one every 7 s, never in cutscenes
  or on turrets, and none for a feature you switched off. With Controller
  hints on auto each one comes once per profile ("on": once per session,
  "off": never).
- **Haptics**: controller rumble on the gun hand, **ProTubeVR**
  (ForceTube / ProVolver) recoil and **bHaptics** vest and sleeves.
- **Virtual cinema screen** for videos, menus and loading screens; scripted
  in-engine scenes stay in 3D.
- **Comfort options**: smooth turning at a fixed speed or snap turning,
  movement where you look, vignette, recentering with both sticks (the
  headset's own recenter is absorbed: the view stays still), left-handed mode (gun buttons on the left
  controller and Walker holds the gun in his left hand).
- **Sniper scopes** work as a lens along the barrel.
- Fast GPU-to-GPU frame copy (D3D9Ex shared surfaces to Direct3D 11 / OpenVR).
- **Performance**: each eye drawn with its exact field of view (no wasted
  pixels), resolution taken from the VR runtime (SteamVR's resolution slider;
  with Virtual Desktop through OpenXR, its own graphics quality setting),
  dynamic resolution that keeps the headset frame rate, a VR graphics preset
  and low-latency frame submission. Works with canted-display headsets
  (Pimax and similar).
- **Automatic updates** from the GitHub releases (stable or beta channel,
  withdrawn versions replaced automatically, `rollback.bat`,
  `reset_settings.bat`).
- Detailed **logs and crash dumps** for bug reports.

## Requirements

- Windows 10/11, a recent NVIDIA/AMD GPU.
- *Spec Ops: The Line*. Developed and tested on the Steam version; non-Steam
  copies are welcome too, see [Non-Steam copies](#non-steam-copies).
- A PC VR headset with two motion controllers. With the default
  `runtime auto` the mod picks by itself: **Virtual Desktop (VDXR)** when the
  Virtual Desktop Streamer is running, otherwise **SteamVR**. `runtime openxr`
  (in `specopsvr.ini` or the in-headset menu) uses the OpenXR runtime active
  on your PC instead (Meta Quest Link, Pimax...; experimental on Link). Developed and tested
  with a Meta Quest 3 over Link; Virtual Desktop works too. Default bindings
  for Quest/Rift Touch, Valve Index, Vive, Vive Cosmos, HP Reverb G2, Pico and
  Windows Mixed Reality controllers.

## Install

1. Download `0-SpecOpsTheLineVR.zip` (the first file) from the
   [latest release](https://github.com/Mavic-Pro/SpecOpsTheLineVR/releases/latest)
   and extract it anywhere.
2. Run `install.bat`. It looks for `SpecOpsTheLine.exe`: Steam libraries,
   GOG/Epic/other installs, the usual game folders, then every drive (if it
   finds more than one copy it asks which one). If it cannot find it, or to
   pick a copy, pass the folder:
   `install.bat "D:\Games\Spec Ops The Line"`.
3. In Steam, *Spec Ops: The Line > Properties > Launch options*, add
   `-windowed ResX=2400 ResY=2500`. Once the headset is on, the mod switches
   the game to the resolution SteamVR recommends for your headset: to change
   sharpness, use the SteamVR resolution slider.
4. Put the headset on (connect it to Virtual Desktop or Link, or start
   SteamVR) and start the game. Through OpenXR (VDXR, Link) SteamVR is not
   needed and the resolution follows that runtime's own setting (VD's
   Graphics Quality, up to Godlike at 120 Hz) instead of the SteamVR slider.

The game files are never modified: the mod only adds `d3d9.dll` and
`openvr_api.dll` next to `SpecOpsTheLine.exe`. `uninstall.bat` removes them.

## Updates

The mod checks GitHub at start-up and installs new versions for the next
launch. New versions can first come out as test builds (GitHub
pre-releases): add `updatechannel beta` to `specopsvr.ini` to get them too.
Updates are always on: there is no switch to turn them off and a crash never
blocks them. If an update causes problems, close the game and run
`rollback.bat` to go back to the previous version until a newer one comes
out. Updates also refresh the helper scripts and `openvr_api.dll` when
the release lists them. Your `specopsvr.ini` is normally migrated, not replaced
(`reset_settings.bat` restores the defaults and keeps a copy of yours).

**0.10.0 resets the settings once.** Dozens of options and defaults changed
since 0.9.18, so the first time 0.10.0 starts it puts the shipped
`specopsvr.ini` in place of an older one (any 0.9.x file, `configversion`
below 12). Your old file is kept next to it as
`specopsvr.ini.bak-<date>_<time>`, and a message in the headset says so after
10 seconds of play. Re-apply from that copy the changes you want to keep
(for example `updatechannel beta`, *Controller hold* gunstock or your stock
angle). If the copy cannot be made, the old file is migrated as before.

## Controls

| Input | Action |
|---|---|
| Left stick | Move |
| Right stick | Turn (smooth or snap) |
| Right trigger / left trigger | Fire / aim |
| Left grip | Grenade (hold: trajectory, release: throw); on the gun it just holds the gun |
| Right grip (hold, look at an enemy, release) | Squad attack order |
| Both hands on a ground turret's handles | The gun pivots in front of your hands like a real mounted gun: hands left = barrel right (*Turret grip* normal) |
| Both grips on the helicopter minigun's handles | Turn it like handlebars (*Turret grip* normal) |
| A / B / X / Y | Action-cover / melee / reload / next weapon |
| B + Y held for 1 s | Gunstock calibration (stock angle) |
| Left menu button | Pause (hold 0.6 s: Back / objectives) |
| Left stick click in the pause menu or the game's menus | Open the in-headset settings menu again |
| Right stick held + direction | D-pad (secondary fire, grenade type) |
| Both sticks held for 1 s | Recenter (stand straight and look ahead) |
| Both sticks held for 3 s | Also set and save your standing height (`height` in the ini) |

When the game says **RB** it means the **right grip**. Chapter 1, sniper on the
billboard ("While holding RB target the enemy, then release"): hold the right
grip, point the gun **or just look** at the sniper (the red triangle follows
your view) until it is highlighted, then release. The target stays locked
until you release, even if the gun drifts off it.

On **Valve Index** controllers A and B on the right hand are A and B, on the
left hand X and Y; the left trackpad click pauses. Every action can be
rebound in SteamVR: *Settings > Controllers > Manage controller bindings >
Spec Ops: The Line*. Aiming is aligned to each controller's pointing
direction, so it feels the same on every controller.

| Gesture | Action |
|---|---|
| Overhand throw with the left hand | Grenade (flies where you throw) |
| Straight punch with the left hand (chest height) | Melee |
| Left hand on the magazine (under the gun), then pull down | Reload (`reload physical`: grip the magazine and pull it out, grip at your belt for a new one, push it into the gun; each step vibrates) |
| Duck quickly next to cover | Take cover |
| Lean out sideways (or stand up) while in cover | Pop out and aim |
| Gun up to your eye | Aim down the sights |
| Right hand over the right shoulder (or at the hip) + grip | Next weapon |
| Left hand on the barrel + grip (or rest it there) | The hand grips the gun; two-handed aiming (open the grip or move away to let go) |
| Turn your head past 45 degrees | Walker turns with you |

## Gunstock

Set *Controller hold* to *gunstock* in the in-headset menu (Basic). Aim the stock at
something far away, looking at it through the sights in the middle of your view,
and hold **B + Y** for one second. The angle between the stock and the controller
is measured, applied and saved (`stockangle` in `specopsvr.ini`); it works with the
controller tilted on the stock. Repeat if you move the controllers on the stock.
B + Y also saves where the left controller sits on the stock (`stockmount`), even when
the angle is not accepted (then the old stock angle stays): with it
there the game counts the gun as held in two hands (spread, vibration, Walker's left
hand on the gun), and the aim stays with the stock (the left hand never steers it).
Within `stocktol` 6 cm and `stocktoldeg` 25 degrees of that place counts as on the stock (until you save it, a hint reminds you once per session).

With bare controllers (*Controller hold* normal) the gun follows the controller's
pointing direction, the same as the menu laser. If it feels too low or too high,
change *Gun angle* (Basic, -15..+15 degrees, + = barrel up; `gunangle`). Do not
use B + Y for this: lining up the drawn gun, which already has the current gun
angle, measures that same angle again. `gunyaw` (Advanced) turns the gun outward
or inward if it points to the side. With *Controller hold* gunstock, *Gun angle*
shows and changes the stock angle. An ini from 0.9.x is reset to the defaults
when 0.10.0 first starts (see *Updates*): *Controller hold* normal, *Gun angle*
0. If you had calibrated `aimpitch` for a stock, set *Controller hold* to
gunstock and redo B + Y; the old value stays in the `specopsvr.ini.bak-<date>`
copy. (The old `aimpitch` conversion to gunstock or `gunangle` runs only if
that copy cannot be made.)

## Haptic devices (optional)

- **ProTubeVR ForceTube / ProVolver**: install the free **ProTubeVR
  Companion App** (Steam; it provides the up-to-date ProTube driver), pair the
  devices in Windows Bluetooth settings, close the Companion App and start the
  game. Recoil on every shot, scaled by weapon; with two ProVolvers the second
  one kicks in the support hand while aiming two-handed (`protube swap` in
  `specopsvr.ini` if they come out the wrong way round). The mod starts a
  small 64-bit helper, `specopsvr_protube.exe`, because the game is 32-bit;
  its log is `specopsvr_protube.log`.
- **bHaptics** vest and arm sleeves: start the bHaptics Player before the
  game. Recoil on the arms, hits taken on the vest, grenade / melee / reload
  on the left arm.

Both are off automatically when no device is found; `protube off` /
`bhaptics off` in `specopsvr.ini` disable them.

## Frame rate

The mod keeps the game at the headset refresh rate with dynamic resolution.
It changes the resolution only after the GPU has stayed over or well under
the frame time for a few seconds, and remembers the levels that were too
heavy, so it does not flip back and forth (each change costs a frame). A
change only makes the scene sharper or softer: the framing, the green marker
and the HUD stay the same in both eyes (before, each step zoomed the two eyes
differently, giving double vision and two markers). Its
thresholds are in Advanced (`dynresdown`, `dynresslow`, `dynresup`,
`dynresdwell`, `dynresbackoff`).
`submitpose auto` (default, Advanced) tells the headset the exact pose each
frame was drawn with whenever the world would shake without it, which happens
above all when the game runs below the headset's refresh rate; after 5 calm
seconds it goes back to plain frames. `on` / `off` force it. With an OpenXR
runtime (for example Virtual Desktop) the view is drawn `xroverscan` degrees
wider per side for it.
On SteamVR (Steam Link) `predictframe auto` adds one frame to the head
prediction only after the log's `POSEAGE` line has shown, twice in a row,
that the drawn pose is one frame behind SteamVR's own pose for that frame;
`predictframe on` / `off` force it. `predictmax` caps the prediction (150 ms).
The headset software can still lock the game at half the refresh rate (for
example 36 fps on a 72 Hz Quest) with its frame-synthesis feature, even when
there is plenty of headroom:

- **Quest over Link**: Meta ASW is turned off automatically while the game
  runs and set back to Auto when it closes (`linkasw keep` to leave it).
- **Index, Vive, Pimax, WMR**: SteamVR Motion Smoothing is forced off for
  this game only (`motionsmoothing keep` to leave it).
- **Virtual Desktop**: set *Synchronous Spacewarp* to *Disabled* in the
  Virtual Desktop menu in the headset (Streaming tab) if the game stays at half rate (the mod cannot
  change it; it writes a reminder in the log when it detects VD).
  For the lowest latency use the H.264+ or AV1 codec and the *High* quality
  preset (higher presets add encoding/decoding time). When the game starts,
  SteamVR may show its dashboard: close it with the menu button.

## Configuration

The **in-headset settings menu** opens by itself on the game's main menu
(point the controller laser and pull the trigger; PLAY closes it). To open it
again later, pause the game and click the left stick: *Basic*
has the main options with a short explanation, *Advanced* every option of the
ini (except `updatechannel`, which is set only in the file), *Controls* the
full control list. Changes apply at once and are saved.
In *Advanced*, - and + move numbers by a step that suits the option (angles by
1 degree, `height` by 1 cm), and options with named values (`predictframe`,
`predict`, `headcollision`, `crouchview`, `aim`, `movedir`, `turretaim`, `turretlimits`, ...)
cycle through them like the *Basic* choices.

`specopsvr.ini` in the game's `Binaries\Win32` folder; every option is
documented in the file (aiming, arms, cover, comfort, cinema screen, haptics,
performance, updates). The same commands can be sent while the game runs by
writing them to `specopsvr_cmd.txt` in the same folder. If something looks
wrong, most features can be switched off one by one (`ads off`,
`coverpeek off`, `coverlean off`, `turretaim stick`, `cinemawalker screen`, `gesture melee off`,
`vrinput legacy`).

## Non-Steam copies

Since 0.10.1 the mod no longer depends on the exact Steam executable. At start
it checks the game code it needs; with another executable (retail/other store
copy, other build) it **searches for that code by signature**. Whatever it
finds works as usual; whatever it cannot find is switched off instead of
hooking the wrong place, so the game does not crash because of it.

The headset tells you the result once the game starts:

- *Non-Steam copy of the game: all game addresses found*: everything should
  work. Feedback is still very welcome, it has only been tested on Steam.
- *Non-Steam copy: N game addresses not found, some features are off*: please
  help add support for your copy. Run `collect_logs.bat` and
  [open an issue](https://github.com/Mavic-Pro/SpecOpsTheLineVR/issues/new)
  with the zip. The `ADDR:` lines at the top of `specopsvr.log` say exactly what
  is missing. Do not send game files, the log is enough.

## Reporting bugs

1. Run `collect_logs.bat`: it creates `SpecOpsTheLineVR_report_<date>.zip` on
   your Desktop with the logs of the last four sessions, your configuration
   and any crash dump.
2. [Open an issue](https://github.com/Mavic-Pro/SpecOpsTheLineVR/issues/new),
   describe what happened (level, what you were doing, what you expected) and
   attach the zip.

Two things that can look like mod bugs:

- **Controllers stop responding for a few seconds** (no stick, buttons or
  hands): the headset runtime is reporting no controllers at all. Seen with
  Virtual Desktop; the log then shows `OPENXR: right hand profile none` and
  `INPUT: hand devices: left none, right none`. Likely causes: the Quest
  switching to hand tracking by itself (turn off its automatic switch between
  controllers and hands), low controller batteries, or the controllers out of
  the headset cameras' view.
- **Bodies vanish as soon as enemies die**, with their guns: in one fight the
  game's own limit on bodies was 1 instead of the usual 25, so the game removed
  them right away. The mod never changes that limit. If you see it, tell us
  which level and checkpoint.

## Building from source

Requirements: Visual Studio 2022 (C++ desktop workload, 32-bit toolset), CMake
3.16+.

```
cmake -S . -B build -A Win32
cmake --build build --config Release
python tools/package.py        # dist/0-SpecOpsTheLineVR.zip
python tools/regress.py        # release gate (runs the game, no headset needed)
```

Headless VR tests use a fake VR runtime (`runtime fake` in `specopsvr.ini`:
no headset, SteamVR or Virtual Desktop involved) that paces frames like a
headset at 72/90/120 Hz, plays scripted head and controller motion and logs
how far the drawn view is from the declared pose (`JITTER:` lines). It sits
behind the OpenVR interfaces, so the OpenXR backend (VDXR) is not exercised
by it: OpenXR-only behaviour still needs a headset check. No automated test
compares what each eye shows either, so a change to the image (crop,
resolution, markers, HUD) also needs a look in the headset. `fpscap <hz>`
(caps the game tick) and a file named `specopsvr_keepfocus` next to
`d3d9.dll` (the game window keeps the focus without a headset) are for
these tests only: do not leave them in a game you play.

How it works, the reverse-engineered engine addresses and the test tools are
described in [docs/TECHNICAL.md](docs/TECHNICAL.md).

## Credits and license

- [MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-Clause) and the
  [OpenVR SDK](https://github.com/ValveSoftware/openvr) (BSD 3-Clause) are
  included under `vendor/`.
- ProTubeVR and bHaptics integrations follow the public APIs of those
  devices; their software is not included.
- Spec Ops: The Line is © 2K Games / Yager. This project is not affiliated
  with them and ships no game files.
- Released under the MIT License (see [LICENSE](LICENSE)).
