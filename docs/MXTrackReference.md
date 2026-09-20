# What a motocross track actually is — reference numbers

Researched September 2026. Every number the dirt simulator uses should trace back
to something here. Where a real-world number contradicted a guess in our code, the
real number wins and the code comment says so.

Two governing sources: the **FIM Standards for Motocross, Arenacross/Supercross and
Supermoto Circuits** (the homologation rulebook, cited as article numbers below),
and industry/track-builder practice for the things regulations don't specify.

---

## 1. Outdoor motocross — the regulated skeleton

| Thing | Value | Source |
|---|---|---|
| Course length | **1.5 – 1.75 km** along the centre line (solo) | FIM 047.3.1 |
| Sidecar / Junior length | 1.5 – 2.0 km | FIM 047.3.1 |
| Width at narrowest | **≥ 5 m** solo, 6 m sidecar | FIM 047.3.2 |
| Recommended riding width | **≈ 8 m** | FIM 047.3.2 |
| Free vertical space | ≥ 3 m above the course | FIM 047.3.3 |
| Average race speed | capped at **65 km/h** over a full race | FIM 047.3.4 |
| Neutral safety zone | ≥ 1 m each side, earth banking ≈ 50 cm high | FIM 047.3.5, 047.4 |
| Separation of adjacent track passes | ≈ 10 m between contiguous tracks | FIM 047.3.8 |
| Start straight | **80 m min recommended, 125 m max** to the first bend, **no jumps** | FIM 047.5.2 |
| Start gate | 40 positions × 1 m = **40 m wide**, gate 500–520 mm high | FIM 047.5.3 |
| Course markers | ≤ 500 mm above ground | FIM 047.4 |
| Surface | natural (sand, dirt), must retain water, no concrete, no stones | FIM 047.3 |

### The rule that surprised me

**Whoops are illegal outdoors.** FIM 047.3.7: *"Washboards/Whoops sections are not
allowed in a course."* What outdoor tracks may have is **"rolling waves"**:

- peak-to-peak spacing **≈ 10 m**
- height limited to **≈ 80 cm**

So whoops belong to Supercross only. The testbed now carries both, side by side and
correctly sized — FIM rolling waves (10 m / 80 cm) and SX whoops (4.3 m / 90 cm) —
because they are genuinely different obstacles and ride nothing alike.

---

## 2. Supercross — the stadium version

| Thing | Value | Source |
|---|---|---|
| Course length | ≥ 300 m covered / ≥ 400 m open stadium; **World Champ: ≥ 400 / ≥ 500 m** | FIM 048.2.2 |
| Width at narrowest | ≈ 5 m, no sudden narrowing | FIM 048.2.3 |
| **Landing wider than takeoff** | landing zone **≥ 1 m wider** than the take-off point | FIM 048.2.3 |
| Free vertical space | ≈ 3 m | FIM 048.2.4 |
| Start gate | ≥ 20 m wide | FIM 048.3 |
| Start straight | 30 – 80 m, and **must be flat** to the first bend exit | FIM 048.2.8.3 |
| Racing surface area | ≈ 6,500 m² (70,000 sq ft) | industry |
| Dirt used | ≈ **4,200 m³** (5,500 cu yd), 500–600 truckloads | industry |
| Typical lane width | ≈ 6 m (20 ft) | industry |
| Start straight speeds | 80–97 km/h; track peak ≈ 97–105 km/h | industry |

**The dirt-depth number that matters to us:** 4,200 m³ over 6,500 m² = **~65 cm of
workable dirt** across the whole track. That is an independent check on our 60 cm
movable dirt layer — it is right.

---

## 3. Jumps

### Faces and lips

The take-off face angle is the single biggest lever on how a jump feels:

| Face angle | Behaviour at 15 mph |
|---|---|
| 35° | ~14 ft forward, 2.5 ft up |
| **45°** | **~15 ft forward, 3.8 ft up — maximum distance** |
| 55° | similar distance to 35°, but double the height (5 ft) |
| 80° | maximum height (7 ft), minimal distance |

