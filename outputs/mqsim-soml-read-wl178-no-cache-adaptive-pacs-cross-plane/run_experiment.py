import argparse
from concurrent.futures import ThreadPoolExecutor
import filecmp
from hashlib import sha256
import importlib.util
from pathlib import Path
import shutil
from statistics import median
import sys
from tempfile import TemporaryDirectory
from xml.etree import ElementTree as ET


OUTPUT = Path(__file__).resolve().parent
TEMPLATE = OUTPUT / 'template'
REFERENCE = OUTPUT.parent / 'mqsim-soml-read-wl178-no-cache-wl-aware-lepa-vs-adaptive-pacs-all-traces'
MODES = {'baseline': None, 'lepa-wl-aware': 'LEPA', 'pacs-adaptive': 'PACS'}


def load_reference_runner():
    spec = importlib.util.spec_from_file_location('reference_runner', REFERENCE / 'run_experiment.py')
    reference_runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reference_runner)
    runner = reference_runner.load_runner()
    runner.EXPERIMENT = OUTPUT
    runner.MODES = MODES
    reference_runner.TEMPLATE = TEMPLATE
    return reference_runner, runner


def prepare(runner, simulator):
    reference = runner.read_json(REFERENCE / 'experiment.json')
    TEMPLATE.mkdir(exist_ok=True)
    if not (TEMPLATE / 'MQSim').is_file():
        if simulator is None:
            raise ValueError('Pass the compiled Adaptive PACS binary with --simulator on the first run')
        shutil.copy2(simulator, TEMPLATE / 'MQSim')
    if simulator is not None:
        assert filecmp.cmp(simulator, TEMPLATE / 'MQSim', shallow=False), 'Simulator differs from the recorded run'
    for name in ('ssdconfig.xml', 'workload.xml'):
        if not (TEMPLATE / name).is_file():
            shutil.copy2(REFERENCE / 'template' / name, TEMPLATE / name)
        assert sha256((TEMPLATE / name).read_bytes()).hexdigest() == reference['template_sha256'][name], name
    runner.verify_inputs(TEMPLATE)

    for name, trace in reference['traces'].items():
        metadata = Path(trace['path']).stat()
        assert (metadata.st_size, metadata.st_mtime_ns) == (trace['size'], trace['mtime_ns']), name
    manifest = {
        'traces': reference['traces'],
        'settings': reference['settings'],
        'policies': list(MODES),
        'policy_options': reference['policy_options'],
        'adaptive_pacs': {
            'include_single_page_writes': True,
            'allow_different_page_types': True,
            'allow_different_planes': True,
            'queue_estimate': 'Offline latency at each queued program address and residual active work',
            'local_gain': 'Strictly positive predicted request completion-time gain',
            'cross_channel_gain': 'Strictly positive and at least beta times the critical page program latency',
        },
        'reference_metrics': str(REFERENCE / 'metrics-with-baseline.json'),
        'template_sha256': {name: sha256((TEMPLATE / name).read_bytes()).hexdigest()
                            for name in ('MQSim', 'ssdconfig.xml', 'workload.xml')},
    }
    path = OUTPUT / 'experiment.json'
    if path.is_file():
        assert runner.read_json(path) == manifest, 'Experiment inputs changed'
    else:
        runner.write_json(path, manifest)
    return manifest


