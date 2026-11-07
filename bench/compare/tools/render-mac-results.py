from pathlib import Path
import argparse
import collections
import csv
import json
import math
import os
import re
import shutil
import statistics

parser = argparse.ArgumentParser(description="Audit and publish the Apple M3 Max comparison")
parser.add_argument("--check", action="store_true", help="validate without publishing")
arguments = parser.parse_args()
accepted_copies = []

repo = Path(__file__).resolve().parents[3]
# RELIC_BENCH names a directory of recorded benchmark results; its m3-max
# subdirectory holds this study.
if not os.environ.get('RELIC_BENCH'):
    raise SystemExit('RELIC_BENCH must name a directory of recorded benchmark results')
record = Path(os.environ['RELIC_BENCH']) / 'm3-max'
if not record.is_dir():
    raise SystemExit(f'recorded benchmark results not found at {record}; check RELIC_BENCH')
engines = ['relic', 'souffle', 'souffle-magic', 'souffle-interp', 'nemo', 'swipl', 'clingo', 'duckdb', 'sqlite']
parallel = ['relic', 'souffle', 'souffle-magic', 'souffle-interp', 'duckdb']
labels = ['relic', 'Souffl\u00e9 compiled', 'Souffl\u00e9 Magic', 'Souffl\u00e9 interpreter', 'Nemo', 'SWI-Prolog', 'clingo', 'DuckDB', 'SQLite']
runs_by_scale = {1:10,2:3,4:3,8:3}
workloads = sorted(p.parent.name for p in (repo/'bench/compare').glob('*/dataset'))
external = ['cspa-httpd', 'cspa-linux', 'cspa-postgresql']
all_rows = []
columns = (record/'scale-1-threads-1.tsv').read_text().splitlines()[0].split('\t')
unresolved = []
resolution_path = record/'investigated-failures.json'
resolutions = json.loads(resolution_path.read_text()) if resolution_path.exists() else []
used_resolutions = set()
resolution_index = {(int(r['scale']),int(r['threads']),r['workload'],r['engine']):r for r in resolutions}
if len(resolution_index) != len(resolutions):
    raise RuntimeError('Duplicate investigated failure')
metadata = json.loads((record/'environment.json').read_text())
interruptions = metadata.get('interrupted_cells', [])
interruption_index = {(int(r['scale']), int(r['threads']), r['workload'], r['engine']): r for r in interruptions}
if len(interruption_index) != len(interruptions):
    raise RuntimeError('Duplicate interrupted cell')
def validate_samples(r, reference, sample_path):
    """Check a successful cell's raw invocations and recompute its summaries."""
    engine = r['engine']
    workload = r['workload']
    scale = int(r['scale'])
    key = (workload, engine)
    j = json.loads(sample_path.read_text())
    if j.get('schema_version') != 2 or len(j.get('results', [])) != 1:
        raise RuntimeError(f'Invalid JSON schema/result count: {sample_path}')
    measurements = j['results'][0]['measurements']
    if len(measurements) != runs_by_scale[scale]:
        raise RuntimeError(f'Expected {runs_by_scale[scale]} samples: {sample_path}')
    for sample in measurements:
        successes = {10, 30} if engine == 'clingo' else {0}
        if sample['exit_code'] not in successes:
            raise RuntimeError(f'Failed timed invocation: {sample_path}')
        for metric, unit in [('time_wall_clock','second'), ('time_user','second'), ('memory_peak_resident','byte')]:
            value = sample[metric]['value']
            if sample[metric].get('unit') != unit or not math.isfinite(value) or value < 0:
                raise RuntimeError(f'Invalid metric/unit: {sample_path} {metric}')
    if j['results'][0]['command'] != f'{engine} {workload}':
        raise RuntimeError(f'Mismatching command label: {sample_path}')
    if reference['status'] != 'ok':
        raise RuntimeError(f'Successful timing without a verified reference: {sample_path.name} {key}')
    if r['rows'] != reference['rows'] or int(r['rows']) <= 0:
        raise RuntimeError(f'Invalid/mismatching row count: {sample_path.name} {key}')
    times = [v['time_wall_clock']['value'] for v in measurements]
    users = [v['time_user']['value'] for v in measurements]
    memory = [v['memory_peak_resident']['value'] for v in measurements]
    actual = [statistics.median(times), min(times), statistics.stdev(times), statistics.mean(users), max(memory)]
    for name, value in zip(['median', 'fastest', 'spread', 'user', 'peak_bytes'], actual):
        reported = float(r[name])
        if not math.isfinite(reported) or reported < 0:
            raise RuntimeError(f'Invalid reported summary: {sample_path.name} {name}')
        if name == 'median' and reported == 0:
            raise RuntimeError(f'Unmeasurable zero wall median: {sample_path.name}')
        if abs(reported-value) > max(1e-10, abs(value)*1e-8):
            raise RuntimeError(f'{sample_path.name}: {name} differs from samples')

