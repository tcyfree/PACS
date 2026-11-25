import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import filecmp
from hashlib import sha256
import importlib.util
from pathlib import Path
from statistics import median
import sys
from tempfile import TemporaryDirectory
from xml.etree import ElementTree as ET


OUTPUT = Path(__file__).resolve().parent
TEMPLATE = OUTPUT / 'template'
HISTORY = OUTPUT.parent
REPLAY = HISTORY / 'mqsim-soml-read-wl178-no-cache-related-work-all-traces'
REFERENCE = HISTORY / 'mqsim-soml-read-wl178-no-cache-lepa-wl-aware-1612_2'
NORMALIZED = HISTORY / 'mqsim-soml-read-wl178-no-cache'
LEPA, ADAPTIVE = 'lepa-wl-aware', 'pacs-adaptive'
MODES = {LEPA: 'LEPA', ADAPTIVE: 'PACS'}
OPTIONS = {
    LEPA: {'LEPA_WL_Aware_Enabled': 'true'},
    ADAPTIVE: {
        'PACS_Request_Level_Enabled': 'true',
        'PACS_Cross_Channel_Enabled': 'true',
        'PACS_Cross_Channel_Adaptive_Enabled': 'true',
        'PACS_Cross_Channel_Gain_Beta': '1',
        'PACS_Cross_Channel_Search_Budget': '256',
    },
}


def load_runner():
    spec = importlib.util.spec_from_file_location('replay_runner', REPLAY / 'run_experiment.py')
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    runner.EXPERIMENT = OUTPUT
    runner.MODES = MODES
    return runner


def prepare(runner):
    runner.verify_inputs(TEMPLATE)
    old_batch = runner.read_json(REPLAY / 'experiment.json')
    traces = old_batch['traces']
    for name, details in traces.items():
        path = NORMALIZED / Path(details['history_batch']).name / 'sortedTrace' / (name + '.csv')
        info = path.stat()
        assert (info.st_size, info.st_mtime_ns) == (details['size'], details['mtime_ns']), name
        details['path'] = str(path)
        details['history_batch'] = str(path.parents[1])
    assert len(traces) == 16 and sum(t['reads'] + t['writes'] for t in traces.values()) == 30_560_937
    reference = runner.read_json(REFERENCE / 'experiment.json')
    manifest = {
        'policies': list(MODES), 'settings': reference['settings'], 'traces': traces,
        'policy_options': OPTIONS,
        'template_sha256': {name: sha256((TEMPLATE / name).read_bytes()).hexdigest()
                            for name in ('MQSim', 'ssdconfig.xml', 'workload.xml')},
    }
    assert manifest['template_sha256'] == reference['template_sha256']
    manifest_path = OUTPUT / 'experiment.json'
    if manifest_path.is_file():
        assert runner.read_json(manifest_path) == manifest, 'Experiment inputs changed'
    else:
        runner.write_json(manifest_path, manifest)
    return manifest


def device_template(workdir, mode):
    template = Path(workdir) / mode
    template.mkdir()
    for item in ('MQSim', 'workload.xml'):
        (template / item).symlink_to(TEMPLATE / item)
    content = (TEMPLATE / 'ssdconfig.xml').read_text()
    marker = f'<{MODES[mode]}_Enabled>false</{MODES[mode]}_Enabled>'
    assert content.count(marker) == 1
    options = OPTIONS[mode]
    assert all(f'<{tag}>' not in content for tag in options)
    tags = ''.join(f'\n\t\t<{tag}>{value}</{tag}>' for tag, value in options.items())
    (template / 'ssdconfig.xml').write_text(content.replace(marker, marker + tags))
    return template


def replay(runner, template, trace, mode, details):
    result = runner.run_case(template, 'MQSim', trace, mode, details)
    counters = result['policy_counters']
    if mode == LEPA:
        assert counters['WL_Aware'] == 'true' and counters['Program_Latency_Source'] == 'CONFIGURED'
        assert counters['Outstanding_Program_Time_ns'] == '0'
    else:
        assert counters['Program_Latency_Source'] == 'CONFIGURED'
        assert counters['Qualified_Program_Observations'] == counters['Discarded_Program_Observations'] == '0'
        assert counters['Open_Request_Predictions_At_End'] == '0'
        xml = ET.parse(OUTPUT / result['result_xml'])
        search = xml.find('./SSDDevice/PACS/Adaptive_Search')
        assert float(search.findtext('Gain_Beta')) == 1.0 and int(search.findtext('Search_Budget')) == 256
    return result


