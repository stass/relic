#!/bin/sh
# Check the tree verifier against an independent distinct-row oracle.
set -eu
if ! command -v python3 > /dev/null 2>&1; then
	echo 'bench-pairset: Python 3 is required; skipping' >&2
	exit 77
fi
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
checker=${RELIC_BUILDDIR:-$work}/bench-pairset
if [ -n "${RELIC_BUILDDIR:-}" ]; then
	checker=$RELIC_BUILDDIR/tests/bench-pairset
else
	${CC:-cc} ${CFLAGS:--O2} -std=c11 -Wall -Wextra -Werror -pthread \
	    "$root/bench/compare/pairset.c" -o "$checker"
fi
# The bounded tree query prints one column and must retain the TSV verifier.
awk '/^(tree_bits|bitmap_answer)\(\)/ { copying = 1 }
    copying { print }
    copying && /^}/ { copying = 0 }' "$root/bench/compare/run.sh" > "$work/scope.sh"
. "$root/bench/datasets.sh"
. "$work/scope.sh"
pairset=$checker
scale=8
bitmap_answer tree-sg-all
if bitmap_answer tree-sg-bound || bitmap_answer chain-closure; then
	echo 'pairset: incompatible answer schema selected' >&2
	exit 1
fi
# The generated tree's IDs fit the width the run passes pairset.
scale=2
mkdir "$work/tree"
dataset small-tree "$work/tree" "$scale"
"$checker" -b "$(tree_bits)" "$work/tree/parent.tsv" > /dev/null
# Scale 128's largest tree ID is the last to fit pairset's 17 bits.
scale=128
bitmap_answer tree-sg-all
if [ "$(tree_bits)" -ne 17 ]; then
	echo 'pairset: scale 128 needs 17 bits' >&2
	exit 1
fi
scale=129
if bitmap_answer tree-sg-all; then
	echo 'pairset: out-of-domain tree selected' >&2
	exit 1
fi
python3 - "$work" "$checker" <<'PY'
import pathlib
import random
import subprocess
import sys

work = pathlib.Path(sys.argv[1])
checker = sys.argv[2]
reference = work / 'reference.tsv'
candidate = work / 'candidate.tsv'
rng = random.Random(317)

def run(data, other=None, workers=16, bits=6, expected=True):
    reference.write_bytes(data)
    command = [checker, '-b', str(bits), '-j', str(workers), str(reference)]
    if other is not None:
        candidate.write_bytes(other)
        command.append(str(candidate))
    result = subprocess.run(command, capture_output=True)
    assert b'Sanitizer' not in result.stderr and b'runtime error:' not in result.stderr, result
    assert result.returncode == (0 if expected else 1), (command, result, data[:80], other)
    if expected:
        assert int(result.stdout) == len(set(data.splitlines())), result

for workers in (1, 2, 16, 64):
    for count in (0, 1, 2, 31, 1024):
        rows = [f't{rng.randrange(64)}\tt{rng.randrange(64)}'.encode()
                for _ in range(count)]
        data = b'\n'.join(rows) + (b'\n' if rows else b'')
        shuffled = rows * 3
        rng.shuffle(shuffled)
        run(data, b'\n'.join(shuffled), workers)
        run(data, workers=workers)
        if rows:
            unique = sorted(set(rows))
            run(data, b'\n'.join(unique[1:]), workers, expected=False)
    # Canonical ID bounds and the single-word bitmap boundary.
    run(b't0\tt1\nt1\tt0\n', b't1\tt0\nt0\tt1', workers, bits=1)
    run(b't63\tt63\n', b't63\tt63\nt63\tt63\n', workers)

run(b't1\tt2\n', b't1\tt3\n', expected=False)
for invalid in (b'\n', b't01\tt2\n', b't1\tt02\n', b't1\tt64\n',
                b't64\tt1\n', b't1 t2\n', b't1\tt2\t3\n',
                b't1\tt2\r\n', b't1\tt\n', b't1\tt2junk',
                b't99999999999999999999\tt2\n', b't1\tt2\n\x00',
                b't\tt2\n', b't1\nt2\n'):
    run(invalid, expected=False)
    run(b't1\tt2\n', invalid, expected=False)

for arguments in ([], ['-b', '0'], ['-b', '18'], ['-j', '-1'],
                  ['-j', '0'], ['-j', '1025'], ['-b', '2x']):
    assert subprocess.run([checker] + arguments, capture_output=True).returncode == 2
assert subprocess.run([checker, '-b', '6', str(work / 'missing')],
                      capture_output=True).returncode == 2
assert subprocess.run([checker, '-b', '6', str(work)],
                      capture_output=True).returncode == 2
print('pairset: exact equality, duplicates, partitions and invalid input passed')
PY