Track builders talk in **run:rise ratios**. Outdoor faces are around **3:1** (≈18°
average), Supercross uses **2½:1 or 2:1** (≈22–27° average) because it needs height
in a short space. The *average* ramp is shallower than the *lip*, which is kicked
steeper — that difference is exactly what our `LipRoundM` parameter models, and it
is why a "sharp lip" and a "rounded lip" at the same average angle ride completely
differently.

### Sizes

| Level | Length | Height |
|---|---|---|
| Beginner (50–85cc) | 1.5–3 m | 0.3–0.9 m |
| Intermediate (125–250cc) | 3–6 m | 0.9–1.8 m |
| Advanced (250cc+) | 4.5–9 m | 1.5–3 m |

### Gaps and spacing

| Feature | Value |
|---|---|
| Supercross triple | **20–23 m** (65–75 ft) top-of-takeoff to top-of-landing |
| Rhythm lane spacing | **7.6–9 m** (25–30 ft) between jumps |
| SX whoops height | ≈ 0.9 m (3 ft) |
| SX whoops spacing | 2.4–3 m base spacing, ≈ 4.3 m (14 ft) peak-to-peak |
| FIM rolling waves | ≈ 10 m peak-to-peak, ≤ 80 cm high |

---

## 4. Corners and berms

| Thing | Value |
|---|---|
| Berm height | **0.3 – 1.2 m** (12 in to 4 ft) |
| Berm proportion | height ≈ ⅓ to ½ of berm width |
| Bank angle | steepening toward **near-vertical at the top** |
| Corner radius | tight hairpin ~2.5 m (trail) up to 5 m+ for fast corners; MX corners run larger |

**Our testbed berm was 2.8 m tall — more than twice the maximum a real berm reaches.**
It is now 1.1 m, and the bank profile steepens toward the top rather than being a
constant 40°.

---

## 5. Ruts — and why they decide our grid resolution

| Thing | Value |
|---|---|
| Rear tyre tread width | **110–120 mm** (110/90-19, 120/80-19) |
| Corner rut depth, typical | 15–30 cm |
| Supercross rut depth, extreme | up to **90 cm** (3 ft) |
| Rut shape | steep-walled, "like an ocean wave"; the steeper it is, the better it holds a bike |

This is the hard constraint on the whole simulator. A rut is **~12 cm wide at the
bottom** and maybe 25–35 cm across including its spoil edges. To make a rut read as
a rut rather than a single stair-step you want **4–8 cells across it**, so:

| Cell size | Cells across a 12 cm rut | Verdict |
|---|---|---|
| 3 cm | 4 | ruts look right |
| 6 cm | 2 | marginal |
| **12.5 cm** (Dirtbox at 128 m / 1024) | **1** | ruts are a single cell |
| 37.5 cm (track at 384 m / 1024) | 0 | no rut detail at all |

**Conclusion:** a single fixed grid cannot give both track scale and rut detail.
1024² at 5 cm covers only 51 m. So the sim grid has to become a high-resolution
window that follows the rider over a larger static track. That is now a known,
measured requirement rather than a vague "later" — see the goals in ROADMAP.md.

---

## 6. Soil — the numbers the simulation actually runs on

### Angle of repose

| Material | Angle |
|---|---|
| Dry sand | **34°** |
| Damp sand | **45°** |
| Water-saturated sand | **15–30°** |
| Dry clay lump | 25–40° |
| Wet excavated clay | **15°** |
| Gravel / crushed stone | 45° |
| General soil | 30–45° |
| Dry sand + fine clay | 38–42° |

**This corrected our moisture model.** We had cohesion peaking at half-saturation
and returning to the dry angle when fully wet. Reality is worse than that: saturated
soil is **weaker than dry** — wet clay slumps at 15°, well below dry sand's 34°. The
model now adds cohesion up to a peak and then applies a saturation penalty that takes
fully-wet dirt down to about 15°. That is why mud runs away from you.

### Composition

The industry standard track soil is a **sandy-clay loam**:

