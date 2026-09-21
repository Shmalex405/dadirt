# Windows handoff — build it, look at it, tell me what broke

> **Status (2026-09-21): done — see `docs/DirtboxFirstRun.md` for what broke and
> what was measured.** Sections 2 and 3 below no longer need doing by hand:
> `Tools/BuildDirtAssets.py` builds the material and the level from the editor's
> command line, and `Tools/DirtboxChecklist.txt` runs section 4 unattended via the
> game mode's `-DirtScript=` runner. The manual steps are kept for reference.

Everything in this project is code except one thing: the **ground material**. A
material is a binary asset that has to be built in the editor's node graph, so it
cannot be written as a text file. It is about ten nodes and takes five minutes.

Work through the sections in order.

---

## 1. Build

1. Pull the latest code onto the Windows machine.
2. Delete `Binaries`, `Intermediate`, `.vs` and `DaDirt.sln` if they exist. There is
   a new module (`DaDirtShaders`), so the old project files are stale.
3. Right-click `DaDirt.uproject` → **Generate Visual Studio project files**.
4. Open `DaDirt.sln`, set configuration **Development Editor** / platform **Win64**,
   and Build (Ctrl+Shift+B).

**If it does not compile, send me the first error and stop.** Do not try to fix it.
None of this code has ever been through a compiler — I wrote it on a Mac with no
Unreal install. The logic and the maths I have verified by other means; what I cannot
verify is the exact spelling of Unreal's APIs in 5.8.

### The five lines most likely to break, and why

If the error is at one of these, tell me the line number and I will fix it in one
edit. They are all "does this API still look like this in 5.8" questions, not design
problems.

| File | Line | What it does | If it fails |
|---|---|---|---|
| `Dirt/DirtBox.cpp` | 105 | `RT->bCanCreateUAV = true` — lets compute shaders write render targets | property renamed or moved |
| `Dirt/DirtSimulation.cpp` | 187–190 | pulling `FRHITexture*` out of texture resources | accessor renamed |
| `Dirt/DirtSimulation.cpp` | 204 | `CreateRenderTarget(...)` into the render graph | signature changed |
| `Dirt/DirtBox.cpp` | 196, 217 | `GetPlatformData()->Mips[0]` to upload pixel data | accessor renamed |
| `Dirt/DirtBox.cpp` | 609 | `ReadLinearColorPixels` for the GPU readback | signature changed |

What I *have* verified statically:

- All four shader entry points match their C++ declarations
- All 42 shader parameters match between C++ and HLSL by name (they bind by name, so
  a typo there is a silent black screen rather than a compile error)
- Every declared function has a definition in both terrain generators

---

## 2. Make the ground material

Launch `DaDirt.uproject`.

### Create the asset

1. In the **Content Browser**, right-click empty space → **New Folder**, name it
   exactly `Dirt`.
2. Double-click into `Dirt`. Right-click → **Material**.
3. Name it exactly `M_DirtGround`. The full path must be `/Game/Dirt/M_DirtGround` —
   the code looks for it there by name.
4. Double-click to open the Material Editor.

### Material options first

In the **Details** panel on the left (click empty graph space if you cannot see it):

- Find **Tangent Space Normal** and **untick** it. Our normal texture is world space,
  not tangent space. Leave this ticked and the lighting will be wrong.
- Leave Blend Mode `Opaque` and Shading Model `Default Lit`.

### Node 1 — the height, which moves the mesh

1. Right-click → search `TextureSampleParameter2D` → add it.
2. Select it, and in Details set:
   - **Parameter Name**: `DirtDisplay`
   - **Sampler Type**: `Linear Color`
   - **MipValueMode**: `MipLevel`
3. Right-click → `Constant` → set its Value to `0`. Drag its output into the
   `Level` input of the DirtDisplay node.

   *Moving vertices happens in the vertex shader, and the vertex shader has to be
   told which mip to read. Without this the material will not compile.*

4. Right-click → `Constant2Vector` → leave both values `0`.
5. Right-click → `AppendVector`.
   - Constant2Vector output → `A`
   - DirtDisplay's **A** output pin (the fourth, alpha) → `B`
6. AppendVector output → **World Position Offset** on the result node.

   *That builds `(0, 0, height)` — no sideways movement, only up.*

### Node 2 — the colour

1. Add another `TextureSampleParameter2D`:
   - **Parameter Name**: `DirtDebug`
   - **Sampler Type**: `Linear Color`
2. Its top **RGB** output → **Base Color**.

### Node 3 — the surface normal

1. Add another `TextureSampleParameter2D`:
   - **Parameter Name**: `DirtNormal`
   - **Sampler Type**: `Linear Color`
