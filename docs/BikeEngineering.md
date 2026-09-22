# The bike, built the way the dirt was built

*Research and plan, 2026-09-21. Alex: "I really want this to be 100% what a
dirt bike is, we build each component, from the hubs on the wheels, to the
spokes on the rims, to the brakes, to the internal components of the
suspension. This is as if we were building a 3D model of a full dirt bike."*

This is the plan for Phase 2. Nothing here is built yet. It starts when Dirt
V1 is signed off (persistence, water grip, soil compounds, erosion and the
remaining ground items in ROADMAP.md). It is written the way `SoilPhysics.md`
was written before the dirt was built: the real physics first, real numbers
with sources, then what the simulator does with them and how each piece gets
measured before it is called done.

The rule carries over unchanged: **no magic numbers.** A part's behaviour comes
from its geometry, its material and its physics. If a tyre grips more, it is
because its knobs are taller or its pressure is lower, not because a grip
slider moved. Every number below that came from a manufacturer or a textbook is
marked with where it came from; every one I have supplied from general
knowledge is marked *(typical, confirm)* and goes on the list in section 9.

---

## 1. The reference machine

One bike, fully specified, so every component has a real answer. A 450 cc
four-stroke motocross bike, the class Alex rides in his head when he says
"dirt bike". The KTM 450 SX-F is the best-documented; the Japanese four are
within a few percent of every number.

