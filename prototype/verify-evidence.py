#!/usr/bin/env python3
"""Check the saved VM reports and match packaged PE files to native hashes."""
from pathlib import Path
import hashlib
import json

root = Path('evidence/async-final-results')
def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))

reg = read(root / 'cross-report.json')
external = read(root / 'unmodified/report.json')
exceptions = read(root / 'exception-report.json')
assert reg['unexpectedFailures'] == 0 and len(reg['tests']) == 29
assert external['requiredFailures'] == 0 and len(external['tests']) == 16
assert exceptions['requiredFailures'] == 0 and len(exceptions['tests']) == 8
assert sum(row['required'] for row in exceptions['tests']) == 7
assert sum(row['required'] for row in external['tests']) == 15
assert all(not row.get('timedOut', False) for row in reg['tests'] + external['tests'] + exceptions['tests'])
assert all(row['passed'] for row in external['roundTripHashes'])
for name, count in [('runtime-repeat.json', 120), ('unmodified/repeat.json', 50), ('exception-repeat.json', 90)]:
    rows = read(root / name)
    assert len(rows) == count
    assert all(row.get('passed', row['exitCode'] == 0 and not row['timedOut']) for row in rows)
initial_io = read(root / 'async-initial-report.json')
repeat_io = read(root / 'async-report.json')
assert len(initial_io) == 2 and all(row['passed'] for row in initial_io)
assert len(repeat_io) == 31 and all(row['passed'] for row in repeat_io)
assert sum(not row['native'] for row in repeat_io) == 30
initial_pipe = read(root / 'pipe-control-initial-report.json')
repeat_pipe = read(root / 'pipe-control-report.json')
assert len(initial_pipe) == 2 and all(row['passed'] for row in initial_pipe)
assert len(repeat_pipe) == 31 and all(row['passed'] for row in repeat_pipe)
assert sum(not row['native'] for row in repeat_pipe) == 30
for rows, name in [(initial_io, 'async_event32'), (repeat_io, 'async_event32'),
                   (initial_pipe, 'pipe_control32'), (repeat_pipe, 'pipe_control32')]:
    for row in rows:
        assert row['probe'] == 'guest/' + name + '.exe'
        assert name + ':' in row['stdout'] and 'passed' in row['stdout']
        assert not row['timedOut'] and row['exitCode'] == row['peerExitCode'] == 0

prefix = 'c:/wineprobe/prototype/out/'
matched = set()
for row in read('evidence/async-final-native-hashes.json'):
    native_path = row['Path'].replace('\\', '/')
    assert native_path.lower().startswith(prefix), native_path
    relative = native_path[len(prefix):]
    path = Path('prototype/out') / relative
    # Windows filenames are case-insensitive. These outputs use consistent
    # source spelling, apart from Windows occasionally capitalizing drives.
    assert path.is_file(), path
    assert hashlib.sha256(path.read_bytes()).hexdigest().upper() == row['Hash'], path
    matched.add(path)
pe_files = {p for p in Path('prototype/out').rglob('*') if p.suffix.lower() in ('.exe', '.dll')}
assert pe_files == matched, sorted(pe_files.symmetric_difference(matched))
implementation = read('evidence/tested-implementation.json')
execution = read('evidence/candidate-files.json')
assert execution['commit'] == implementation['commit']
for row in execution['files']:
    path = Path('prototype') / row['path']
    assert hashlib.sha256(path.read_bytes()).hexdigest().upper() == row['sha256'], path
for row in implementation['files']:
    path = Path(row['path'])
    assert hashlib.sha256(path.read_bytes()).hexdigest() == row['sha256'], path
summary = {
    'requiredChecks': 55, 'requiredFailures': 0, 'roundTripHashes': 2,
    'repeatRuns': 320, 'repeatFailures': 0,
    'failingDiagnostics': [
        {'name': row['name'], 'exitCode': row['exitCode']}
        for row in external['tests'] + exceptions['tests'] if not row['passed']
    ],
    'os': 'Windows 11 24H2 x64 build 26100',
    'matchingNativePEFiles': len(matched),
    'matchingExecutionInputs': len(execution['files']),
    'matchingSourceAndInputFiles': len(implementation['files']),
    'wineOwnsApplicationLoading': True, 'pendingEventReadWriteEstablished': True,
    'targetedCancelIoExEstablished': True, 'pendingNamedPipeControlsEstablished': True,
    'issuingThreadExitEstablished': True, 'multipleObjectWaitsEstablished': True,
    'testedImplementationCommit': implementation['commit'],
    'apcIocpEventlessIOEstablished': False
}
Path('evidence/async-final-summary.json').write_text(json.dumps(summary, indent=2) + '\n')
paths = []
for directory in ['prototype', 'wine-11.0-source']:
    for path in Path(directory).rglob('*'):
        if path.is_file() and not path.is_symlink() and not (
            'out/results' in str(path) or path.suffix in ['.o', '.obj']
        ):
            paths.append(path)
paths += [Path('wine-wow64-feasibility.html'), Path('evidence/async-final-summary.json')]
paths += [Path('evidence/async-final-native-hashes.json'), Path('evidence/tested-implementation.json')]
paths += [Path('evidence/candidate-files.json')]
paths += list(root.rglob('*.json'))
Path('evidence/async-final-SHA256SUMS').write_text(''.join(
    hashlib.sha256(path.read_bytes()).hexdigest() + '  ' + str(path) + '\n'
    for path in sorted(set(paths))
))
print(json.dumps(summary, indent=2))
