# Dirtbox first run on Windows — what broke, what was measured

Date: 2026-09-21. Machine: the dev laptop (Intel Arc 140T iGPU, UE 5.8, VS 2022).
This is the answer to `docs/DirtboxSetup.md`. Everything below was run unattended
from scripts, so it can be re-run the same way any time.

## Short version

- The code compiled first time with zero warnings. Two runtime problems stopped
  the editor from launching; both are fixed.
- The material and level are now built by a script, not by hand.
- The track builds to the numbers the design predicted, and after one fix the
  earthmoving ledger balances with **0 m³ imported**.
- Volume conservation is exact (drift under 0.0001 m³ on 8,000 m³).
- Digging, focusing, mud and slumping all work. Slumping had a real bug: with
  four neighbours the angle of repose depended on direction (32° along the grid,
  41° on the diagonal) and piles settled into square pyramids. Fixed with an
  8-neighbour slump; a loose cone now settles to 32° in every direction within
  5 s and to the exact height volume conservation predicts.
- Performance: 60 fps in track mode and ~80 fps in the testbed at 1080p once the
  template's volumetric clouds were removed. The dirt sim itself costs ~3 ms.

## What broke

### 1. Global shaders registered from the wrong module (editor crashed on launch)

`DirtSimulation.cpp` declared the four compute shaders inside the `DaDirt` game
module, which loads at the `Default` phase. The engine had already built its
global shader map by then and asserted:

```
Assertion failed: !AreShaderTypesInitialized()
Shader type was loaded too late, use ELoadingPhase::PostConfigInit on your module
```

**Fix:** moved `DirtSimulation.h/.cpp` into the `DaDirtShaders` module (which
already loads at `PostConfigInit` for the path mapping). That module cannot
depend on `Engine`, so `FDirtSimFrame` now carries raw `FRHITexture*` pointers and
the Dirtbox resolves them from its texture resources inside the render-thread
lambda. `FDirtBrushStroke` moved with it (its `Mode` is now an `int32` so the
header needs no reflected types). `DaDirt` now depends on `DaDirtShaders`.

### 2. Shader virtual path did not include the folder (fatal on launch)

The mapping `/DaDirt -> <Project>/Shaders` was correct, but the C++ asked for
`/DaDirt/DirtSim.usf` while the file lives in `Shaders/Private/`. Fatal error:
`Couldn't find source file of virtual shader path '/DaDirt/DirtSim.usf'`.

**Fix:** the four `IMPLEMENT_GLOBAL_SHADER` lines now use
`/DaDirt/Private/DirtSim.usf`.

### 3. Borrow pits sized with the wrong bowl volume (ledger imported dirt)

First run: `borrowed 474, imported 323 m³`. Three pits at r=8 m recovered only
60% of the deficit. `DigBorrowPits` sized the pits assuming a cosine bowl holds
πR²D/2; it holds πR²D·(½ − 2/π²) ≈ 0.297·πR²D.

**Fix:** corrected the constant. Now `borrowed 797, imported 0`, pits r=11 m.
Note the setup doc's target of "~1,500 recovered" cannot be right given its own
cut/fill/built figures: the deficit is 796 m³ and that is what the pits must
supply.

### 4. The five "most likely to break" lines all compiled unchanged

`bCanCreateUAV`, `GetPlatformData()->Mips[0]`, `CreateRenderTarget`,
`ReadLinearColorPixels` and the RHI accessors are all still spelled that way in
5.8.

## What was added to make this repeatable

- **`Tools/BuildDirtAssets.py`** — editor Python that builds `/Game/Dirt/M_DirtGround`
  node-for-node from the setup doc, a 4×4 linear placeholder render target for
  the parameter defaults (a Linear Color sampler refuses the engine's sRGB default
  texture), and `/Game/Maps/L_Dirtbox` from the Basic template with the Floor and
  the VolumetricCloud removed. Re-runnable. Needs `PythonScriptPlugin`, now
  enabled in the uproject.
- **`-DirtScript=<file>`** on the game mode — runs a text file of console commands
  with `wait <s> [label]` (logs avg frame / game / render / GPU ms) and
  `screenshot <name>`. `Tools/DirtboxChecklist.txt` is section 4 of the setup doc;
  `Tools/DirtboxProbe.txt` measures slump and mud with height probes.

Launch (from Git Bash, `MSYS_NO_PATHCONV=1`; PowerShell 5.1 splits `x.txt` args):

```
UnrealEditor.exe DaDirt.uproject /Game/Maps/L_Dirtbox -game -windowed -ResX=1920 -ResY=1080 ^
    "-DirtScript=C:\...\Tools\DirtboxChecklist.txt" -unattended -nosplash -abslog=<log>
```

## Track mode

