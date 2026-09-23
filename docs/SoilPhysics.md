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

**Standing water and the tyre (2026-09-22).** A puddle does three things to a
tyre, none of them a grip slider:

1. The soil under it is saturated whatever the moisture channel has had time
   to say, so the strength and stiffness the tyre feels are the saturated ones
   (φ × 0.4, cohesion gone, Bekker k × 0.2): mud.
2. The submerged front of the tyre has to push the water aside:
   R_w = ½ ρ_w C_d A v² with A = b × depth, C_d ≈ 1. Twenty centimetres of
   water at 6 m/s is ~500 N on a 100 kg wheel, which is what slows a bike
   through a puddle and throws the water.
3. Deeper than the knobs, the water cannot escape between them fast enough and
   the wedge of water under the patch carries part of the load: L = ½ ρ_w v² A_patch C_L.
   NASA's hydroplaning speed v = 6.36 √p (mph, psi) puts a 12 psi tyre at
   9.8 m/s, which on the tyre's own 25 cm patch is C_L = 0.68. The share of the
   load on water has no cohesion and no friction: the knobs float. Below knob
   height nothing floats; the water only wets the soil.

The pond depth reaches the wheel through the height window (a second readback
of the pond texture), the display height carries the water top so a puddle is
a flat sheet over the rut rather than a groove in it, and every dirt-surface
query takes the pond off again. Section 9k.

**5b. Erosion: run-off carrying dirt (2026-09-22).** Water that runs over
dirt shears the bed. The shear stress of a sheet of water h deep on a slope S
is τ = ρ_w g h S; grains move once it beats the soil's **critical shear** τ_c
(Shields: ~0.5 Pa for loose sand, a few Pa for loam, 8 Pa or more for a clay
that holds together; packing raises it), and they are detached at a rate set
by the soil's **erodibility**, D = k_d (τ − τ_c), the rill erodibility of the
WEPP family of models scaled to the game's water pace. The water can only
carry so much: its **transport capacity** is a volume fraction that grows with
τ, and what it carries beyond that settles at the grains' **settling
velocity** (Stokes: sand a few cm/s, silt 0.1, clay 0.02, which is why clay
water stays brown). Water that goes, by soaking in or drying, drops all of it.

The simulator keeps the suspended dirt as a second channel of the pond
texture, solid centimetres beside the water's depth. In the water pass the
run-off between cells carries sediment with it in proportion to the water
moved; then each cell computes τ from its pond depth and the steepest fall of
its water surface, detaches from the layer up to capacity, or drops the excess,
and dried cells drop everything. Settled dirt lands loose and soaked, taking
the surface over by the deposit fold's skin rule. Every centimetre detached
leaves the layer and every centimetre dropped returns to it, so the audit adds
a column, *in the run-off*, and the total still books to the baseline. The four
erosion numbers are soil properties (section 8, row 5 of the table).
`DaDirt.Erosion 0|1` switches it for attribution. Section 9m.

**5c. The skin: a crust over a tacky base (2026-09-23).** Alex's list: a dry
crust over a tacky base. Real dirt dries from the top: evaporation takes the
top centimetre in an hour and the base stays damp for days, because once the
top is dry the water below can only leave as vapour diffusing through it
(stage-two drying, the rate falling as the dry layer thickens). A track after
watering is that: a dusty crust the knobs break through to tacky loam, and a
line that gets worked becomes one layer again, blue-grooved. The same
two-layer column is what a few centimetres of loose roost on a hardpack line
are, or the infill of a crater: loose to the knobs and to the eye, hard to
the load.

So every cell is now a **skin over a base**. The state texture's compaction
and moisture are the skin's (what you see, what the knobs are in); the pond
texture carries the skin's thickness in bulk cm and, packed into one float,
the base's compaction and moisture. No skin means one layer, and then the base
is kept equal to the surface. The rules, each a physical one:

- **Deposits** (parcels landing, a dump, slump inflow, settled sediment) join
  the skin, mixed by mass; a column with no skin gets one and its old surface
  becomes the base. **Takes** (a dig, a scoop, slump outflow, erosion) come
  off the skin first and out of the base for the rest; when the skin is gone
  the base is the surface again. Breaking ground up (a tool's disturb, a
  shedding face) makes a loose skin of its top; nothing below is loosened.
- **Packing** works the skin (it packs and thins as it does), reaches the base
  by the share of the bearing depth the skin does not fill, kneads the skin's
  moisture toward the base's, and once skin and base match the skin is folded
  in. A dry crust worked by a tyre becomes the tacky line under it.
