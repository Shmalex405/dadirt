# DaDirt — Motocross Game Roadmap

**Vision:** A motocross game where the dirt is the star. Before bikes, tracks, or
racing, we build dirt that has real structure and life — dirt you'd believe if you
were playing in an actual sandbox. Every later phase stands on top of that.

**Engine:** Unreal Engine 5.8 · **Language:** C++ (with Niagara GPU particles)
**Target:** 60 fps at 1080p on the dev machine (Intel Arc 140T iGPU, 32 GB RAM)

---

## Goals

Measurable targets, so "does the dirt feel right" has an answer that isn't a shrug.
The numbers come from `docs/MXTrackReference.md`.

**G1 — Dirt behaves like dirt, measurably.**
Loose dry dirt settles at 32°, damp at ~44°, saturated mud collapses to ~15°, packed
hardpack stands past 65°. `DaDirt.Audit` reports the measured angle; it has to land
within a couple of degrees of the setting, and volume drift has to stay at 0.000 m³.

**G2 — A rut reads as a rut.** *(resolution: done. persistence: not yet.)*
A rear tyre is 110–120 mm wide, so a rut is ~12 cm across at the bottom and 25–35 cm
with its spoil edges. Four to eight cells across it means **cells of 3–6 cm**. One
fixed grid over a whole track cannot do that: 1024² over 384 m is 37.5 cm.

**Done —** the simulated region is now decoupled from the box. The same 1024 cells
can be pointed at part of the world, and the terrain under them is *regenerated at
full resolution* for wherever they are pointed, so nothing is upsampled or smeared:

| Region | Cell | A 12 cm rut is |
|---|---|---|
| whole 384 m track | 37.5 cm | ⅓ of a cell |
| 51 m | 5.0 cm | 2.4 cells |
| 31 m | 3.0 cm | 4.0 cells |

`DaDirt.Focus <x> <y> [sizeM]` drops the simulator onto any corner of the real track
at rut resolution. `DaDirt.Focus off` goes back to the whole site.

**Not done —** deformation does not persist outside the focused region. Dig a rut,
focus elsewhere, come back, and it is gone. Making it stick needs a world-resolution
deformation layer that the region writes back to and reads in from when it moves.
That matters when something is *travelling* through the world, so it is the piece to
build alongside the test wheel — not before it.

**G3 — The track is a real track.**
1,520 m lap, 8 m wide, tightest corner 17 m radius, ~22 m of elevation, no jumps on
the start straight. All inside FIM Appendix 047. *(Done — `DaDirt.Mode track`.)*

**G4 — It runs on the Arc.**
60 fps at 1080p. Nothing has been measured on real hardware yet; every performance
claim in this repo is a guess until it has.

**G5 — The bike arrives last, and inherits everything.**
No suspension, no engine, no rider until a powered wheel can already trench, roost,
pack a line and build a berm out of the dirt system alone.

---

## Phase 0 — Toolchain *(DONE — July 19, 2026)*

Get the machine ready to build.

- [x] Epic Games Launcher installed
- [x] Visual Studio 2022 Community + "Game development with C++" workload
- [x] Unreal Engine 5.8.0 installed
- [x] DaDirt C++ project created, compiles (98 s), opens in editor (78 s init)

## Phase 1 — The Dirtbox (sandbox dirt simulator) ★ CURRENT FOCUS

A **128 m × 128 m** dirt box where the only goal is dirt that feels real. We don't
leave this phase until playing in it feels like a real sandbox. The box is big
enough to hold dramatic hills, a lineup of jump faces and a full spectrum of
slope angles at once, so every dirt behaviour has something in the box that
provokes it.

**Architecture decision (locked):** hybrid heightfield + particles.
The ground is a GPU-simulated deformable heightmap — ruts, berms, and piles that
persist and conserve volume (dug dirt has to go somewhere). Loose dirt is a layer
of GPU particles (Niagara) that kick up, scatter, and settle back into the surface.
This is the approach real MX titles use and it scales from a sandbox to a full track.