- **Clay 30–40%** — the binder. This is what lets a jump face or a berm hold its shape.
- **Sand/silt 60–70%** — drainage and the loose "loam" riders want for cornering grip.

Named conditions, and what each one means for the sim:

| Condition | What it is | Sim parameters |
|---|---|---|
| **Loam** | the good stuff: grip, drains well, stays soft, keeps changing through the day | mid compaction, moisture ≈ 0.4 |
| **Blue groove** | hardpack clay polished by tyres, slick, almost no traction | compaction → 1.0, low moisture |
| **Sand** | soft, "washy", holds a shallow angle, completely different technique | low compaction, low cohesion |
| **Mud** | saturated, slumps, no shape | moisture → 1.0, repose collapses to ~15° |

### Track preparation

- Base ripped **18–30 cm** (7–12 in) deep with a plough before events
- Tilled roughly monthly to stop a hard crust forming
- After a race: graded and smoothed back to contour, soft spots filled, then
  **aerated to loosen the compacted upper layer**

So a real track has a loose worked layer of 18–30 cm over a firmer base, and a built
Supercross track has ~65 cm of dirt total. Our 60 cm movable layer over undiggable
bedrock is a fair model of the built case; the ripped-depth number is what the
initial compaction profile should reflect.

---

## 7. Elevation

Regulations say nothing about elevation change; terrain does.

| Track | Elevation change |
|---|---|
| Glen Helen (CA) | **≈ 137 m (450 ft)** — the extreme, billed as unmatched worldwide |
| Monster Mountain (AL) | ≈ 24 m (80 ft) |
| Typical outdoor national | tens of metres, following natural hillside |

Outdoor tracks are routed over natural terrain and use it: uphill starts, downhill
braking bumps, off-camber traverses. Supercross, on a stadium floor, has effectively
zero natural elevation — every metre of height is built dirt.

Our generated track uses **≈ 22 m of elevation range**, in the normal outdoor band
rather than the Glen Helen extreme.

---

## 9. How a track is actually built

This is the section that matters most, because it is how our generator works.

Tracks are earthworks. The sequence and the machines:

| Stage | Machine | What it does |
|---|---|---|
| Site assessment | — | land size, soil type, topography, drainage |
| Bulk earthworks | dozers, excavators, graders | move the site to the design shape |
| Cut and carry | wheel tractor scrapers | cut material, haul it, spread it in controlled layers |
| Lanes and pads | dozers | build the running surface, mould jumps into rough shape |
| Jumps | wheel loaders, excavators | carry dirt in, roll and pack it into shape |
| Finishing | riders + operators | "finishing touches" to jump faces, with a rider present (FIM 047.3.6) |

**Where the dirt comes from.** Two sources, and real tracks use both:

- **On site**, by cut and fill. Grading a corridor across rolling ground produces
  spoil from the high spots that goes straight into the low spots.
- **Borrow pits.** A borrow pit is a hole dug purely to supply material. The standard
  advice for a flat site is blunt: dig a pond, and use what comes out to build the
  pad and the jumps. The pond stays as a feature.
- Supercross, built on a stadium floor with no site to cut, **imports all of it** —
  ~4,200 m³, 500–600 truckloads.

**This is why our generator is a construction process and not a shape function.**
The track's grade comes from the natural ground. The corridor is cut and filled to
it. Jumps and berms are built from the spoil. Borrow pits are dug until the ledger
balances. If a feature appeared without dirt having been moved to make it, the
simulator would be lying about its own hard invariant.

---

## 8. What this changed in our code