2. Add a **Multiply**. DirtNormal **RGB** → `A`. Set **Const B** to `2.0`.
3. Add a **Subtract**. Multiply output → `A`. Set **Const B** to `1.0`.
4. Subtract output → **Normal**.

   *The texture stores directions squashed into 0–1; this stretches them back to
   −1…1.*

### Node 4 — stop it looking like plastic

1. **Constant** `0.9` → **Roughness**
2. **Constant** `0.05` → **Specular**

Click **Apply**, then **Save**. Close it.

---

## 3. Make a level and press play

1. **File → New Level → Basic**. (Basic already has sun, sky and a PlayerStart.)
2. In the **Outliner**, select `Floor` and delete it — the Dirtbox is the ground now.
3. **File → Save Current Level As** → new `Maps` folder, name it `L_Dirtbox`.
4. Press **Play** (Alt+P).

Nothing needs placing. The game mode spawns a Dirtbox at the origin, finds
`M_DirtGround` by path, and puts the camera where it can see what got built.

Fly with **W A S D**, up/down with **E** and **Q**, look with the mouse. Open the
console with **`** (backtick).

---

## 4. What you should see

It starts in **track mode**: a real FIM-legal motocross circuit at true scale —
1,520 m lap, 8 m wide, about 20 m of elevation change on the lap, 15 obstacles, with
berms on the outside of every corner.

Run `DaDirt.Info` first. It prints the grid settings, every obstacle and where it sits
along the lap, and the **earthmoving ledger** — how much dirt was cut, filled, built
into jumps and berms, and dug out of the borrow pits.

`DaDirt.View` puts the camera back somewhere sensible if you get lost.

### Track mode — is it a real track?

| Check | How | Correct |
|---|---|---|
| The circuit closes | fly up and look down | one continuous 8 m ribbon, no gaps, no crossings |
| Scale | `DaDirt.Info` | 1,520 m lap on a 384 m site |
| It follows the land | fly a lap | the track climbs and drops with the terrain, it does not cut through it |
| Berms | fly a lap low | banked wall on the **outside** of every turn, biggest in the tightest ones |
| Start straight | look at the start | no jumps on it |
| Borrow pits | look at the far corners of the site | three shallow bowls — that is where the jump dirt came from |
| Nothing melts | watch for ten seconds | the track should be stable; if jumps or berms visibly slump, the repose numbers are off |

**The ledger should read**: roughly 27,000 m³ cut, 22,000 filled, 6,400 built,
1,500 recovered from pits, **0 imported**. I verified those numbers by replicating
the whole build in Python at grid resolution, so if the log disagrees badly with
them, something is wrong in the C++ rather than in the design.

### Focusing in — ruts at track scale

The sim grid is fixed at 1024 cells. Spread over the whole 384 m site that is 37.5 cm
per cell, and a real 12 cm rut is a third of one cell. Point the same grid at less
ground and the cells get finer:

```
DaDirt.Focus 93 -39 31      ->  31 m region, 3 cm cells, a rut is 4 cells across
DaDirt.View                     look at it
DaDirt.Dig 93 -39 15 12         cut something rut-sized
DaDirt.Focus off                back to the whole site
```

The terrain under the region is **regenerated at full resolution** for wherever you
point it, so it is real detail, not a zoom on blurry data. Good places to try:
`93 -39` (a corner), `-51 -39` (the triple), `115 24` (a berm).

| Check | Correct |
|---|---|
| `DaDirt.Info` after focusing | reports the region size and "a 12 cm rut is 4.0 cells" |
| The mesh | follows the region — you see a 31 m patch of track, not the whole site |
| `DaDirt.Dig` at the focus centre | lands where you aimed, not offset |
| The earthmoving ledger | says it is *not meaningful while focused* — correct, since cut/fill were only counted over the region |

**Known limit:** deformation does not survive moving the region. Dig a rut, focus
somewhere else, come back, and it is gone. That needs a world-resolution deformation
layer, which is the next piece of work and belongs with the test wheel.

### Testbed mode — does the dirt behave?

`DaDirt.Mode testbed` rebuilds as the 128 m measuring rig, where cells are 12.5 cm
instead of 37.5 and dirt behaviour is actually visible.

Positions are **metres from the centre of the box**, so `0 0` is the middle.

| Check | How | Correct |
|---|---|---|
| Slumping is alive | `DaDirt.Test repose` | the cone at (3, −45) visibly collapses; log reports max loose slope near 32° |
| Angles behave | `DaDirt.Test anglefan` then `DaDirt.DebugView 4` | piles on faces under ~32° stay; steeper ones slide to the bottom |
| Volume is conserved | `DaDirt.Test conserve` | drift near `0.000 m3` |
| Digging works | `DaDirt.Dig 0 0 200 30` | a hole with a raised rim of spoil around it |
| Ruts | `DaDirt.Test trench` | a 40 m trench with spoil ridges down both sides |
| Mud | `DaDirt.Wet 0 0 400 1` then watch | saturated dirt should slump *flatter* than dry — mud runs away |
| Frame rate | `stat fps`, `stat gpu` | 60 fps at 1080p |

### Debug views

`DaDirt.DebugView <n>`:

| n | View |
|---|---|
| 0 | Plain dirt, shaded by compaction and moisture |
| 1 | Loose dirt depth above bedrock |
| 2 | Compaction — pale loose, dark red packed |
| 3 | Moisture — pale dry, blue saturated |
| 4 | **Stability** — green stable, red steeper than this dirt can hold |
| 5 | Bedrock exposure — red where dirt is scraped to hardpack |
| 6 | Slope angle, 0–90° |

View 4 is the important one. It is the direct readout of whether the angle of repose
is doing what we think.

### Every command

```
DaDirt.Info                  grid settings, obstacles, earthmoving ledger
DaDirt.Mode testbed|track    rebuild as the measuring rig or the real circuit
DaDirt.Focus <x> <y> [sizeM] | off    point the sim at part of the box for fine cells
DaDirt.View                  camera back to a viewpoint that frames the box
DaDirt.Audit                 volume conservation check (hitches: reads the GPU back)
DaDirt.Probe <x> <y>         surface height at a point, in cm
DaDirt.Reset                 rebuild the terrain, discard everything dug