allowed_statuses = {'ok', 'timeout', 'n/a', 'failed', 'mismatch', 'empty', 'no-data'}
def read_cohort(scale, threads):
    """Read original cells and validate the completed cohort boundary."""
    path = record/f'scale-{scale}-threads-{threads}.tsv'
    if not path.exists():
        raise RuntimeError(f'Missing scenario: {path.name}')
    with path.open() as f:
        reader = csv.DictReader(f, delimiter='\t')
        if columns is not None and columns != reader.fieldnames:
            raise RuntimeError(f'Inconsistent columns: {path.name}')
        rows = list(reader)
    selected = engines if threads == 1 else parallel
    selected_workloads = workloads if scale == 1 else [w for w in workloads if w not in external]
    index = {(r['workload'], r['engine']): r for r in rows}
    expected = {(w, e) for w in selected_workloads for e in selected}
    if len(index) != len(rows) or not set(index) <= expected:
        raise RuntimeError(f'Duplicate or unexpected cell in {path.name}')
    for r in rows:
        if int(r['scale']) != scale or int(r['threads']) != threads:
            raise RuntimeError(f'Incorrect coordinates in {path.name}: {r}')
        if r['status'] not in allowed_statuses:
            raise RuntimeError(f'Unknown status in {path.name}: {r}')

    exit_path = path.with_suffix('.exit-status')
    if not exit_path.exists():
        raise RuntimeError(f'Missing completed-scenario exit status: {path.name}')
    exit_code = int(exit_path.read_text().strip())
    expected_exit = int(any(r['status'] in {'failed', 'mismatch', 'empty'} for r in rows))
    if exit_code != expected_exit:
        issue=metadata.get('harness_epilogue_issue',{})
        if issue.get('scale')!=scale or issue.get('threads')!=threads or issue.get('observed_exit')!=exit_code or issue.get('expected_from_cells')!=expected_exit:
            raise RuntimeError(f'Unexpected scenario exit status: {path.name}: {exit_code}')
    return rows, index, selected_workloads, selected, expected


