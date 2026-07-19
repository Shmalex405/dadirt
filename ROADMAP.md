# DaDirt — Motocross Game Roadmap

**Vision:** A motocross game where the dirt is the star. Before bikes, tracks, or
racing, we build dirt that has real structure and life — dirt you'd believe if you
were playing in an actual sandbox. Every later phase stands on top of that.

**Engine:** Unreal Engine 5.8 · **Language:** C++ (with Niagara GPU particles)
**Target:** 60 fps at 1080p on the dev machine (Intel Arc 140T iGPU, 32 GB RAM)

---

## Phase 0 — Toolchain *(DONE — July 19, 2026)*

Get the machine ready to build.

- [x] Epic Games Launcher installed
- [x] Visual Studio 2022 Community + "Game development with C++" workload
- [x] Unreal Engine 5.8.0 installed
- [x] DaDirt C++ project created, compiles (98 s), opens in editor (78 s init)

## Phase 1 — The Dirtbox (sandbox dirt simulator) ★ CURRENT FOCUS

A contained dirt box (roughly 30 m × 30 m) where the only goal is dirt that feels
real. We don't leave this phase until playing in it feels like a real sandbox.

**Architecture decision (locked):** hybrid heightfield + particles.
The ground is a GPU-simulated deformable heightmap — ruts, berms, and piles that
persist and conserve volume (dug dirt has to go somewhere). Loose dirt is a layer
of GPU particles (Niagara) that kick up, scatter, and settle back into the surface.
This is the approach real MX titles use and it scales from a sandbox to a full track.

### 1a — Deformable ground core
- Heightfield simulated on the GPU (render targets), rendered as a displaced mesh
- Dig / raise deformation brushes working end to end
- Volume conservation: removed dirt piles up nearby, added dirt comes from somewhere
- Physics objects (a dropped ball) read the deformed ground and rest in dents

### 1b — Granular behavior (what makes it feel like *dirt*, not clay)
- **Angle of repose:** slopes steeper than dirt can hold slump and avalanche
  naturally — piles form real cones, trench walls crumble
- **Compaction states:** loose fluffy dirt vs packed dirt (packed resists digging,
  loose moves easily; riding over loose dirt packs it down)
- **Moisture parameter:** dry sand → tacky loam → mud; changes slump angle,
  color, and how well dirt holds shape

### 1c — Loose dirt particle layer
- Niagara GPU particles spawn when dirt is moved aggressively
- Particles collide with the heightfield, roll/bounce, then settle and
  write their volume back into the ground (nothing vanishes)

### 1d — Sandbox tools & feel
- Tools: dig, push, pile, smooth, carve; droppable objects (ball, plate, block)
- Debug views: height, compaction, moisture, volume-conservation audit
- Camera + controls that make poking at dirt satisfying

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
