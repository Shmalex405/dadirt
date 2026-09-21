# What dirt is — a soil-science model for the Dirtbox

Written 2026-09-21 at Alex's request: stop tuning angles by feel and derive the
simulator from how soil actually behaves. Everything below is standard soil
mechanics and terramechanics (Terzaghi, Mohr–Coulomb, Proctor, Culmann, Bekker,
Janosi–Hanamoto). Each section ends with **→ Simulator**: what it means for our
grid. Section 10 is the implementation plan; Phases A to E of it are implemented
and measured (sections 9b to 9f).

Units: SI throughout (m, s, kg, N, kPa). The grid stores centimetres; convert at
the edges.

---

## 1. Dirt is a three-phase material

Soil is solid grains, water and air. Everything else follows from the proportions.

**Grain size** (USDA): clay < 0.002 mm, silt 0.002–0.05 mm, sand 0.05–2 mm,
gravel > 2 mm. A soil's *texture* is its sand/silt/clay split. Motocross dirt
spans three families:

| track type | texture | behaviour |
|---|---|---|
| sand (Lommel, Southwick) | > 85% sand | no cohesion when dry, deep ruts, soft, drains instantly, cannot be packed hard |
| loam (Red Bud, most prepped tracks) | 40/40/20 sand/silt/clay | the "ideal": packs well when damp, holds ruts and berms, tacky |
| hardpack (Glen Helen, SoCal) | clay-rich, silty | rock hard when dry, glass-slick when wet, "blue groove" |

**Phase relations.** With grain density ρ_s ≈ 2650 kg/m³:

- porosity n = V_voids / V_total; void ratio e = n/(1−n)
- dry density ρ_d = ρ_s (1 − n). Loose sand n ≈ 0.46 → ρ_d ≈ 1430; dense n ≈ 0.34 → ρ_d ≈ 1750 kg/m³
- degree of saturation S = V_water / V_voids ∈ [0, 1]; volumetric water θ = n S
- bulk (moist) unit weight γ = g [ρ_s(1−n) + ρ_w n S] ≈ 14–21 kN/m³

**→ Simulator.** Our two channels map cleanly: `compaction` c ∈ [0,1] is relative
density, n = n_loose − c (n_loose − n_dense) = 0.46 − 0.12 c; `moisture` is S.
Unit weight follows: γ = 9.81 [2650 (1−n) + 1000 n S] / 1000 kN/m³. Crucially, **our
"layer height" is bulk volume, and bulk volume is not conserved when dirt is
compacted** — packing loose sand from n = 0.46 to 0.34 shrinks it by 18%. The
audit today conserves bulk volume; it should conserve *solid* volume
V_s = h (1 − n). That is Phase B.

---

## 2. Strength: Mohr–Coulomb

Soil fails in shear when the shear stress on a plane reaches

    τ_f = c′ + σ′ tan φ′

- φ′, the **friction angle**: loose sand 28–32°, dense sand 38–42° (angular grains
  interlock), silt 26–32°, clay 20–28°
- c′, **cohesion**: dry sand 0; loam 5–20 kPa; clay 10–50 kPa; hardpack (cemented,
  dried) higher still
- σ′ = σ − u, **effective stress** (Terzaghi): the pore-water pressure u carries
  part of the load and does not contribute to friction

Two things make *apparent* cohesion appear and vanish with water:

**Suction.** In partially saturated soil the water sits in menisci between grains
and pulls them together: matric suction ψ > 0 acts like extra confining stress,
c_app = ψ tan φ_b. For sand ψ peaks at a few kPa around S ≈ 0.3–0.6 and drops to
zero both when dry (no water) and when saturated (menisci gone). That hump is
why damp sand builds castles and dry or soaked sand does not. At grain scale the
same thing is a capillary bridge: F ≈ 2π γ_w R cos θ_c with γ_w = 0.072 N/m; for a
1 mm grain that is ≈ 0.45 mN against a grain weight of 14 µN — wet grains stick
with 30× their own weight.

**Saturation and pore pressure.** Past ~S = 0.6 the air is disconnected, loading
raises u instead of σ′, and tan φ′ acts on a shrinking effective stress. A loose
saturated sand can liquefy (σ′ → 0). A clay past its **liquid limit** (Atterberg)
is a slurry. Either way: mud, holding 10–15°.