DaDirt.Dig    <x> <y> [radiusCm=150] [depthCm=15]
DaDirt.Raise  <x> <y> [radiusCm=150] [heightCm=15]
DaDirt.Smooth <x> <y> [radiusCm=200] [strength=0.5]
DaDirt.Wet    <x> <y> [radiusCm=300] [amount=0.5]
DaDirt.Pack   <x> <y> [radiusCm=300] [amount=0.5]
DaDirt.Loosen <x> <y> [radiusCm=300] [amount=0.5]

DaDirt.Ball   <x> <y> [dropM=5] [radiusCm=30] [vxMps vyMps]   drop or throw a ball
DaDirt.ClearBalls
DaDirt.Wheel  <x> <y> [headingDeg]   the powered test wheel (replaces the old one)
DaDirt.Drive  <throttle -1..1> [steer -1..1] [brake 0..1]   inputs, held until changed
DaDirt.Anchor [0|1]          hold the wheel in place: a burnout on a stand
DaDirt.Follow [0|1]          chase camera behind the wheel

DaDirt.Test   repose | anglefan | conserve | trench
DaDirt.DebugView <0-6>
DaDirt.Pause  [0|1]
DaDirt.Slump  <iterations>
DaDirt.Repose <looseDeg> [denseDeg] [suctionKPa] [packedKPa]   soil strength (docs/SoilPhysics.md)
```

---

## 5. Performance

Two numbers matter. Run `stat gpu` and look for the `DirtSlump` line.

If the sim is too expensive, in this order:

1. `DaDirt.Slump 1` — fewer slump passes per step. Cheapest fix; dirt settles slower.
2. Select the DirtBox in the Outliner while playing and drop **Sim Resolution** to
   512 — quarter the sim cost.
3. Drop **Mesh Verts Per Side** if the *mesh* rather than the sim is the cost. Track
   mode defaults to 768 per side (~1.2 M triangles), which is the most likely thing
   to be too heavy on an integrated GPU. Try 512, then 384.

Tell me which line is expensive and by how much. The slump shader currently does
about 25 texture reads per cell, and there is a known optimisation (a groupshared
tile with a halo) that cuts it to roughly 2 — but it is only worth doing if the
measurement says so.

---

## 6. Known limits, all deliberate

- **No mouse tools.** Everything is console-driven. That costs no input assets and
  makes every action a repeatable test. Mouse tools are Phase 1d.
- **No collision.** The ground is displaced in the vertex shader, so its collision
  would be a flat plane and would lie about where the ground is. `DaDirt.Probe`
  reads the true height; that is what physics objects will use.
- **No loose dirt particles.** Phase 1c.
- **Track mode at full extent cannot show ruts** — 37.5 cm cells against a 12 cm
  rut. Use `DaDirt.Focus` to point the grid at a smaller region and the cells get
  fine enough. What is still missing is *persistence*: deformation outside the
  focused region is not retained, so you cannot yet ride a lap and leave ruts behind
  you. That is the remaining half of goal G2 in ROADMAP.md.
- **Digging can lose a little volume at bedrock.** Dig somewhere already scraped to
  hardpack and the dirt that "should" have moved is not there. Digging into normal
  60 cm dirt is exactly zero-sum. The audit reports it rather than hiding it.
