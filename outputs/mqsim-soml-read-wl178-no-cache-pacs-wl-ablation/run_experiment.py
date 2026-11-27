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
CURRENT = OUTPUT.parent / 'mqsim-soml-read-wl178-no-cache-adaptive-pacs-cross-plane'
RELATED = OUTPUT.parent / 'mqsim-soml-read-wl178-no-cache-related-work-all-traces'
TRACES = ('1612_2', '1612_4', 'src2_2', 'hm_0', 'web_0')
MODES = {'baseline': None, 'lepa': 'LEPA', 'lepa-wl-aware': 'LEPA', 'pacs-nowl': 'PACS', 'pacs-full': 'PACS'}
COMMON_PACS = {
    'PACS_Request_Level_Enabled': 'true',
    'PACS_Cross_Channel_Enabled': 'true',
    'PACS_Cross_Channel_Adaptive_Enabled': 'true',
    'PACS_Cross_Channel_Gain_Beta': '1',
    'PACS_Cross_Channel_Search_Budget': '256',
}
OPTIONS = {
    'baseline': {},
    'lepa': {'LEPA_WL_Aware_Enabled': 'false'},
    'lepa-wl-aware': {'LEPA_WL_Aware_Enabled': 'true'},
    'pacs-nowl': {**COMMON_PACS, 'PACS_Use_Average_Program_Latency': 'true', 'PACS_WL_Decision_Stats': 'false'},
    'pacs-full': {**COMMON_PACS, 'PACS_Use_Average_Program_Latency': 'false', 'PACS_WL_Decision_Stats': 'true'},
}
CATEGORIES = ('CWDP_Equals_NoWL_Equals_Full', 'Non_WL_Only_Redirections', 'WL_Triggered_Redirections',
              'WL_Changed_Redirections', 'WL_Restored_CWDP')


def setup(simulator):
    spec = importlib.util.spec_from_file_location('replay', RELATED / 'run_experiment.py')
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    runner.EXPERIMENT = OUTPUT
    runner.MODES = MODES
    reference = runner.read_json(CURRENT / 'experiment.json')
    TEMPLATE.mkdir(exist_ok=True)
    if not (TEMPLATE / 'MQSim').is_file():
        source = simulator or OUTPUT.parents[1] / 'MQSim'
        temporary = TEMPLATE / '.MQSim-copy'
        shutil.copy2(source, temporary)
        temporary.replace(TEMPLATE / 'MQSim')
    if simulator:
        assert filecmp.cmp(simulator, TEMPLATE / 'MQSim', shallow=False), 'Simulator differs from the frozen binary'
    for filename in ('ssdconfig.xml', 'workload.xml'):
        if not (TEMPLATE / filename).exists():
            shutil.copy2(CURRENT / 'template' / filename, TEMPLATE / filename)
        assert sha256((TEMPLATE / filename).read_bytes()).hexdigest() == reference['template_sha256'][filename]
    runner.verify_inputs(TEMPLATE)
    wl = [int(v) for v in ET.parse(TEMPLATE / 'ssdconfig.xml').findtext('.//Wordline_Program_Latencies').split()]
    traces = {name: reference['traces'][name] for name in TRACES}
    for name, trace in traces.items():
        info = Path(trace['path']).stat()
        assert (info.st_size, info.st_mtime_ns) == (trace['size'], trace['mtime_ns']), name
    manifest = {
        'traces': traces,
        'settings': reference['settings'],
        'policy_options': OPTIONS,
        'template_sha256': {name: sha256((TEMPLATE / name).read_bytes()).hexdigest()
                            for name in ('MQSim', 'ssdconfig.xml', 'workload.xml')},
        'physical_program_latency': 'Same configured WL table for all five policies',
        'average_program_latency_ns': sum(wl) // len(wl),
        'decision_snapshot_source': 'PACS-Full replay before committing its selected plan',
    }
    path = OUTPUT / 'experiment.json'
    if path.exists():
        assert runner.read_json(path) == manifest, 'Experiment inputs changed'
    else:
        runner.write_json(path, manifest)
    return runner, manifest, wl


def device_template(workdir, mode):
    if not OPTIONS[mode]:
        return TEMPLATE
    directory = Path(workdir) / mode
    directory.mkdir()
    for filename in ('MQSim', 'workload.xml'):
        (directory / filename).symlink_to(TEMPLATE / filename)
    contents = (TEMPLATE / 'ssdconfig.xml').read_text()
    tag = f'<{MODES[mode]}_Enabled>false</{MODES[mode]}_Enabled>'
    assert contents.count(tag) == 1 and all(f'<{key}>' not in contents for key in OPTIONS[mode])
    flags = ''.join(f'\n\t\t<{key}>{value}</{key}>' for key, value in OPTIONS[mode].items())
    (directory / 'ssdconfig.xml').write_text(contents.replace(tag, tag + flags))
    return directory