**→ Simulator (Phase A, implemented).** Per cell:

    Sat        = smoothstep(0.55, 1, S)                         pore pressure takes over
    tan φ_eff  = tan( lerp(φ_loose, φ_dense, c) ) · (1 − k_sat · Sat)     k_sat ≈ 0.6
    c_eff      = c_suction · 4 S (1 − S)  +  c_pack · c · (1 − Sat)      [kPa]

with φ_loose = 32°, φ_dense = 42°, c_suction ≈ 3 kPa (peak apparent cohesion),
c_pack ≈ 8 kPa (interlock / cementation of fully packed loam). Sand tracks:
c_pack ≈ 2; hardpack: c_pack ≈ 25.

---

## 3. Why a slope stands: repose, and the height cohesion buys

**Cohesionless** (dry sand): a slope is stable iff β ≤ φ. The angle of repose *is*
the friction angle — 30–35° for dry sand, which is what the cone test measures.

**Cohesive**: cohesion is a *stress*, so the height it can hold is a *length*,
c/γ. For a planar failure surface through the toe of a slope at angle β
(Culmann):

    H_c(β) = (4 c / γ) · sin β cos φ / (1 − cos(β − φ))

- β → φ:  H_c → ∞ (friction alone holds it)
- β = 90°: H_c = (4c/γ) tan(45° + φ/2), the classic vertical-cut height

Numbers, γ = 17 kN/m³, φ = 32°:

| c (kPa) | vertical cut stands up to | 60° face stands up to |
|---|---|---|
| 0 (dry sand) | 0 | 0 |
| 1.5 (barely damp) | 0.65 m | 2.2 m |
| 3 (damp sand, the castle) | 1.3 m | 4.5 m |
| 8 (packed damp loam) | 3.5 m | 12 m |
| 25 (hardpack) | 11 m | — |

A sandcastle wall stands when it is short; a tall one slumps to a lower angle
until H_c(β) exceeds its height. A watered, packed jump face at 60–70° stands.
A dry sand pile never does. **This is the sandcastle rule, and it is the piece
the angle-only model could not express.**

**→ Simulator (Phase A, implemented).** Between two cells a horizontal run L
apart with a drop D (face angle β = atan(D/L)):

    allowed = L tan φ_eff                                    friction
    if D > allowed and c_eff > 0:
        H_c = min( 400 · c_eff/γ · sinβ cosφ / (1 − cos(β−φ)),  H_max )   [cm]
        allowed = max(allowed, D ≤ H_c ? D : H_c)
    excess = D − allowed   → slumps at the usual rate

Known limit: D is the *local* face height, so a long slope of many small steps
gets the cohesive allowance at every step and can stand steeper than Culmann says
for tall slopes. At sandcastle and MX-obstacle scale (≤ 3 m) the local rule is
close; a multi-scale check (drop measured over 4 and 16 cells as well) is the
fix if it matters.

---

## 4. Compaction: Proctor

Compacting soil pushes grains into a denser packing. The **Proctor curve** plots
dry density against water content for a fixed effort: it rises to a peak at the
**optimum water content** w_opt then falls, because past optimum the water fills
the voids and, being incompressible, resists further packing.

- w_opt: sand 8–12%, loam 12–18%, clay 18–25% by mass — roughly S ≈ 0.75–0.9
- dry sand barely compacts (grains cannot rearrange without lubrication and
  suction to hold them); saturated soil cannot compact at all, it *pumps*
- dry density rises roughly with log(number of passes); the first pass does most
  of the work

Consequences on a track: watering before grooming is not for dust, it is to get
the dirt to w_opt so the tractor and the bikes pack it. Sand tracks stay soft
because they cannot be packed. Over-watered hardpack turns slick because the
surface goes past optimum.

**→ Simulator (Phase C).** The wheel's Pack increment should be
`ΔC = k_p · (pressure/p_ref) · f(S)` with f a hump peaking near S = 0.8
(`f = smoothstep(0, 0.8, S) · (1 − smoothstep(0.85, 1, S))`), so dry dirt packs
slowly, damp dirt packs fast, mud does not pack. And packing must **shrink the
layer** (section 1) — that is where most of a rut's depth really comes from.

