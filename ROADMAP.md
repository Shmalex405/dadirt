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
Derived from soil mechanics (`docs/SoilPhysics.md`), not tuned: loose dry dirt
settles at its friction angle (32°), saturated mud at ~14°, and cohesion from
moisture and packing lets faces stand vertical up to the Culmann height (a damp
1 m sandcastle wall stands; a barely damp one keeps 60 cm; a soaked one runs).
Probes have to land within a couple of degrees / centimetres of the prediction,
and volume drift has to stay at 0.000 m³. *(Phases A–E measured 2026-09-21:
strength, solid volume, water, terramechanics and parcels, sections 9b–9f. The
audit counts solids, in the ground and in the air.)*

**G2 — A rut reads as a rut.** *(resolution: done. persistence: done 2026-09-21.)*
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

**Done —** deformation persists outside the focused region. The world is tiled at a
quarter of the window; the window slides over it by whole tiles, reading leaving
tiles back into a cache and bringing them back as they were. `DaDirt.Focus follow`
keeps the window on the wheel as it drives; the rest of the box is drawn as a far
mesh updated from the cache. The audit counts the cached tiles, so a lap of ruts
still books to zero (docs/SoilPhysics.md 9h). Not yet: changing the cell size
(`DaDirt.Focus off`, or a different size) starts a fresh world, because the cache
is at one cell size.

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
- [x] Physics objects read the deformed ground and rest in dents: `DaDirt.Ball`
      drops a ball that dents where it lands, bounces, rolls downhill leaving a
      groove, and sleeps in its own crater. It reads the ground through a
      non-stalling GPU height window, the same path the wheel will use
      (verified 2026-09-21: 5 m drop hits at 9.9 m/s, rests 15 cm into the pad)

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
- [x] Riding over loose dirt packs it down: four passes take a line from 0.25 to
      0.91 compaction, at the Proctor rate (damp packs fastest, mud not at all)
- [x] **Solid volume (2026-09-21):** the layer stores solids; packing drops the
      surface 2.8 cm and loosening lifts it 0.9 cm with zero drift. A rut from
      compaction alone is real now. `docs/SoilPhysics.md` 9c
- [x] **Water (2026-09-21):** run-off, soaking at the soil's conductivity,
      drainage to field capacity, drying to ambient. 400 L on loose dirt is gone
      in 20 s; the same on hardpack stands 9 cm deep in a hole. `DaDirt.Rain`,
      `DaDirt.Water`. 9d
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

### 1c — Loose dirt particle layer ★ first-class, not decoration *(first version running, 2026-09-21)*
Alex (2026-09-21): individual particles "in their most intense but clean sense"
are half of what the dirt simulator *is*. Push this as hard as the hardware allows.
Built as **parcels**, our own GPU compute passes rather than Niagara (materials
cannot read structured buffers and Niagara cannot be authored from text):
- [x] Parcels spawn from roost and from `DaDirt.Throw`; each carries a real solid
      volume and its moisture, scooped out of the heightfield
- [x] They fly with quadratic drag, hit the heightfield, bounce (dry) or splat
      (mud), roll with Coulomb friction at the ground's own friction angle so
      they keep going down faces steeper than repose, come to rest and deposit
- [x] Both hand-offs are exact: 4 L thrown = 0.0040 m³ in the air, total
      unchanged; 143,000 parcels through a burnout, drift −0.00001 m³
- [x] Budget measured on the Arc: the parcel system costs ~0.5 ms at 74,000 live
      (4 mm clods, a full roost at 40–46 fps); the frame is the heightfield's.
      Size is one knob: `DaDirt.Parcel <cm>` or `soil` (clods sized by cohesion).
      `docs/SoilPhysics.md` 9f
- [x] Grains shedding down an over-steep face (the slump pass sheds parcels that
      roll off the fall line), spray off the tyre's flank in a slide, and a
      splash on landing. All audited: 27,000 spray parcels at 0.00000 m³
- [x] Dust: a second pool of soft camera-facing motes puffed with dry roost,
      spray, splash and throws; drifts, fades, carries no audited volume