def time_summary(node):
    return {child.tag: int(child.text) if child.tag == 'Samples' else float(child.text)
            for child in node if not len(child) and child.tag != 'Above_Last_Bin'}


def gap_distribution(node):
    stats = time_summary(node)
    bins = [(int(bin_.findtext('Upper_ns')), int(bin_.findtext('Samples'))) for bin_ in node.findall('Bin')]
    overflow = int(node.findtext('Above_Last_Bin'))
    assert sum(count for _, count in bins) + overflow == stats['Samples']
    for name, percentage in (('P50_interval_ns', 50), ('P95_interval_ns', 95)):
        rank = (stats['Samples'] * percentage + 99) // 100
        previous, seen = None, 0
        for upper, count in bins + [(None, overflow)]:
            seen += count
            if rank <= seen:
                stats[name] = [previous, upper] if upper != 0 else [0, 0]
                break
            previous = upper
    stats['Counts_by_upper_ns'] = dict(bins)
    stats['Above_Last_Bin'] = overflow
    return stats


def decision_data(node, requests, pages):
    data = {child.tag: int(child.text) for child in node if not len(child)}
    assert data['Compared_Requests'] == data['Committed_Plans'] == requests
    assert data['Compared_Pages'] == pages
    assert sum(data[k] for k in CATEGORIES) == requests
    assert data['WL_Decision_Changed_Requests'] == sum(data[k] for k in CATEGORIES[2:])
    assert data['Non_WL_Redirections'] == sum(data[CATEGORIES[i]] for i in (1, 3, 4))
    assert data['WL_Decision_Changed_Page_Lanes'] <= data['WL_Decision_Changed_Pages'] <= pages
    changed_pages = {int(row.findtext('Pages')): int(row.findtext('Requests'))
                     for row in node.findall('Changed_Pages_Per_Request')}
    assert sum(changed_pages.values()) == requests
    assert requests - changed_pages.get(0, 0) == data['WL_Decision_Changed_Requests']
    assert sum(k * count for k, count in changed_pages.items()) == data['WL_Decision_Changed_Pages']
    data['Changed_Pages_Per_Request'] = changed_pages
    data['WL_Decision_Change_Rate_percent'] = 100 * data['WL_Decision_Changed_Requests'] / requests
    data['WL_Page_Change_Rate_percent'] = 100 * data['WL_Decision_Changed_Pages'] / pages
    for key in ('Candidate_WL_Latency_Gap', 'Candidate_Load_Gap'):
        data[key] = gap_distribution(node.find(key))
        assert data[key]['Samples'] == pages
    for key in ('Original_Program_Latency', 'Selected_Program_Latency', 'CWDP_Minus_NoWL_Completion', 'NoWL_Minus_Full_Completion'):
        data[key] = time_summary(node.find(key))
        assert data[key]['Samples'] == (pages if key.endswith('Latency') else requests)
    signs = node.findall('Single_Gain_Sign_Count')
    if signs:
        data['Single_Gain_Sign_Count'] = [{item.tag: int(item.text) for item in row} for row in signs]
        redirected = data['Non_WL_Only_Redirections'] + data['WL_Triggered_Redirections'] + data['WL_Changed_Redirections']
        assert sum(row['Requests'] for row in data['Single_Gain_Sign_Count']) == redirected
        for key in ('Single_Redirected_Load_Gain', 'Single_Redirected_Program_Gain'):
            data[key] = time_summary(node.find(key))
            assert data[key]['Samples'] == redirected
    return data


def check_run(runner, trace, mode, case, original, current, wl, average_ns):
    cfg = ET.parse((OUTPUT / case['result_xml']).parent / 'ssdconfig.xml')
    assert [int(v) for v in cfg.findtext('.//Wordline_Program_Latencies').split()] == wl
    metrics = current[trace]['policies']
    for key in ('requests_completed', 'reads', 'writes', 'single_page_writes', 'multi_page_writes',
                'host_write_page_transactions'):
        assert case[key] == metrics['baseline'][key], (trace, mode, key)
    references = {
        'lepa': (RELATED, original[trace]['policies']['lepa']),
        'baseline': (CURRENT, metrics['baseline']),
        'lepa-wl-aware': (CURRENT, metrics['lepa-wl-aware']),
        'pacs-full': (CURRENT, metrics['pacs-adaptive']),
    }
    if mode in references:
        reference, record = references[mode]
        assert case['latency'] == record['latency'], (trace, mode)
        assert all(case['policy_counters'][key] == value for key, value in record['policy_counters'].items()), (trace, mode)
        for index in range(4):
            name = f'{trace}_latency_{index}.csv'
            assert filecmp.cmp(OUTPUT / case['latency_samples'] / name, reference / record['latency_samples'] / name,
                               shallow=False), (trace, mode, name)
    if MODES[mode] == 'PACS':
        result = ET.parse(OUTPUT / case['result_xml'])
        pacs = result.find('./SSDDevice/PACS')
        search = pacs.find('Adaptive_Search')
        assert float(search.findtext('Gain_Beta')) == 1.0 and int(search.findtext('Search_Budget')) == 256
        assert pacs.findtext('Open_Request_Predictions_At_End') == pacs.findtext('Qualified_Program_Observations') == '0'
        assert int(pacs.findtext('Average_Program_Latency_ns')) == average_ns
        assert pacs.findtext('Program_Latency_Source') == ('AVERAGE' if mode == 'pacs-nowl' else 'CONFIGURED')
        decisions = pacs.find('WL_Decision_Stats')
        if mode == 'pacs-full':
            single = case['single_page_writes']
            case['wl_decisions'] = {
                'single_page': decision_data(decisions.find('Single_Page'), single, single),
                'multipage': decision_data(decisions.find('Multipage'), case['multi_page_writes'],
                                           case['host_write_page_transactions'] - single),
            }
        else:
            assert decisions is None
    return case