---

## 5. Water in the ground

**Infiltration.** Rain or a water truck adds water at the surface; it enters at a
rate limited by the saturated hydraulic conductivity K_s (Green–Ampt gives the
falling infiltration curve). K_s: sand 10⁻⁴–10⁻³ m/s (a 5 mm watering vanishes in
seconds), loam 10⁻⁶–10⁻⁵, clay < 10⁻⁸ (it puddles). **Compaction lowers K by one
to two orders of magnitude** — a packed racing line stays wet after the loose
verges have drained, which is why the groove goes slick.

**Redistribution.** Water moves down under gravity and toward drier soil under
suction (Richards' equation; we do not need it in full). Ruts and holes collect
run-off and hold water because their floors are packed.

**Drying.** Evaporation ≈ 3–8 mm/day in the open; the top few cm dry first. For
game pacing this gets scaled, but the order matters: surface → crust, subsoil
stays damp (a real MX phenomenon: dry dusty top over tacky base).

**→ Simulator (Phase C).** Per sim step on the moisture channel:
1. drain: S −= k_drain(c) · max(0, S − S_field) · dt, with k_drain falling with
   compaction (K_s effect) and S_field ≈ 0.3 (field capacity, water held by
   suction)
2. run-off: where S > 1 the excess is a thin surface water layer that flows to
   the lowest neighbour and infiltrates where K allows; ponds in ruts
3. evaporate: S −= k_evap · S · dt at the surface
4. `DaDirt.Rain <mm>` and `DaDirt.Water <x> <y> <litres>` as sources

---

## 6. The tyre and the soil: terramechanics

**Pressure–sinkage (Bekker).** A plate of width b pushed into soil to depth z
carries pressure

    p = (k_c / b + k_φ) z^n

Wong's measured values:

| soil | n | k_c (kN/m^{n+1}) | k_φ (kN/m^{n+2}) |
|---|---|---|---|
| dry sand | 1.10 | 0.99 | 1528 |
| sandy loam | 0.70 | 5.27 | 1515 |
| clayey soil | 0.50 | 13.2 | 692 |

For our tyre (b = 0.12 m, N ≈ 1 kN, contact length l ≈ 2√(2 r z)): dry sand gives
z ≈ 4–5 cm of static sinkage, loam ≈ 2 cm, hardpack < 1 cm. That is the rut a
single pass presses, before any wheelspin. Slip adds **slip-sinkage**, the tyre
digging as it spins.

**Shear and traction (Janosi–Hanamoto).** Shear stress under the tyre builds with
shear displacement j along the contact:

    τ(j) = (c + σ tan φ) · (1 − e^{−j/K})

K, the shear deformation modulus: sand 1–2.5 cm, loam 2–5 cm. Slip ratio
i = (ω r − v)/ω r; j grows with i along the patch, so traction rises with slip
and saturates at the Mohr–Coulomb limit. This is the real version of our
`tanh(slip / slip_scale)`, and it says grip = **c·A + N tan φ**: cohesion gives
grip even at low load, friction scales with load, and saturation kills both.

**Motion resistance** is the work of compacting the soil in the rut:
R_c = b (k_c/b + k_φ) z^{n+1} / (n+1), large in loose sand, near zero on hardpack.

**Excavation.** Beyond the traction limit the tyre lugs fail the soil and eject
it at roughly the slip velocity: roost mass rate ∝ (ω r − v) · b · (failure depth).
Ejection speed 10–25 m/s at 30–60° gives ballistic ranges of 10–40 m — matches a
real roost.

**→ Simulator (Phase D).** Replace the wheel's `tanh` with Janosi–Hanamoto on
(c_eff, φ_eff, N); replace the fixed rut-per-pass with Bekker sinkage from N, b
and the soil's (n, k_c, k_φ) derived from compaction; motion resistance from R_c;
roost rate from excavation, capped by available layer as now.

---

## 7. Individual particles: when dirt stops being a surface

Alex's stated focus: particles, intense but clean. Soil physics says exactly
when a continuum surface is the wrong model — when grains or clods leave contact
with the mass:

- **Roost**: shear failure under a spinning tyre ejects material. In sand it is
  grains and spray (0.1–2 mm); in loam it is clods 1–5 cm held together by
  cohesion; in clay, 5–20 cm lumps. Clod size scales with c_eff (a clod holds
  together while c > its self-weight stress ≈ γ d).
- **Spray off a berm or landing**: the same, driven by impact rather than slip.
- **Avalanching grains** on a face just past repose: a rolling layer one to a few
  grains deep, not a bulk slump.

Grain-scale contact (DEM): Hertz normal force, Coulomb tangential friction
μ = tan φ, rolling resistance for angularity, and for wet grains the capillary
bridge of section 2. Millions of grains per litre rule out one-grain-per-particle
for sand; the honest model is a **parcel**: one particle carries a volume V_p of
dirt (and its moisture and compaction), sized by clod physics, rendered at
d = (6 V_p/π)^{1/3}. A 3 L roost per second at 1 cm clods is ≈ 6,000 parcels/s;
the Arc iGPU can carry ~100–300 k live parcels at 60 fps.

**Clean hand-off, both ways** — the invariant that makes this a simulator and not
an effect:

- ground → air: `TransferDirt`'s Scoop half removes V from the heightfield and
  spawns parcels totalling exactly V
- air → ground: a parcel that comes to rest on the heightfield Dumps V_p at its
  landing cell, arriving loose (compaction ← 0.1) and carrying its moisture
- the audit counts heightfield + airborne parcels and must still read zero drift
- a parcel landing on a slope steeper than φ_eff rolls (heightfield normal +
  friction), it does not stick

**→ Simulator (Phase E).** Niagara GPU emitter fed by the Scoop events with
(position, velocity, V_p); collision against the display height texture in the
particle sim (it is already on the GPU); a settle event queues Dumps back into
the brush pass. Dust is the tail of the size distribution: parcels below ~1 mm
render as sprites with drag, never carry volume worth auditing, and fade.

---

## 8. Three reference soils

Parameters the simulator should expose as presets. `c` = fully packed values;
loose values follow from the formulas in sections 2–4.

| | sand track | loam (default) | hardpack clay |
|---|---|---|---|
| φ loose / dense | 30° / 38° | 32° / 42° | 26° / 34° |
| c_suction peak | 2 kPa | 3 kPa | 4 kPa |
| c_pack (full compaction) | 2 kPa | 8 kPa | 25 kPa |
| k_sat (friction lost at S = 1) | 0.5 | 0.6 | 0.8 |
| γ (kN/m³, moist, mid-pack) | 17 | 17.5 | 19 |
| n loose / dense | 0.46 / 0.36 | 0.48 / 0.34 | 0.50 / 0.36 |
| K_s (m/s) | 3·10⁻⁴ | 5·10⁻⁶ | 10⁻⁸ |
| S_opt for packing | 0.7 | 0.8 | 0.85 |
| Bekker n, k_c, k_φ | 1.1, 1, 1528 | 0.7, 5.3, 1515 | 0.5, 13, 692 |
| Janosi K | 1.5 cm | 3 cm | 4 cm |
| roost | spray, grains | 1–5 cm clods | lumps, or dust when dry |

---

## 9. What this predicts that the old model could not

1. A damp sandcastle wall stands vertical up to ~1 m; the same wall soaked or
   dried collapses. A 3 m damp pile stands at ~60°, not vertical.
2. A watered and packed jump face holds 65–70° because of cohesion, not because
   "packed dirt has a 70° angle of repose" (it does not; dense sand is 42°).
3. Dry sand cannot be packed by riding; damp loam packs on the first pass; mud
   pumps and ruts without packing.
4. Packing shrinks the ground: a rut is mostly compaction, not displacement,
   until the tyre spins.
5. The packed racing line drains slowest, so it goes slick first after watering
   and stays tacky longest.
6. Grip = c·A + N tan φ: on damp loam a light wheel still grips; in mud nothing
   does; on hardpack it is all friction and it vanishes with a film of water.
7. Roost is clods on loam, spray on sand, and its volume is exactly what left the
   ground.

---

## 9b. Phase A measured (Tools/DirtboxSoil.txt, 2026-09-21)

The testbed's 100 m³ block (1 m tall, vertical walls, compaction 0.1, moisture
0.05) in three states, probing across its east wall after 5 s:

| state | c_eff | Culmann H_c(90°) | what happened |
|---|---|---|---|
| as built | ≈ 1.4 kPa | ≈ 60 cm | top of the wall sheared off, a **60 cm** vertical face stands (140 → 80 cm over one cell) with the spoil at its foot |
| dampened to S ≈ 0.4 | ≈ 3.8 kPa | ≈ 160 cm | the **full 1 m wall stands vertical** (160 → 60 cm over one cell) |
| soaked, S = 1 | 0, friction × 0.4 | — | the block runs out as mud at **16°**, still flowing at 6 s |

Control: the bone-dry cone (S = 0, c = 0) settles at 32° on both axis and diagonal
as before. Volume drift 0.00000 m³. GPU cost of the slump rose ~1.3 ms for the
extra trigonometry; the testbed runs at 56–58 fps with it, 77 fps idle.

## 9c. Phase B measured (Tools/DirtboxSolid.txt, 2026-09-21)

The pad is 60 cm of bulk dirt at compaction 0.25 = 33.3 cm of solids. Solid
volume is what the audit sums now; the box holds 4661.458 m³ of solids, which a
ruler would call 8274.6 m³.

| action | surface | solids | compaction | drift |
|---|---|---|---|---|
| as built | 60.0 cm | 33.3 cm | 0.25 | 0 |
| `DaDirt.Pack` to 1.0 | **57.2 cm** (−2.8) | 33.3 cm | 1.00 | 0.00000 m³ |
| `DaDirt.Loosen` to 0 | **60.9 cm** (+0.9) | 33.3 cm | 0.00 | 0.00000 m³ |
| `DaDirt.Dig` 30 cm | 31.1 cm hole, spoil rim at 63.9 cm | 16.7 / 35.5 cm | 0.00 / 0.07 | 0.00000 m³ |

