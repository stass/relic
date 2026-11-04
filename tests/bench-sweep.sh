#!/bin/sh
# Exercise sweep scheduling and evidence preservation without benchmarking.
set -eu
: "${RELIC_SRCDIR:?RELIC_SRCDIR must name the source directory}"
if ! command -v python3 > /dev/null 2>&1; then
    echo 'bench-sweep: Python 3 is required; skipping' >&2
    exit 77
fi
python3 - "$RELIC_SRCDIR" <<'PY'
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile

source = Path(sys.argv[1])
with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary) / "checkout's space"
    compare = root / 'bench/compare'
    compare.mkdir(parents=True)
    shutil.copy2(source / 'bench/compare/sweep.sh', compare / 'sweep.sh')
    (root / 'bench/datasets.sh').write_text('placeholder\n')
    for workload, dataset in [('startup', 'startup'), ('graph-left', 'graph'),
                              ('cspa-httpd', 'graspan-httpd')]:
        folder = compare / workload
        folder.mkdir()
        (folder / 'dataset').write_text(dataset + '\n')
    (compare / 'startup/nemo.rls').write_text('placeholder\n')
    (compare / 'pairset.c').write_text('placeholder\n')
    tool = root / 'engine'
    tool.write_text('''#!/bin/sh
set -eu
if [ "${1:-}" = --version ]; then printf 'Options enabled: openmp\\nWord size: 32 bits\\n'; exit 0; fi
out=
generate=no
stdout=no
while [ "$#" -gt 0 ]; do
    case $1 in
    -D) out=$2; shift ;;
    -g) out=$2; generate=yes; shift ;;
    --param) [ "$2" = 'answer_resource=""' ] && stdout=yes; shift ;;
    esac
    shift
done
if [ "$generate" = yes ]; then printf placeholder > "$out"; exit 0; fi
if [ "$out" = - ] || [ "$stdout" = yes ]; then
    printf '1\\t2\\n'
else
    printf '1\\t2\\n' > "$out/answer.tsv"
fi
''')
    tool.chmod(0o755)
    (compare / 'run.sh').write_text('''#!/bin/sh
python3 - "$@" <<'CHILD'
import json, os, sys
with open(os.environ['SWEEP_TEST_CALLS'], 'a') as stream:
    stream.write(json.dumps(dict(scale=os.environ['BENCH_SCALE'],
        threads=os.environ['BENCH_THREADS'], runs=os.environ['BENCH_RUNS'],
        arguments=sys.argv[1:], artifacts=os.environ['BENCH_ARTIFACTS'])) + '\\n')
# Alter the live harness after its first invocation; later scenarios must
# continue from the frozen source, never execute this replacement.
from pathlib import Path
live = Path(os.environ['SWEEP_TEST_LIVE'])
live.write_text('#!/bin/sh\\nexit 99\\n')
code = int(os.environ.get('SWEEP_TEST_EXIT', '0'))
raise SystemExit(code)
CHILD
''')
    compiler = root / 'compiler'
    compiler.write_text("""#!/bin/sh
set -eu
printf '%s\n' "$@" > "$SWEEP_TEST_COMPILE_ARGS"
binary=
while [ "$#" -gt 0 ]; do
    if [ "$1" = -o ]; then binary=$2; shift; fi
    shift
done
cat > "$binary" <<'BINARY'
#!/bin/sh
printf '1\\t2\\n'
BINARY
chmod +x "$binary"
""")
    compiler.chmod(0o755)
    # The sweep requires hyperfine on PATH; the stubbed harness never runs it.
    stubs = root / 'bin'
    stubs.mkdir()
    (stubs / 'hyperfine').write_text('#!/bin/sh\nexit 0\n')
    (stubs / 'hyperfine').chmod(0o755)
    calls = root / 'calls.jsonl'
    environment = dict(os.environ, BENCH_PAIRSET=str(tool), SWEEP_TEST_CALLS=str(calls),
                       SWEEP_TEST_LIVE=str(compare / 'run.sh'),
                       PATH=str(stubs) + os.pathsep + os.environ.get('PATH', ''))
    original_harness = (compare / 'run.sh').read_text()
    for name in ['RELIC', 'SWIPL', 'SOUFFLE', 'CLINGO', 'NMO', 'DUCKDB', 'SQLITE', 'CXX', 'CC']:
        environment[name] = str(tool)
    compile_args = root / 'compile-args'
    environment['CXX'] = str(compiler)
    environment['SWEEP_TEST_COMPILE_ARGS'] = str(compile_args)
    executable = ['sh', str(compare / 'sweep.sh'), '-j', '4']
    def run(record, extra=(), code=0, exit_code='0'):
        (compare / 'run.sh').write_text(original_harness)
        env = dict(environment, SWEEP_TEST_EXIT=exit_code)
        result = subprocess.run(executable + ['-o', str(record)] + list(extra),
                                env=env, text=True, capture_output=True, timeout=15)
        assert result.returncode == code, result
        return result

    record = root / 'results'
    run(record, ['-n'])
    assert not record.exists() and not calls.exists(), 'Preflight started measurements'
    assert '-fopenmp' in compile_args.read_text().splitlines()
    assert '-DRAM_DOMAIN_SIZE=32' in compile_args.read_text().splitlines()
    environment['BENCH_OPENMP_FLAGS'] = "-fopenmp -I '/include with spaces'"
    run(record, ['-n'])
    assert '/include with spaces' in compile_args.read_text().splitlines()
    del environment['BENCH_OPENMP_FLAGS']
    run(record)
    invocations = [json.loads(line) for line in calls.read_text().splitlines()]
    assert len(invocations) == 8
    assert [(r['scale'], r['threads']) for r in invocations] == [
        (str(scale), str(threads)) for scale in [1, 2, 4, 8] for threads in [1, 4]]
    for invocation in invocations:
        args = invocation['arguments']
        workloads = args[args.index('-w') + 1].split(',')
        engines = args[args.index('-e') + 1].split(',')
        assert invocation['runs'] == ('10' if invocation['scale'] == '1' else '3')
        assert ('cspa-httpd' in workloads) == (invocation['scale'] == '1')
        assert {'startup', 'graph-left'} <= set(workloads)
        assert ('nemo' in engines) == (invocation['threads'] == '1')
        assert invocation['artifacts'] == str(record / 'samples')
    assert (record / 'main-sweep-complete').exists()
    assert not list(record.glob('*.interrupted'))
    assert len(list(record.glob('*.exit-status'))) == 8
    assert (record / 'source.tar').exists()
    assert 'bench/compare/run.sh' in (record / 'source-sha256.txt').read_text()
    saved_calls = calls.read_bytes()
    run(record, code=2)
    assert calls.read_bytes() == saved_calls, 'Existing cohort was restarted'
    assert 'discarded stdout' in (record / 'environment.txt').read_text()
    aborted = root / 'aborted'
    run(aborted, code=2, exit_code='2')
    assert (aborted / 'scale-1-threads-1.interrupted').exists()
    assert not (aborted / 'main-sweep-complete').exists()
    failed = root / 'engine-failures'
    run(failed, exit_code='1')
    assert (failed / 'main-sweep-complete').exists()
    assert all(p.read_text().strip() == '1' for p in failed.glob('*.exit-status'))

    # Expose one checksum interface at a time, independently of host tools.
    isolated = root / 'isolated-bin'
    isolated.mkdir()
    for name in ['awk', 'cat', 'chmod', 'cmp', 'cp', 'date', 'dirname', 'find',
                 'grep', 'ln', 'mkdir', 'mktemp', 'realpath', 'rm', 'sed', 'sh',
                 'tar', 'uname', 'wc', 'python3']:
        path = shutil.which(name)
        assert path is not None, f'Missing test tool: {name}'
        (isolated / name).symlink_to(path)
    for name in ['git', 'sysctl', 'timeout', 'gtimeout']:
        path = shutil.which(name)
        if path is not None:
            (isolated / name).symlink_to(path)
    (isolated / 'hyperfine').symlink_to(stubs / 'hyperfine')
    environment['PATH'] = str(isolated)
    for checksum in ['sha256sum', 'sha256', 'shasum']:
        provider = isolated / checksum
        provider.write_text('''#!/bin/sh
exec python3 - "$0" "$@" <<'HASH'
import hashlib, sys
from pathlib import Path
name = Path(sys.argv[1]).name
arguments = sys.argv[2:]
flags = {'sha256': ['-r'], 'sha256sum': [], 'shasum': ['-a', '256']}[name]
assert arguments[:len(flags)] == flags, arguments
for filename in arguments[len(flags):]:
    print(hashlib.sha256(Path(filename).read_bytes()).hexdigest(), '  ', filename, sep='')
HASH
''')
        provider.chmod(0o755)
        checksummed = root / ('checksum-' + checksum)
        run(checksummed)
        with tarfile.open(checksummed / 'source.tar') as archive:
            expected = {member.name: hashlib.sha256(archive.extractfile(member).read()).hexdigest()
                        for member in archive.getmembers() if member.isfile()}
        actual = {}
        for line in (checksummed / 'source-sha256.txt').read_text().splitlines():
            digest, filename = line.split('  ', 1)
            actual[filename] = digest
        assert actual == expected, (checksum, actual, expected)
        assert (checksummed / 'main-sweep-complete').exists()
        provider.unlink()
print('bench-sweep: preflight, scheduling, cohorts, abort evidence and checksums passed')
PY