def audit_retry(scale, threads, rows, index):
    """Audit fresh references and return rival overrides without altering raw rows."""
    overrides = {}
    interrupted = {(w, e) for (s, t, w, e) in interruption_index if (s, t) == (scale, threads)}
    for key in interrupted:
        if key not in index or index[key]['status'] not in {'timeout', 'failed'}:
            raise RuntimeError('Interrupted verification has an unexpected raw outcome')
        detail = interruption_index[(scale, threads) + key]
        if detail['phase'] != 'answer verification' or detail['retry_report'] != f'retry-scale-{scale}-threads-{threads}.tsv':
            raise RuntimeError('Interrupted cell has an unexpected phase or retry report')
    retry_sources = {}
    retry_file = record/f'retry-scale-{scale}-threads-{threads}.tsv'
    if interrupted and not retry_file.exists():
        raise RuntimeError('Interrupted verification requires a completed fresh retry')
    if retry_file.exists():
        with retry_file.open() as source:
            retry_reader=csv.DictReader(source,delimiter='\t')
            if retry_reader.fieldnames != columns:
                raise RuntimeError('Retry schema differs from scenario')
            retry_rows=list(retry_reader)
        retry_index={(r['workload'],r['engine']):r for r in retry_rows}
        retry_engines={r['engine'] for r in retry_rows if r['engine']!='relic'}
        if not retry_engines or not retry_engines <= {'swipl','duckdb'}:
            raise RuntimeError('Unauthorized retry engine')
        retry_targets = {(r['workload'], r['engine']) for r in rows
                         if r['status'] == 'failed' and r['engine'] in retry_engines} | interrupted
        targets = {w for w, e in retry_targets}
        expected_retry = {(w, 'relic') for w in targets} | retry_targets
        if len(retry_index)!=len(retry_rows) or set(retry_index)!=expected_retry:
            raise RuntimeError('Unexpected, missing or duplicate retry cells')
        for r in retry_rows:
            if int(r['scale'])!=scale or int(r['threads'])!=threads or r['status'] not in allowed_statuses:
                raise RuntimeError('Invalid retry coordinates/status')
        retry_exit=int(retry_file.with_suffix('.exit-status').read_text().strip())
        if retry_exit!=int(any(r['status'] in {'failed','mismatch','empty'} for r in retry_rows)):
            raise RuntimeError('Unexpected retry exit status')
        for w in targets:
            reference=retry_index[w,'relic']
            if reference['status']!='ok' or reference['rows']!=index[w,'relic']['rows']:
                raise RuntimeError('Retry has no matching reference answer')
            validate_samples(reference, index[w, 'relic'], record/f'retry-samples/scale-{scale}/threads-{threads}/{w}.relic.json')
        for r in rows:
            if (r['workload'],r['engine']) in retry_targets:
                accepted = dict(retry_index[r['workload'],r['engine']])
                overrides[r['workload'],r['engine']] = accepted
                if accepted['status']=='ok':
                    retry_sources[r['workload'],r['engine']]=record/f"retry-samples/scale-{scale}/threads-{threads}/{r['workload']}.{r['engine']}.json"
    return overrides, retry_sources


def classify_outcome(r, scale, threads):
    """Classify only failures tied to cell-specific diagnostic evidence."""
    retry_file = record/f'retry-scale-{scale}-threads-{threads}.tsv'
    key = (scale,threads,r['workload'],r['engine'])
    resolution = resolution_index.get(key)
    if resolution:
        if resolution['raw_status'] != r['status'] or r['status'] != 'failed':
            raise RuntimeError(f'Invalid investigated raw outcome: {key}')
        classified = resolution['classified_status']
        if classified == 'engine-limit':
            if r['engine'] != 'clingo' or resolution['exit_code'] != 65:
                raise RuntimeError(f'Invalid native capacity classification: {key}')
            evidence = f'scale-{scale}-threads-{threads}.stderr.txt'
            signature = 'Value too large to be stored in data type: Id out of range'
        elif classified == 'resource-limit':
            if r['engine'] != 'duckdb' or resolution['exit_code'] != 1 or resolution.get('attempt') != 'retry':
                raise RuntimeError(f'Invalid memory classification: {key}')
            if resolution.get('memory_limit_gib') != 120 or not retry_file.exists():
                raise RuntimeError(f'Memory classification lacks declared retry: {key}')
            evidence = f'retry-scale-{scale}-threads-{threads}.stderr.txt'
            signature = 'Out of Memory Error:'
            header = retry_file.with_suffix('.txt').read_text()
            if 'memory limit 120 GiB' not in header:
                raise RuntimeError(f'Memory classification lacks matching allowance: {key}')
        else:
            raise RuntimeError(f'Unknown investigation classification: {key}')
        if resolution['evidence'] != evidence:
            raise RuntimeError(f'Wrong investigation evidence: {key}')
        diagnostic = (record/evidence).read_text()
        marker = f"compare: {r['engine']} failed on {r['workload']} with status {resolution['exit_code']}:"
        if marker not in diagnostic:
            raise RuntimeError(f'Investigated outcome lacks matching failure header: {key}')
        block = diagnostic.split(marker,1)[1].split('\ncompare:',1)[0]
        if signature not in block or resolution['diagnostic'] not in block:
            raise RuntimeError(f'Investigated outcome lacks cell-specific diagnostic: {key}')
        used_resolutions.add(key)
        return classified
    return r['status']