Packing drops the surface (the 15 cm skin shrinks by 1 − 0.52/0.66 = 19%) and
loosening lifts it, with not one grain moved: the rut a tyre presses by packing
alone is now a real thing. The 40-hole conserve test reads −0.00000 m³. Over a
minute of drying and settling the audit shows ±0.00001–0.00003 m³ (4·10⁻⁹): the
float rounding of a million cells exchanging tiny flows, not a leak.

## 9d. Phase C measured (Tools/DirtboxWater.txt, 2026-09-21)

`DaDirt.Water 3 -15 400` (a 400 L truck load over a 2.5 m radius, 6 cm at the
centre) on the loose pad (K = 0.5 cm/s, game-paced):

| | pond | moisture |
|---|---|---|
| 1 s | 4.4 cm | 0.17 |
| 21 s | 0 | 0.29 (field capacity is 0.30) |

The same pour on a patch packed to 0.7 with an 8 cm hole dug in it (K = 0.5 ×
10^(−2·0.7) = 0.02 cm/s): **8.2 cm** of water standing at 1 s, **9.1 cm** at
10 s as the surroundings run into the hole, moisture only 0.13 → 0.16. A puddle
on hardpack, gone-in-seconds on loose dirt, exactly the split section 5 asks for.

Rain at 60 mm/min for 8 s over the whole box: 26.8 m³ ponded (on the packed
faces and in the trench), 17.9 m³ twenty seconds later, the loose pad at 0.23.

Proctor: one pass of the wheel at 0.35 throttle over dry (0.14), damp (0.71)
and soaked (1.00) pad:

| moisture | compaction after one pass |
|---|---|
| 0.14 dry | 0.39 |
| 0.71 damp | **0.49** |
| 1.00 mud | 0.06 (it pumps; the tyre also loosened it) |

Dirt volume drift through all of it: 0.00000 m³. Water is not audited against a
baseline; it drains and dries by design.

## 9e. Phase D measured (Tools/DirtboxTerra.txt, 2026-09-21)