- **Water** enters at the surface at the skin's conductivity and fills the
  skin's pores first, then the base's; the skin drains into the base and the
  base drains away. Drying is in two stages, as in the field: while the base
  holds water above field capacity it resupplies the skin by capillary rise,
  the surface stays as wet as the base and what evaporates comes out of the
  base (stage one); once the base is down to field capacity the supply stops,
  the skin dries in the air (the drying rate is a flux, so a thin skin's
  moisture fraction falls many times faster than the wet depth's), the base
  dries only through the skin, choked by its thickness, and the drying front
  descends while the skin is drier than the base, to a few centimetres: the
  crust (stage two). Nothing dries under a pond; the pond itself evaporates at
  the potential rate.
- **Strength** for the slump reads the skin by its thickness against the
  depth a face fails through (5 cm): a thin dry crust over damp loam fails
  through the loam. The **tyre** reads the skin against knob height for grip,
  roost and dust, and against its own sinkage for the load: a loose skin on
  hardpack sinks as hardpack and dusts as loose.
- The audit's bulk and pore water count skin and base; solid centimetres are
  still one number and still book to the baseline.

`DaDirt.Probe` prints the skin and the base under it; `DaDirt.DebugView 7`
is skin thickness, `8` the base's moisture; `DaDirt.Evap <x>` scales the
drying for tests. Settings: `CrustSeedCm`, `CrustMaxCm`, `CrustGrowCmPerSec`,
`SkinEvapChokeCm`. Not yet: the cementation a dried loam crust has (a dry crust
is cohesionless here, so it crumbles rather than plates), and salts. Section 9o.

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

**A pile is not a half-space (bulldozing).** Bekker's pressure–sinkage assumes
soil confined on every side. Dirt that stands above the ground around it at the
scale of the wheel — a spoil pile, a rut shoulder, the mound a locked brake shoves
up at the end of a rut — has nothing behind it, and the tyre's leading face can
either climb it or shove it. It does whichever costs less:

    climb a heap of height h:  F_climb = N tan θ,   cos θ = 1 − h / r
    shove it (passive wedge):  R_b = b ( ½ γ h² K_p + 2 c h √K_p ),  K_p = tan²(45° + φ/2)

The share of the heap that holds is `carried = R_b / F_climb`, clamped to 1. For
a 12 cm loose dry pile (φ 32°, c ≈ 1 kPa): R_b ≈ 50 N against 1,150 N to climb,
so 4 % holds; the tyre sinks through the other 96 % to the ground the pile sits
on, and the dirt it sweeps through (b × thickness × distance) is moved to just in
front of it, where it piles up, stands higher, and pushes back with the square
of its height — the same expression, now as motion resistance, capped so it can
stop the tyre against the pile but never push it backwards. A packed damp lip
(φ 42°, c ≈ 10 kPa) of the same height carries ~60 % and is a kicker; a slope,
whose ground ahead is higher still, is not a heap at all and is climbed as
ground. The obstacle is judged over the whole thing in front of the tyre, so the
toe of a packed lip is held by the lip behind it, and anything lower than the
tyre's own static sinkage is inside the contact patch and is pressed, not
shoved. This is Bekker's bulldozing resistance term (Wong, *Theory of Ground
Vehicles*, ch. 2) turned into a kinematic rule for the contact. Section 9i.

**The tyre's own patch.** A pneumatic tyre flattens under its load whatever
the ground does: an MX tyre at 12 psi deflects 2–3 cm, a contact patch of
2√(2 r δ) ≈ 25 cm even on concrete. Bekker's rigid wheel alone gave a 7 cm
patch on hardpack, the Janosi shear could not build along it, and the tyre spun
on every packed face it met. The patch is now sinkage plus flattening in one
arc, and the flattening is no longer a fixed 2.2 cm: it is the tyre spring
derived from pressure and shape in section 6b (1.4 cm at 12 psi under 981 N).

**Where the rut is pressed.** At the front of the contact patch, where the tyre
first meets the ground, so the axle rides on floor it has already made. Pressed
under the axle, the unpressed ground ahead was a step the tyre had to climb
every stroke on top of R_c, which already charges for pressing it: a wheel at
quarter throttle dug itself in and never got going.

**→ Simulator (Phase D).** Replace the wheel's `tanh` with Janosi–Hanamoto on
(c_eff, φ_eff, N); replace the fixed rut-per-pass with Bekker sinkage from N, b
and the soil's (n, k_c, k_φ) derived from compaction; motion resistance from R_c;
roost rate from excavation, capped by available layer as now.

