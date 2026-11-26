#!/usr/bin/env python3
"""Names for Game.exe's addresses in the profile tools (issue #107).

Two sources, ours first:
- our natives: the HOOK list in src/game/d2_native.cpp (the address and the
  name each hook is logged under);
- d2decomped (the third_party/d2d submodule, GPL-3.0-or-later, issue #105),
  read from its checkout at run time, never copied: docs/research/re/
  names.tsv (the names it has given), game-functions.tsv (each function's
  start and size, and Ghidra's own names for imports, thunks and Storm's
  exports: the FUN_ ones are not names) and structs.tsv (struct fields).

Everything is optional: without the submodule or the source, an address is
printed bare as before. Labels look like `0x67b850` (PathSearch); an address
inside a named function, like a return address, is `0x5ff5f3` (in
SpriteCacheTouch +0x43).

    python3 -I tools/game_names.py [0x67b850 ...]     what the tables hold, and those addresses' labels
"""
import bisect
import csv
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
D2D = os.path.join(ROOT, "third_party", "d2d")
RE_DIR = os.path.join("docs", "research", "re")
NATIVE_SRC = os.path.join(ROOT, "src", "game", "d2_native.cpp")
HOOK = re.compile(r"HOOK\(c,\s*0x([0-9a-fA-F]+),(.*?)\);", re.S)


def _tsv(path):
    with open(path, newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f, delimiter="\t"))


def native_hooks(path=NATIVE_SRC):
    """{address: name} of the hooks in d2_native.cpp: the last string in
    each HOOK(...) is the name the hook is logged under."""
    try:
        text = open(path, encoding="utf-8").read()
    except OSError:
        return {}
    out = {}
    for addr, body in HOOK.findall(text):
        names = re.findall(r'"([A-Za-z0-9_]+)"', body)
        if names:
            out[int(addr, 16)] = names[-1]
    return out


class Names:
    """Function names and extents for labels. `natives` (ours) take
    precedence over d2d's names, and those over Ghidra's; d2d's sizes say
    whether an address is inside a function."""

    def __init__(self, d2d=D2D, natives=None):
        self.natives = dict(native_hooks() if natives is None else natives)
        self.d2d_names, self.ghidra_names, self.extents, self.structs = {}, {}, {}, []
        self.d2d = None
        base = os.path.join(d2d, RE_DIR)
        try:
            for r in _tsv(os.path.join(base, "game-functions.tsv")):
                a = int(r["address"], 16)
                self.extents[a] = int(r["size"])
                if not r["name"].startswith("FUN_"):
                    self.ghidra_names[a] = r["name"]
            for r in _tsv(os.path.join(base, "names.tsv")):
                self.d2d_names[int(r["address"], 16)] = r["name"]
            self.structs = _tsv(os.path.join(base, "structs.tsv"))
            self.d2d = d2d
        except (OSError, KeyError, ValueError):
            self.extents, self.d2d_names, self.ghidra_names, self.structs = {}, {}, {}, []
        self.starts = sorted(self.extents)

    def name(self, addr):
        """The function's own name at its start, or None."""
        return self.natives.get(addr) or self.d2d_names.get(addr) or self.ghidra_names.get(addr)

    def containing(self, addr):
        """d2d's function that holds `addr` (its start), or None."""
        i = bisect.bisect_right(self.starts, addr) - 1
        if i >= 0 and addr < self.starts[i] + self.extents[self.starts[i]]:
            return self.starts[i]
        return None

    def label(self, addr, code=True):
        """`0x67b850` (PathSearch) at a start; `0x5ff5f3` (in Name +0x43)
        inside a named d2d function; else the bare address. code=False:
        without the backticks."""
        a = f"`{addr:#x}`" if code else f"{addr:#x}"
        n = self.name(addr)
        if n:
            return f"{a} ({n})"
        f = self.containing(addr)
        n = self.name(f) if f is not None else None
        return f"{a} (in {n} +{addr - f:#x})" if n else a

    def summary(self):
        if not self.d2d:
            return (f"names: {len(self.natives)} natives; d2d's tables not found "
                    f"(git submodule update --init third_party/d2d)")
        return (f"names: {len(self.natives)} natives, d2d {len(self.d2d_names)} names and {len(self.ghidra_names)} "
                f"of Ghidra's, {len(self.extents)} functions with sizes, {len(self.structs)} struct fields")


def main():
    names = Names()
    print(names.summary())
    for a in sys.argv[1:]:
        print(names.label(int(a, 16)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