The test wheel (0.35 m radius, 0.12 m wide, 100 kg on it) parked on three soils.
Section 6 predicted loose 3–4 cm, hardpack under 1 cm, mud more:

| soil | compaction / moisture | Bekker sinkage | motion resistance |
|---|---|---|---|
| loose pad | 0.23 / 0.15 | **2.6–3.1 cm** | 48 N |
| packed | 1.00 / 0.14 | **0.1 cm** | 12 N |
| soaked | 0.23 / 0.98 | **8–9 cm** | 88 N |

Grip (the Mohr–Coulomb ceiling as a coefficient) reads 0.78 on the pad, 0.96
on hardpack (cohesion over the patch), 0.29 in mud. Full throttle from rest on
the pad: slip ratio 0.57 and 605 N of traction at launch, falling to 220 N at
i = 0.08 once rolling; 13.7 m/s (49 km/h) after 36 m. A locked brake at speed
slides with slip = −v and shoves dirt forwards (2.8 L over the stop), then holds
the wheel dead still: no phantom slip at rest.

Four passes at 0.5 throttle over the same line at 3.9 cm cells:

| pass | sinkage | floor compaction |
|---|---|---|
| 1 | 2.2 cm | 0.25 → 0.55 |
| 2 | 1.3 cm | 0.40 |
| 3 | 0.7 cm | 0.57 |
| 4 | 0.9 cm | 0.91 (in the rut) |

Rut after four passes: floor at 55.7 cm, shoulders at 59.4–59.5 cm on a 60 cm
pad, a **4.3 cm rut with 0.5 cm shoulders**, floor packed to 0.91. The depth
comes from Bekker sinkage pressed plastic plus the compaction shrink, and it
saturates because the packed floor barely sinks, with no rule saying so.

Burnout on the stand (anchored, full throttle, 3 s): wheel speed 9.9 m/s,
**5.3 L of solid dirt thrown** as 10,316 parcels, a 15 cm hole under the tyre
(60 → 44.9 cm), every parcel landed and audited, drift 0.00000 m³.

## 9f. Phase E measured (Tools/DirtboxParcels.txt and DirtboxPerf.txt, 2026-09-21)

**The hand-off is exact.** `DaDirt.Throw 3 -20 4` scoops 4.00 L of solids and
throws it at 6 m/s: the audit 0.3 s later reads ground 4661.4536 + air 0.0040 =
4661.4576 m³, drift −0.00000; three seconds later all 4,096 parcels have landed,
air 0, drift −0.00000. A burnout that put 143,267 parcels through the system
(0.4 cm clods, 5 L) came back at −0.00001 m³. Mud parcels (moisture 0.95) splat
where they land; dry ones hop and roll.

**The size question.** The same anchored burnout (full throttle, 3 s, chase
camera) at four parcel sizes, on the Arc 140T at 1080p:

| parcel diameter | parcels per litre | live at 3 s | fps | GPU ms |
|---|---|---|---|---|
| 2 cm | 239 | 627 | 45 | 19.0 |
| 1 cm | 1,910 | 4,621 | 42 | 20.9 |
| 5 mm | 15,279 | 34,410 | 32 | 28.3 |
| 4 mm (the floor) | 29,842 | 66,171 | 33 | 27.2 |
| `soil` (damp, 0.5) | 46 (3.5 cm clods) | 272 | 39 | 22.4 |

Idle, with nothing in the air, the testbed runs at 52 fps (16 ms GPU), of which
the heightfield is nearly all: three slump passes at 1.4 ms each, twice a frame
whenever the frame runs under 60 Hz, water 0.5 ms, deposit 0.4 ms. Two things
keep an idle pool free: the free list hands out the lowest slots first so only
the mesh sections up to the highest live slot are drawn (an idle pool of
262,144 cost 7 ms before that), and the per-parcel "highest live slot" atomic
is reduced per thread group first (20,000 parcels on one address cost 5 ms).

Where the parcel cost is, with 66,000 in the air: in a `ProfileGPU` frame of the 4 mm burnout (74,000 live) the parcel passes are 0.11 ms (spawn 0.03, sim 0.08) and the scene render is 1.2 ms above idle (8.7 vs 7.5 ms), so the parcels are about 1.3 ms of a 21 ms frame. The rest is the heightfield sim running twice per frame once the frame is over 16.7 ms. Over whole 3 s runs the same burnout measured 42 fps in one run and 33 fps in another with the machine warmer: the Arc's run-to-run variance is ±3 ms, more than the parcels cost.