def audit_scenario(scale, threads):
    """Build a complete canonical matrix and audit every accepted sample."""
    raw_rows, raw_index, selected_workloads, selected, expected = read_cohort(scale, threads)
    overrides, retry_sources = audit_retry(scale, threads, raw_rows, raw_index)
    # The index points only into owned canonical copies; raw evidence is immutable.
    rows = [dict(overrides.get((r['workload'], r['engine']), r)) for r in raw_rows]
    index = {(r['workload'], r['engine']): r for r in rows}
    path = record/f'scale-{scale}-threads-{threads}.tsv'
    for r in rows:
        r['status'] = classify_outcome(r, scale, threads)
        if r['status'] in {'failed', 'mismatch', 'empty', 'no-data'}:
            unresolved.append(dict(scale=scale, threads=threads, workload=r['workload'], engine=r['engine'], status=r['status']))
    for workload in selected_workloads:
        for engine in selected:
            key = (workload, engine)
            if key not in index:
                reference = index.get((workload, 'relic'))
                if reference is None or reference['status'] == 'ok':
                    raise RuntimeError(f'Missing completed cell: {path.name} {key}')
                r = {c: '-' for c in columns}
                r.update(workload=workload, engine=engine, threads=str(threads), scale=str(scale), status='no-reference')
                rows.append(r)
            else:
                r = index[key]
            if r['status'] == 'ok':
                sample_path = record/f'samples/scale-{scale}/threads-{threads}/{workload}.{engine}.json'
                accepted_path=sample_path
                sample_path=retry_sources.get((workload,engine),sample_path)
                validate_samples(r, index[workload, 'relic'], sample_path)
            if r['status']=='ok' and (workload,engine) in retry_sources:
                accepted_copies.append((sample_path,accepted_path))
    if {(r['workload'],r['engine']) for r in rows} != expected:
        raise RuntimeError(f'Incomplete matrix after recording no-reference: {path.name}')
    return rows


scenarios = []
for scale in [1, 2, 4, 8]:
    for threads in [1, 16]:
        rows = audit_scenario(scale, threads)
        all_rows.extend(rows)
        scenarios.append((scale, threads, rows))

if len(all_rows) != 1610:
    raise RuntimeError(f'Expected 1610 cells, got {len(all_rows)}')
if used_resolutions != set(resolution_index):
    raise RuntimeError('Investigation contains an unused cell')
if unresolved:
    raise RuntimeError(f'Uninvestigated outcomes: {unresolved}')
if list(record.glob('*.interrupted')):
    raise RuntimeError('Interrupted cohort cannot be published')
# Narrative conditions are pinned to this recorded study and checked before
# either publication or --check can accept changed evidence.
expected_context = {
    'processor': 'Apple M3 Max', 'cores': 16, 'performance_cores': 12,
    'efficiency_cores': 4, 'memory_bytes': 128 * 1073741824,
    'relic_configure': '--disable-shared --enable-threads',
    'relic_cflags': '-O3 -g', 'souffle_compiler': 'Homebrew clang 23.1.3',
    'souffle_release': '2.5', 'verification_timeout_seconds': 300,
    'scales': [1, 2, 4, 8], 'thread_counts': [1, 16],
    'timed_runs_by_scale': {str(scale): runs for scale, runs in runs_by_scale.items()},
}
for name, expected_value in expected_context.items():
    if metadata.get(name) != expected_value:
        raise RuntimeError(f'Publication context changed: {name}')
header = (record/'scale-1-threads-1.txt').read_text().split('\n\n', 1)[0]
for version in ['hyperfine 2.0.0', 'relic 1.0.0', 'SWI-Prolog version 9.2.9',
                'souffle 2.5', 'clingo version 5.8.0', 'nemo-cli 0.10.1',
                'duckdb v1.5.5', 'sqlite 3.51.2', 'Homebrew clang version 23.1.3']:
    if version not in header:
        raise RuntimeError(f'Publication version changed: {version}')