| Check | Result |
|---|---|
| Circuit closes, 1,520 m lap, 8 m wide | yes, 15 obstacles, 20 m grade change, steepest 21% |
| Ledger | cut 27,812 / fill 22,158 / built 6,451 / borrowed 797 / **imported 0** m³ |
| Stable after 10 s | drift +0.00000 m³, steepest loose slope 9.3°, 0 cells scraped to rock |
| Focus 93 −39 31 | 31 m region, 3.03 cm cells, "a 12 cm rut is 4.0 cells" |
| Dig 15 cm radius, 12 cm deep at focus centre | probe 395.2 → 383.6 cm (11.6 cm deep, on target) |
| Focus ledger | reports "not meaningful while focused" as designed |

Cut/fill/built match the Python replica (27k / 22k / 6.4k).

## Testbed mode — does the dirt behave?

Volume is conserved to the cell in every test: baseline 8025.7729 m³, drift
between −0.00006 and −0.00010 m³ after repose, anglefan, conserve, trench, dig and
mud.

**Dig 0 0 200 30:** surface at the centre 60 → 30.1 cm (30 cm hole), rim at 3 m
raised to 66.6 cm. Correct.

**Loose cone at (3, −45)** (2 m radius, 3 m tall, 56° faces), profile along +X:

| distance from apex | 0 | 0.5 | 1.0 | 1.5 | 2.0 | 2.5 | 3.0 m |
|---|---|---|---|---|---|---|---|
| t = 0 (cm) | 347 | 284 | 210 | 135 | 65 | 60 | 60 |
| t = 5 s (cm) | 250 | 222 | 188 | 154 | 119 | 77 | 60 |
| slope at 5 s | | 29° | 34° | 35° | 35° | 40° | 19° |

It collapsed from 56° to ~35° flanks in 5 s — and then stopped. A 30 s run with
8 slump passes per step gave the same heights at 5, 15 and 30 s, so this was not
slow convergence but the scheme's own equilibrium. Probing along the diagonal
explained it: 41° there against 32° along the axis, which is exactly
atan(√2·tan 32°). The four-neighbour slump only measured drops along the grid
axes, so a diagonal could stand √2 times steeper, and every pile settled into a
square pyramid with flat facets (visible in the close-ups).

**Fix:** slump over all eight neighbours with diagonals treated as √2 cells away,
flow across a diagonal divided by that distance. The audit and the stability view
use the same eight directions. Re-measured with the fix:

| distance from apex | 0 | 1.0 | 2.0 | 3.0 m |
|---|---|---|---|---|
| along the axis (cm) | 227.6 | 169.0 | 106.4 | 60 |
| along the diagonal (cm) | 227.6 | 171.6 | 109.1 | 60 |

Both directions 32°, identical at 5 s, 15 s and 30 s, and the apex sits at
227.6 cm — a 32° cone of the original 12.6 m³ works out to 227 cm. Volume drift
−0.00003 m³.

### Sandcastle test (`Tools/DirtboxSandcastle.txt`)

Four identical 150 cm towers on the pad, built while paused, then released. The
pad itself carries 0.15 moisture, so its "dry" dirt is really slightly damp:

| tower | recipe | predicted repose | apex after 5 s | measured flank |
|---|---|---|---|---|
| loose | Loosen then Raise | 38° (32 + cohesion at m=0.15) | 147 cm | 35–38° |
| damp | Wet 0.5 then Raise | 41° | 157 cm | 41–42° |
| packed | Raise then Pack 1.0 | ~68° | 207.7 cm, did not move | held |
| mud | Wet 1.0 then Raise | 15° | 93 cm | 13–17° |

Every tower landed on the number the repose formula gives for its actual
compaction and moisture. The packed tower keeps a sharp cone; the mud tower is a
flat splat with a wide skirt.

The `camera` and `sun` script commands were added for this, and the plain-dirt
colour was darkened to a real soil albedo (0.40/0.29/0.19 loose, 0.24/0.16/0.10
packed); with a low sun the shapes finally read.

**Fresh loose pile on the pad** (Raise 100 cm over 150 cm core, ~45° peak slope):
after 5 s the mid-flank went from 45° to 38°, the lower flank sits on the 32° line,
the apex has not moved (the kernel is flat on top so it is below repose there).

**Mud vs dry** — two identical piles, one soaked (`Wet 3 15 800 1`), after 6 s:

| | apex | slope |
|---|---|---|
| dry pile | 159 cm | 38° mid-flank, still relaxing |
| wet pile | 115 cm | **15.6°** all the way down |

The saturated pile slumped to within a degree of the 15° mud repose. Mud works.

**Audit's "steepest loose" reads 61–65° in every testbed run.** This is not the
cone. The testbed has a 1 m block of loose dirt with vertical walls, and 60 cm of
loose layer on top of every wedge and jump block. Those edges retreat as scarps,
and a scarp cell mid-collapse is exactly "loose, >2 cm, steep". The number is
honest but it measures the wrong thing for the repose test — see open items.

## Performance (1080p, GPU profile from `ProfileGPU`)