So the honest answer to "how small": 4 mm is the floor today. A full roost at
4 mm is ~66,000 live parcels at 33 fps, and the cost is drawing them, not
simulating them (the sim and spawn passes are 0.15 ms together). The next step
down needs a cheaper draw, not a cheaper sim: one triangle per parcel instead
of a tetrahedron, or sprites for anything under a few pixels. The pool
(262,144) would cap a 3 mm roost (~175,000 live); a 2 mm one (~600,000) needs a
1024² pool, a setting, not a redesign. Below ~4 mm at chase-camera distance a
parcel is under a pixel anyway; `ParcelMinScreenSize` holds it at ~1.5 px, so
what changes as they get smaller is the density of the spray, which is exactly
what reads as fine dirt. Dust proper (the sub-millimetre tail that never
carries auditable volume) is still to come.

## 9g. The second pass (later on 2026-09-21)

**The slump on a shared tile.** The slump pass now runs on 16 × 16 tiles held in
group shared memory with a two-cell halo (`SLUMP_TILE` in DirtSim.usf): 1.6
texture loads per cell instead of 81. Each slump pass fell from 1.4–3.3 ms to
0.75–0.87 ms, and the sim no longer double-steps at 1080p:

| scene | before | after |
|---|---|---|
| testbed idle, everything on | 52 fps (16 ms GPU) | **75–86 fps** (10 ms) |
| soil test, block as built | 56–58 fps | **88–99 fps** |
| burnout, 1 cm parcels, ~5,000 live | 43 fps | **67 fps** |
| burnout, 4 mm parcels, ~74,000 live | 33–42 fps | **54 fps** |
| slump off (the rest of the frame) | 81–89 fps | 101 fps |

The tiled pass reproduces the Phase A numbers to the centimetre: the barely
damp wall shears to 140.1 → 80.0 cm over one cell, the damp wall stands, the
soaked block runs out, the dry cone settles at 32° on axis and diagonal.

**Parcels from more than roost.**
- *Grains shedding down a face* (`DaDirt.Shed`): a cell avalanching more than
  `ShedMinOutCm` per step has a `ShedChance` of turning part of that outflow
  into a parcel that rolls off along the fall line, capped at a few grains'
  worth so a 6 mm grain never carries a cell's worth of mud. The loose cone
  sheds **8,600 grains** as it collapses, still settles at 32°, drift 0.00000.
- *Spray off a berm*: sideways slither above `SpraySlipThresholdMps` shears
  `SprayFailureDepthCm` of soil per metre off the tyre's flank and throws it
  outward, low and fast. A hard turn on the pad: 27,000 parcels, drift 0.00000.
- *A landing*: touching down faster than `SplashImpactMps` scoops
  `SplashLitresPerMps` per m/s of impact and throws it out both sides. Off the
  block's edge: 22,000 parcels, drift −0.00001.
- *Dust* (`DaDirt.Dust`): a second pool of camera-facing soft quads, puffed with
  roost, spray, splash and throws at `DustPerLitre` motes per litre of dry
  dirt (wet dirt makes none), on a 1 mm speck's drag, neutrally buoyant with a
  touch of lift, fading over `DustLifetime`. It carries no audited volume and
  never deposits: it is the one deliberate effect. A dry burnout keeps ~800 motes in the air at 400 per litre (`Tools/DirtboxSpray.txt`); a soaked one makes 20.

**The deposit fold moved after the parcel sim.** The audit reads the display
texture and the live parcels; volume landed in the fixed-point accumulators but
not yet folded into the layer was in neither, and heavy shedding made that
visible as −0.0006 m³ for a frame. Folding at the end of the step put it back to
0.00000 with 4,000 parcels in the air.