by_cell = {(int(r['scale']), int(r['threads']), r['workload'], r['engine']): r for r in all_rows}
for cell in [(2, 1, 'cspa', 'swipl'), (4, 1, 'graph-left', 'duckdb'),
             (4, 16, 'graph-left', 'duckdb'), (8, 1, 'graph-left', 'duckdb'),
             (8, 16, 'graph-left', 'duckdb')]:
    if by_cell[cell]['status'] != 'timeout':
        raise RuntimeError(f'Publication retry outcome changed: {cell}')
for threads, held in [(1, '114.1'), (16, '114.2')]:
    diagnostic = resolution_index[8, threads, 'tree-sg-bound', 'duckdb']['diagnostic']
    if f'({held} GiB/120.0 GiB used)' not in diagnostic or '32.0 GiB' not in diagnostic:
        raise RuntimeError('Publication memory diagnosis changed')
if len(workloads) != 31 or len(set(workloads) - set(external)) != 28:
    raise RuntimeError('Publication workload count changed')
if arguments.check:
    print(f'Validated {len(all_rows)} cells and every successful timing sample')
    raise SystemExit(0)
for source, target in accepted_copies:
    if target.exists() and target.read_bytes() != source.read_bytes():
        raise RuntimeError(f'Refusing to overwrite original timing evidence: {target}')
for source, target in accepted_copies:
    shutil.copy2(source,target)

def format_time(seconds):
    """Use explicit units so sub-millisecond and multi-minute cells are readable."""
    value = float(seconds)
    if value < 0.001:
        return f'{value * 1000000:.3g} us'
    if value < 1:
        return f'{value * 1000:.3g} ms'
    return f'{value:.3g} s'


def format_memory(byte_count):
    value = float(byte_count)
    if value >= 1073741824:
        return f'{value / 1073741824:.3g} GiB'
    return f'{value / 1048576:.3g} MiB'


def comparison(rows, workload):
    """Rank medians only when both relic and a competitor completed."""
    successful = [row for row in rows if row['workload'] == workload and row['status'] == 'ok']
    reference = next((row for row in successful if row['engine'] == 'relic'), None)
    others = [row for row in successful if row['engine'] != 'relic']
    rival = min(others, key=lambda row: float(row['median'])) if others else None
    return reference, rival


def metric_table(rows, metric):
    """One value per cell; CPU and memory tables remain separate from wall time."""
    selected = engines if int(rows[0]['threads']) == 1 else parallel
    headings = ['relic', 'Souffle', 'SF Magic', 'SF interp', 'Nemo', 'SWI', 'clingo', 'DuckDB', 'SQLite']
    lines = ['| Workload | ' + ' | '.join(headings[engines.index(e)] for e in selected) + ' |',
             '| --- | ' + ' | '.join('---:' for e in selected) + ' |']
    by_cell = {(row['workload'], row['engine']): row for row in rows}
    for workload in sorted({row['workload'] for row in rows}):
        values = [by_cell[workload, engine] for engine in selected]
        successful = [float(row[metric]) for row in values if row['status'] == 'ok']
        minimum = min(successful) if successful else None
        cells = []
        for row in values:
            if row['status'] != 'ok':
                cells.append(row['status'])
                continue
            value = format_memory(row[metric]) if metric == 'peak_bytes' else format_time(row[metric])
            if metric == 'median' and float(row[metric]) == minimum:
                value = '**' + value + '**'
            cells.append(value)
        lines.append('| `' + workload + '` | ' + ' | '.join(cells) + ' |')
    return '\n'.join(lines)


# Preserve raw cohorts; aggregation adds explicit requested/accepted run counts.
counts = collections.Counter(row['status'] for row in all_rows)
for row in all_rows:
    requested = runs_by_scale[int(row['scale'])]
    row['timed_runs_requested'] = requested
    row['timed_runs'] = requested if row['status'] == 'ok' else 0
