import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
from os.path import relpath
import shutil
from tempfile import TemporaryDirectory

import run_experiment as intensity
from scale_traces import file_sha256, prepare_traces, write_json


OUTPUT = intensity.OUTPUT.with_name(intensity.OUTPUT.name + '-all-traces')
TRACE_SET = OUTPUT.parent / 'mqsim-soml-read-wl178-no-cache-adaptive-pacs-cross-plane' / 'experiment.json'
FACTORS = (1, 100, 1000, 10000)
COMPARISONS = (
    ('pacs-full', 'baseline'), ('pacs-full', 'pacs-nowl'), ('pacs-full', 'lepa'),
    ('pacs-full', 'lepa-wl-aware'), ('pacs-nowl', 'baseline'),
)


def reuse_case(case, trace, timing, source_dir, output, source_label):
    assert (case['reads'], case['writes']) == (timing['reads'], timing['writes'])
    assert case['requests_completed'] == timing['reads'] + timing['writes']
    assert case['simulated_duration_ns'] >= timing['last_timestamp_ns']
    files = {}
    for key in ('latency_samples', 'result_xml', 'log'):
        source = (source_dir / case[key]).resolve(strict=True)
        files[key] = relpath(source, output)
    assert len(list((output / files['latency_samples']).glob(f'{trace}_latency_*.csv.gz'))) == 4
    return {**case, **files, 'source': source_label}


def archived_controls(reference, traces):
    stored = intensity.OUTPUT / 'metrics.json'
    results = json.loads(stored.read_text())
    trace_manifest = intensity.OUTPUT / 'trace_manifest.json'
    assert file_sha256(trace_manifest) == reference['trace_manifest_sha256']
    previous = json.loads(trace_manifest.read_text())['traces']
    assert results['settings'] == reference['settings']
    controls = {}
    for trace in reference['trace_names']:
        assert traces[trace]['1'] == previous[trace]['1']
        policies = results['per_trace'][trace]['1']['policies']
        assert set(policies) == set(reference['policy_options'])
        for mode, case in policies.items():
            controls[trace, 1, mode] = reuse_case(case, trace, traces[trace]['1'], intensity.OUTPUT, OUTPUT, 'archived_1x')
    return controls, file_sha256(stored)


def compare(case, reference):
    return {key: 100 * (value / reference['latency'][key] - 1) for key, value in case['latency'].items()}


def summarize(cases, manifest, traces, output=OUTPUT):
    modes = manifest['policy_options']
    factors = manifest['factors']
    report = {}
    for trace, timing in traces.items():
        by_factor = {}
        for factor in factors:
            policies = {mode: cases[trace, factor, mode] for mode in modes}
            by_factor[str(factor)] = {
                'policies': policies,
                'latency_change_vs_1x_percent': {mode: compare(case, cases[trace, 1, mode])
                                                for mode, case in policies.items()},
                'latency_change_percent': {f'{method}_vs_{reference}': compare(policies[method], policies[reference])
                                          for method, reference in COMPARISONS},
            }
        report[trace] = {
            'zero_interval_pairs': timing['1']['zero_interval_pairs'],
            'request_count': timing['1']['reads'] + timing['1']['writes'],
            'factors': by_factor,
        }
    archived_count = manifest.get('archived_case_count', manifest['archived_1x_count'])
    write_json(output / 'metrics.json', {
        'trace_count': len(traces), 'run_count': len(cases),
        'new_replays': len(cases) - archived_count,
        'archived_1x_count': manifest['archived_1x_count'],
        **({'archived_case_count': archived_count} if 'archived_case_count' in manifest else {}),
        'factors': factors, 'settings': manifest['settings'],
        'experiment': 'experiment.json', 'trace_manifest': manifest.get('trace_manifest', 'trace_manifest.json'),
        'definitions': {
            'latency': 'Device response time per host request in ms. Percentiles use nearest rank.',
            'latency_change_vs_1x_percent': 'Compare each policy with its own 1x result on the same trace.',
            'latency_change_percent': 'Compare two policies on the same trace and at the same factor.',
            'zero_interval_pairs': 'Adjacent requests sharing the same arrival timestamp. Scaling preserves all of them.',
        }, 'per_trace': report,
    })


