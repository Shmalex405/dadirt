# What dirt is — a soil-science model for the Dirtbox

Written 2026-09-21 at Alex's request: stop tuning angles by feel and derive the
simulator from how soil actually behaves. Everything below is standard soil
mechanics and terramechanics (Terzaghi, Mohr–Coulomb, Proctor, Culmann, Bekker,
Janosi–Hanamoto). Each section ends with **→ Simulator**: what it means for our
grid. Section 10 is the implementation plan; Phase A of it is implemented.

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

## 10. Implementation plan

- **Phase A — strength (done 2026-09-21):** Mohr–Coulomb φ_eff and c_eff per cell
  from compaction and saturation; Culmann standing height in the slump and the
  stability view; wheel grip = tan φ_eff + c_eff A / N. Settings: `LooseReposeDeg`
  (φ loose), `PackedReposeDeg` (φ dense, now 42), `SuctionCohesionKPa`,
  `PackedCohesionKPa`, `UnitWeightKNm3`, `SaturationFrictionLoss`.
- **Phase B — solid-volume conservation:** porosity from compaction; packing
  shrinks the layer; audit reports solid m³; Scoop/Dump carry solid volume and
  arrive loose (expanding).
- **Phase C — water:** drain / run-off / evaporate passes; K from compaction;
  Proctor-shaped packing; `DaDirt.Rain`, `DaDirt.Water`; a moisture debug view
  that shows ponded water.
- **Phase D — terramechanics wheel:** Bekker sinkage, Janosi–Hanamoto traction,
  compaction resistance, excavation-based roost.
- **Phase E — parcels:** the particle layer with volume-conserving hand-off, the
  audit extended to airborne dirt. Alex's headline feature.
- **Phase F — soil presets:** sand / loam / hardpack as one switch, testbed
  sections built from each, and the FIM track assigned a soil per section (sand
  section, hardpack start straight).
