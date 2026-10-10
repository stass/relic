#!/usr/bin/env python3
"""Publication rejects damaged evidence without changing original records."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

source = Path(__file__).resolve().parents[1]
relative_record = Path('bench/compare/recorded/m3-max')
relative_renderer = Path('bench/compare/tools/render-mac-results.py')


def fingerprint(folder):
    return {str(path.relative_to(folder)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in folder.rglob('*') if path.is_file()}


def replace_text(path, text):
    # Replace the entry so each damage case can restore its original bytes.
    temporary = path.with_name(path.name + '.test-edit')
    temporary.write_text(text)
    temporary.replace(path)


with tempfile.TemporaryDirectory() as directory:
    fixture = Path(directory)
    script = fixture / relative_renderer
    script.parent.mkdir(parents=True)
    shutil.copy2(source / relative_renderer, script)
    record = fixture / relative_record
    shutil.copytree(source / relative_record, record)
    for dataset in (source / 'bench/compare').glob('*/dataset'):
        target = fixture / 'bench/compare' / dataset.parent.name / 'dataset'
        target.parent.mkdir()
        shutil.copy2(dataset, target)

    def check(expected=None):
        result = subprocess.run([sys.executable, str(script), '--check'], capture_output=True, text=True)
        if expected is None:
            assert result.returncode == 0, result.stderr
        else:
            assert result.returncode == 1 and expected in result.stderr, result

    before = fingerprint(record)
    check()
    assert fingerprint(record) == before, '--check changed evidence'

    def damaged(path, transform, expected):
        original = path.read_text()
        replace_text(path, transform(original))
        check(expected)
        replace_text(path, original)

    retry_reference = record / 'retry-samples/scale-2/threads-1/cspa.relic.json'
    def failed_invocation(text):
        report = json.loads(text)
        report['results'][0]['measurements'][0]['exit_code'] = 1
        return json.dumps(report)
    damaged(retry_reference, failed_invocation, 'Failed timed invocation')

    def wrong_median(text):
        lines = text.splitlines()
        fields = lines[0].split('\t')
        row = lines[1].split('\t')
        row[fields.index('median')] = '99'
        lines[1] = '\t'.join(row)
        return '\n'.join(lines) + '\n'
    cohort = record / 'scale-1-threads-1.tsv'
    damaged(cohort, wrong_median, 'differs from samples')
    damaged(cohort, lambda text: text + text.splitlines()[1] + '\n', 'Duplicate or unexpected cell')

    resource_evidence = record / 'retry-scale-8-threads-1.stderr.txt'
    damaged(resource_evidence, lambda text: text.replace('tree-sg-bound', 'wrong-workload'),
            'lacks matching failure header')
    damaged(record / 'retry-scale-8-threads-1.txt',
            lambda text: text.replace('memory limit 120 GiB', 'memory limit 121 GiB'),
            'lacks matching allowance')

    def orphan_investigation(text):
        entries = json.loads(text)
        entries[-1]['threads'] = 128
        return json.dumps(entries)
    damaged(record / 'investigated-failures.json', orphan_investigation, 'Investigation contains an unused cell')

    interrupted = record / 'scale-1-threads-16.interrupted'
    interrupted.write_text('Requires a fresh cohort\n')
    check('Interrupted cohort cannot be published')
    interrupted.unlink()
    check()

print('Report audit: valid evidence, damaged samples, diagnostics and interruption gates passed')