columns += ['timed_runs_requested', 'timed_runs']
all_rows.sort(key=lambda row: (int(row['scale']), int(row['threads']), row['workload'], engines.index(row['engine'])))
for destination in [repo/'bench/compare/results.tsv', record/'results.tsv']:
    with destination.open('w') as stream:
        writer = csv.DictWriter(stream, fieldnames=columns, delimiter='\t', lineterminator='\n')
        writer.writeheader()
        writer.writerows(all_rows)
(record/'unresolved.json').write_text('[]\n')

summary = []
overview = ['| Scale | Threads | Repetitions | relic lowest median / comparable workloads | Rival cells without a completed time |',
            '| ---: | ---: | ---: | ---: | ---: |']
for scale, threads, rows in scenarios:
    wins = comparable = 0
    for workload in sorted({row['workload'] for row in rows}):
        reference, rival = comparison(rows, workload)
        if reference and rival:
            comparable += 1
            wins += float(reference['median']) <= float(rival['median'])
        summary.append(dict(scale=scale, threads=threads, workload=workload,
                            relic_median_seconds=reference['median'] if reference else '-',
                            best_other_engine=rival['engine'] if rival else '-',
                            best_other_median_seconds=rival['median'] if rival else '-'))
    incomplete = sum(row['engine'] != 'relic' and row['status'] not in {'ok', 'n/a'} for row in rows)
    overview.append(f'| {scale} | {threads} | {runs_by_scale[scale]} | {wins} / {comparable} | {incomplete} |')
with (repo/'bench/compare/suite-summary.tsv').open('w') as stream:
    writer = csv.DictWriter(stream, fieldnames=list(summary[0]), delimiter='\t', lineterminator='\n')
    writer.writeheader()
    writer.writerows(summary)

conditions = f'''{metadata['processor']}: **{metadata['performance_cores']} performance + {metadata['efficiency_cores']} efficiency cores, {metadata['memory_bytes'] / 1073741824:g} GiB RAM**.
OS: {metadata['os']}. Power: {metadata['power']}.
Start load/context: {metadata['load_start']}. Background OS activity remains.
Thermal observations at start: {metadata['thermal_start']}; at end: {metadata['thermal_end'].replace(chr(10), '; ')}. These results describe this machine. FreeBSD measurements
are excluded from these tables.

Scale 1 uses **10 repetitions**; scales 2, 4 and 8 use **3**. The 31 workloads
include three fixed Graspan datasets at scale 1. The other 28 workloads run
at all scales. Scaling follows each generator: the small tree of
`tree-sg-all` has 1024 * scale - 1 nodes, while the tree of `tree-sg-bound`
has depth 11 + scale, so its size grows exponentially. All accepted answers
are checked as exact sets against relic before timing. Verification and compilation
are excluded from execution times.

Times include process startup, input loading, evaluation and output writing.
relic, SWI, clingo, DuckDB and SQLite write to discarded standard output;
Souffle and Nemo export files. Large-answer comparisons include this I/O
difference. User CPU time sums across threads; it can exceed wall time.
Small differences need to be read alongside the recorded timing spread.
'''
method = '''relic 1.0.0 uses Apple clang 21 with `-O3 -g`, `--disable-shared
--enable-threads`. Rivals are Souffle 2.5, Nemo 0.10.1, SWI-Prolog 9.2.9,
clingo 5.8.0, DuckDB 1.5.5 and SQLite 3.51.2; hyperfine is 2.0.0.
Generated Souffle programs use Homebrew clang 23.1.3 and `-O3`, with OpenMP
for parallel runs. The parallel Souffle build uses the
[documented ARM locking patch](bench/compare/tools/README.md); it differs
from the serial build, so its speedup cannot be attributed solely to cores.

`timeout` is a verification that exceeds 300 seconds. `n/a` means no program
for that engine. `no-reference` means relic did not complete verification;
rivals are unmeasured. `engine-limit` diagnoses clingo's internal ID limit.
`resource-limit` diagnoses memory exhaustion after a 120 GiB DuckDB retry.
None of these statuses is a successful timing or an invented 300-second
measurement. Bold marks the lowest observed wall median, without asserting
statistical significance. The summary compares only workloads with a
successful relic time and at least one successful rival time.

SWI's scale-2 `cspa` retry raises table space from 16 to 64 GiB and times out.
DuckDB's graph-left retries raise memory from 102.3 to 120 GiB and time out.
Its scale-8 bounded-tree retries fail while holding 114.1/114.2 GiB and
requesting another 32 GiB, beyond this machine's physical RAM. The interrupted scale-8 serial
graph-left verification is replaced by a complete fresh attempt, which
times out. The scale-4 serial report lacks its shell epilogue; its cell
matrix and samples are independently validated.

The huge scale-8 tree verification uses the
[exact parallel bitmap verifier](bench/compare/tools/README.md#exact-tree-answer-verification).
It preserves set equality and duplicates semantics; its speed is excluded
from engine timings. The recovery retains the original 130 completed
parallel cells and records the remaining tree workloads separately.
'''