**The rut-resolution leak, hunted.** `Tools/DirtboxLeak.txt` drives the same
two passes in a 40 m region with one part of the wheel's mark at a time: no
strokes, rut only, pack only, roost only (dumped in place), all parts with
parcels off, and the hand-tool conserve test. Every one audits to 0.00000 m³.
The culprit was the wheel's rut stroke passing over cells its launch spin had already emptied to bedrock: the Dig's core could not give the full amount, but its rim still received it, and every such stroke created dirt. The same clamp bit the roost scoop, whose parcels then landed dirt that never left. Both are fixed the same way: every mass-moving stroke is now two halves, a taking half that runs first and reports per stroke what it could not find (`DirtScoopShortfall`, fixed point), and a giving half (the rim of a dig, the core of a raise, a dump, or the parcels of a scoop) dispatched afterwards that gives only what was taken. With that, the four-pass rut at 3.9 cm cells reads **0.00000 m³** with parcels flying and grains shedding, as does every other case in the script. Parcels switched off now means the throw lands where it started through the same books, rather than a dump the shortfall could not reach.

## 10. Implementation plan

- **Phase A — strength (done 2026-09-21):** Mohr–Coulomb φ_eff and c_eff per cell
  from compaction and saturation; Culmann standing height in the slump and the
  stability view; wheel grip = tan φ_eff + c_eff A / N. Settings: `LooseReposeDeg`
  (φ loose), `PackedReposeDeg` (φ dense, now 42), `SuctionCohesionKPa`,
  `PackedCohesionKPa`, `UnitWeightKNm3`, `SaturationFrictionLoss`.
- **Phase B — solid-volume conservation (done 2026-09-21):** the layer channel
  now stores solid centimetres; bulk height = `DirtBulkCm(solid, C)`, a skin of
  `CompactionDepthCm` (15 cm) at the cell's compaction over natural ground at
  `DeepCompaction`. Packing drops the surface, loosening lifts it, every mass
  move is in solids so it is exactly zero-sum. Settings: `LoosePorosity` 0.48,
  `DensePorosity` 0.34. Audit reports solid and bulk m³. Section 9c.
- **Phase C — water (done 2026-09-21):** one water pass per step: run-off of
  ponded water over four neighbours (gather form, like the slump), rain,
  infiltration at K = `InfiltrationCmPerSec` · 10^(−2C), overflow past
  saturation into the pond, drainage above `FieldCapacity` at a rate falling
  10^(−1.5C), surface drying. Pack strokes from a tyre are scaled by the
  Proctor hump `DirtPackingEfficiency(M)`; tools are not. `DaDirt.Rain`,
  `DaDirt.Water`, `DaDirt.WaterSim`; debug view 3 shows ponds in blue. Pore
  water and pond are audited separately from dirt. Section 9d.
- **Phase D — terramechanics wheel (done 2026-09-21):** Bekker sinkage in closed
  form from load, width, radius and (n, k_c, k_φ) interpolated by compaction
  (k in log space) and weakened by saturation; the rut pressed per pass is
  `PlasticSinkage` (0.7) of it plus slip-sinkage; motion resistance
  R = b K z^(n+1)/(n+1) replaces the rolling coefficient; traction is the
  Mohr–Coulomb ceiling c·A + N·tan φ times the Janosi–Hanamoto build-up over
  the contact patch; roost volume = width × lug failure depth × shear speed,
  ejected at 0.85 of the slip speed, 35° up, as parcels. Section 9e.
- **Phase E — parcels (done 2026-09-21):** `Shaders/Private/DirtParcels.usf`.
  A parcel carries a real solid volume and its moisture; spawn requests split a
  scooped volume into equal parcels of the chosen diameter (`DaDirt.Parcel`,
  or `soil` for cohesion-sized clods); flight with quadratic drag a = K v²/d;
  bilinear heightfield contact with restitution by wetness (mud splats), Coulomb
  friction at the ground's φ (so a parcel keeps rolling down a face steeper than
  repose), rest detection, and an atomic fixed-point deposit that the next step
  folds into the layer. The audit counts ground + air. A quarter-million pool,
  drawn as tetrahedra only up to the highest live slot. Section 9f.
- **Phase F — soil presets:** sand / loam / hardpack as one switch, testbed
  sections built from each, and the FIM track assigned a soil per section (sand
  section, hardpack start straight).