def replay(runner, template, trace, mode, details, reference):
    actual = runner.run_case(template, 'MQSim', trace, mode, details)
    prior = reference[trace]['policies'][mode]
    for key in ('requests_completed', 'reads', 'writes', 'single_page_writes', 'multi_page_writes',
                'host_write_page_transactions'):
        assert actual[key] == prior[key], (trace, mode, key)
    if mode in ('baseline', 'lepa-wl-aware'):
        assert actual['latency'] == prior['latency'] and actual['policy_counters'] == prior['policy_counters'], (trace, mode)
        for index in range(4):
            file = f'{trace}_latency_{index}.csv'
            assert filecmp.cmp(OUTPUT / actual['latency_samples'] / file,
                               REFERENCE / prior['latency_samples'] / file, shallow=False), (mode, file)
    else:
        counters = actual['policy_counters']
        assert counters['Program_Latency_Source'] == 'CONFIGURED'
        for key in ('Qualified_Program_Observations', 'Discarded_Program_Observations', 'Open_Request_Predictions_At_End'):
            assert counters[key] == '0', (trace, key)
        for key in ('Single_Page_Cross_Channel_Writes', 'Single_Page_Cross_Plane_Writes'):
            assert 0 <= int(counters[key]) <= actual['single_page_writes'], (trace, key)
        assert 0 <= int(counters['Cross_Plane_Multipage_Write_Pages']) <= actual['host_write_page_transactions'] - actual['single_page_writes']
        search = ET.parse(OUTPUT / actual['result_xml']).find('./SSDDevice/PACS/Adaptive_Search')
        assert float(search.findtext('Gain_Beta')) == 1.0 and int(search.findtext('Search_Budget')) == 256
    return actual


def summarize(runner, manifest, results):
    per_trace = {}
    comparisons = (('lepa-wl-aware', 'baseline'), ('pacs-adaptive', 'baseline'), ('pacs-adaptive', 'lepa-wl-aware'))
    for trace in manifest['traces']:
        cases = {mode: results[trace, mode] for mode in MODES}
        relative = {
            mode: {metric: case['latency'][metric] / cases['baseline']['latency'][metric]
                   for metric in runner.LATENCY_METRICS}
            for mode, case in cases.items()
        }
        per_trace[trace] = {
            'policies': cases,
            'latency_relative_to_baseline': relative,
            'change_vs_baseline_percent': {
                mode: {metric: (ratio - 1) * 100 for metric, ratio in metrics.items()}
                for mode, metrics in relative.items()
            },
        }
    relative_summary = {}
    for method, reference in comparisons:
        metrics = {}
        for metric in runner.LATENCY_METRICS:
            delta = [(row['policies'][method]['latency'][metric] / row['policies'][reference]['latency'][metric] - 1) * 100
                     for row in per_trace.values()]
            metrics[metric] = {
                'median_percent': median(delta), 'lower': sum(x < 0 for x in delta),
                'equal': sum(x == 0 for x in delta), 'higher': sum(x > 0 for x in delta),
            }
        relative_summary[f'{method}_vs_{reference}'] = metrics
    runner.write_json(OUTPUT / 'metrics-with-baseline.json', {
        'trace_count': len(per_trace), 'run_count': len(results), 'settings': manifest['settings'],
        'requests_per_policy': sum(t['reads'] + t['writes'] for t in manifest['traces'].values()),
        'simulator_sha256': manifest['template_sha256']['MQSim'],
        'reference_sample_parity': {'baseline': len(per_trace), 'lepa-wl-aware': len(per_trace)},
        'normalization': 'Divide by the same trace and metric under Baseline. Negative % means lower latency.',
        'relative_summary': relative_summary,
        'per_trace': per_trace,
    })
    print(f'{len(results)} verified replays. Results in {OUTPUT / "metrics-with-baseline.json"}', flush=True)


def main():
    parser = argparse.ArgumentParser(description='Replay Baseline, WL-aware LEPA and cross-plane offline Adaptive PACS.')
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--simulator', type=Path)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    reference_runner, runner = load_reference_runner()
    manifest = prepare(runner, args.simulator)
    reference = runner.read_json(REFERENCE / 'metrics-with-baseline.json')['per_trace']
    traces = manifest['traces']
    order = sorted(traces, key=lambda name: -(traces[name]['reads'] + traces[name]['writes']))
    with TemporaryDirectory(prefix='mqsim-pacs-cross-plane-') as work:
        templates = {
            mode: reference_runner.device_template(work, mode) if policy else TEMPLATE
            for mode, policy in MODES.items()
        }
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            pending = {
                (trace, mode): pool.submit(replay, runner, templates[mode], trace, mode, traces[trace], reference)
                for trace in order for mode in ('pacs-adaptive', 'lepa-wl-aware', 'baseline')
            }
            results = {case: job.result() for case, job in pending.items()}
    summarize(runner, manifest, results)


if __name__ == '__main__':
    sys.dont_write_bytecode = True
    main()