full = ['# Benchmark results', '', conditions, '## Comparison overview', '', '\n'.join(overview), '',
        '## Method and limits', '', method, '## Full Mac comparisons', '',
        'Souffle is the compiled mode; SF Magic is compiled with Magic Sets; SF interp is the interpreter. Each cell has explicit time or memory units. `us` denotes microseconds.', '']
for scale, threads, rows in scenarios:
    full.extend([f'### Scale {scale}, {threads} thread' + ('s' if threads != 1 else ''), '',
                 '**Median wall time**', '', metric_table(rows, 'median'), '',
                 '<details>', '<summary>Mean user CPU time and peak resident memory</summary>', '',
                 '**Mean user CPU time**', '', metric_table(rows, 'user'), '',
                 '**Maximum peak resident memory across timed runs**', '', metric_table(rows, 'peak_bytes'), '', '</details>', ''])
full.extend(['## Raw results and reproduction', '',
             '| Artifact | Contents |', '| --- | --- |',
             '| [Canonical results](bench/compare/results.tsv) | All 1,610 cells; median, fastest, spread, user CPU, peak bytes, row counts, compilation time and requested/accepted repetitions |',
             '| [Comparison summary](bench/compare/suite-summary.tsv) | Fastest completed competitor for every workload and scenario |',
             '| [Build instructions](bench/compare/tools/README.md) | Native engine builds and patches |', '',
             'This report and the Performance section of the README are generated by `bench/compare/tools/render-mac-results.py`.', '',
             'The recorded tables use file exports for Souffle/Nemo. The comparison harness serializes timed answers to discarded stdout for every engine. Preserve measurements from these output policies in separate cohorts; the commands below do not reproduce the recorded file-output policy.', '',
             'Run with the documented engines installed (use a fresh artifacts directory):', '',
             '```sh', 'RELIC=/path/to/relic BENCH_SCALE=1 BENCH_RUNS=10 BENCH_THREADS=1 \\',
             '    BENCH_ARTIFACTS=/path/to/fresh-artifacts bench/compare/run.sh -o single.tsv', '',
             'RELIC=/path/to/relic BENCH_SCALE=1 BENCH_RUNS=10 BENCH_THREADS=16 \\',
             '    BENCH_ARTIFACTS=/path/to/fresh-artifacts bench/compare/run.sh \\',
             '    -e souffle-interp,souffle,souffle-magic,duckdb -o parallel.tsv', '```', '',
             'Include `-w cspa-httpd,cspa-postgresql,cspa-linux` in separate scale-1 invocations for the downloaded datasets. Repeat generated workloads at scales 2, 4 and 8 with `BENCH_RUNS=3`. See [methodology](specs/design-bench.md).', ''])
(repo/'BENCHMARKS.md').write_text('\n'.join(full))
(record/'README.md').write_text('# Apple M3 Max raw benchmark results\n\nSee BENCHMARKS.md (relic `BENCHMARKS.md`) for all comparison tables, conditions, limitations and reproduction commands.\n\nThe files here retain all original cohorts, retry outcomes, diagnostics and individual timed samples. [results.tsv](results.tsv) is the validated canonical matrix; [investigated-failures.json](investigated-failures.json) explains diagnosed limits.\n')

