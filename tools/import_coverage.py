"""Which imports of a PE32 does the runtime cover?

    python3 -I tools/import_coverage.py <objdump -p output> <WinVita docs/shims.md> <rt_shims_*.cpp ...>

Covered = a shim is registered under the same DLL and name: WinVita's generic
sets (its generated shims.md) or 3ds-d2's rt_shims_*.cpp. Prints a per-DLL
summary and the missing names.
"""
import collections
import re
import sys


def main():
    dump, shims_md, *rt_files = sys.argv[1:]
    imports = collections.OrderedDict()
    dll = None
    for line in open(dump, errors="replace"):
        m = re.match(r'\s*DLL Name: (\S+)', line)
        if m:
            dll = m.group(1).upper()
            imports.setdefault(dll, [])
            continue
        if not dll:
            continue
        m = re.match(r'\s*[0-9a-f]+\s+<none>\s+[0-9a-f]+\s+(\S+)\s*$', line)
        if m:
            imports[dll].append(m.group(1))
            continue
        m = re.match(r'\s*[0-9a-f]+\s+([0-9]+)\s*(<none>)?\s*$', line)   # imported by ordinal
        if m:
            imports[dll].append('#' + m.group(1))

    shims = collections.defaultdict(set)
    cur = None
    for line in open(shims_md):
        m = re.match(r'^## (\S+)', line)
        if m:
            cur = m.group(1).upper()
            continue
        if cur:
            shims[cur].update(re.findall(r'`([A-Za-z0-9_]+)`', line))
    for f in rt_files:
        src = open(f).read()
        shims['KERNEL32.DLL'].update(re.findall(r'\bK\("([A-Za-z0-9_]+)"', src))
        shims['USER32.DLL'].update(re.findall(r'\bU\("([A-Za-z0-9_]+)"', src))

    total = covered = 0
    missing = collections.OrderedDict()
    for d, names in imports.items():
        have = [n for n in names if n in shims.get(d, set())]
        miss = [n for n in names if n not in shims.get(d, set())]
        total += len(names)
        covered += len(have)
        missing[d] = miss
        print(f"{d:14} imports={len(names):3} covered={len(have):3} missing={len(miss):3}")
    print(f"TOTAL imports={total} covered={covered} ({100 * covered // max(total, 1)}%) missing={total - covered}")
    for d, miss in missing.items():
        if miss:
            print(f"\n{d} missing ({len(miss)}): " + ' '.join(miss))


if __name__ == '__main__':
    main()
