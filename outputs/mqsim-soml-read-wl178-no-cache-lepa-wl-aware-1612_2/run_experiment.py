from concurrent.futures import ThreadPoolExecutor
import filecmp
from hashlib import sha256
import importlib.util
from pathlib import Path
import sys
from tempfile import TemporaryDirectory


OUTPUT = Path(__file__).resolve().parent
HISTORY = OUTPUT.parent
TEMPLATE = OUTPUT / 'template'
TRACE = '1612_2'
REPLAY = HISTORY / 'mqsim-soml-read-wl178-no-cache-related-work-all-traces/run_experiment.py'
ORIGINAL_LEPA = HISTORY / 'mqsim-soml-read-wl178-no-cache-pacs-request-1612_2'
ORIGINAL_ADAPTIVE = HISTORY / 'mqsim-soml-read-wl178-no-cache-pacs-adaptive-offline-1612_2'
DEVICES = {
    'lepa': ('LEPA', {'LEPA_WL_Aware_Enabled': 'false'}),
    'lepa-wl-aware': ('LEPA', {'LEPA_WL_Aware_Enabled': 'true'}),
    'pacs-adaptive': ('PACS', {
        'PACS_Request_Level_Enabled': 'true',
        'PACS_Cross_Channel_Enabled': 'true',
        'PACS_Cross_Channel_Adaptive_Enabled': 'true',
        'PACS_Cross_Channel_Gain_Beta': '1',
        'PACS_Cross_Channel_Search_Budget': '256',
    }),
}


def load_runner(mode, policy):
    spec = importlib.util.spec_from_file_location('replay_runner', REPLAY)
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    runner.EXPERIMENT = OUTPUT
    runner.MODES = {mode: policy}
    return runner


def replay(mode, trace):
    policy, flags = DEVICES[mode]
    runner = load_runner(mode, policy)
    with TemporaryDirectory(prefix='mqsim-wl-aware-') as workdir:
        template = Path(workdir)
        for name in ('MQSim', 'workload.xml'):
            (template / name).symlink_to(TEMPLATE / name)
        config = (TEMPLATE / 'ssdconfig.xml').read_text()
        marker = f'<{policy}_Enabled>false</{policy}_Enabled>'
        assert config.count(marker) == 1
        assert all(f'<{tag}>' not in config for tag in flags)
        options = ''.join(f'\n\t\t<{tag}>{value}</{tag}>' for tag, value in flags.items())
        (template / 'ssdconfig.xml').write_text(config.replace(marker, marker + options))
        return runner.run_case(template, 'MQSim', TRACE, mode, trace)


def main():
    runner = load_runner('lepa', 'LEPA')
    runner.verify_inputs(TEMPLATE)
    archived = runner.read_json(ORIGINAL_ADAPTIVE / 'experiment.json')
    trace = archived['trace_file']
    trace_path = HISTORY / 'mqsim-soml-read-wl178-no-cache/lun/sortedTrace/1612_2.csv'
    info = trace_path.stat()
    assert (info.st_size, info.st_mtime_ns) == (trace['size'], trace['mtime_ns'])
    trace['path'] = str(trace_path)
    trace['history_batch'] = str(trace_path.parents[1])
    manifest = {
        'trace': TRACE, 'trace_file': trace, 'settings': archived['settings'],
        'policies': list(DEVICES),
        'template_sha256': {name: sha256((TEMPLATE / name).read_bytes()).hexdigest()
                            for name in ('MQSim', 'ssdconfig.xml', 'workload.xml')},
    }
    path = OUTPUT / 'experiment.json'
    if path.is_file():
        assert runner.read_json(path) == manifest, 'Experiment inputs changed'
    else:
        runner.write_json(path, manifest)

    with ThreadPoolExecutor(max_workers=len(DEVICES)) as pool:
        results = dict(zip(DEVICES, pool.map(lambda mode: replay(mode, trace), DEVICES)))

    for mode, old_output in (('lepa', ORIGINAL_LEPA), ('pacs-adaptive', ORIGINAL_ADAPTIVE)):
        current = results[mode]
        old_report = runner.read_json(old_output / 'metrics.json')
        old = old_report['policies'][mode]
        assert old_report['settings'] == archived['settings']
        assert current['latency'] == old['latency'] and current['policy_counters'] == old['policy_counters']
        for idx in range(4):
            name = f'{TRACE}_latency_{idx}.csv'
            assert filecmp.cmp(OUTPUT / current['latency_samples'] / name, old_output / old['latency_samples'] / name,
                               shallow=False), (mode, name)
    old_case, new_case, adaptive_case = (OUTPUT / 'run' / f'{TRACE}-{mode}' for mode in DEVICES)
    workload = (old_case / 'workload.xml').read_bytes()
    assert workload == (new_case / 'workload.xml').read_bytes() == (adaptive_case / 'workload.xml').read_bytes()
    marker = b'<LEPA_WL_Aware_Enabled>false<'
    assert (new_case / 'ssdconfig.xml').read_bytes() == (old_case / 'ssdconfig.xml').read_bytes().replace(
        marker, b'<LEPA_WL_Aware_Enabled>true<')

    variant = results['lepa-wl-aware']
    assert variant['policy_counters']['WL_Aware'] == 'true'
    assert variant['policy_counters']['Program_Latency_Source'] == 'CONFIGURED'
    assert variant['policy_counters']['Outstanding_Program_Time_ns'] == '0'
    for case in results.values():
        assert case['requests_completed'] == variant['requests_completed']
        assert case['host_write_page_transactions'] == variant['host_write_page_transactions']
    report = {
        'trace': TRACE, 'settings': manifest['settings'],
        'requests_per_policy': variant['requests_completed'], 'policies': results,
        'control_samples_match_prior_experiments': True,
        'wl_aware_lepa_definition': {
            'score': 'Read and erase load plus the configured program times of all outstanding writes and the next candidate page.',
            'minimum_gain': 'Configured program time of the page at the original target’s write frontier.',
        },
        'wl_aware_lepa_vs_reference_percent': {
            mode: {metric: (value / results[mode]['latency'][metric] - 1) * 100 for metric, value in variant['latency'].items()}
            for mode in ('lepa', 'pacs-adaptive')
        },
    }
    runner.write_json(OUTPUT / 'metrics.json', report)
    print(f'All {len(results)} replays verified. Results in {OUTPUT / "metrics.json"}', flush=True)


if __name__ == '__main__':
    sys.dont_write_bytecode = True
    main()
