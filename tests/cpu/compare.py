"""Compare a CPU backend's lane output with the x86 golden file.

    python3 -I compare.py <golden.txt> <results.txt>

Reports, per fixture, how many cases match and which fields differ, then one
RESULT line. A case missing from the results (fault, crash) counts as a failure.
"""
import collections
import sys


def load(path):
    states, mems = {}, {}
    for line in open(path, errors="replace"):
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "STATE":
            states[(parts[1], int(parts[2]))] = dict(p.split("=", 1) for p in parts[3:])
        elif parts[0] == "MEM":
            mems[(parts[1], int(parts[2]))] = parts[3]
    return states, mems


def main():
    gold_s, gold_m = load(sys.argv[1])
    res_s, res_m = load(sys.argv[2])
    per_fixture = collections.OrderedDict()
    for key in gold_s:
        per_fixture.setdefault(key[0], []).append(key)

    total_bad = 0
    for name, keys in per_fixture.items():
        bad, missing, fields, example = 0, 0, collections.Counter(), None
        for key in keys:
            if key not in res_s:
                missing += 1
                continue
            diff = [f for f, v in gold_s[key].items() if res_s[key].get(f) != v]
            if key in gold_m and res_m.get(key) != gold_m[key]:
                diff.append("membytes")
            if diff:
                bad += 1
                fields.update(diff)
                if example is None:
                    example = (key[1], {f: (gold_s[key].get(f), res_s[key].get(f)) for f in diff if f != "membytes"})
        status = "ok" if not bad and not missing else "DIFF"
        line = f"{status:4} {name:16} {len(keys) - bad - missing:5}/{len(keys):<5}"
        if missing:
            line += f" missing={missing}"
        if fields:
            line += " fields=" + ",".join(f"{f}:{n}" for f, n in fields.most_common())
        if example:
            line += f"  e.g. case {example[0]} " + " ".join(f"{f}={g}->{r}" for f, (g, r) in example[1].items())
        print(line)
        total_bad += bad + missing

    cases = len(gold_s)
    print(f"RESULT cpu.compare {'PASS' if total_bad == 0 else 'FAIL'} cases={cases} "
          f"matching={cases - total_bad} differing={total_bad}")
    return 0 if total_bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