def replay_missing(cases, manifest, traces, output, jobs):
    pilot = intensity.load_module('wl_reference', intensity.PILOT / 'run_experiment.py')
    prior = json.loads((intensity.PILOT / 'metrics.json').read_text())['per_trace']
    assert set(pilot.MODES) == set(manifest['policy_options'])
    factors = manifest['factors']
    order = sorted(traces, key=lambda name: -(traces[name]['1']['reads'] + traces[name]['1']['writes']))
    tasks = [(trace, factor, mode) for trace in order for mode in reversed(pilot.MODES) for factor in reversed(factors)
             if (trace, factor, mode) not in cases]
    runners = {}
    for factor in sorted({factor for _, factor, _ in tasks}):
        runner = intensity.load_module(f'replay_all_{factor}x', intensity.RELATED / 'run_experiment.py')
        runner.EXPERIMENT = output / f'{factor}x'
        runner.MODES = pilot.MODES
        runner.verify_inputs(intensity.TEMPLATE)
        runners[factor] = runner
    count = len(tasks) + len(cases)
    print(f'{len(cases)} archived cases, {len(tasks)} replays to verify with {jobs} workers', flush=True)
    failures = []
    with TemporaryDirectory(prefix='mqsim-intensity-all-') as work:
        templates = {mode: intensity.device_template(work, mode, policy, manifest['policy_options'][mode])
                     for mode, policy in pilot.MODES.items()}

        def replay(trace, factor, mode):
            runner = runners[factor]
            timing = traces[trace][str(factor)]
            case = runner.run_case(templates[mode], 'MQSim', trace, mode, timing)
            older = prior.get(trace, {}).get('policies', {}).get(mode) if factor == 1 else None
            return intensity.verify_case(case, runner, trace, factor, mode, timing, older, pilot, manifest, output)

        with ThreadPoolExecutor(max_workers=jobs) as pool:
            pending = {pool.submit(replay, *key): key for key in tasks}
            for job in as_completed(pending):
                trace, factor, mode = pending[job]
                try:
                    cases[trace, factor, mode] = job.result()
                    print(f'[{len(cases)}/{count}] {trace} {factor}x {mode} validated', flush=True)
                except Exception as error:
                    failures.append((trace, factor, mode))
                    print(f'FAILED {trace} {factor}x {mode}: {error!r}', flush=True)
    if failures:
        raise SystemExit(f'{len(failures)} failed cases: {failures}')


def main():
    parser = argparse.ArgumentParser(description='Replay the full 16-trace workload at 1x, 100x, 1000x and 10000x.')
    parser.add_argument('--jobs', type=int, default=16)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if shutil.which('pigz') is None:
        parser.error('pigz is required to store lossless latency archives')

    originals = json.loads(TRACE_SET.read_text())['traces']
    assert len(originals) == 16
    traces = prepare_traces(output=OUTPUT, factors=FACTORS, trace_names=tuple(originals),
                            originals=originals, jobs=args.jobs)['traces']
    reference = json.loads((intensity.OUTPUT / 'experiment.json').read_text())
    for name, digest in reference['template_sha256'].items():
        assert file_sha256(intensity.TEMPLATE / name) == digest
    cases, control_sha = archived_controls(reference, traces)
    manifest = {
        **reference, 'factors': list(FACTORS), 'trace_names': list(traces),
        'trace_manifest_sha256': file_sha256(OUTPUT / 'trace_manifest.json'),
        'reference_experiment': relpath(intensity.OUTPUT / 'metrics.json', OUTPUT),
        'reference_metrics_sha256': control_sha, 'archived_1x_count': len(cases),
    }
    path = OUTPUT / 'experiment.json'
    if path.exists():
        assert json.loads(path.read_text()) == manifest, 'Experiment inputs changed'
    else:
        write_json(path, manifest)
    replay_missing(cases, manifest, traces, OUTPUT, args.jobs)
    summarize(cases, manifest, traces)
    print(f'{len(cases)} verified combinations. Results in {OUTPUT / "metrics.json"}', flush=True)


if __name__ == '__main__':
    main()