**A landing is Proctor's hammer (impact compaction, 2026-09-22).** The Proctor
test compacts soil by dropping a hammer on it: what packs dirt is energy per
volume, and a tyre touching down at v brings ½ m v² of it. Two springs in
series take that energy at one force F: the tyre, the linear spring section 6b
derives from its pressure and shape (73 kN/m at 12 psi), stores F² / 2k_t; the
soil, whose force at a punch depth z is Bekker's pressure over the patch the
tyre has made by then (b the crown's chord at that depth), gives

    F(z) = K b 2√(2r) z^(n+½),    W(z) = K b 2√(2r) z^(n+3/2) / (n + 3/2).

F² / 2k_t + W(z(F)) = ½ m v² is monotonic in F and is bisected. Then: the
ground under the patch (a disc of the patch's area) is packed at that load,
`PackPerPass · F / mg` before the Proctor moisture curve, exactly as a rolling
pass is packed at its load; the plastic share of the punch stays as a crater
whose rim is the heaved dirt (a dig, zero-sum); and past `SplashImpactMps` a
share ½ (1 − C) of the punched dirt squirts out from under the tyre as splash
instead of heaving, so loose dirt splashes half and packed dirt none. There is
no suspension yet: the whole hundred kilograms lands on the tyre, which is why
these hits are hard (a 1 m drop onto loose loam is about 7.5 g); the bike build
puts a spring and a damper between the wheel and the rest. A hop under 1 m/s
(a 5 cm fall off a rut shoulder) is not a landing and is left to the rolling
load, and a tyre re-landing in its own fresh crater within 0.3 s is not a
second landing. **Rolling dynamic
load:** the velocity the ground takes from the tyre each substep *is* its
normal force, m(−v_n)/dt — the weight on level ground, m v²/ρ more in a
transition or the bottom of a bowl. What it gives beyond the static load
grips more at once and, time-averaged over the stroke with the airborne
substeps counting nothing, presses the rut deeper and packs harder, capped at
`MaxDynamicLoadG` so a landing's first substep does not count twice. Read at
the instant instead, a hop's landing spike pressed the rut many times too deep
and the tyre stalled in its own trench. Landings go hard and hollow,
transitions pack, by themselves. Section 9n.

**Cornering: the sheared layer goes to the outside (2026-09-22).** In a corner
the tyre runs at a slip angle α: it slides sideways at v tan α while it rolls.
Lateral grip builds with the sideways shear displacement along the patch,
j = x tan α, by the same Janosi–Hanamoto law as drive traction, and the two
share one Mohr–Coulomb ceiling (a friction circle: what the drive takes, the
side has not got), so half the grip needs 5–10° of slip angle as on a real
tyre and a spinning rear has almost nothing left to hold the side and steps
out. Where the patch is sliding — the share of the ceiling the side is using —
the knobs drag the layer they are in sideways with them: knob height, or the
Bekker sinkage where the knobs do not reach, times the patch length, times the
slide speed, the roost rule turned through ninety degrees. That layer leaves
the line and is put down just outside the tyre's outer flank, loose, as the
outer shoulder (the line under the tyre stays pressed: the knobs took its top); sliding faster than `SpraySlipThresholdMps` a growing share of
it (all but a fifth by `SprayFullSlideMps`) is flung outward as spray instead.
Lap after lap the line sinks and packs and the outer shoulder grows into a berm
that nothing placed: the track develops from the line the rider takes.
`DaDirt.Orbit` is that rider in its simplest form, steering round a circle.
Section 9n.

---

**6b. The tyre is a real one (2026-09-22).** Alex: a round tyre with real
motocross numbers, so the dirt tests are pure. The test wheel now carries a
110/90-19 soft-to-intermediate rear, the size every 250 and 450 races on
(Dunlop Geomax MX33, Michelin Starcross 6 and Bridgestone Battlecross X30 are
all made in it), and can swap to the 80/100-21 front, a sand rear or a
hard-terrain rear with `DaDirt.Tyre`. The numbers and where they come from:

| | rear 110/90-19 | front 80/100-21 | source |
|---|---|---|---|
| outside diameter | 680 mm (19 in rim + 2 × 0.90 × 110) | 693 mm | the size code; tirecalculatorhub gives 693.4 for the front |
| section width | 110 mm | 80 mm | size code |
| tread width across the blocks | 105 mm | 76 mm | a little under the section |
| crown drop, centre to tread edge | 35 mm | 30 mm | Sumitomo's off-road tyre patent US 7,874,330: "from 30 to 40 mm", camber ratio 0.35–0.75 |
| crown radius (an arc through both edges) | 57 mm | 39 mm | (w² + d²) / 2d |
| block height | 19 mm | 13 mm | the patents' range "from 7 to 19 mm" (US 7,874,330) and "6 to 19 mm" (EP 3,047,981); soft-terrain rears at the top, fronts and hard-terrain rears near 12–13 |
| land ratio (block tops / tread area) | 0.20 | 0.22 | patents: "preferably from 10 to 30 %" |
| pressure | 83 kPa (12 psi) | 90 kPa (13 psi) | Motocross Action: 12 psi front and rear as the ballpark, 13–13.5 on hardpack, 11–11.5 in sand; the patents test at 80 kPa |
| tyre mass | 5.5 kg | 3.8 kg | Dunlop MX33 110/90-19 listed at 5.5 kg; MX34 front 8.35 lb |
| complete wheel | 11.7 kg | 8.0 kg | tyre + tube 1.0 + rim 1.7 (OEM 19 in Dirtstar 1,707 g) + hub 1.06 (KTM OEM 1,059 g) + spokes 0.9 + sprocket 0.5 + disc 0.4 + axle and spacers 0.6 |
| load on the wheel | 100 kg | 91 kg | a 2025 KTM 450 SX-F is 103.9 kg without fuel; with 5 kg of fuel and an 80 kg rider, 52 % of 189 kg sits on the back |
| tread rubber | 75–80 Shore A | | US 7,874,330 (not used yet) |

**What the shape does.** The crown is an arc of 57 mm radius, so the tyre
touches the ground in a strip whose width is the arc's chord at the depth it
sits: b = 2√(2 r_c (z + δ)) for a sinkage z and a flattening δ, never wider
than the tread. On hardpack that is 78 mm; in loam, where the tyre sinks
2–3 cm, it is the whole 105 mm. Bekker's b is that width, so the two settle
together (a narrower strip carries more pressure and sinks more). The rut,
the pack, the roost and the sideways shear are all as wide as the contact,
and the tyre reads the ground at its two contact edges. The shoulder blocks
are only reached by leaning, which the single wheel cannot do yet.

**The tyre spring, derived.** At pressure p a pneumatic carcass carries its
load over a patch of area about N / p. For a round tyre that patch is an
ellipse 2√(2Rδ) long and 2√(2 r_c δ) wide, area 2π δ √(R r_c), so

    δ = N / (2π p √(R r_c)),    k_t = 2π p √(R r_c) ≈ 73 kN/m at 12 psi,

a linear spring: 1.4 cm under the 981 N static load, a hard-ground patch of
192 × 78 mm. Cossalter (*Motorcycle Dynamics*) uses 180 kN/m for a road tyre
at about 2.3 bar; scaled to 0.83 bar that is 65 kN/m, so the derived number
sits where it should. This replaces the fixed 2.2 cm deflection, and it is the
k_t the landing model (section 6) shares the impact energy with. The wheel's
inertia is 0.8 m r²: tyre, tube and rim sit at the radius.

**Not yet.** Lean and the shoulder blocks; the knob-top pressure (the load
over the land ratio) as what actually bears on the soil, which Bekker's
gross-patch pressure understates; the carcass's share of the load beyond the
air; rubber hardness; a tyre mesh that is not a cylinder.

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

## 8. The soils (rewritten 2026-09-22)

A soil is a set of measured properties, and every cell of the box belongs to
one: a static soil map built with the terrain (the testbed's pad is loam with a
five-lane quilt across its north end; the track is loam with a sand section).
The shaders read a table of five float4 rows per soil through the cell's id;
the wheel reads the Bekker and Janosi numbers of the soil under it. **Wet and
dry are not soils.** Mud is any of these at saturation; dust is any of these
dry; a wet sand is sand with the moisture channel high. What differs between
soils is what a moisture *does* to them: how fast water goes in and out, where
the packing optimum sits, how much suction cohesion a damp state buys, how
the friction angle collapses.

| | sand | loam (default) | pnw | granite | clay |
|---|---|---|---|---|---|
| what it is | rounded quartz, beach and pit sand | classic track dirt, sandy loam | Pacific Northwest: dark silty loam, organic | American Southwest: decomposed granite, angular grit | hardpack clay / caliche, the SW base and any blue-groove line |
| porosity loose / dense | 0.44 / 0.36 | 0.48 / 0.34 | 0.52 / 0.36 | 0.42 / 0.33 | 0.45 / 0.30 |
| friction loose / dense, deg | 31 / 38 | 32 / 42 | 28 / 38 | 36 / 44 | 24 / 34 |
| suction cohesion peak, kPa | 2 | 3 | 6 | 1.5 | 8 |
| packed cohesion, kPa | 0.5 | 8 | 12 | 3 | 25 |
| unit weight, kN/m³ | 16.5 | 17 | 16 | 17.5 | 18 |
| friction lost saturated | 0.50 | 0.60 | 0.65 | 0.45 | 0.75 |
| K_s loose, cm/s (game-paced, fifty times the field) | 0.12 | 0.05 | 0.015 | 0.1 | 0.002 |
| field capacity | 0.10 | 0.30 | 0.45 | 0.12 | 0.40 |
| Proctor optimum moisture | 0.70 | 0.55 | 0.60 | 0.45 | 0.50 |
| drying rate × | 1.5 | 1.0 | 0.6 | 1.6 | 0.8 |
| dust × | 0.6 | 1.0 | 0.3 | 1.6 | 0.8 |
| Bekker n loose / dense | 1.1 / 0.9 | 0.9 / 0.5 | 0.7 / 0.5 | 1.0 / 0.6 | 0.5 / 0.4 |
| Bekker k_c loose / dense | 0.99 / 3 | 1 / 15 | 5.27 / 13 | 2 / 10 | 13.2 / 40 |
| Bekker k_φ loose / dense | 1528 / 4000 | 400 / 5000 | 1515 / 5000 | 1200 / 6000 | 692 / 8000 |
| Janosi K loose / dense, m | 0.015 / 0.025 | 0.02 / 0.045 | 0.03 / 0.05 | 0.015 / 0.03 | 0.04 / 0.06 |
| stiffness lost saturated | 0.5 | 0.8 | 0.85 | 0.55 | 0.9 |
| erodibility, solid cm/s/Pa | 0.010 | 0.005 | 0.006 | 0.008 | 0.003 |
| critical shear, Pa | 0.5 | 3 | 2 | 1 | 8 |
| settling velocity, cm/s | 3 | 0.5 | 0.1 | 4 | 0.02 |
| transport capacity per Pa | 0.002 | 0.003 | 0.004 | 0.002 | 0.005 |

Where the numbers come from: friction angles from the standard geotechnical
ranges (rounded sand 30–34°, angular sand and gravel 35–45°, silt 26–32°,
clay 20–28°; dense a few degrees more); porosities from typical void ratios
(sand 0.6–0.8, silt loam 1.0–1.2, clay 0.6–1.0); conductivities and field
capacities from USDA texture classes (sand drains in minutes at field capacity
~0.1, silt loam holds ~0.45, clay under 1 cm/h), scaled to the same game pace as
loam's 0.5; Proctor optima from the standard compaction curves (sands pack best
near saturation, clays at 15–20 % gravimetric); Bekker and Janosi loose values
from Wong's table in section 6 (dry sand 1.1 / 0.99 / 1528, sandy loam
0.70 / 5.27 / 1515, clayey soil 0.50 / 13.2 / 692), dense values from the same
soils compacted. Dustiness is the one number chosen by observation: a decomposed
granite track in August is a dust storm, a PNW loam is not. Every entry is in
`FDirtSoil::Presets` (DirtSoils.cpp) with the same names.

