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

**Individual particles are half the point.** (Alex, 2026-09-21.) The heightfield
gives dirt its structure; discrete particles give it life: roost, thrown dirt,
grains rolling down a face, spray off a berm. Treat the particle layer (Phase 1c)
as a first-class system to push as hard as the hardware allows, "intense but
clean": particles collide with the heightfield, roll, settle and hand their volume
back to it, nothing pops or vanishes, and the hand-off in both directions is
volume-conserving. It is not a decorative effect bolted on at the end.

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
- **Dirt strength is soil mechanics, not tuned angles.** Mohr–Coulomb per cell:
  friction angle from compaction (32° loose → 42° dense) collapsing with
  saturation, cohesion in kPa from moisture suction (a hump, zero dry and zero
  soaked) plus packing, and the Culmann standing height deciding whether a face
  holds. Derive new behaviour from `docs/SoilPhysics.md`; do not add magic
  angles. The shader and C++ strength functions are mirrored and must stay
  identical.
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
  The same goes for the soil strength and the solid-volume functions
  (`DirtSoilStrength`, `DirtAllowedDrop`, `DirtSolidFraction`, `DirtBulkCm`,
  `DirtSolidCm`, `DirtPackingEfficiency`): mirrored in both files, keep identical.
- **Soils are a table, cells carry an id.** `FDirtSoil` (DirtSimTypes.h, presets
  in DirtSoils.cpp) holds every measured soil property; `FDirtSimSettings::Soils`
  is the table and each cell's `SoilId` (a static R8_UINT map built with the
  terrain, like bedrock) picks its row. The shaders read `DirtSoilTable` /
  `DirtSoilIn` through `DirtSoilAt(P)`; the five float4 rows are laid out by
  `FDirtSoil::ToRows` and documented in DirtCommon.ush, keep them identical.
  Nothing reads a soil number from Settings any more: the C++ functions take a
  `const FDirtSoil&` (the cell's, `SoilAtTexel` / `SoilAtWorld`), and a new
  property goes into the struct, the rows and the docs table, never a global.
  Wet, mud and dust are moisture states of a soil, not soils.
- **The layer channel stores SOLID centimetres**, not bulk thickness. Bulk height
  comes from porosity via `DirtBulkCm` (a packed skin over natural ground), so
  packing lowers the surface and loosening raises it without moving any grains.
  Every mass move (brush, slump, scoop, dump, parcel deposit) transfers solid cm,
  which is why it is exactly zero-sum. Amounts handed to Dig/Raise are bulk cm of
  natural ground; `MakeStroke` converts. The audit reports solid m³ and bulk m³.
- **Parcels** (dirt in the air) live in `Shaders/Private/DirtParcels.usf` and
  three RGBA32F textures; the mesh that draws them is one tetrahedron per slot,
  moved in the vertex shader by `M_DirtParcel` (built by `Tools/BuildDirtAssets.py`).
  The free list is a stack that starts with slot 0 on top, so live parcels always
  sit in the lowest slots and only the mesh sections up to the highest live slot
  are drawn. Never break that: an idle pool of a quarter million would cost 7 ms.
  A parcel's volume was scooped from the heightfield and is deposited back by
  atomics into fixed-point textures; the audit counts ground + air. Dust is a
  second pool with the same shaders and NO audited volume: the one deliberate effect.
- **The wheel decides climb-or-shove, never rigid contact.** Dirt standing above
  the ground around it at the wheel's scale is a heap; the tyre sinks through
  the share the passive wedge cannot hold and shoves that share ahead
  (`TransferDirt`), with the wedge's resistance as a force. Do not fix a launch
  or a stall by hand-tuning the contact: change the soil numbers that feed
  `R_b` and `F_climb` (docs/SoilPhysics.md 6), and check `Tools/DirtboxPlough.txt`.
- **Every mass-moving stroke is two halves.** A taking half (dig core, raise rim,
  scoop) runs first and reports per stroke what the cell did not have (bedrock)
  into `DirtScoopShortfall`; the giving half (dig rim, raise core, dump, or the
  parcels of a scoop) is dispatched afterwards and gives only what was taken.
  `ApplyBrush` emits both; `PendingGiving` holds the second. This is what keeps
  a dig zero-sum over a hole. Do not add a stroke mode that adds dirt without a
  link to what removed it.
- The slump runs on a 16 x 16 group-shared tile with a two-cell halo
  (`SLUMP_TILE`); the shed-grain spawn lives inside it. Any change to the slump
  must be checked against `Tools/DirtboxSoil.txt` (60 cm face, damp wall stands,
  cone at 32°) before it replaces the old numbers. Flows under
  `DIRT_SLUMP_MIN_FLOW_CM` are dropped on purpose: float rounding below that
  creates dirt (docs/SoilPhysics.md 9h). Print drift in cm³ when hunting a leak.
- **The window is a view onto a tiled world.** A focused region is 4 x 4 tiles
  (`FDirtTile`, `TileCache` in DirtBox); it slides by whole tiles (`ShiftWindow`
  → `MainShiftCS`), reads leaving tiles back into the cache, flushes parcels
  over leaving ground first (`MainParcelFlushCS`), and the audit counts
  ground + air + cached tiles against a baseline that grows with every tile
  first generated. Anything that addresses the grid by texel (strokes, water
  sources, height windows) must be offset or invalidated on a slide; anything
  in box centimetres (parcels, the wheel) needs nothing. The far mesh draws the
  rest of the box from the cache. `Tools/DirtboxPersist.txt` is the test.
- Alex works on a Mac; the project only builds on the Windows machine. Code
  written on the Mac has never seen a compiler, so hand it over with that said
  plainly and expect a first-compile fixing pass.
- Keep every phase-1 feature toggleable with debug visualization (height,
  compaction, moisture, volume audit) — dirt tuning is the whole game.
  `DaDirt.DebugView 0-6`; view 4 (stability vs angle of repose) is the one that
  says whether the dirt physics is behaving. `DaDirt.Parcels`, `DaDirt.WaterSim`
  and `DaDirt.Slump 0` switch whole systems off for attribution
  (`Tools/DirtboxPerf.txt` does this and prints GPU ms per system).
- The testbed terrain is a measuring instrument, not scenery: it exists to
  provoke every dirt behaviour we care about (a full spectrum of slope angles,
  a range of jump-lip sharpnesses, concave/convex pairs, a flat calibration pad).
  Add to it rather than making the box prettier.
- Volume conservation is a hard invariant: any system that moves dirt must
  account for where it goes. The debug audit view must stay ~zero-sum — with
  dirt in the air included. Water is audited separately (pore water and pond)
  and is allowed to leave (drain, dry) because it is not dirt.
- Scripted tests are the unit tests: one `Tools/Dirtbox*.txt` per system
  (Solid, Water, Terra, Parcels, Soil, Wheel, Sandcastle, Perf, Leak, Persist,
  Plough, Hills, WaterGrip, Soils). Anything that touches the wheel or soil strength runs
  `DirtboxHills.txt` too: the pad only exercises ruts, and every contact bug so
  far showed up on the dome, the jump faces or the berm. `DaDirt.Wheel` makes a
  new wheel with every part on, so `DaDirt.WheelParts` must follow it. Run them with
  the `-DirtScript=` launch and read the numbers out of the log; a change that
  moves a measured number is not done until the doc that quotes it is updated.