| quantity | value | source |
|---|---|---|
| wheelbase | 1493 mm | [KTM 450 SX-F 2025, Dirt Rider](https://www.dirtrider.com/dirt-bikes/2025-ktm-450-sx-f-technical-information/) |
| steering head angle | 63.9° (rake 26.1°) | same |
| ground clearance | 343 mm | same |
| seat height | 958 mm | same |
| curb mass, no fuel | 102.6 kg | same |
| fuel | 7.2 L (≈5.3 kg) | same |
| front suspension | WP XACT 48 mm USD air fork, 310 mm travel | same |
| rear suspension | WP XACT monoshock with linkage, coil spring, 300 mm travel | same |
| primary drive | 29:72 (2.483) | [KTM technical specifications](https://www.ktm.com/en-int/models/motocross/4-stroke/2026-ktm-450-sx-f/technical-specifications.html) |
| clutch | wet DDS multi-disc, hydraulic | same |
| transmission | 5-speed | same |
| peak power | 59.9 hp (44.7 kW) @ 9360 rpm | [Dirt Bike Magazine dyno, 2023 450s](https://dirtbikemagazine.com/2023-450-mx-bikes-on-the-dyno-the-wrap/) |
| peak torque | 36.4 ft·lb (49.3 N·m) @ 7480 rpm | same |
| final drive | 13:48 (3.69) *(typical, confirm)* | [MXA on gearing](https://motocrossactionmag.com/ask-the-mxperts-understanding-motocross-gearing/) |
| tyres | 80/100-21 front, 120/80-19 rear | [Dunlop MX34 set](https://level10motorsports.com/products/dunlop-tire-set-geomax-mx34-front-80-100-21-rear-120-80-19) |
| rear tyre mass | 5.9 kg (Dunlop MX34 120/80-19) | [Maciag](https://www.maciag-offroad.com/dunlop-rear-tire-geomax-mx34-120-80-19-sid157602.html) |
| rims | 1.60 × 21 front, 2.15 × 19 rear, 36 spokes | [Excel A60](https://rkexcelamerica.com/products/excel-rims/), [D.I.D DirtStar](https://www.didchain.com/collections/dirtstar-rims) |
| front brake | 260 mm disc, Brembo two-piston caliper *(typical, confirm bore sizes)* | KTM spec sheet (page did not load; see section 9) |
| rear brake | 220 mm disc, single-piston *(typical, confirm)* | same |
| rider | 80 kg, gear included *(assumption)* | |

Derived: all-up mass with rider and fuel ≈ 188 kg. Static weight split about
50/50 on the axles with the rider seated *(assumption; measured on the sim's
scales once the chassis exists)*.

---

## 2. The component tree

This is the "3D model" list. Each line is a thing with mass, geometry and a
physical job; each becomes a parametric procedural mesh built from the same
numbers the physics uses, so the drawn bike and the simulated bike cannot
disagree.

```
Bike
├─ Front wheel
│  ├─ Tyre 80/100-21: carcass (bias ply), tube, tread knobs
│  ├─ Rim 1.60 × 21 (aluminium, spoke bed, bead seats)
│  ├─ 36 spokes + nipples (prestressed)
│  ├─ Hub (flanges, bearings), axle, spacers
│  └─ Brake disc 260 mm (floating or fixed), caliper, pads
├─ Rear wheel
│  ├─ Tyre 120/80-19, rim 2.15 × 19, 36 spokes, hub, axle
│  ├─ Sprocket 48 T, cush-less (MX hubs are rigid)
│  └─ Brake disc 220 mm, caliper, pads
├─ Front suspension: two fork legs
│  ├─ Outer tube (in the clamps), inner tube (48 mm), bushings, seals
│  ├─ Spring: air chamber + negative chamber (XACT AER) or coil (KYB/Showa)
│  ├─ Damping cartridge: base valve (compression), mid valve (piston),
│  │  rebound stack, bleed adjusters, bottoming cone
│  └─ Oil (5 W), oil level (air volume above it)
├─ Steering: triple clamps (offset 22 mm typical), steering head, bars
├─ Rear suspension
│  ├─ Swingarm (pivot, length ≈ 580 mm typical), chain slider
│  ├─ Linkage (rocker + pull rod): the rising rate
│  └─ Shock: spring (coil, 42–57 N/mm by rider), piston + shim stacks,
│     high/low-speed compression, rebound, nitrogen bladder reservoir
├─ Frame, subframe, footpegs, seat
├─ Drivetrain: engine (torque map, inertia), clutch, 5-speed gearbox,
│  countershaft sprocket 13 T, 520 chain, rear sprocket
└─ Rider: mass, posture (seated / standing / attack), inputs
```

---

## 3. The physics, component by component

### 3.1 The tyre — "the angles of the roundness"

A motocross tyre is a bias-ply toroid with tall, sparse knobs. Three things
about its shape decide how the bike feels, and none of them is a tuning knob.

**Section and crown.** A 120/80-19 is 120 mm wide with a sidewall 80 % of
that, 96 mm; the rim is 19 in (482.6 mm), so the outside diameter is about
675 mm and the rolling radius about 337 mm *(computed; confirm against a
tyre)*. The front 80/100-21 is narrower and taller: OD ≈ 693 mm. The tread
surface across the section is close to an arc, the **crown radius** r_c: for a
narrow MX rear it is of the order of the half width, 60–75 mm *(typical,
confirm by measuring)*; the front, narrower and rounder, 40–50 mm. This is the
"roundness". Its consequences:

- **Lean moves the contact point.** At lean angle φ the contact point sits
  r_c·sin φ to the inside and the effective rolling radius drops by
  r_c·(1 − cos φ). The rear tyre, with the bigger crown radius, walks further
  across its tread than the front as the bike leans, which is why the two ends
  of a bike do not turn on the same arc and why a rounder front "falls in".
- **Camber thrust.** A leaned rolling tyre generates a lateral force roughly
  proportional to lean at small angles even with no slip
  ([camber thrust](https://en.wikipedia.org/wiki/Camber_thrust)). On tarmac
  this is most of a motorcycle's cornering force. On dirt it is the knobs on
  the shoulder of the tyre digging into the surface: the edge knobs are taller
  and spaced for exactly this.
- **Contact patch.** The tyre deflects under load like a spring; the patch
  length is 2√(2 R δ) and the width 2√(2 r_c δ) for deflection δ. At 12–13 psi
  an MX tyre deflects about 14 mm under a 1 kN wheel load when the carcass is
  taken as a membrane carrying the load over a patch of N / p (derived in
  docs/SoilPhysics.md 6b, 73 kN/m at 12 psi, next to Cossalter's 180 kN/m for
  a road tyre at 2.3 bar): a patch about 19 cm long and 8 cm wide on hard
  ground. The test wheel now runs exactly this (`ADirtWheel::TyreDeflectionM`).

**Carcass.** Bias-ply, two or more cord plies at 15–45° to the circumference
([tyre patent survey](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/9290060)),
which makes the sidewall stiff in torsion (chain pull) and compliant radially.
Vertical stiffness at 12 psi is of the order of 100–150 N/mm *(typical,
confirm by a drop test)* with a few percent hysteresis; in the model the tyre
is a nonlinear spring plus damper between the rim and the ground reaction the
soil already supplies (section 4). Pressure is a parameter: it sets the
stiffness, the patch and how much the carcass rather than the soil deforms.

**Knobs.** MX knobs are 15–20 mm tall *(typical, confirm)*, sparse, and the
tyre's grip on soft ground is mostly the soil sheared between them (the lug
effect in terramechanics is handled with an equivalent radius and a failure
depth: [Taheri et al.](https://www.sciencedirect.com/science/article/abs/pii/S0022489814000664)).
The sim's `LugFailureDepthCm` and the Janosi–Hanamoto shear are already this;
the tyre model adds the geometry: knob height, spacing, edge-knob offset, and
knob bending stiffness (a knob is a rubber cantilever; a mountain-bike study
models tread knobs as springs in parallel with the carcass:
[Dressel & Sadauckas](https://www.mdpi.com/2076-3417/10/9/3156)). Soft compounds
flex more and wear faster; that becomes a compound parameter, not a grip one.

**What the tyre model must output**, per wheel, per substep: normal force
(soil bearing + carcass spring), longitudinal force (JH shear with slip, capped
by Mohr–Coulomb of the soil under the knobs), lateral force (camber thrust +
sideslip with the soil's lateral bearing on the knob edges), aligning moment
(from the patch offset and the trail), rolling and bulldozing resistance (the
wheel already has these), and the deformation handed to the dirt (rut, roost,
spray). The `MF-MCTyre` idea of a single first-order relaxation length for the
lateral force build-up ([Schmeitz, BMD 2010](http://www.bicycle.tudelft.nl/ProceedingsBMD2010/papers/schmeitz2010application.pdf))
applies: lateral force does not appear instantly, it builds over a fraction of
a wheel revolution.

### 3.2 The wheel as a structure

A spoked wheel is a prestressed structure: every spoke is pulled to a
pre-tension and the rim carries that as circumferential compression; an axle
load does not "hang" from the top spokes, it *unloads the bottom ones*
([Gavin, spoke fatigue](https://people.duke.edu/~hpgavin/papers/HPGavin-Wheel-Paper.pdf);
[Sheldon Brown](https://sheldonbrown.com/wheelbuild.html)). Motocross wheels
use 36 spokes, which are noticeably stronger than fewer
([Master Spokesman](https://masterspokesman.com/2020/08/11/wheel-components-strength/));
dirt-track spokes are about 3.2 mm (0.125 in) diameter and 220 mm long
([spoked wheel patent](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/6666525)),
MX spokes 8–9 gauge (3.7–4.1 mm) *(typical)*, pre-tensioned to the order of
1 kN each *(typical, confirm)*. Spokes on one side may run up to three times
the tension of the other to centre the rim over an offset hub.

For the simulator the wheel is: a mass and inertia (front ≈ 8–9 kg including
tyre, disc and axle; rear ≈ 12–13 kg with sprocket and the heavier tyre;
*typical, confirm from part masses*), a radial and lateral stiffness (the
spoke structure, tens of kN/mm, far stiffer than the tyre so it is nearly
rigid in the dynamics), a gyroscopic moment I·ω·(steer rate) that matters for
steering feel, and a set of parts to draw. The spoke tension only matters to
the physics when a spoke breaks; it matters a great deal to the model.

Hub: two bearings on the axle, flanges the spokes lace to, the disc carrier,
and on the rear the sprocket carrier. MX hubs have no cush drive: chain
impulses go straight into the wheel.

### 3.3 Brakes

Hydraulic disc brakes: lever → master cylinder piston → fluid → caliper
pistons → pads clamp the disc. Brake torque on the wheel is

    T_b = 2 · μ_pad · p · A_caliper · r_eff,   p = F_lever · (lever ratio) / A_master

with μ_pad 0.35–0.45 for sintered pads *(typical)*, front disc 260 mm
(r_eff ≈ 118 mm), rear 220 mm, front caliper two pistons of about 24 mm and a
master cylinder of 9–10 mm bore, rear one piston of about 26 mm with an 11 mm
master *(all typical, confirm against a Brembo/Nissin parts list — section 9)*.
The lever ratio and the piston areas give the hydraulic advantage; a 200 N
finger pull produces roughly 2 kN of clamp and, at the front, a brake torque
in the hundreds of N·m — more than the tyre can transmit on dirt, which is why
the interesting physics is at the tyre: locking, and the soil failing under a
sliding knob (the wheel's brake already behaves as a clutch that locks, and
the roost under a locked brake is thrown forward).

Also modelled: pad/disc friction fade with temperature (a disc is ≈ 1 kg of
steel absorbing the bike's kinetic energy), and the rear brake's role in
squat and rear steer.

### 3.4 The front fork

An inverted telescopic fork: 48 mm inner tubes in the triple clamps, outer
tubes on the axle, 310 mm of travel. Inside each leg (in WP's XACT, one leg
holds the spring, the other the damping cartridge; KYB/Showa put both in each):

**The spring.** Either a coil (KYB SSS springs are 4.4–5.0 N/mm per leg by
rider weight, *typical*) or an air chamber (WP AER: ≈ 10.5 bar at full
extension, *typical*). Air is a gas spring: F = (P₀ V₀ⁿ / (V₀ − A x)ⁿ − P_atm)·A
with a polytropic n between 1.1 and 1.3 at riding speeds, so it is
progressive by nature, and a negative chamber (a second air volume that pulls
the fork *down* at full extension) softens the initial travel. This is the
entire reason air forks feel different: "the spring force is position-sensitive
rather than speed-sensitive" ([ThumperTalk on AER valving](https://www.thumpertalk.com/forums/topic/1464987-wp-aer-xact-48mm-valving-and-solutions-restackor/)).
Both are modelled exactly: a coil is k·x plus preload; air is the gas law
with the measured volumes.

**The damping cartridge.** A sealed cylinder of oil with a piston on the
rod. Oil displaced by the rod must pass through valves:

- the **base valve** (compression): a port stack at the bottom of the
  cartridge that the rod's displaced volume passes on compression;
- the **mid valve** (on the piston): a check valve that opens on compression
  (in WP's AER "a single wave spring and check shim controls fluid flow", same
  source) and a rebound shim stack that the oil passes on extension;
- **bleed** orifices (the clicker screws) in parallel with the stacks, which
  dominate at very low shaft speed.

A **shim stack** is a set of thin steel discs clamped at the centre over the
piston's ports; oil pressure bends them open like a cantilevered plate, so
the flow area grows with pressure drop and the damping force versus shaft
velocity is *digressive*: steep at low speed (bleed + stiff stack), flattening
at high speed as the stack opens. Restackor's zones for a motocross setup:
ultra-low speed below 0.15 m/s targets a rebound/compression ratio near 0.8;
low speed force should stay under the bike's weight; the jump-landing zone is
3.8–4.6 m/s; above 5 m/s the damping force should match the spring force
([Restackor, compression damping](https://restackor.com/sample-apps/compression-damping)).
Damping force magnitudes are hundreds of newtons at low speed to a few
kilonewtons at landing speeds.

The model: each valve is an orifice with an area that is a function of the
pressure drop across it (bleed: fixed area; stack: area from plate bending
stiffness, preload/float and the port geometry), oil flow from the rod
displacement, pressure drop from the orifice equation Δp = ρ Q² / (2 C_d² A²),
force = Δp × rod area. That reproduces low-speed, high-speed, and the clicker
adjusters *from their geometry*, which is the point: the suspension tuner's
language (stack stiffness, crossover, float, bleed) maps one-to-one onto
parameters that are physical dimensions.

Also: a **bottoming cone** (hydraulic bump stop over the last 30–40 mm),
seal and bushing friction (20–40 N stiction, *typical*), oil temperature
(viscosity halves from cold to hot; fade after long motos is real), and the
**oil level**, which sets the trapped-air spring above the oil and is the
classic way to change bottoming resistance.

### 3.5 The rear shock and linkage

The shock is the same physics as one fork leg — coil spring, piston with
compression and rebound stacks, separate high- and low-speed compression
adjusters that split the flow between a stack and a poppet, a nitrogen
bladder reservoir at ≈ 10–12 bar *(typical)* that keeps the oil from
cavitating — but it acts through a **linkage**: a rocker and pull rod between
swingarm and frame that give the wheel 300 mm of travel from a shock stroke
of the order of 130 mm *(typical, confirm)*, at a **link ratio** (shock shaft
velocity over wheel velocity) that rises through the stroke. Dirt-bike
linkages "stay within a narrow range producing approximately a factor of two
increase in spring force through the stroke"
([Restackor, link ratio](https://restackor.com/physics/response/link-ratio)).
The wheel-rate is the shock rate times the link ratio squared, so the ratio
squares into both spring and damping at the wheel and rebound has to be
digressive to keep the damping ratio constant through the stroke (same
source). The spring on a 450 is 42–57 N/mm depending on rider weight
*(typical)*; race sag ≈ 105 mm with the rider aboard and static sag ≈ 35 mm
are the standard setup targets *(typical)*, and both fall straight out of the
linkage curve and the spring, so the sim's sag test is a check on the linkage
geometry, not a setting.

The linkage is modelled as the real planar four-bar (swingarm, rocker, pull
rod, frame mounts) so the ratio curve is computed from pivot coordinates, not
typed in.

### 3.6 Chassis geometry

- **Rake, offset, trail.** Rake 26.1°, triple-clamp offset 22 mm *(typical)*,
  front rolling radius ≈ 0.347 m → mechanical trail
  ≈ (R sin ε − offset)/cos ε ≈ 145 mm *(computed)*; trail is what makes the
  front wheel self-centre and what makes the bike steer by leaning. Both
  change with suspension position (the fork compressing steepens the rake)
  and with lean ([Cossalter, *Motorcycle Dynamics*](https://catalogimages.wiley.com/images/db/pdf/9781119950189.excerpt.pdf)).
- **Swingarm and anti-squat.** The chain's top run pulls the rear axle
  forward and up; whether the rear squats or lifts under power depends on the
  angle between chain line and swingarm relative to the centre of gravity.
  The construction: swingarm line and chain line meet at an instant centre;
  the line from the rear contact patch through it, extended to the front
  contact patch's vertical, gives the anti-squat height as a percentage of the
  CoG height. Near static sag it is close to 100 % on a sportbike and falls
  through the stroke ([datamc anti-squat](https://www.datamc.org/data-acquisition/suspension-data-analysis/anti-squat-geometry/);
  [Verdone](http://www.peterverdone.com/emotorcycle-anti-squat/)). On an MX bike
  this is why the rear hooks up out of a berm and why gearing changes the
  handling. Falls out of the geometry once the swingarm pivot, sprockets and
  CoG are real.
- **Centre of gravity.** Bike alone ≈ 0.6 m high *(typical)*; the rider moves
  it by 0.3 m standing versus seated, and fore-aft by leaning. The rider is a
  bigger control input than the bars.
- **Frame stiffness.** Torsional and lateral compliance of frame and swingarm
  are real tuning parameters at pro level (the "chassis flex" wars); first
  model rigid, measure what the rigid bike does wrong, then add the two
  compliances as springs at the steering head and swingarm pivot.

### 3.7 Drivetrain

Engine as a torque map: torque versus rpm and throttle from the dyno curve
(peak 49 N·m at 7480 rpm, 44.7 kW at 9360 rpm, rev limit ≈ 11,500), engine
inertia (crank + flywheel, the thing that makes a 450 "hit"), engine braking.
Clutch as a friction plate stack with a lever (the wheel's clutch-style brake
already has the maths of a slipping friction element). Gearbox: primary 2.483
× gear × final 3.69 (13:48). In first gear (≈ 2.3 *typical*) that is 49 × 2.483
× 2.3 × 3.69 ≈ 1030 N·m at the wheel, 2.9 kN of thrust at a 0.35 m radius —
three times the rear tyre's grip on loose dirt, which is why a 450 spins its
wheel at will and why the roost model matters. Chain pull is the force that
feeds anti-squat; sprocket teeth, chain pitch (520) and the countershaft
position are geometry the anti-squat construction reads.

### 3.8 The rider

An 80 kg mass with a controllable posture is most of the suspension on a
motocross bike: legs and arms absorb whoops and landings before the fork does.
Modelled first as a mass on stiff springs with damping at the pegs and bars,
with inputs: throttle, clutch, front and rear brake, steer torque, lean
(body offset), fore-aft (attack position), stand/sit. Later a proper
articulated rider if the feel demands it. The rider is what the player will
eventually be; the sim's scripted tests drive the same inputs.

---

## 4. How it fits the dirt

The wheel model that exists (`DirtWheel`) is already the contact layer:
Bekker sinkage, Janosi–Hanamoto traction, Mohr–Coulomb ceiling, bulldozing,
roost/spray/splash excavation, the rut pressed at the front of the patch,
the heap rule. The bike is built *on* it: two of them, with a tyre model
(3.1) between soil and rim, and a multibody bike above.

**Rigid multibody, own integrator, no physics engine.** Bodies: main frame
(with engine, rider mass attached through the rider springs), steering
assembly (clamps, fork uppers, bars), two fork lowers (sliding), swingarm,
linkage rocker and rod, front and rear wheels (spinning). Constraints:
steering axis revolute, fork telescopic, swingarm pivot, linkage pins, axles.
Forces: gravity, tyre forces at two contact patches, fork and shock spring +
damping (from the valve model), chain pull, brake torques, engine torque
through the clutch, aerodynamic drag. Integrated at 240–1000 Hz fixed step,
locked to the dirt's 60 Hz step the way the wheel is now. Chaos is rejected
for the same reason the test wheel never used it: the ground is the soil
model, not a collision mesh, and determinism across frame rates is
non-negotiable for the scripted tests.

**The tyre–soil hand-off** stays exactly what it is: every newton the tyre
puts into the ground is a stroke or a parcel the audit can see.

---

## 5. Build order

Each stage has a scripted test and a doc section with measured numbers before
the next starts, like the dirt.

- **B1 — The real wheel.** Replace the test wheel's point contact with the
  tyre model: crown radius and lean, carcass spring and damper, knob geometry,
  camber thrust and lateral relaxation, wheel mass and inertia, gyroscopic
  moment. Tests: tyre drop test (deflection versus load, at 3 pressures),
  lean test on the pad (lateral force versus lean and slip), the existing
  Plough and Hills scripts.
- **B2 — Suspension in the plane.** A 2D "skeleton" (frame + swingarm +
  linkage + fork as bodies, no steering) with the valve-model dampers and
  real springs. Tests: fork and shock dynos (force versus velocity, matched to
  the Restackor zones), linkage ratio curve, sag, whoops at speed (the rigid
  wheel's 69 cm hops in `SoilPhysics.md` 9j are the benchmark to beat), jump
  landing bottoming.
- **B3 — The chassis in 3D.** Steering geometry (rake, offset, trail),
  lean-to-steer, the rider mass and posture, anti-squat from the real
  sprockets. Tests: straight-line stability at speed, a berm carve without
  the hop, a flat corner on the pad at three lean angles.
- **B4 — Drivetrain and brakes.** Torque map, clutch, gearbox, chain pull,
  hydraulic brakes with pad friction and fade. Tests: standing start (wheel
  spin versus hook-up by soil), braking distance on three soils, front-brake
  stoppie threshold.
- **B5 — The rider.** Inputs, posture, the leg/arm springs. Tests: the
  whoops again, seated versus standing.
- **B6 — The model.** Every component drawn from its numbers: hub, spokes
  under tension, rim, tyre with its knobs, fork tubes sliding, shock and
  linkage moving, chain running over the sprockets, discs and calipers.
  Nothing hand-modelled: change a spec and the part changes.
- **B7 — Against reality.** Sag, dyno curves, lap behaviour and rider
  reports compared with the real bike; the suspension tuner's vocabulary
  (clickers, oil level, spring rate, link) working as it does on the real
  thing.

---

## 6. What the sim keeps from Phase 1

- The soil and the audit. Nothing in the bike bypasses the dirt.
- The scripted-test discipline: one `Tools/Bike*.txt` per component.
- Console-first: `DaDirt.Bike`, `DaDirt.Ride`, `DaDirt.Clicker`, `DaDirt.Spring`
  before any input assets.
- The status line: every quantity above (sag, fork/shock velocity, valve
  forces, lean, camber thrust, slip, chain pull, brake torque) printed twice a
  second while anything moves.

## 7. What is deliberately out of scope for Phase 2

Engine internals (combustion, valvetrain), electronics beyond a torque map,
crash and deformation, multiple bikes. A second rider model. Sound.

## 8. Risks

- **Numerical stiffness.** Tyre springs, valve models and a 9 kg wheel at
  1000 Hz; the traction cap the test wheel needed is a taste of it. Semi-
  implicit integration and per-constraint impulse limits from the start.
- **The rider is the suspension.** Without a plausible rider the bike will
  hop off everything; B5 cannot slip.
- **Data.** Fork and shock internals are proprietary; the model is built from
  the physics and tuned to published dyno zones and to sag, not from
  factory shim lists.

## 9. Numbers still to confirm (the shopping list)

1. KTM 450 SX-F owner's manual, technical data (gear ratios, tyre sizes, fork
   air pressure, shock spring rate and sag, brake disc sizes) — the KTM spec
   page did not load for me; ManualsLib has the manual's technical data page
   ([page 73](https://www.manualslib.com/manual/805931/Ktm-450-Sx-f.html?page=73)).
2. Brembo/Nissin MX caliper piston and master cylinder bores, pad friction.
3. Dunlop MX34 dimensions: OD, section, crown radius, knob height (measure a
   tyre with a ruler if the sheet does not say).
4. MX spoke gauge and tension spec (Excel/Bulldog wheel-building guides).
5. Front and rear wheel masses as assembled.
6. A shock stroke and linkage pivot layout for a real 450 (from a service
   manual drawing) so the four-bar is real, not typical.
7. Fork and shock dyno traces (Restackor sample apps, Race Tech) to match the
   valve model against.

---

## Sources

- KTM 450 SX-F 2025 specifications: [Dirt Rider](https://www.dirtrider.com/dirt-bikes/2025-ktm-450-sx-f-technical-information/), [Vital MX](https://www.vitalmx.com/product/guide/bikes/ktm/450-sx-f-16631), [KTM technical specifications](https://www.ktm.com/en-int/models/motocross/4-stroke/2026-ktm-450-sx-f/technical-specifications.html), [owner's manual technical data](https://www.manualslib.com/manual/805931/Ktm-450-Sx-f.html?page=73)
- 450 dyno numbers: [Dirt Bike Magazine](https://dirtbikemagazine.com/2023-450-mx-bikes-on-the-dyno-the-wrap/), [Dirt Rider CRF450R dyno](https://www.dirtrider.com/tests/honda-crf450r-dyno-test-2026/)
- Gearing: [Motocross Action](https://motocrossactionmag.com/ask-the-mxperts-understanding-motocross-gearing/), [JT gear ratio chart](https://www.jtsprockets.com/fileadmin/files/jtgearratio.pdf)
- WP XACT / AER forks: [WP XACT Pro 7448](https://www.wp-suspension.com/procomponents/offroad/xact-pro-7448/), [ThumperTalk AER valving thread](https://www.thumpertalk.com/forums/topic/1464987-wp-aer-xact-48mm-valving-and-solutions-restackor/), [Kreft Moto](https://www.kreftmoto.com/xact-aer-48-2023), [Keefer Inc.](https://www.keeferinctesting.com/comparing-the-wp-xact-6500-cartridge-kit-and-cone-valve-fork-on-the-track/)
- Damping and linkage physics: [Restackor compression damping](https://restackor.com/sample-apps/compression-damping), [Restackor link ratio](https://restackor.com/physics/response/link-ratio), [Penske on low/high-speed damping](https://www.penskeshocks.com/blog/what-is-the-difference-between-low-speed-damping-and-high-speed-damping), [MXA on rising rate](https://motocrossactionmag.com/death-of-the-rising-rate/), [ProMechA leverage and linkages](https://www.promecha.net/leverage-and-linkages), [Motorcycle.com on linkage](https://www.motorcycle.com/ask-mo-anything/the-missing-linkage.html)
- Tyres: [Dunlop Geomax MX34](https://www.dunlopmotorcycletires.com/tire-line/geomax-mx34/), [MX34 120/80-19](https://www.maciag-offroad.com/dunlop-rear-tire-geomax-mx34-120-80-19-sid157602.html), [rough-terrain tyre patent](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/9290060), [camber thrust](https://en.wikipedia.org/wiki/Camber_thrust), [relaxation length](https://en.wikipedia.org/wiki/Relaxation_length), [Schmeitz, BMD 2010](http://www.bicycle.tudelft.nl/ProceedingsBMD2010/papers/schmeitz2010application.pdf), [Dressel & Sadauckas, MTB tyre knobs](https://www.mdpi.com/2076-3417/10/9/3156)
- Terramechanics tyre models: [Taheri et al. survey](https://www.sciencedirect.com/science/article/abs/pii/S0022489814000664), [tractive tests on soft soil](https://www.sciencedirect.com/science/article/abs/pii/S0022489819301028)
- Wheels: [Gavin, spoke fatigue](https://people.duke.edu/~hpgavin/papers/HPGavin-Wheel-Paper.pdf), [Sheldon Brown wheelbuilding](https://sheldonbrown.com/wheelbuild.html), [Master Spokesman](https://masterspokesman.com/2020/08/11/wheel-components-strength/), [spoked wheel patent](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/6666525), [Excel A60](https://rkexcelamerica.com/products/excel-rims/), [D.I.D DirtStar](https://www.didchain.com/collections/dirtstar-rims)
- Chassis: [Cossalter, Lot, Massaro — Motorcycle Dynamics (excerpt)](https://catalogimages.wiley.com/images/db/pdf/9781119950189.excerpt.pdf), [datamc anti-squat geometry](https://www.datamc.org/data-acquisition/suspension-data-analysis/anti-squat-geometry/), [Verdone anti-squat](http://www.peterverdone.com/emotorcycle-anti-squat/), [Inside Motorcycles on anti-squat and gearing](https://www.insidemotorcycles.com/gearing-and-anti-squat/)
- Brakes: [Rocky Mountain ATV/MC brakes](https://www.rockymountainatvmc.com/parts/dirt-bike-brakes) (parts only; engineering figures still to confirm)