**Regional presets** are the last two columns. "pnw" is Washougal / Woodland
dirt: dark, organic, holds water for days, tacky when damp, packs into a
blue-groove line. "granite" is Glen Helen / Fox Raceway: decomposed granite
that dries in an hour, never really packs, roosts hard and dusts the whole
valley; "clay" is the caliche base under it and any watered, rolled hardpack.

**Tools.** `DaDirt.Soil` lists the soils; `DaDirt.Soil <name>` paints the whole
window (and survives Reset until `DaDirt.Soil built`); `DaDirt.Soil <name> x y r`
paints a disc; `DaDirt.Repose` edits the default soil, or the soil named as its
fifth argument. `DaDirt.Probe` names the soil at the point.

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

## 9b. Phase A measured (Tools/DirtboxSoil.txt, 2026-09-21; re-run 2026-09-22 after the soil table: 140.6 → 80.5, damp wall stands, soaked block runs out; re-run 2026-09-23 with the skin: the soaked block runs out slower, 23–29° at 6 s, see 9o)

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

Rut after four passes (2026-09-21 evening, passes started on fresh ground
2 m apart so a spin-up hole is not the next pass's pit): floor at 55.3 cm,
compaction 0.84, shoulders at 58.8–59.3 cm on a 60 cm
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
- *Spray off a berm*: the layer the sliding patch shears sideways (section 6,
  cornering) is flung outward, low and fast, for the share of it that a slide
  above `SpraySlipThresholdMps` earns; the rest stays as the outer shoulder.
  A hard turn on the pad (first version): 27,000 parcels, drift 0.00000.
- *A landing*: past `SplashImpactMps`, ½ (1 − C) of the dirt the tyre punches
  out of the ground (section 6, impact) squirts out both sides; the rest heaves
  the rim. Off the block's edge (first version): 22,000 parcels, drift −0.00001.
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

## 9h. The world outside the window (2026-09-21, later)

**Ruts persist.** The simulated window is now a view onto a bigger world. The
world is cut into tiles of a quarter of the window (10 m at a 40 m window, 256
cells each) and the window slides over it by whole tiles. A tile leaving the
window is read back from the GPU (state and pond) into a CPU cache; a tile
entering comes back from that cache or, the first time, from the builders,
which generate exactly that tile at the window's cell size. The slide itself is
one compute pass (`MainShiftCS`): staying cells move within the texture,
entering cells are read from a patch the CPU uploaded, bedrock is shifted on the
CPU and re-uploaded, the surface is recomputed. Before the slide, every parcel
over ground that is leaving lands at once into that ground (`MainParcelFlushCS`),
so the tile carries its dirt with it. Strokes queued in the old coordinates are
offset by the shift; height windows from before the slide are discarded by
generation. `DaDirt.Focus follow` keeps the window on the wheel, sliding when it
comes within a tile of the edge; `DaDirt.Focus <x> <y>` snaps to the tile grid
and slides there if the cell size is unchanged, so nothing is lost on the way.

**The ledger grew a column.** The baseline is now the sum of every tile ever
generated, and the audit counts ground + air + *tiles out of the window*, read
from the cache. `Tools/DirtboxPersist.txt`: a straight rut cut north over four
tiles, the window sent five tiles back to where it started, the rut probed
(2.1 cm deep, compaction 0.50 at the centre, 0.25 on the shoulders, exactly as
left), then a second pass south that deepens it to 3.9 cm at compaction 0.78.
Drift over the whole trip: +1.1 cm³ on 1,261 m³.

**A bias in the slump, found by the finer books.** Printing the drift in cm³
showed +11.6 cm³ appearing in six seconds from nothing but slumping over the
100 m³ block and the cut dome. Flows are moved in float: the giver subtracts
its total outflow, each receiver adds its share. Below half a float step of the
giver's depth (4 µm at 33 cm, 8 µm at 100 cm) the subtraction rounds to
nothing while the thinner receiver still gains, and every face that is still
creeping manufactures dirt. A floor of `DIRT_SLUMP_MIN_FLOW_CM` = 10⁻⁴ cm per
iteration per neighbour (a 0.03° excess at 4 cm cells) stops the creep instead:
the same six seconds now read +0.37 cm³, and the residual is a random walk
rather than a bias. The audit also counts cells holding less than no dirt and
warns if there are any; there are none.

**The far ground.** What the window does not simulate is still drawn: a CPU
mesh of the whole box, one section per world tile at 25 cm spacing, coloured
with the resolve pass's plain tint from the whole-site build, hidden under the
window and rebuilt per tile from the cache when a tile leaves, so a rut stays
visible after the window has moved on (`M_DirtFar`).

## 9i. A pile is not a ramp (2026-09-21, evening)

Alex: the wheel got air when it hit the end of a rut. The contact treated every
bump as concrete. `Tools/DirtboxPlough.txt` builds a 12 cm loose spoil pile
(25 cm across, compaction 0) and a packed damp one on the pad and drives at
them; the status line prints heap / carried / plough and air time.

| encounter | rigid contact | climb-or-shove |
|---|---|---|
| loose pile at 5.5 m/s | 0.80 s in the air, 52 cm up | a 0.03 s hop, the pile cut through and its dirt spilled ahead and to both sides |
| loose pile at quarter throttle (crawl) | | heap 8–11 cm under the tyre, carried 0.09–0.11, 60–100 N of push back, through at 0.5–1.3 m/s, the pile cut down to the pad |
| packed damp lip at 8 m/s | 0.5 s, 42 cm | carried ~0.6: still a kicker, 40 cm |

Things the pile exposed on the way:
- The traction integration chattered about zero at low throttle (the shear
  law is stiff against a 9 kg wheel at 240 Hz); a per-substep cap at the
  impulse that stops the slip removed it.
- The bulldozed spoil, dumped in a narrow heap ahead, stood as a 25 cm spire
  the tyre then stalled against; it is put down over the footprint a heap of
  that volume spreads to at its angle of repose, 40 % ahead and 30 % to each
  side, since a tyre is a blade with no wings.
- Judged against the mean of a ring, a hillside read as a 26 cm heap and the
  wheel tried to bulldoze the dome. The reference is now the highest point of
  two rings, one and two tyre radii out, read *aside* (90° and more off the
  heading, which follows the contour, so a hillside is level ground); a heap
  is what stands above both by about the same amount (a hill crest stands far
  taller over the far ring). A pile just ahead, not yet under the tyre, must
  also clear the far ring's forward sample: beyond a pile that sample is back
  at the base, beyond a slope it is higher still. Piles wider than the far
  ring (about 70 cm) are terrain and are climbed.
- Anything lower than the tyre's static sinkage is pressed as part of the rut,
  not shoved, so a packed rut floor is not churned by its own shoulders.

Drift in every case: under 0.05 cm³.

## 9j. Off the pad: the hills (2026-09-21, evening)

Alex: test on the hills and the other things, not just the flat terrain.
`Tools/DirtboxHills.txt` drives every shape in the testbed with the window
following the wheel at 3.9 cm cells. With the tyre's own patch and the heap
rule above (final run, 2026-09-22):

| shape | what happened |
|---|---|
| 7.5 m dome, 40° cosine face | climbs, slowing to 3.7 m/s at slip 0.31 on the steepest part, crests at 11 m/s and flies 2.6 m off the back |
| SX whoops, 90 cm at 4.3 m, 5–6 m/s | airborne 3.5 of 8 s, hops to 70 cm: a rigid wheel with no suspension skips them |
| FIM rolling waves, 80 cm at 10 m, 10 m/s | hops of 44 cm at most |
| 32° jump face, flat out (12 m/s) | 4.8 m of air, lands on the landing 10 m out |
| 22° rounded tabletop, 10.6 m/s | 1.3 m of air |
| 25° packed wedge, standing start | climbs at 3 m/s, slip 0.21, over the crest |
| 40° packed wedge, standing start | crawls up at 1.2–1.5 m/s, slip 0.48, 640 N of traction against 630 N of gravity |
| berm arc, 34° bank, 1.1 m | a 180° turn at 6–8 m/s; a 74 cm hop off the bank, again the rigid wheel |
| loose 3 m mound (compaction 0.3) | climbed at 5.5 m/s, sinking 1.8 cm |
| 3 m bowl, 10 m/s | through, a 53 cm hop off the far rim |

Drift in every section: 0.5 cm³ or less. Found on the way: the window could
not reach the last, partial row of tiles at the box edge, so the whoops at
Y = +60 were out of reach and a wheel parked at the window edge lost 150 cm³
to giving strokes clipped there; the window now reaches the edge and the audit
is clean. The two heap misfires above (hillside, crest) were both found here
and not on the pad.

## 9k. Standing water and the tyre (2026-09-22)

`Tools/DirtboxWaterGrip.txt`: a 3 m basin dug and packed on the pad, 2,500 L
poured in, a puddle 30 cm deep at the centre (deeper than meant; the basin is
a bowl). The status line prints pond / drag / lift.

| run | in the puddle |
|---|---|
| through at 8 m/s | drag 480–490 N, lift 350–400 N, grip 0.33, the wheel spins to 16 m/s in the mud; out at 5.7–5.9 m/s, a third of the speed gone |
| flat out, 10.4 m/s in | drag 1,120 N, lift 946 N of a 981 N load, traction 8 N: hydroplaning; out at 9.2 m/s |
| locked-brake stop from 7.1 m/s, dry pad | 693 N of retardation, about 3.7 m to stop |
| locked-brake stop from 7.5 m/s, into the puddle | 213–387 N in the water (grip 0.29–0.44), about 5.5 m to stop |

Pond and pore water are audited throughout (2.50 m³ ponded at the start,
2.47 m³ at the end: the packed basin lets under 0.001 cm/s through since
infiltration went to a tenth of its pace for erosion, section 5b; before that
it read 2.48 → 2.07 m³ in forty seconds); dirt drift under 0.5 cm³. Re-run
after that change: through at 7.9 m/s, drag 650 N, lift 420 N, grip 0.34, out
at 6.0 m/s; flat out at 9.7 m/s, drag 920 N, lift 970 N of 981 N, traction
3 N, out at 8.0 m/s; the locked-brake stops read the same. Not yet: the water
the tyre throws (a spray pool like dust), and puddles in the far mesh.

## 9l. The soils measured (2026-09-22)

`Tools/DirtboxSoils.txt` paints each soil over the pad and asks the same
questions. The cone is the testbed's loose cone at (3, −45), 3 m tall on a 2 m
radius, read as a profile 6 s after the reset (whole box, 12.5 cm cells); the
tyre is the 100 kg test wheel at rest and rolling at ~6 m/s; the puddle is
600 L on a 1.5 m disc after 6 s; the dust is what a 2 s burnout leaves in the
air.

| | sand | granite | loam | pnw | clay |
|---|---|---|---|---|---|
| cone face, deg (setting) | 32 (31) | 37 (36) | 33 (32) | 29 (28) | 25 (24) |
| sinkage at rest, cm | 2.4 | 1.9 | 2.9 | 0.6 | 0.4 |
| grip at rest / rolling | 0.66 / 0.68 | 0.79 / 0.83 | 0.75 / 0.79 | 0.70 / 0.77 | 0.70 / 0.81 |
| rut after one pass, cm (compaction) | 1.5 (0.25) | 1.7 (0.62) | 2.0 (0.39) | 1.6 (0.50) | 0.9 (0.41) |
| puddle left after 6 s, cm (moisture under it) | 15.8 (0.16) | 15.9 (0.16) | 16.0 (0.15) | 16.1 (0.15) | 16.1 (0.14) |
| dust after a 2 s burnout, motes | 207 | 480 | 312 | 92 | 264 |

(Re-run 2026-09-22 after infiltration went to a tenth of its pace for erosion,
section 5b. Before that the puddle row read 13.8 / 14.2 / 15.1 / 15.8 / 16.0 cm
with 0.39 / 0.36 / 0.25 / 0.17 / 0.15 moisture under it: the soils separated in
six seconds. Now they separate in a minute, in the same order, and six seconds
only shows the first millimetres of it. The rut row moves a few millimetres run
to run with where the brake locks; the order is the same.)

Every cone lands one degree over its friction angle, the same discretisation
bias the loam cone has always had (9b). Drift after every section: under
0.02 cm³. The quilt drive (one pass across all five lanes at Y = 25) reads the
same sinkages lane by lane, with the loam lane the pad's own numbers, which is
the point of putting loam in the middle. Not yet: the roost volume and the
lug failure depth do not depend on the soil (they should: sand roosts more,
clay less), and a parcel keeps the soil it lands on rather than the one it came
from.

## 9m. Erosion measured (2026-09-22)

`Tools/DirtboxErosion.txt`: a 1.2 m mound (22° flanks) raised on the pad,
then 60 s of rain at 60 mm/min (a cloudburst; the game's rain is in mm per
minute), in loam, sand and clay; then a rut driven up the loam mound and
rained on. Probes read the crest, the flank and the toe before and after,
and what is in the water. Erosion pace 3, infiltration at the new game pace
(a tenth of what it was, section 5b).

| | loam | sand | clay |
|---|---|---|---|
| mid-flank (Y = −10.5 / −9.5) | −0.6 cm | −0.3 cm | 0 |
| lower flank (Y = −8.5) | −4.2 cm | | |
| toe (Y = −7.5) | +4.5 cm, under a 14 cm puddle carrying 1.1 cm of dirt | +3.4 cm, 0.65 cm of sand in the puddle | 0, 0.03 cm of clay in a 22 cm puddle |
| in the run-off after the storm | 7.7 m³ | 2.8 m³ | 6.9 m³ |
| the big dome's packed flank (18, −20) | −4.0 cm | | |
| drift | +8 cm³ | +9 cm³ | +1 cm³ |

What the three columns say: loam erodes as sheet flow gathers down the
flank and drops its load where the slope dies, at the toe. Sand's grains
move at half a pascal but settle in a metre, so the flank thins a little and
the toe grows a lot. Clay's loose mound needs 8 Pa, more than a 22° sheet
flow supplies, so it stands; what clay is in the water came off the packed
dome flank under concentrated flow, and with a settling velocity of 0.02 cm/s
it stays there: the puddles are brown.

**The rut washes out.** A rut driven up the loam mound and rained on: on the
flank (Y = −14) the rut floor cut 1.4 cm deeper and its shoulder 1.3 cm, as
the run-off concentrated in it; at the mound's foot (Y = −16) the rut became
a channel 14 cm deep in water and filled with 2.5 cm of what came down it,
with 2.4 cm of dirt still in suspension. Drift +11 cm³.

Two things found on the way. The first pass read the shear from the model's
per-step water: a filling pond's head differences looked like slopes and
scoured 22 cm out of a clay toe, and a one-step rain sheet was too thin to
move anything on a flank; Manning depth from the throughput on the bed slope
fixed both. The second: at the old infiltration pace loose ground drank
0.5 cm/s, more than a cloudburst, so nothing ever ran off a loose mound and
only packed ground eroded; infiltration is now a tenth of that everywhere
(still fifty times the field). Not yet: rain-splash detachment, and the
rills are as fine as the cells let them be, no finer.

## 9n. Impact, corners and the real tyre measured (2026-09-23)

All with the 110/90-19 rear of section 6b (73 kN/m spring, rim bottoming at
6.9 cm), no suspension: the whole 100 kg lands on the tyre, so the g figures
are a rigid axle's and will fall when the bike build puts a spring and a
damper between the wheel and the rest.

**Drops (`Tools/DirtboxImpact.txt` A).** The wheel dropped 1 m (4.4 m/s) onto
the same spot five times, at 3.9 cm cells, probed at the centre after each hit.

| | loam (C 0.25) | sand (C 0.25) | hardpack clay (C 0.38) |
|---|---|---|---|
| first hit | 12 g, punched 13 cm | 15 g, 13 cm | 24 g, 6.3 cm |
| fifth hit | 33 g, 4.5 cm | 20 g, 8.6 cm | 56 g, 2.6 cm |
| crater after five, depth (floor compaction) | 17 cm (0.99) | 19 cm (0.92) | 9 cm (1.00) |
| drift | −0.03 cm³ | 0.00 | +0.01 |

Each hit finds a floor the last one packed, so the punch shrinks and the peak
load climbs; sand, whose Bekker stiffness grows least with packing, keeps
punching. Before the wheel read its own circumference it fell into each crater
it had punched and sand went to bedrock in five hits; now it rests on the
crater's rim.

**The jump landing (B).** The 32° face flat out, three times: 12 m/s off the
lip, 4.4 m of air, landing on the packed 22° ramp at 42–50 g with a 6–7 cm
punch. The landing packs to 1.00 where the tyre comes down and gains 1.2 cm of
splashed dirt a metre down the ramp (C 0.84); the rest of the ramp is
untouched. Drift −0.02 cm³.

**G-out in the bowl (C).** Two passes through the 3 m bowl at 6–10 m/s: the
rolling dynamic load reads 1.2–2.3 g at the bottom and packs it from 0.30 to
0.99, sinking the floor 4.5 cm. Drift −0.01 cm³.

**Corners (`Tools/DirtboxCorner.txt`).** The rider is
`DaDirt.Orbit` (a circle) and `DaDirt.Lap` (the track's centre line), and the
probes read a radial cross-section through the line.

*The flat circle (A).* An 8 m circle on the loam pad at a gentle throttle, 3.9 cm
cells. After 230 m (about five laps): a rut 2.2 cm deep packed 0.60–0.75 at
the line, and just outside it a loose shoulder 1.8 cm tall (compaction 0.00)
of sheared dirt, 36 L moved outward in all. After 370 m the rut is 3.6 cm
deep and packed to 1.00 with the shoulder still 1.8 cm; the line by then
wanders 20–30 cm lap to lap, which is the rigid wheel at 5–30° of slip angle
on its own ruts and shoulders, not the rider. Harder on the throttle (0.5) the
slip angle reaches 60°, the tyre spends a third of its time in the air off its
own shoulders and cuts two more lines 30 cm apart. Drift −0.08 to −0.28 cm³.

*The banked berm (B).* Ten passes through the testbed berm (34° bank, 1.1 m)
on one 14.6 m line at 8–10 m/s, slip angle 3–17°, each pass shearing
0.3–1.3 L outward and hitting the bank at up to 19 g. Cross-section at the
arc's middle, before / after five / after ten passes: the rut at the toe
(Y −14.9) 55.8 → 51.0 → 51.1 cm, packed 0.82 → 1.00; the shoulder against
the bank (Y −15.0) 57.4 → 60.4 → 59.9 cm, loose after five passes and packed
to 1.00 by the tenth as the tyre rides it; the face above (Y −15.1) up 0.4 cm.
A second line 40 cm inside cut 3.5 cm on the later passes. The berm grows
where the tyre leans on it and nothing placed the dirt. Drift −0.01 to
−0.03 cm³.

*The track (C).* `DaDirt.Lap` follows the centre line through the tightest
corner; at a third throttle the rigid wheel still flies 3.4 m off the first
jump it meets, lands at 34 g and buries itself, and while the wheel is on the
track the audit moves by ±100 cm³ at the window slides that follow it (slides
without a wheel, in track and testbed mode alike, audit at exactly zero, and
the same wheel on the testbed with the window following it audits clean). The
fault is in what the wheel's strokes, craters or parcels do at a slide on the
tiled track; it is logged in the roadmap and the track numbers are not quoted
until it is fixed.

**The hills again (`Tools/DirtboxHills.txt`).** Every shape again with the real tyre, the round contact
and the strips. Dome: climbs at 7.4 m/s, crests at 11.8 and flies 3 m off the
back; the rigid wheel lands at 32 g, punches 15 cm and stays in its crater (a
wheel with no suspension does). SX whoops at 6 m/s: hops of 50 cm, landings of
9–10 g. Rolling waves at 11 m/s: hops of 36 cm, 10–13 g. The 32° jump flat out:
4.4 m of air, a 32 g landing that punches 17 cm, and it drives on at 10.7 m/s.
Tabletop: 2.2 m of air. The 25° and 40° wedges are climbed from a standing
start. The berm arc: a 12.7 g hit into the bank, a 62 cm hop, 5.3 L sheared
into the outside of the turn. The mound is climbed without a hop; the bowl's
far rim throws a 20 g landing. Drift 0.08 cm³ or less in every section.

**Straight ruts (`Tools/DirtboxWheel.txt`).** Four passes down the same line at 3.9 cm cells: a rut 4.6–5.1 cm deep with its
floor packed 0.91–1.00, and narrow: 6 cm off the centre the ground is down only
1 cm, because a packed line is the crown's 78 mm strip, not the whole tread.
Shoulders of 0.1–0.4 cm, compaction 0.36–0.85 along the flanks. The burnout,
the straight run and the turn read as before; the turn's slither shears
5–7 L into the outside of the arc. Drift under 1 cm³ in every section.

## 9o. The skin measured (2026-09-23)

`Tools/DirtboxCrust.txt`, on the loam pad at 3.9 cm cells, drying at twenty
times the game pace (`DaDirt.Evap 20`) where it says so.

**A crust over a tacky base.** 800 L on a 3 m disc and 15 s of cloudburst,
then 100 s to soak in: the surface saturated under a puddle, the base at 0.34
(field capacity is 0.30). Drying, twenty times:

| after | skin | skin moisture | base moisture |
|---|---|---|---|
| soaked in | 0.5 cm | 1.00 (puddle 0.9 cm) | 0.34 |
| 30 s | 4.0 cm | 0.05 | 0.29 |
| 60 s | 4.0 cm | 0.05 | 0.26 |
| 120 s | 4.0 cm | 0.05 | 0.21 |

The base first drains to field capacity through the wet skin (stage one),
then the crust forms and reaches its 4 cm in under thirty seconds, and behind
it the base loses 0.05 a minute where the bare column of the old model lost
0.30 in ten. A crust in minutes, the base damp for hours: the order the field
has. Unwatered ground alongside dries to 0.14 in the same time.

**The wheel on the crust.** Three passes at half throttle over the crusted
pad: the line's skin is kneaded from 0.05 to 0.14 (the base is at 0.21) and
packed 0.45 → 0.77 with the base under it packed 0.40 → 0.65; where the line
packs to 1.00 the skin folds into the base and the column is one layer again.
A metre off the line the crust is untouched, 4 cm at 0.05. A burnout on the
crust throws 0.98 L and 247 motes of dust; the same burnout on watered ground
throws 0.60 L and no dust.

**Loose over hardpack.** A spot packed to 1.00, then 24 L thrown onto it: the
probe reads a loose skin 0.8–1.7 cm thick at compaction 0.39–0.75 over a base
at 1.00, and a pass of the wheel packs the skin to 0.77 without disturbing the
base. Drift −0.30 cm³.

**Regressions.** `Tools/DirtboxPersist.txt` (the four-channel pond through
every slide): drift 0.63 cm³ or less. `Tools/DirtboxSoil.txt`: the as-built
wall shears to 68 cm as before and the damp wall stands; the soaked block now
runs out at 23–29° after 6 s where 9b had 16°, because its surface is held
saturated by capillary supply from the base rather than read over the whole
wet depth, and the run-out is slower for it; the cone is unchanged at 32°.
Drift −1.8 cm³. The crust section itself carries a −75 cm³ dip from the
ponded, eroding pad that the flood fault in the roadmap describes.

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
  infiltration at K = the soil's K_s · 10^(−2C), overflow past
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
- **Phase F — soil presets (done 2026-09-22):** five soils as a table of
  measured properties on a static per-cell soil map; the testbed's quilt and
  the track's sand section built from them. Section 8, 9l.