**Two-layer ground (locked):** the ground is bedrock plus a dirt layer.
*Bedrock* is a static sculpted surface — the hills and the jump faces — and is
never simulated, so big dramatic shapes cost nothing. The *dirt layer* is ~60 cm of
movable material sitting on top, and that is what the GPU simulates. Digging eats
into the layer and stops at bedrock. Volume conservation only has to hold over the
layer, which is what makes the audit meaningful instead of noisy. Slumping uses the
*total* surface slope but moves dirt only within the layer, so loose dirt slides off
a steep face while packed dirt holds it.

### 1a — Deformable ground core *(code written, not yet verified on hardware)*
- [x] Heightfield simulated on the GPU (compute shaders, ping-ponged render targets)
- [x] Rendered as a pre-tessellated grid mesh displaced by world position offset
- [x] Dig / raise / smooth deformation brushes working end to end
- [x] Volume conservation: dig is zero-sum by construction — the brush kernel's
      core and rim are normalised against discrete sums over the exact cells the
      shader touches, so the spoil equals the hole
- [x] Volume-conservation audit (`DaDirt.Audit`) reporting drift against a baseline
- [ ] Physics objects (a dropped ball) read the deformed ground and rest in dents
      — the height query exists (`DaDirt.Probe`), the collision hookup does not

### 1b — Granular behavior (what makes it feel like *dirt*, not clay)
- [x] **Angle of repose:** mass-conserving slump relaxation. Slopes steeper than
      the dirt can hold avalanche; the gather formulation means every centimetre
      that leaves one cell arrives in another
- [x] **Compaction states:** loose vs packed, and packed dirt stands far steeper
      (~32° loose, ~70° packed). Avalanching dirt arrives loose
- [x] **Moisture parameter:** changes the angle dirt holds and its colour. Cohesion
      peaks at half-saturation, so damp loam holds better than dry sand *or* soup
- [x] **Verified on hardware (2026-09-21, `docs/DirtboxFirstRun.md`):** a loose
      cone settles to 32° in every direction within 5 s and to the exact height
      volume conservation predicts; damp holds 41°, mud runs to 15°, packed does
      not move; volume drift < 0.0001 m³. Slumping is now 8-neighbour — the
      4-neighbour version let diagonals stand at 41° and made square pyramids
- [ ] Riding over loose dirt packs it down — needs the test wheel (1e)
- [ ] Tuning pass on feel (rates, moisture curve) now that the numbers are trusted

### 1b-test — The testbed *(new, code written)*
The box is a measuring instrument, not a landscape. Built procedurally at startup:
- **Angle spectrum:** twelve packed wedges from 10° to 80°, bracketing both the
  loose and packed repose angles, so some faces must hold and some must shed
- **Jump lineup:** four takeoff/landing pairs with deliberately different lip
  character — rounded tabletop, crisp 32° face, sharp 45° kicker, near-vertical step-up
- **Dramatic hills:** an 11 m dome, a 7.5 m dome and a 9 m ridge
- **Calibration pad:** dead flat, known area, for volume audits and repose
  measurement; plus a cone far steeper than dry dirt can stand, which must
  collapse on the first second, and a known 100 m³ block
- **Berm arc and a concave/convex pair,** to check slumping respects curvature
- **Scripted tests** (`DaDirt.Test repose|anglefan|conserve|trench`) that reset
  first and run on a fixed timestep, so the same test twice gives the same numbers

### 1b-track — Real-scale track, *built* rather than drawn *(new, code written)*
A FIM-legal outdoor circuit at true scale, produced the way a real one is: by moving
dirt. `DaDirt.Mode track`.

**The construction sequence**, which is also the code's structure:
1. **The site exists first** — natural rolling ground, a function of position only,
   with no idea a track is coming.
2. **The grade follows the land.** The design profile is the natural ground sampled
   along the centre line, smoothed until rideable and clamped to 22% — not an
   invented elevation curve.
