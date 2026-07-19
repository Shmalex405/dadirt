# DaDirt — project context for Claude

Motocross game built dirt-first in Unreal Engine 5.8. See ROADMAP.md for phases;
Phase 1 (sandbox dirt simulator, "the Dirtbox") is the current focus.

## Working with Alex

Alex is new to both Unreal and programming. Claude does all engineering. Explain
what things are and why choices were made in plain language — short and clear,
not condescending. Never assume Alex can debug, edit code, or navigate the editor
without directions; when Alex must do something manually (installers, Epic
sign-in, editor clicks), give exact step-by-step instructions.

## Locked technical decisions

- **Engine:** UE 5.8 (binary install via Epic Launcher). Fallback to 5.7 only if
  5.8 proves unstable.
- **Toolchain:** Visual Studio 2022 Community, "Game development with C++" +
  Desktop C++ workloads.
- **Dirt architecture:** hybrid — GPU deformable heightfield (render-target
  ping-pong sim: deformation, volume conservation, angle-of-repose slumping,
  compaction + moisture channels) + Niagara GPU particles for loose/airborne
  dirt that settle back into the heightfield. Rejected: full granular/MPM sim
  (can't scale past ~1–2 m² on this GPU, dead end for a track-scale game) and
  pure heightfield (reads as clay, no life).
- **Sandbox interaction order:** hand tools (dig/push/pile/smooth/carve) first,
  then a driveable powered test wheel (slip digging, roost, compaction).

## Hardware budget (dev machine = target machine for now)

Intel Arc 140T integrated GPU, 32 GB RAM, Windows 11. Target 60 fps @ 1080p.
Consequences: keep heightfield sim ≤ 1024×1024, prefer a pre-tessellated grid
mesh with WPO displacement over Nanite tessellation until proven cheap, keep
Lumen/heavy features off in the sandbox map, budget Niagara particle counts.

## Conventions

- Project root: `C:\Users\alex\Desktop\DaDirt` (git repo). The UE project lives
  at the repo root: `DaDirt.uproject`, `Source/`, `Config/`, `Content/`.
- C++ module naming: `DaDirt` game module; dirt sim code under
  `Source/DaDirt/Dirt/`.
- Keep every phase-1 feature toggleable with debug visualization (height,
  compaction, moisture, volume audit) — dirt tuning is the whole game.
- Volume conservation is a hard invariant: any system that moves dirt must
  account for where it goes. The debug audit view must stay ~zero-sum.