def compare_to_reference(results, runner):
    reference = runner.read_json(REFERENCE / 'metrics.json')['policies']
    for mode in MODES:
        actual = results[('1612_2', mode)]
        previous = reference[mode]
        assert actual['latency'] == previous['latency'] and actual['policy_counters'] == previous['policy_counters']
        for index in range(4):
            file = f'1612_2_latency_{index}.csv'
            assert filecmp.cmp(OUTPUT / actual['latency_samples'] / file,
                               REFERENCE / previous['latency_samples'] / file, shallow=False), (mode, file)


def summarize(results, manifest, runner):
    per_trace = {}
    for trace in manifest['traces']:
        policies = {mode: results[(trace, mode)] for mode in MODES}
        lepa, adaptive = (policies[mode] for mode in (LEPA, ADAPTIVE))
        for key in ('requests_completed', 'reads', 'writes', 'multi_page_writes', 'host_write_page_transactions'):
            assert lepa[key] == adaptive[key], (trace, key)
        old, new = lepa['latency'], adaptive['latency']
        per_trace[trace] = {
            'policies': policies,
            'adaptive_vs_wl_aware_lepa_percent': {
                key: (new[key] / old[key] - 1) * 100 if old[key] and new[key] is not None else None
                for key in runner.LATENCY_METRICS
            },
        }
    summary = {}
    for key in runner.LATENCY_METRICS:
        changes = {trace: row['adaptive_vs_wl_aware_lepa_percent'][key] for trace, row in per_trace.items()
                   if row['adaptive_vs_wl_aware_lepa_percent'][key] is not None}
        values = list(changes.values())
        summary[key] = {
            'median_percent': median(values),
            'adaptive_lower': sum(v < 0 for v in values), 'equal': sum(v == 0 for v in values),
            'wl_aware_lepa_lower': sum(v > 0 for v in values),
            'min_percent': min(values), 'max_percent': max(values),
            'min_trace': min(changes, key=changes.get), 'max_trace': max(changes, key=changes.get),
        }
    totals = {
        mode: {
            field: sum(row['policies'][mode][field] for row in per_trace.values())
            for field in ('requests_completed', 'reads', 'writes', 'single_page_writes', 'multi_page_writes',
                          'host_write_page_transactions', 'page_programs', 'gc_executions', 'gc_moved_pages')
        }
        for mode in MODES
    }
    result = {
        'trace_count': len(per_trace), 'run_count': len(results), 'settings': manifest['settings'],
        'policy_options': OPTIONS, 'previous_1612_2_samples_match': True,
        'totals': totals, 'per_trace': per_trace, 'relative_summary': summary,
        'definitions': {
            'latency': 'Device response time per host request in ms. Percentiles use nearest rank.',
            'adaptive_vs_wl_aware_lepa_percent': 'Each trace uses (Adaptive / WL-aware LEPA - 1) × 100.',
            'relative_summary': 'Median and sign counts of the per-trace percentage changes.',
        },
    }
    runner.write_json(OUTPUT / 'metrics.json', result)
    print(f'{len(results)} verified cases. Results in {OUTPUT / "metrics.json"}', flush=True)


def main():
    parser = argparse.ArgumentParser(description='Compare WL-aware LEPA and offline Adaptive PACS on all 16 traces.')
    parser.add_argument('--jobs', type=int, default=8)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    runner = load_runner()
    manifest = prepare(runner)
    traces = manifest['traces']
    ordered = sorted(traces, key=lambda t: -(traces[t]['reads'] + traces[t]['writes']))
    results, failures = {}, []
    with TemporaryDirectory(prefix='mqsim-wl-batch-') as workdir:
        templates = {mode: device_template(workdir, mode) for mode in MODES}
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            tasks = {pool.submit(replay, runner, templates[mode], trace, mode, traces[trace]): (trace, mode)
                     for trace in ordered for mode in MODES}
            for task in as_completed(tasks):
                case = tasks[task]
                try:
                    results[case] = task.result()
                except Exception as error:
                    failures.append(case)
                    print(f'FAILED {case}: {error!r}', flush=True)
    if failures:
        raise SystemExit(f'{len(failures)} cases did not pass validation: {failures}')
    compare_to_reference(results, runner)
    summarize(results, manifest, runner)


if __name__ == '__main__':
    sys.dont_write_bytecode = True
    main()