3. **Grade the corridor.** Cut where the ground is too high, fill where it is too
   low. Because a centred moving average preserves the mean, this comes out close to
   balanced — which is exactly what a grading contractor designs for.
4. **Build the jumps and berms from the spoil.** Every cubic metre placed is drawn
   from a ledger.
5. **Dig borrow pits** until the ledger closes. The holes in the outfield corners are
   where the jump dirt came from. Measured at grid resolution: **27,382 m³ cut,
   22,467 filled, 6,434 built, 1,518 recovered from three 12 m pits, 0 m³ imported.**

Verified before it ever compiled, by replicating the whole build in Python at grid
resolution. That caught two bugs of the same kind, both worth naming because the
pattern will recur: a `SmoothStep` that ramps up and then *stays* there, laying dirt
forever instead of falling back to ground.
- The neutral-zone banking terraced 0.5 m across 20 m of verge for the whole lap —
  **30,000 m³ of dirt nobody moved.**
- The berm crest carried outward to the fine radius instead of ending — another
  **4,300 m³.**

In a simulator whose hard invariant is that dirt comes from somewhere, both were the
worst class of bug available: invisible, plausible-looking, and a lie.
- Centre line: closed Catmull-Rom serpentine, **1,520 m** lap (FIM 047.3.1 wants
  1.5–1.75 km), four passes across the site linked by U-turns plus an outer return
- **8 m** riding width (FIM 047.3.2 recommends 8), tightest corner 17 m radius,
  no two passes closer than 36 m
- Start straight carries no jumps, per FIM 047.5.2
- 15 obstacles: tabletops, doubles, a triple with 21 m of gap, step-up, step-down,
  FIM rolling waves, a deep sand section
- **Berms are generated from the centre line's curvature**, not placed by hand —
  they appear on the outside of every corner, sized by how tight it is, which is
  where and why they form on a real track
- ~20 m of elevation change on the lap, inherited from the site rather than invented;
  the finished ground spans about 29 m across the whole 384 m site
- Known limit: 384 m site at 1024 cells is **37.5 cm per cell**, so this mode is for
  layout and scale. Dirt behaviour gets tuned in testbed mode at 12.5 cm. See G2.

### 1c — Loose dirt particle layer
- Niagara GPU particles spawn when dirt is moved aggressively
- Particles collide with the heightfield, roll/bounce, then settle and
  write their volume back into the ground (nothing vanishes)

### 1d — Sandbox tools & feel
- [x] Debug views: dirt, layer depth, compaction, moisture, stability vs repose,
      bedrock exposure, slope angle — plus the volume audit
- [x] Console-driven tools (dig, raise, smooth, wet, pack, loosen). Deliberately
      console-first: it needs no input assets and makes every action repeatable
- [x] Focus the simulation on part of the box (`DaDirt.Focus`) for rut-scale work
- [ ] Mouse tools: click and drag to sculpt
- [ ] Droppable objects (ball, plate, block)
- [ ] Camera + controls that make poking at dirt satisfying

### 1e — The test wheel
- A driveable powered wheel (throttle / brake / steer) — the ancestor of the bike
- Slip-based digging: a spinning tire trenches in and roosts dirt behind it
- Rolling compaction: driving packs a line into loose dirt
- Berm carving: repeated cornering builds up a banked wall

**Phase 1 exit bar:** dig a hole and the dirt piles beside it; piles slump at a
believable angle; the wheel roosts, ruts in, and packs a racing line; nothing in
the box gains or loses dirt out of nowhere.

## Phase 2 — Bike & rider *(later)*

Two-wheeled physics on top of the proven dirt: suspension, tire model reusing the
Phase 1 wheel work, rider lean, whips/scrubs eventually.

## Phase 3 — Track & riding loop *(later)*

Scale the dirt system from sandbox patch to a full track: jumps, rhythm sections,
ruts that develop over laps, terrain streaming/budgets.

## Phase 4 — Game wrapper *(later)*

Modes, AI riders or multiplayer, progression, audio, UI. Deliberately undefined
until the riding feels right.
