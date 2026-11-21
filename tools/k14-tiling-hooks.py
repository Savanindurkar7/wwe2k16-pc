#!/usr/bin/env python3
"""Find WWE 2K16's scene-target builder in generated_k14/ and hook it, so the game can build its 3D scene without MSAA
(Options > Graphics > Anti-aliasing (MSAA) > OFF, cvar wwe2k16_force_no_msaa).

Why: with the game's 2x MSAA the native renderer cannot take the scene (its truth frames are 1x only): entrances fall
back to the slow emulated path on every frame (5-9 fps, log "[native][truth] ... fallbacks (msaa / clearing resolve)")
and a match flickers when single frames fall back. The WWE 2K14 port fixes the same thing in the game itself
(src/tiling_hooks.h in Jarrod Feng's wwe2k14-recomp, its sub_82C22AF8): WWE 2K16 is the next version of the same
Yuke's engine, so the same function is looked for by its shape, not by its address:

  scene object R (a non-volatile copy of r3):
    - custom layout read from R + 296 and R + 300,
    - a layout lookup call fills the 256-byte layout at R + 12 (width +12, height +16, MSAA type +20, tiles +24,
      tile 0 at +28), the next call sizes the targets from it,
    - "lwz rX,20(R)" then "cmpwi rX,0": a type of 0 skips creating the colour (+272) and depth (+276) targets,
    - the requested type / flag stored at R + 4 / R + 8.

Two hooks, as in the 2K14 port (src/k14_tiling_hooks.h has the reasoning):
  wwe2k16_tiling_layout_chosen(R)     right after the layout lookup's call: rewrites the layout to 1 tile, type 0
  wwe2k16_tiling_create_gate(rX, R)   at the compare: reports non-zero so the targets are still created (1x)

What it writes (only on exactly one match; anything less changes nothing and prints why):
  config/k14_tiling_hooks.toml        the [[midasm_hook]] entries, for any later full code generation
  generated_k14/<file>.cpp            the same two calls put straight into the already generated function (marked
                                      "wwe2k16 tiling hook"), so no new code generation is needed: one file rebuilds

Usage:
  python3 tools/k14-tiling-hooks.py            report only
  python3 tools/k14-tiling-hooks.py --apply    report, then write the two files above (run by android-build-k14.sh)
  python3 tools/k14-tiling-hooks.py sub_XXXX   one function with guest addresses (to send for a closer look)
Read the game's code only from your own generated_k14/ (never committed).
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
GEN = ROOT / "generated_k14"
TOML = ROOT / "config" / "k14_tiling_hooks.toml"
FUNC = re.compile(r"^(?:PPC_FUNC_IMPL\(__imp__|DEFINE_REX_FUNC\()(\w+)\)")
SUBADDR = re.compile(r"^sub_([0-9A-Fa-f]{8})$")
LABEL = re.compile(r"^loc_([0-9A-Fa-f]+):")
INSN = re.compile(r"^\t// ([a-z][a-z0-9.+_-]*) ?(.*)$")
MEM = re.compile(r"^(r\d+),(-?\d+)\((r\d+)\)$")
MARK = "wwe2k16 tiling hook"


class Insn:
    __slots__ = ("addr", "op", "args", "line")

    def __init__(self, addr, op, args, line):
        self.addr, self.op, self.args, self.line = addr, op, args, line


def functions():
    """(name, path, lines, [Insn]) per generated function; addresses re-synced at every label."""
    for path in sorted(GEN.glob("*.cpp")):
        lines = path.read_text(errors="replace").splitlines(keepends=True)
        name, insns, addr = None, [], 0
        for n, line in enumerate(lines):
            m = FUNC.match(line)
            if m:
                if name:
                    yield name, path, lines, insns
                a = SUBADDR.match(m.group(1))
                name, insns, addr = (m.group(1), [], int(a.group(1), 16)) if a else (None, [], 0)
                continue
            if not name:
                continue
            lab = LABEL.match(line)
            if lab:
                addr = int(lab.group(1), 16)
                continue
            i = INSN.match(line)
            if i:
                insns.append(Insn(addr, i.group(1), i.group(2).replace(" ", ""), n))
                addr += 4
        if name:
            yield name, path, lines, insns


def mem(insn):
    m = MEM.match(insn.args)
    return (m.group(1), int(m.group(2)), m.group(3)) if m else None


def find_in(insns):
    """The hook points in one function, or None. Strict: every part of the 2K14 shape, the object register the same."""
    loads, stores = {}, {}
    for k, insn in enumerate(insns):
        m = mem(insn)
        if not m:
            continue
        reg, off, base = m
        if insn.op == "lwz":
            loads.setdefault(base, set()).add(off)
        elif insn.op == "stw":
            stores.setdefault(base, set()).add(off)
    found = []
    for base in loads:
        if base == "r1" or not {296, 300} <= loads[base]:
            continue
        if not {4, 8} <= stores.get(base, set()):
            continue
        # The type compare: lwz rX,20(R) right before cmpwi/cmplwi [crN,]rX,0.
        gates = []
        for k in range(len(insns) - 1):
            m = mem(insns[k])
            if insns[k].op != "lwz" or not m or m[1] != 20 or m[2] != base:
                continue
            nxt = insns[k + 1]
            parts = nxt.args.split(",")
            if nxt.op in ("cmpwi", "cmplwi") and parts[-1] == "0" and parts[-2] == m[0]:
                gates.append((k + 1, m[0]))
        if len(gates) != 1:
            continue
        gate, rx = gates[0]
        # The two calls before the compare: the layout lookup, then the sizer (within 8 instructions of the compare).
        calls = [k for k in range(max(0, gate - 40), gate) if insns[k].op == "bl"]
        if len(calls) < 2 or gate - calls[-1] > 8:
            continue
        chosen = calls[-2] + 1
        if insns[chosen].addr != insns[calls[-2]].addr + 4:
            continue
        # The targets: stores (or loads) of +272 / +276 through R after the compare.
        uses = set()
        for i in insns[gate:]:
            m = mem(i)
            if m and m[2] == base:
                uses.add(m[1])
        if not ({272, 276} & uses):
            continue
        found.append((base, rx, chosen, gate))
    return found[0] if len(found) == 1 else None


def reg_expr(lines, first, last, reg):
    """How the generated C++ of this function names a register: ctx.rN or a local rN."""
    text = "".join(lines[first:last])
    if re.search(r"\bctx\." + reg + r"\b", text):
        return "ctx." + reg
    if re.search(r"(?<![\w.])" + reg + r"\.", text):
        return reg
    return "ctx." + reg


def dump(name):
    for fname, _, _, insns in functions():
        if fname == name:
            print(f"{fname}: {len(insns)} instructions")
            for i in insns:
                print(f"  0x{i.addr:08X}  {i.op} {i.args}")
            return
    sys.exit(f"{name} not found")


def main():
    if not GEN.is_dir():
        sys.exit("generated_k14/ not found: run tools/native-stage1.sh first")
    if len(sys.argv) > 1 and sys.argv[1].startswith("sub_"):
        dump(sys.argv[1])
        return
    apply = "--apply" in sys.argv
    matches, near = [], []
    for name, path, lines, insns in functions():
        hit = find_in(insns)
        if hit:
            matches.append((name, path, lines, insns, hit))
        elif 150 <= len(insns) <= 1200:
            offs = {}
            for i in insns:
                m = mem(i)
                if m and i.op == "lwz":
                    offs.setdefault(m[2], set()).add(m[1])
            if any(b != "r1" and {296, 300} <= o for b, o in offs.items()):
                near.append(name)
    print(f"[tiling] scene-target builder: {len(matches)} strict match(es)")
    for name, _, _, insns, (base, rx, chosen, gate) in matches:
        print(f"  {name}: object {base}, layout final at 0x{insns[chosen].addr:08X} ({insns[chosen].op} "
              f"{insns[chosen].args}), type compare at 0x{insns[gate].addr:08X} ({insns[gate].op} {insns[gate].args})")
    if len(matches) != 1:
        print(f"  not hooked (needs exactly 1). Functions with the +296/+300 loads: {', '.join(near[:12]) or 'none'}")
        print("  Send this output and: python3 tools/k14-tiling-hooks.py <one of them> > out/scene.txt")
        return 0
    name, path, lines, insns, (base, rx, chosen, gate) = matches[0]
    c, g = insns[chosen], insns[gate]
    toml = (
        "# Scene tile-layout hooks for the native (WWE 2K14 SDK) build: src/k14_tiling_hooks.h. Written by\n"
        "# tools/k14-tiling-hooks.py from the local generated code (addresses only, no game code).\n"
        f"# {name}: the scene-target builder (2K14's sub_82C22AF8), scene object {base}.\n\n"
        "[[midasm_hook]]\n"
        f"address = 0x{c.addr:08X}\n"
        'name = "wwe2k16_tiling_layout_chosen"\n'
        f'registers = ["{base}"]\n\n'
        "[[midasm_hook]]\n"
        f"address = 0x{g.addr:08X}\n"
        'name = "wwe2k16_tiling_create_gate"\n'
        f'registers = ["{rx}", "{base}"]\n')
    if not apply:
        print("  (report only: --apply writes config/k14_tiling_hooks.toml and hooks the generated code)")
        return 0
    if not TOML.exists() or TOML.read_text() != toml:
        TOML.write_text(toml)
        print(f"  wrote {TOML.relative_to(ROOT)}")
    # The generated code: the calls go right after each instruction's comment line (where the code generator puts a
    # mid-asm hook that runs before the instruction), the declarations before the function.
    text = "".join(lines)
    if MARK in text:
        print(f"  {path.name}: already hooked")
        return 0
    start = next(n for n, l in enumerate(lines) if FUNC.match(l) and FUNC.match(l).group(1) == name)
    end = insns[-1].line + 1
    obj, regx = reg_expr(lines, start, end, base), reg_expr(lines, start, end, rx)
    for line_no, call in sorted([(c.line, f"\twwe2k16_tiling_layout_chosen({obj});  // {MARK}\n"),
                                 (g.line, f"\twwe2k16_tiling_create_gate({regx}, {obj});  // {MARK}\n")], reverse=True):
        lines.insert(line_no + 1, call)
    lines.insert(start, f"extern void wwe2k16_tiling_layout_chosen(PPCRegister& object);  // {MARK}\n"
                        f"extern void wwe2k16_tiling_create_gate(PPCRegister& type, PPCRegister& object);  // {MARK}\n")
    path.write_text("".join(lines))
    print(f"  hooked {name} in {path.name} ({obj}, {regx})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
