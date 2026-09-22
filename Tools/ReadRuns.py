# Reads the numbers out of a Dirtbox test log: python Tools/ReadRuns.py Impact Corner Hills Wheel
import re, sys, os, math
os.chdir(r"C:\Users\alex\Desktop\DaDirt")

def load(name):
    p = f"Saved/Logs/Run{name}.log"
    return open(p, encoding="utf-8", errors="ignore").read() if os.path.exists(p) else ""

PROBE = re.compile(r"Surface at \(([-0-9.]+), ([-0-9.]+)\) m is Z = ([-0-9.]+) cm  \[([a-z]+), layer ([0-9.]+) cm bulk / ([0-9.]+) solid, compaction ([0-9.]+), moisture ([0-9.]+)")
STATUS = re.compile(r"Wheel at \(([-0-9.]+), ([-0-9.]+)\) m hdg ([-0-9.]+): ([0-9.]+) m/s.*?slip ([-+0-9.]+) m/s \(i ([-+0-9.]+)\), grip ([0-9.]+), traction ([-0-9]+) N, sink ([0-9.]+) cm.*?heap ([0-9.]+) cm carried ([0-9.]+) plough ([0-9]+) N.*?lateral ([-0-9]+) N at ([0-9.]+) deg, dyn ([-+0-9.]+) g, impact ([0-9.]+) g ([0-9.]+) cm, patch [0-9.]+ mm wide at [-+0-9.]+ cm, compaction ([0-9.]+).*?(on ground|airborne) \(air ([0-9.]+) s, max ([0-9.]+) cm\), odometer ([0-9.]+) m, roost ([0-9.]+) L, ploughed ([0-9.]+) L, shoved ([0-9.]+) L")
SCRIPT = re.compile(r"DirtScript> (.*)")
DRIFT = re.compile(r"drift\s+([-+0-9.]+) m3.*?= ([-+0-9.]+) cm3")

def sections(text):
    """Yield (script line, log chunk after it)."""
    parts = SCRIPT.split(text)
    for i in range(1, len(parts) - 1, 2):
        yield parts[i].strip(), parts[i + 1]

def probes_in(chunk):
    return [(float(x), float(y), float(z), soil, float(bulk), float(solid), float(c), float(m)) for x, y, z, soil, bulk, solid, c, m in PROBE.findall(chunk)]

def summarize_status(chunk, every=1):
    rows = STATUS.findall(chunk)
    return rows

which = sys.argv[1:] or ["Impact", "Corner", "Hills", "Wheel"]
for name in which:
    text = load(name)
    if not text:
        print(f"== {name}: no log"); continue
    print(f"\n==================== {name} ====================")
    label = ""
    for line, chunk in sections(text):
        if line.startswith("wait") and len(line.split()) >= 3:
            label = line.split()[2]
            rows = STATUS.findall(chunk)
            if rows:
                sp = [float(r[3]) for r in rows]
                lat = [abs(float(r[12])) for r in rows]
                ang = [float(r[13]) for r in rows]
                dyn = [float(r[14]) for r in rows]
                imp = [(float(r[15]), float(r[16])) for r in rows if float(r[15]) > 0]
                air = max(float(r[19]) for r in rows)
                mx = max(float(r[20]) for r in rows)
                last = rows[-1]
                print(f"[{label}] {len(rows)} status: speed {min(sp):.1f}-{max(sp):.1f} m/s, lateral max {max(lat):.0f} N, slip angle {min(ang):.1f}-{max(ang):.1f} deg, dyn max {max(dyn):.1f} g, "
                      f"impacts {sorted(set(imp))[-3:] if imp else 'none'}, air {air:.2f} s max {mx:.1f} cm, odometer {last[21]} m, roost {last[22]} L ploughed {last[23]} L shoved {last[24]} L, last compaction {last[17]}")
        elif line.startswith("DaDirt.Probe"):
            for x, y, z, soil, bulk, solid, c, m in probes_in(chunk):
                print(f"    probe ({x:.2f}, {y:.2f}) Z {z:.1f}  bulk {bulk:.1f} solid {solid:.1f}  C {c:.2f} M {m:.2f} {soil}")
        elif line.startswith("DaDirt.Audit"):
            d = DRIFT.findall(chunk)
            if d:
                print(f"    AUDIT drift {d[0][1]} cm3")
        elif line.startswith("DaDirt.Wheel") or line.startswith("DaDirt.Orbit") or line.startswith("DaDirt.Drive") or line.startswith("DaDirt.Focus") or line.startswith("DaDirt.Mode"):
            print(f"  > {line}")