def summarize(runner, manifest, cases):
    per_trace = {}
    comparisons = (('pacs-nowl', 'baseline'), ('pacs-full', 'pacs-nowl'), ('pacs-full', 'baseline'),
                   ('pacs-full', 'lepa'), ('pacs-full', 'lepa-wl-aware'))
    for trace in TRACES:
        policies = {mode: cases[trace, mode] for mode in MODES}
        per_trace[trace] = {
            'policies': policies,
            'latency_change_percent': {
                f'{method}_vs_{reference}': {
                    k: 100 * (policies[method]['latency'][k] / policies[reference]['latency'][k] - 1)
                    for k in runner.LATENCY_METRICS
                } for method, reference in comparisons
            },
        }
    summary = {}
    for method, reference in comparisons:
        name = f'{method}_vs_{reference}'
        summary[name] = {}
        for metric in runner.LATENCY_METRICS:
            values = [row['latency_change_percent'][name][metric] for row in per_trace.values()]
            summary[name][metric] = {
                'median_percent': median(values), 'lower': sum(x < 0 for x in values),
                'equal': sum(x == 0 for x in values), 'higher': sum(x > 0 for x in values),
            }
    runner.write_json(OUTPUT / 'metrics.json', {
        'trace_count': len(TRACES), 'run_count': len(cases), 'average_program_latency_ns': manifest['average_program_latency_ns'],
        'reference_sample_parity': {mode: len(TRACES) for mode in MODES if mode != 'pacs-nowl'},
        'settings': manifest['settings'], 'decision_snapshot_source': manifest['decision_snapshot_source'],
        'definitions': {
            'latency_change_percent': 'Per-trace host-request latency ratios minus one, times 100.',
            'program_gain': 'tPROG(CWDP) minus tPROG(Full) for single-page writes relocated by Full.',
            'load_gain': 'Difference in predicted program start times for the same single-page writes.',
            'candidate_gaps': 'Maximum minus minimum over writable destinations at the shared CWDP write prefix. Load uses max(channel ready, die ready).',
            'gap_intervals': 'Bins are (previous bound, upper bound], with a separate exact zero bin.',
            'completion_gains': 'All three allocation plans are re-evaluated with configured WL latencies on the same snapshot.',
        },
        'relative_summary': summary, 'per_trace': per_trace,
    })
    print(f'{len(cases)} verified replays. Results in {OUTPUT / "metrics.json"}', flush=True)


def main():
    parser = argparse.ArgumentParser(description='Run the five-trace PACS WL ablation.')
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--simulator', type=Path)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    runner, manifest, wl = setup(args.simulator)
    original = runner.read_json(RELATED / 'metrics.json')['per_trace']
    current = runner.read_json(CURRENT / 'metrics-with-baseline.json')['per_trace']
    traces = manifest['traces']
    order = sorted(traces, key=lambda name: -(traces[name]['reads'] + traces[name]['writes']))

    def replay(trace, mode):
        case = runner.run_case(templates[mode], 'MQSim', trace, mode, traces[trace])
        return check_run(runner, trace, mode, case, original, current, wl, manifest['average_program_latency_ns'])

    with TemporaryDirectory(prefix='mqsim-wl-ablation-configs-') as workdir:
        templates = {mode: device_template(workdir, mode) for mode in MODES}
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            pending = {(trace, mode): pool.submit(replay, trace, mode)
                       for mode in ('pacs-full', 'pacs-nowl', 'lepa-wl-aware', 'lepa', 'baseline') for trace in order}
            cases = {key: future.result() for key, future in pending.items()}
    summarize(runner, manifest, cases)


if __name__ == '__main__':
    sys.dont_write_bytecode = True
    main()