| Was | Now | Why |
|---|---|---|
| Saturated dirt returns to the dry repose angle | drops to ≈ 15° | wet clay 15° vs dry sand 34° |
| Berm 2.8 m tall, constant 40° bank | 1.1 m tall, height ≈ ⅓ of width, face steepening to 59° | real berms are 0.3–1.2 m, ⅓–½ as tall as wide |
| No repeating-bump feature at all | FIM rolling waves (10 m / 80 cm) *and* SX whoops (4.3 m / 90 cm), side by side | whoops are banned outdoors; the two ride nothing alike |
| Jump faces guessed | 22°/32°/45°/70° kept, but lip rounding now tied to run:rise practice | 45° = max distance; SX runs 2:1 |
| 60 cm dirt layer (guess) | 60 cm (confirmed) | SX uses 4,200 m³ over 6,500 m² ≈ 65 cm |
| No track-scale context | 1,520 m circuit, 8 m wide, 384 m site | FIM 1.5–1.75 km, 8 m recommended |
| Track stamped as analytic shapes into bedrock | track *built* by cut, fill, spoil and borrow pits, with a ledger | tracks are earthworks; dirt has to come from somewhere |
| Elevation an invented harmonic curve | natural site ground, smoothed along the centre line, clamped to 22% | tracks follow the land they are cut into |
| Neutral-zone banking saturated into a 20 m wide plateau | a 2 m wide bank that rises and falls back | FIM says *banking* ~50 cm, not a terrace — the bug was 30,000 m³ of phantom dirt |

---

## Sources

- [FIM Standards for Motocross, Arenacross/Supercross and Supermoto Circuits](https://www.fim-moto.com/fileadmin/library/2013-6520003_eng.pdf) — Appendix 047 (motocross), Appendix 048 (supercross)
- [FIM Standards for Motocross and Supermoto Circuits 2023](https://www.fim-moto.com/fileadmin/user_upload/Documents/2023/CIRCUIT_RULES_2023.pdf)
- [Inside the Canadian GP Supercross Track at McMahon Stadium](https://worldsupercrosschampionship.com/canadian-gp-mcmahon-stadium-supercross-track-explained/) — dirt volumes, surface area, speeds
- [Supercross 101 — Monster Energy AMA Supercross](https://www.supercrosslive.com/sx101/) — whoops, triples, obstacle definitions
- [Motocross track designs: understanding tracks and layouts — Red Bull](https://www.redbull.com/us-en/motocross-track-designs-layouts)
- [Dirt Bike Track Design: Your Ultimate Motocross Track Guide — Full Throttle](https://fullthrottle.mx/dirt-bike-track-design/) — jump sizes by skill level, face angles
- [Angle of repose — Wikipedia](https://en.wikipedia.org/wiki/Angle_of_repose) — material angle table
- [Predominant Types of Soil Used in Motocross Tracks — Black Widow](https://www.blackwidowpro.com/blog/dirt-bike/types-of-soil-for-tracks-in-motocross/b/bwdbm3/)
- [Best Soil for Riding Dirt Bikes — MotoSport](https://www.motosport.com/blog/best-soil-for-riding-dirt-bikes) — loam, blue groove, sand
- [Track Prep: Cutting Depth — Vital MX](https://www.vitalmx.com/forums/Moto-Related,20/Track-Prep-Part-1-Cutting-Depth,1307190) — ripping depth
- [Ten Things About Mastering Deep Ruts — Motocross Action](https://motocrossactionmag.com/ten-things-about-mastering-deep-ruts/)
- [Tire Sizing Questions Answered — PulpMX](https://pulpmx.com/2022/04/06/tire-sizing-questions-answered/) — tread widths
- [Glen Helen National Track](https://www.glenhelen.com/national-track) — 450 ft elevation
- [How to Build the Perfect Berm — MTB Trail Building](https://mtbtrailbuilding.com/tutorials/how-to-build-the-perfect-berm) — berm height/width proportion
- [Motocross Track Builders — Trackworks Design](https://trackworksdesign.com.au/earth-moving-contractors-and-the-art-of-constructing-motocross-tracks/) — bulk earthworks, site assessment
- [Jarryd McNeil's Guide to Building Motocross Tracks — Caterpillar](https://www.cat.com/en_US/articles/ci-articles/jarryd-mcneil-building-motocross-tracks.html) — what each machine does
- [Back yard MX track — Vital MX](https://www.vitalmx.com/forums/Moto-Related,20/Back-yard-MX-track,1337633) — dig a pond, build the jumps from it
