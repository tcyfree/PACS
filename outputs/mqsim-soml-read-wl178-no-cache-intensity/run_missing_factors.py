import argparse
import json
from os.path import relpath
from pathlib import Path
import shutil

import run_all_traces as full
from run_experiment import OUTPUT as EARLY, TEMPLATE
from scale_traces import FACTORS as EARLY_FACTORS, FORMULA, file_sha256, prepare_traces, write_json


OUTPUT = EARLY.with_name(EARLY.name + '-seven-factors')
MISSING_FACTORS = (5, 10, 50)
FACTORS = sorted((*full.FACTORS, *MISSING_FACTORS))


def read(path):
    return json.loads(path.read_text())


def record(path, value):
    if path.is_file():
        assert read(path) == value, f'Inputs changed for {path}'
    else:
        write_json(path, value)


def checked_trace_manifest(folder, experiment):
    path = folder / 'trace_manifest.json'
    assert file_sha256(path) == experiment['trace_manifest_sha256']
    data = read(path)
    assert data['timestamp_transform'] == FORMULA and data['time_unit'] == 'NANOSECOND'
    assert data['factors'] == experiment['factors']
    return data['traces']


def prepare(jobs):
    early_inputs = read(EARLY / 'experiment.json')
    long_inputs = read(full.OUTPUT / 'experiment.json')
    for key in ('settings', 'policy_options', 'template_sha256', 'decision_snapshot_source'):
        assert early_inputs[key] == long_inputs[key], key
    for name, digest in early_inputs['template_sha256'].items():
        assert file_sha256(TEMPLATE / name) == digest
    originals = read(full.TRACE_SET)['traces']
    assert list(originals) == long_inputs['trace_names'] and len(originals) == 16
    early = checked_trace_manifest(EARLY, early_inputs)
    long = checked_trace_manifest(full.OUTPUT, long_inputs)
    missing = tuple(trace for trace in originals if trace not in early)
    assert len(early) == 4 and len(missing) == 12 and set(MISSING_FACTORS) <= set(EARLY_FACTORS)
    new = prepare_traces(output=OUTPUT, factors=(1, *MISSING_FACTORS), trace_names=missing,
                         originals=originals, jobs=jobs)['traces']

    traces = {}
    for trace in originals:
        mid = early[trace] if trace in early else new[trace]
        original = long[trace]['1']
        assert original == mid['1'], trace
        archived = list(long[trace].values()) + ([mid[str(factor)] for factor in MISSING_FACTORS] if trace in early else [])
        for timing in archived:
            source = Path(timing['path'])
            info = source.stat()
            assert (info.st_size, info.st_mtime_ns) == (timing['size'], timing['mtime_ns']), source
            assert file_sha256(source) == timing['sha256'], source
        combined = {**long[trace], **{str(factor): mid[str(factor)] for factor in MISSING_FACTORS}}
        traces[trace] = {str(factor): combined[str(factor)] for factor in FACTORS}
        for factor, timing in traces[trace].items():
            assert timing['payload_sha256'] == original['payload_sha256']
            assert timing['span_ns'] == int(factor) * original['span_ns']
            assert timing['zero_interval_pairs'] == original['zero_interval_pairs']
    trace_path = OUTPUT / 'all_trace_manifest.json'
    record(trace_path, {'factors': FACTORS, 'timestamp_transform': FORMULA, 'time_unit': 'NANOSECOND', 'traces': traces})

    cases, archives = {}, {}
    for folder, names, factors, source_label in (
        (full.OUTPUT, originals, full.FACTORS, 'archived_all_traces'),
        (EARLY, early, MISSING_FACTORS, 'archived_four_traces'),
    ):
        path = folder / 'metrics.json'
        data = read(path)
        assert data['settings'] == early_inputs['settings']
        archives[relpath(path, OUTPUT)] = file_sha256(path)
        for trace in names:
            block = data['per_trace'][trace]
            for factor in factors:
                policies = block.get('factors', block)[str(factor)]['policies']
                assert set(policies) == set(early_inputs['policy_options'])
                for mode, case in policies.items():
                    key = trace, factor, mode
                    assert key not in cases
                    cases[key] = full.reuse_case(case, trace, traces[trace][str(factor)], folder, OUTPUT, source_label)
    assert len(cases) == 380
    manifest = {
        **early_inputs, 'factors': FACTORS, 'trace_names': list(traces),
        'trace_manifest': trace_path.name, 'trace_manifest_sha256': file_sha256(trace_path),
        'archives': archives, 'archived_case_count': len(cases), 'archived_1x_count': 80,
    }
    record(OUTPUT / 'experiment.json', manifest)
    return cases, manifest, traces


def main():
    parser = argparse.ArgumentParser(description='Fill the 5x, 10x and 50x gaps and compare all seven factors.')
    parser.add_argument('--jobs', type=int, default=16)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if shutil.which('pigz') is None:
        parser.error('pigz is required to store lossless latency archives')
    cases, manifest, traces = prepare(args.jobs)
    full.replay_missing(cases, manifest, traces, OUTPUT, args.jobs)
    assert len(cases) == 560
    full.summarize(cases, manifest, traces, OUTPUT)
    print(f'{len(cases)} verified combinations. Results in {OUTPUT / "metrics.json"}', flush=True)


if __name__ == '__main__':
    main()
