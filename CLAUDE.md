# DaDirt — project context for Claude

Motocross game built dirt-first in Unreal Engine 5.8. See ROADMAP.md for phases;
Phase 1 (sandbox dirt simulator, "the Dirtbox") is the current focus.

## Working with Alex

Alex is new to both Unreal and programming. Claude does all engineering. Explain
what things are and why choices were made in plain language — short and clear,
not condescending. Never assume Alex can debug, edit code, or navigate the editor
without directions; when Alex must do something manually (installers, Epic
sign-in, editor clicks), give exact step-by-step instructions.

## What this project is

**A dirt simulator that happens to be a dirt bike game.** Not a motocross game with
dirt in it. Every decision resolves that way round: the dirt is the system, and the
track, the bike and the racing are things that sit on top of it.

The practical consequence, and it is not a small one: **terrain is never drawn, it is
built by moving dirt.** A jump exists because dirt was piled there. A berm exists
because dirt was pushed to the outside of a corner. A hole exists because that is
where the dirt for the jumps came from. Analytic shapes stamped into a heightfield
are the thing we are specifically not doing — if a feature appears without dirt
having been moved to make it, that is a bug, however good it looks.

Tracks are modelled the way they are felt in the real world, which means the way they
are made in the real world: graded, cut, filled, piled, packed, watered.

## Locked technical decisions

- **Engine:** UE 5.8 (binary install via Epic Launcher). Fallback to 5.7 only if
  5.8 proves unstable.
- **Toolchain:** Visual Studio 2022 Community, "Game development with C++" +
  Desktop C++ workloads.
- **Dirt architecture:** hybrid — GPU deformable heightfield (render-target
  ping-pong sim: deformation, volume conservation, angle-of-repose slumping,
  compaction + moisture channels) + Niagara GPU particles for loose/airborne
  dirt that settle back into the heightfield. Rejected: full granular/MPM sim
  (can't scale past ~1–2 m² on this GPU, dead end for a track-scale game) and
  pure heightfield (reads as clay, no life).
- **Two-layer ground:** bedrock (static, sculpted, never simulated — the hills
  and jump faces) + a ~60 cm movable dirt layer on top (simulated). Surface =
  bedrock + layer. Digging stops at bedrock. Volume conservation applies only to
  the layer. Slumping reads *total* surface slope but moves dirt only within the
  layer, so loose dirt sheds off a steep face while packed dirt holds it.
- **Terrain is built, not stamped.** The site exists first as natural ground. The
  track's grade is the natural ground smoothed along the centre line and clamped to
  a rideable gradient — so it follows the land. The corridor is then cut and filled
  to that grade, jumps and berms are built from the spoil, and borrow pits are dug
  until the earthmoving ledger balances. Every cubic metre is accounted for, and the
  ledger is logged. Berms are generated from centre-line curvature, never placed.
- **Box size:** 128 m × 128 m, sim grid 1024² → 12.5 cm cells. Cell size is the
  standing tradeoff: bigger box means coarser ruts. `SimResolution` and
  `WorldSizeCm` are both exposed; change them by measurement, not by guess.
- **Sandbox interaction order:** hand tools (dig/push/pile/smooth/carve) first,
  then a driveable powered test wheel (slip digging, roost, compaction).
- **Console-first tools:** the sandbox is driven by `DaDirt.*` console commands,
  not the mouse, until Phase 1d. Reason: no input assets to author, and every
  action becomes a repeatable scripted test. Positions are in metres from the
  box centre.
- **Fixed sim timestep** (default 60 Hz, `SimHz`). Non-negotiable: without it the
  scripted tests give different answers at different frame rates and tuning dirt
  becomes guesswork.

## Hardware budget (dev machine = target machine for now)

Intel Arc 140T integrated GPU, 32 GB RAM, Windows 11. Target 60 fps @ 1080p.
Consequences: keep heightfield sim ≤ 1024×1024, prefer a pre-tessellated grid
mesh with WPO displacement over Nanite tessellation until proven cheap, keep
Lumen/heavy features off in the sandbox map, budget Niagara particle counts.

## Conventions

- Project root: `C:\Users\alex\Desktop\DaDirt` (git repo). The UE project lives
  at the repo root: `DaDirt.uproject`, `Source/`, `Config/`, `Content/`.
- C++ module naming: `DaDirt` game module; dirt sim code under
  `Source/DaDirt/Dirt/`. A second tiny module, `DaDirtShaders`, exists only to
  map `Shaders/` to the virtual path `/DaDirt` at `PostConfigInit` — global
  shaders in a game module are not registered early enough otherwise.
- Compute shaders live in `Shaders/Private/`. Shader parameters bind **by name**,
  so every `SHADER_PARAMETER` entry in `DirtSimulation.cpp` needs a matching
  global declaration in the `.usf`/`.ush`, and vice versa.
- The brush kernel shapes are written twice — `DirtBrushCoreWeight` /
  `DirtBrushRimWeight` in both `DirtSimTypes.h` and `DirtCommon.ush`. They must
  stay identical: the CPU sums them to normalise the kernel, and that is what
  makes dig zero-sum. If they drift apart, the volume audit starts reporting drift.
- Alex works on a Mac; the project only builds on the Windows machine. Code
  written on the Mac has never seen a compiler, so hand it over with that said
  plainly and expect a first-compile fixing pass.
- Keep every phase-1 feature toggleable with debug visualization (height,
  compaction, moisture, volume audit) — dirt tuning is the whole game.
  `DaDirt.DebugView 0-6`; view 4 (stability vs angle of repose) is the one that
  says whether the dirt physics is behaving.
- The testbed terrain is a measuring instrument, not scenery: it exists to
  provoke every dirt behaviour we care about (a full spectrum of slope angles,
  a range of jump-lip sharpnesses, concave/convex pairs, a flat calibration pad).
  Add to it rather than making the box prettier.
- Volume conservation is a hard invariant: any system that moves dirt must
  account for where it goes. The debug audit view must stay ~zero-sum.