| Pass | ms |
|---|---|
| DirtSlump × 3 | 0.55–0.69 each, ~1.8 total |
| DirtResolve | 0.5–0.9 |
| CopyTexture (state B → A) | 0.53 |
| **Dirt sim total** | **~3 ms** |
| VolumetricCloud (Basic template) | **3.2** |
| PostProcessing | 2.6 |
| BasePass (1.18 M tris in track mode) | 1.5 |
| ShadowDepths (4 cascades) | 1.3 |
| Velocities | 1.3 |
| Whole frame, track mode, with clouds | 16.4–17.4 |

With the cloud actor removed from `L_Dirtbox` (final checklist run):

| Mode | fps (avg over 3 s) | GPU ms |
|---|---|---|
| track (768² mesh), 3 slump passes | 64–73 | 11–12 |
| track, 1 slump pass | 77 | 10.8 |
| testbed (512² mesh), 3 slump passes | 71–81 | 10–11.5 |
| testbed, 1 slump pass | 82 | 10.0 |

Hitches of 300–400 ms happen on every `Audit`/`Probe` (blocking readback) and on
mode/focus rebuilds. Expected.

**After the 8-neighbour slump:** each slump pass does ~81 texture loads instead
of ~25 and costs about 1.5 ms instead of 0.6. Testbed GPU went from ~12 to ~14.7 ms
at 3 passes (60 fps), and 8 passes costs 23.5 ms (38 fps). The groupshared-tile
optimisation is now worth doing when the frame needs the headroom; 3 passes
converge a collapsing cone within 5 s, so there is no reason to run more.

## Ball and wheel (later the same day)

Both objects read the ground through a **height window**: a 32–128 texel patch of
the dirt state read back from the GPU every frame through `FRHIGPUTextureReadback`,
arriving a frame or two later. Nothing stalls; the balls and the wheel run at 60 fps
with strokes landing every frame (the brush pass now takes 16 strokes per dispatch).

**Ball** (`Tools/DirtboxBall.txt`): a 30 cm ball dropped 5 m hit at 9.9 m/s — exactly
√(2·g·5) — dented 15 cm, bounced twice and slept in its crater with the probe
agreeing to 0.1 cm. A 60 cm ball from 12 m hit at 15.4 m/s. A ball on the 11 m dome
bounced 20 m down the flank; a thrown ball skipped three times.

**Wheel** (`Tools/DirtboxWheel.txt`), after three tuning rounds:

| test | result |
|---|---|
| anchored burnout, 3 s | tyre at 9 m/s slip, 5 cm hole under it, 2.8 cm pile 1.6 m behind, 2.6 L moved |
| run down the pad | 0 → 13.5 m/s (49 km/h) over 47 m, slip 1.7 → 0.4 m/s as it hooks up |
| locked brake from 49 km/h | 25 m skid, slip −10 m/s, dirt shoved forwards |
| rut, 3.9 cm cells, pass 1 | 1.5 cm deep, 0.5 cm shoulders |
| rut, pass 4 | 2.4 cm deep, 0.8 cm shoulders, floor compaction 1.0, grip 0.55 → 1.00, all passes on the same line |
| turn at 0.7 steer | clean arc, heading 90° → 238° |
| volume | drift ≤ 0.00015 m³ through everything |

Two bugs were caught by the numbers: the first version's tyre spun up without an
engine cap (250 m/s) and each scoop dropped the ground enough to read as "airborne",
which removed traction and let it spin faster — a runaway that dug 1,700 L and hit
bedrock (the audit flagged +4.8 m³ created). A tyre speed cap, a 3 cm contact
tolerance and a scoop capped by the layer available fixed it. Then the wheel would
not stay in its own rut: a point contact tips off a shoulder. A three-point contact
across the tyre's width (rest on the highest point, get pushed toward the lower
edge) made the rut hold it.

## Open items (design calls, not bugs)

1. **The repose test's metric needs a cleaner subject.** The box-wide "steepest
   loose" is dominated by retreating scarps at the loose block and wedge tops.
   Measure the cone's own profile (as `Tools/DirtboxProbe.txt` and
   `Tools/DirtboxLongSettle.txt` do) or exclude cells adjacent to bare bedrock.
2. **Slump cost.** With eight neighbours the three slump passes are ~4.5 ms of GPU.
   The groupshared-tile version is the known fix if the frame gets tight.
3. **Exposure.** The darker albedo helps a lot; pinning exposure in the sandbox
   map would make screenshots comparable run to run.
4. **Screenshots in unattended runs are captured one request late** by the
   engine (`HighResShot` and `FScreenshotRequest` alike), and a direct viewport
   read from the tick returns a black back buffer. The runner works around it by
   requesting each shot on two consecutive frames and then a throwaway `_flush`
   request. Files land in `Saved/Screenshots/WindowsEditor/` and now show the
   frame they were asked for.