highlight_workloads = ['startup', 'andersen', 'chain-closure', 'chain-left', 'cspa',
                       'cspa-httpd', 'cspa-linux', 'cspa-postgresql', 'files-grouped',
                       'graph-left', 'grid-reach', 'tree-sg-all', 'tree-sg-bound']
quick = ['| Workload (scale 1) | relic, 1 thread | Fastest rival, 1 thread | relic, 16 threads | Fastest rival, 16 threads |',
         '| --- | ---: | --- | ---: | --- |']
for workload in highlight_workloads:
    cells = []
    for threads in [1, 16]:
        rows = next(rows for scale, t, rows in scenarios if scale == 1 and t == threads)
        reference, rival = comparison(rows, workload)
        reference_text = format_time(reference['median']) if reference else 'unmeasured'
        rival_text = labels[engines.index(rival['engine'])] + ': ' + format_time(rival['median']) if rival else 'unmeasured'
        if reference and rival:
            if float(reference['median']) <= float(rival['median']):
                reference_text = '**' + reference_text + '**'
            if float(rival['median']) <= float(reference['median']):
                rival_text = '**' + rival_text + '**'
        cells.extend([reference_text, rival_text])
    quick.append('| `' + workload + '` | ' + ' | '.join(cells) + ' |')
by_cell = {(int(r['scale']), int(r['threads']), r['workload'], r['engine']): r for r in all_rows}
tree_relic = by_cell[8, 16, 'tree-sg-all', 'relic']
tree_souffle = by_cell[8, 16, 'tree-sg-all', 'souffle']
bound_relic = by_cell[8, 16, 'tree-sg-bound', 'relic']
bound_magic = by_cell[8, 16, 'tree-sg-bound', 'souffle-magic']
performance = '\n'.join(['## Performance', '',
    'On an Apple M3 Max with **16 cores and 128 GiB RAM**, the comparison covers',
    '**31 workloads at scale 1** and **28 generated workloads at scales 2, 4 and 8**.',
    'Scale 1 uses ten timed runs; larger scales use three. Answers are checked',
    'before timing. These observations describe this Mac; small differences can',
    'overlap timing spread, and not every competitor completes every workload.', '',
    '\n'.join(overview), '',
    'Selected workloads below show median wall time with explicit units (`us` = microseconds).',
    'The rival column picks the fastest completed competitor in that scenario.', '',
    '\n'.join(quick), '',
    'relic does not lead every test: Magic Sets helps Souffle on bound queries,',
    'and DuckDB wins some joins and aggregations. In the scale-8 parallel full-tree',
    f"case, relic takes **{format_time(tree_relic['median'])}** versus compiled Souffle's **{format_time(tree_souffle['median'])}**, while using",
    f"**{format_memory(tree_relic['peak_bytes'])}** versus **{format_memory(tree_souffle['peak_bytes'])}** peak memory. The bounded tree favors Souffle",
    f"Magic: **{format_time(bound_magic['median'])}** versus relic's **{format_time(bound_relic['median'])}**. Large-answer comparisons include",
    'file exports for Souffle/Nemo and discarded stdout for the other engines.',
    'The benchmark harness uses discarded stdout for every engine; results from',
    'that output policy belong in a separate cohort.', '',
    'The [full benchmark report](BENCHMARKS.md) contains every workload and scale,',
    'separate CPU and memory tables, build conditions and diagnosed limits.',
    'It also describes the retries with raised memory limits and their outcomes.',
    '[Methodology](specs/design-bench.md) explains measurement and reproduction.', '',
    'FreeBSD results are excluded from these tables.', '',])
readme = repo/'README.md'
text = readme.read_text()
start = text.index('## Performance\n')
end = re.search(r'^## ', text[start + len('## Performance\n'):], re.M)
if end is None:
    raise RuntimeError('README has no section after Performance')
stop = start + len('## Performance\n') + end.start()
readme.write_text(text[:start] + performance + '\n' + text[stop:])
print(f'Published {len(all_rows)} cells: {dict(counts)}')