- [x] The slump on a shared tile: sim halved, testbed idles at 75–86 fps, a
      74,000-parcel roost at 54 fps. `docs/SoilPhysics.md` 9g
- [ ] Parcels hitting the wheel and the (future) rider
- [ ] Dust that rides the air behind a moving wheel (today it hangs where it
      was puffed; there is no air)

### 1d — Sandbox tools & feel
- [x] Debug views: dirt, layer depth, compaction, moisture, stability vs repose,
      bedrock exposure, slope angle — plus the volume audit
- [x] Console-driven tools (dig, raise, smooth, wet, pack, loosen). Deliberately
      console-first: it needs no input assets and makes every action repeatable
- [x] Focus the simulation on part of the box (`DaDirt.Focus`) for rut-scale work
- [x] Ruts persist outside the window: tiled world, sliding window, cache,
      far ground, `DaDirt.Focus follow` (2026-09-21)
- [x] Standing water and the tyre: saturated mud under a puddle, water drag,
      hydroplaning past knob height (docs/SoilPhysics.md 5 and 9k, 2026-09-22)
- [x] Soil compounds: a per-cell soil map and a table of measured properties,
      five reference soils including PNW loam and Southwest decomposed granite,
      the pad's soil quilt, `DaDirt.Soil` (docs/SoilPhysics.md 8 and 9l, 2026-09-22)
- [x] Erosion: run-off carries dirt by shear, capacity and settling, per soil,
      audited as "in the run-off" (docs/SoilPhysics.md 5b and 9m, 2026-09-22)
- [x] A pile is not a ramp: the tyre climbs or shoves what stands in front of
      it, whichever costs less (bulldozing, docs/SoilPhysics.md 6 and 9i)
- [x] The tyre has its own contact patch (deflection), the rut is pressed at
      the front of the patch, and the wheel is tested on every testbed shape
      (`Tools/DirtboxHills.txt`, docs/SoilPhysics.md 9j)
- [ ] Suspension: the rigid wheel hops off whoops and berm banks a sprung one
      would absorb. Part of the bike build: see `docs/BikeEngineering.md`, the
      Phase 2 research and plan (tyre, wheel, brakes, fork and shock internals,
      chassis, drivetrain, rider), component by component.
- [ ] Mouse tools: click and drag to sculpt
- [ ] Droppable objects (ball, plate, block)
- [ ] Camera + controls that make poking at dirt satisfying

### 1e — The test wheel *(first version running, 2026-09-21)*
- [x] A driveable powered wheel (throttle / brake / steer) — the ancestor of the
      bike. `DaDirt.Wheel`, `DaDirt.Drive`, `DaDirt.Anchor`, `DaDirt.Follow`. No
      physics engine: it integrates against the dirt through a height window,
      with a 12 cm-wide three-point contact so ruts hold it and berm walls push
      it back
- [x] **Terramechanics (2026-09-21):** Bekker sinkage from load and the soil's
      (n, k_c, k_φ) — 3 cm on the loose pad, 1 mm on hardpack, 9 cm in mud;
      Janosi–Hanamoto traction building with slip to the Mohr–Coulomb ceiling;
      motion resistance from pressing the rut; a brake that locks instead of
      oscillating. 49 km/h in 36 m from rest. `docs/SoilPhysics.md` 9e
- [x] Roost as parcels: the lugs shear off width × failure depth × slip speed
      of soil and fling it at 0.85 of the slip speed, 35° up; a locked brake
      shoves it forwards. Anchored burnout: 5.3 L thrown in 3 s, 15 cm hole
- [x] Rutting and packing: four passes at 3.9 cm cells gave a 4.3 cm rut with
      0.5 cm shoulders, floor packed 0.25 → 0.91, sinkage per pass 2.2 → 0.7 cm:
      the rut saturates because the packed floor barely sinks, no rule needed
- [ ] Berm carving: repeated cornering builds up a banked wall — the turn works,
      the berm has not been measured yet
- [ ] Keyboard control for Alex (WASD), and a tyre mesh that is not a cylinder

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
