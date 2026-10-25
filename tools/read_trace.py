"""Debug: find the game's tables indexed by character number from what it
actually reads.

1. instrument: copy the recompiled code with each instruction setting g_pc
     python3 tools/read_trace.py instrument [build/generated] [build/generated_trace]
   then build it with the read trace on:
     cmake -S . -B build/cmake_trace -G Ninja -DSFA3_GEN=$PWD/build/generated_trace -DSFA3_READ_TRACE=ON
     cmake --build build/cmake_trace
2. run the same fight twice, player 1 a different character each time, and
   write the trace at the end with the "rtdump <file>" control command
3. compare: instructions that read address a in one run and a + k x s in the
   other (k the difference between the characters) index a table by character
     python3 tools/read_trace.py compare <trace A> <char A> <trace B> <char B> [player 2's char]
"""
import collections
import os
import re
import sys

LINE = re.compile(r"^(\s*)/\* (8C[0-9A-F]{6}) [0-9A-F]{4} [^*]*\*/\s*$")


def instrument(src, dst):
    os.makedirs(dst, exist_ok=True)
    for name in os.listdir(src):
        p = os.path.join(src, name)
        if not os.path.isfile(p):
            continue
        data = open(p, encoding="utf-8", errors="surrogateescape").read()
        if name.startswith("code_") and name.endswith(".c"):
            out = []
            for line in data.split("\n"):
                m = LINE.match(line)
                out.append(f"{line} g_pc = 0x{m.group(2)}u;" if m else line)
            data = "\n".join(out)
        q = os.path.join(dst, name)
        if os.path.exists(q) and open(q, encoding="utf-8", errors="surrogateescape").read() == data:
            continue  # unchanged: keep its timestamp (no rebuild)
        open(q, "w", encoding="utf-8", errors="surrogateescape").write(data)


def load(path):
    reads = collections.defaultdict(set)
    for line in open(path):
        pc, a = line.split()
        reads[int(pc, 16)].add(int(a, 16))
    return reads


SKIP = [(0x8C17FD38, 0x8C186AE8)]  # fighters and objects: indexed by player, not character


def compare(pa, ca, pb, cb, other=None):
    """other: player 2's character, the same in both runs: a table read for
    player 1 at a + k x s should also be read for player 2 (confirmed)"""
    A, B = load(pa), load(pb)
    k = cb - ca
    found = collections.defaultdict(set)  # (base, scale) -> instructions
    sure = set()
    for pc in sorted(set(A) & set(B)):
        only_a = A[pc] - B[pc]
        only_b = B[pc] - A[pc]
        if not only_a or not only_b or len(only_a) > 64:
            continue
        for b in only_b:
            if any(lo <= b < hi for lo, hi in SKIP):
                continue
            for a in only_a:
                d = b - a
                if d > 0 and d % k == 0 and d // k <= 0x200:
                    s = d // k
                    found[(a - ca * s, s)].add(pc)
                    if other is not None and a + (other - ca) * s in A[pc] & B[pc]:
                        sure.add((a - ca * s, s))
    for (base, s), pcs in sorted(found.items(), key=lambda kv: (kv[0] not in sure, kv[0])):
        tag = "confirmed" if (base, s) in sure else "         "
        print(f"{tag} table {base:08X} entry {s:3d} bytes  read at {' '.join(f'{p:08X}' for p in sorted(pcs)[:6])}")


if __name__ == "__main__":
    if sys.argv[1] == "instrument":
        instrument(sys.argv[2] if len(sys.argv) > 2 else "build/generated",
                   sys.argv[3] if len(sys.argv) > 3 else "build/generated_trace")
    else:
        compare(sys.argv[2], int(sys.argv[3], 16), sys.argv[4], int(sys.argv[5], 16),
                int(sys.argv[6], 16) if len(sys.argv) > 6 else None)
