from concurrent.futures import ThreadPoolExecutor
import filecmp
from hashlib import sha256
import importlib.util
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
from xml.etree import ElementTree as ET


OUTPUT = Path(__file__).resolve().parent
HISTORY = OUTPUT.parent
BASELINE = HISTORY / 'mqsim-soml-read-wl178-no-cache-pacs-hybrid-1612_2'
ADAPTIVE_ONLINE = HISTORY / 'mqsim-soml-read-wl178-no-cache-pacs-adaptive-1612_2'
RELATED_WORK = HISTORY / 'mqsim-soml-read-wl178-no-cache-pacs-request-1612_2'
REPLAY = HISTORY / 'mqsim-soml-read-wl178-no-cache-related-work-all-traces/run_experiment.py'
TEMPLATE = OUTPUT / 'template'
TRACE = '1612_2'
MODES = ('pacs-hybrid', 'pacs-adaptive')
BETA = 1
SEARCH_BUDGET = 256


def load_runner(mode):
    spec = importlib.util.spec_from_file_location('replay_runner', REPLAY)
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    runner.EXPERIMENT = OUTPUT
    runner.MODES = {mode: 'PACS'}
    return runner


def replay(mode, trace):
    runner = load_runner(mode)
    flags = {
        'PACS_Request_Level_Enabled': 'true',
        'PACS_Cross_Channel_Enabled': 'true',
        'PACS_Cross_Channel_Adaptive_Enabled': 'true' if mode == 'pacs-adaptive' else 'false',
        'PACS_Cross_Channel_Gain_Beta': str(BETA),
        'PACS_Cross_Channel_Search_Budget': str(SEARCH_BUDGET),
    }
    with TemporaryDirectory(prefix='mqsim-adaptive-') as workdir:
        template = Path(workdir)
        for name in ('MQSim', 'workload.xml'):
            (template / name).symlink_to(TEMPLATE / name)
        content = (TEMPLATE / 'ssdconfig.xml').read_text()
        marker = '<PACS_Enabled>false</PACS_Enabled>'
        assert content.count(marker) == 1
        assert all(f'<{tag}>' not in content for tag in flags)
        options = ''.join(f'\n\t\t<{tag}>{value}</{tag}>' for tag, value in flags.items())
        (template / 'ssdconfig.xml').write_text(content.replace(marker, marker + options))
        return runner.run_case(template, 'MQSim', TRACE, mode, trace)


