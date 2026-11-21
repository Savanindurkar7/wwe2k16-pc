#!/usr/bin/env python3
"""Find WWE 2K16's scene-target builder: the function the WWE 2K14 port hooks to build the 3D scene without MSAA.

Why: in entrances the native renderer falls back to the slow emulated path on every frame because the scene is drawn
with MSAA (log: "[native][truth] ... fallbacks N (msaa ...)"). The 2K14 port fixes it in the game itself
(src/tiling_hooks.h in Jarrod Feng's wwe2k14-recomp): its sub_82C22AF8(scene object, MSAA type, layout flag, device)
rebuilds a scene object's colour + depth targets. WWE 2K16 is a later version of the same Yuke's engine, so the same
function exists at another address, probably with the same shape:

  - reads a custom layout at scene object + 296 and + 300 (both > 0 -> custom layout),
  - calls a layout filler (256-byte layout at object + 12), then creates the colour target (+272) and depth (+276),
  - stores the requested type / flag at object + 4 / + 8.

This scans the generated C++ (generated_k14/, each instruction is in a "// opcode operands" comment) and ranks the
functions by how much of that shape they have. Read only. Usage:

  python3 tools/find-scene-targets.py            # top 15 candidates
  python3 tools/find-scene-targets.py sub_XXXX   # one function's instructions (to send for a closer look)
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
GEN = ROOT / "generated_k14"
FUNC = re.compile(r"^DEFINE_REX_FUNC\((\w+)\)")
INSN = re.compile(r"^\t// ([a-z.+-]+) ?(.*)$")
MEM = re.compile(r"(-?\d+)\((r\d+)\)")


def functions():
    for path in sorted(GEN.glob("*.cpp")):
        name, body = None, []
        with open(path, errors="replace") as f:
            for line in f:
                m = FUNC.match(line)
                if m:
                    if name:
                        yield name, body
                    name, body = m.group(1), []
                    continue
                if name:
                    i = INSN.match(line)
                    if i:
                        body.append((i.group(1), i.group(2)))
        if name:
            yield name, body


def score(body):
    loads, stores = {}, {}
    calls = 0
    cmp_zero = 0
    for op, args in body:
        m = MEM.search(args)
        if op in ("lwz", "lwzx") and m:
            loads.setdefault(int(m.group(1)), set()).add(m.group(2))
        elif op in ("stw", "stwx") and m:
            stores.setdefault(int(m.group(1)), set()).add(m.group(2))
        elif op == "bl":
            calls += 1
        elif op in ("cmpwi", "cmplwi") and args.endswith(",0"):
            cmp_zero += 1
    s, why = 0, []
    common = loads.get(296, set()) & loads.get(300, set())
    if common:
        s += 6
        why.append("loads +296/+300 from " + ",".join(sorted(common)))
    if stores.get(4, set()) & stores.get(8, set()):
        s += 3
        why.append("stores +4/+8")
    if 272 in loads or 272 in stores or 276 in loads or 276 in stores:
        s += 2
        why.append("uses +272/+276")
    if 288 in loads:
        s += 1
        why.append("reads +288")
    if 4 <= calls <= 20:
        s += 1
        why.append(f"{calls} calls")
    if cmp_zero >= 2:
        s += 1
    if not (150 <= len(body) <= 900):
        s -= 2
    return s, why, len(body)


def main():
    if not GEN.is_dir():
        sys.exit("generated_k14/ not found: run tools/native-stage1.sh first")
    if len(sys.argv) > 1:
        want = sys.argv[1]
        for name, body in functions():
            if name == want:
                print(f"{name}: {len(body)} instructions")
                for op, args in body:
                    print(f"  {op} {args}")
                return
        sys.exit(f"{want} not found")
    ranked = []
    for name, body in functions():
        s, why, n = score(body)
        if s >= 6:
            ranked.append((s, name, n, why))
    ranked.sort(reverse=True)
    print("WWE 2K16 scene-target builder candidates (score, function, instructions, evidence):")
    for s, name, n, why in ranked[:15]:
        print(f"  {s:2d}  {name}  {n:4d}  " + "; ".join(why))
    if not ranked:
        print("  none with loads of +296 and +300 from one register: the 2K16 layout offsets differ; send this output")
    print("\nSend this whole output. For the top one, also: python3 tools/find-scene-targets.py <name> > out/scene.txt")


if __name__ == "__main__":
    main()