def statistics(histogram):
    count = sum(histogram.values())
    assert count > 0
    ranks = {'p50': (count * 50 + 99) // 100, 'p95': (count * 95 + 99) // 100,
             'p99': (count * 99 + 99) // 100}
    result = {'count': count, 'total': sum(k * n for k, n in histogram.items()), 'maximum': max(histogram)}
    result['mean'] = result['total'] / count
    seen = 0
    for k, n in sorted(histogram.items()):
        seen += n
        for label, rank in ranks.items():
            if seen >= rank:
                result.setdefault(label, k)
    return result


def adaptive_diagnostics(case):
    pacs = ET.parse(OUTPUT / case['result_xml']).find('./SSDDevice/PACS')
    assert pacs.findtext('Program_Latency_Source') == 'CONFIGURED'
    assert pacs.findtext('Qualified_Program_Observations') == pacs.findtext('Discarded_Program_Observations') == '0'
    assert not pacs.findall('Program_Class')
    search = pacs.find('Adaptive_Search')
    critical = {int(row.findtext('Pages')): int(row.findtext('Requests')) for row in search.findall('Critical_Page_Count')}
    evaluated = {int(row.findtext('Evaluations')): int(row.findtext('Requests')) for row in search.findall('Candidate_Plan_Count')}
    k_stats, evaluated_stats = statistics(critical), statistics(evaluated)
    planned = int(pacs.findtext('Planned_Multipage_Write_Requests'))
    crossed = int(pacs.findtext('Cross_Channel_Multipage_Write_Requests'))
    pages = int(pacs.findtext('Cross_Channel_Write_Pages'))
    joint = int(pacs.findtext('Joint_Cross_Channel_Multipage_Write_Requests'))
    exhausted = int(search.findtext('Budget_Exhausted_Requests'))
    assert float(search.findtext('Gain_Beta')) == BETA
    assert evaluated_stats['maximum'] <= int(search.findtext('Search_Budget')) == SEARCH_BUDGET
    assert 0 <= exhausted <= evaluated.get(SEARCH_BUDGET, 0)
    assert k_stats['count'] == evaluated_stats['count'] == planned <= case['multi_page_writes']
    assert 0 <= joint <= crossed <= planned and crossed + joint <= pages
    assert int(pacs.findtext('Open_Request_Predictions_At_End')) == 0

    prediction = {}
    for label, tag in (
        ('all', 'Request_Completion_Prediction'), ('cross_channel', 'Cross_Channel_Completion_Prediction'),
    ):
        node = pacs.find(tag)
        samples = int(node.findtext('Samples'))
        prediction[label] = {'samples': samples}
        if samples:
            prediction[label].update({
                'mean_signed_error_ms': float(node.findtext('Mean_Signed_Error_ns')) / 1e6,
                'mean_absolute_error_ms': float(node.findtext('Mean_Absolute_Error_ns')) / 1e6,
                'mean_actual_duration_ms': float(node.findtext('Mean_Actual_Duration_ns')) / 1e6,
            })
    assert prediction['cross_channel']['samples'] <= crossed
    assert prediction['cross_channel']['samples'] <= prediction['all']['samples'] <= planned
    return {
        'gain_beta': BETA, 'search_budget': SEARCH_BUDGET, 'search_budget_exhausted_requests': exhausted,
        'program_latency_source': 'Wordline_Program_Latencies',
        'online_program_samples': 0,
        'critical_page_count_distribution': critical, 'critical_page_count_statistics': k_stats,
        'candidate_evaluation_count_distribution': evaluated, 'candidate_evaluation_count_statistics': evaluated_stats,
        'cross_channel_requests': crossed, 'cross_channel_pages': pages, 'joint_cross_channel_requests': joint,
        'cross_channel_fraction_of_multipage_writes': crossed / case['multi_page_writes'],
        'completion_prediction': {
            'interval': 'From allocation decision to last NAND program completion',
            'error': 'Predicted duration minus actual duration',
            **prediction,
        },
    }


def main():
    runner = load_runner(MODES[0])
    runner.verify_inputs(TEMPLATE)
    archived = runner.read_json(BASELINE / 'metrics.json')
    trace = archived['trace_file']
    trace_path = HISTORY / 'mqsim-soml-read-wl178-no-cache/lun/sortedTrace/1612_2.csv'
    info = trace_path.stat()
    assert (info.st_size, info.st_mtime_ns) == (trace['size'], trace['mtime_ns'])
    trace['path'] = str(trace_path)
    trace['history_batch'] = str(trace_path.parents[1])
    manifest = {
        'trace': TRACE, 'trace_file': trace, 'settings': archived['settings'],
        'policies': list(MODES), 'adaptive_beta': BETA, 'adaptive_search_budget': SEARCH_BUDGET,
        'template_sha256': {name: sha256((TEMPLATE / name).read_bytes()).hexdigest()
                            for name in ('MQSim', 'ssdconfig.xml', 'workload.xml')},
    }
    path = OUTPUT / 'experiment.json'
    if path.is_file():
        assert runner.read_json(path) == manifest, 'Experiment inputs changed'
    else:
        runner.write_json(path, manifest)

    with ThreadPoolExecutor(max_workers=len(MODES)) as pool:
        results = dict(zip(MODES, pool.map(lambda mode: replay(mode, trace), MODES)))
    hybrid, adaptive = (results[mode] for mode in MODES)
    old = archived['policies']['pacs-hybrid']
    assert hybrid['latency'] == old['latency'] and hybrid['policy_counters'] == old['policy_counters']
    for idx in range(4):
        filename = f'{TRACE}_latency_{idx}.csv'
        assert filecmp.cmp(OUTPUT / hybrid['latency_samples'] / filename,
                           BASELINE / old['latency_samples'] / filename, shallow=False)
    old_case, new_case = (OUTPUT / 'run' / f'{TRACE}-{mode}' for mode in MODES)
    assert (old_case / 'workload.xml').read_bytes() == (new_case / 'workload.xml').read_bytes()
    marker = b'<PACS_Cross_Channel_Adaptive_Enabled>false<'
    assert (new_case / 'ssdconfig.xml').read_bytes() == (old_case / 'ssdconfig.xml').read_bytes().replace(
        marker, b'<PACS_Cross_Channel_Adaptive_Enabled>true<')
    assert adaptive['requests_completed'] == hybrid['requests_completed']
    assert adaptive['host_write_page_transactions'] == hybrid['host_write_page_transactions']

    lepa = runner.read_json(RELATED_WORK / 'metrics.json')['policies']['lepa']
    online = runner.read_json(ADAPTIVE_ONLINE / 'metrics.json')
    assert online['settings'] == archived['settings'] and online['trace'] == TRACE
    assert online['adaptive_search']['gain_beta'] == BETA and online['adaptive_search']['search_budget'] == SEARCH_BUDGET
    assert online['requests_per_policy'] == adaptive['requests_completed']
    refs = {'pacs-hybrid': hybrid, 'adaptive-online': online['policies']['pacs-adaptive'], 'lepa': lepa}
    report = {
        'trace': TRACE, 'settings': manifest['settings'], 'policies': results,
        'requests_per_policy': adaptive['requests_completed'], 'hybrid_samples_match_previous_experiment': True,
        'adaptive_search': adaptive_diagnostics(adaptive),
        'reference_latency_ms': {name: case['latency'] for name, case in refs.items() if name != 'pacs-hybrid'},
        'adaptive_online_reference': str(ADAPTIVE_ONLINE / 'metrics.json'),
        'adaptive_vs_reference_percent': {
            name: {metric: (value / case['latency'][metric] - 1) * 100 for metric, value in adaptive['latency'].items()}
            for name, case in refs.items()
        },
    }
    runner.write_json(OUTPUT / 'metrics.json', report)
    print(f'Verified both replays. Results in {OUTPUT / "metrics.json"}', flush=True)


if __name__ == '__main__':
    sys.dont_write_bytecode = True
    main()
