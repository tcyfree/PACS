import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import filecmp
import gzip
import importlib.util
import json
from pathlib import Path
import shutil
import sys
from tempfile import TemporaryDirectory
from xml.etree import ElementTree as ET


OUTPUT = Path(__file__).resolve().parent
INTENSITY = OUTPUT.parent / 'mqsim-soml-read-wl178-no-cache-intensity'
TEMPLATE = OUTPUT / 'template'
BETAS = ('0', '0.05', '0.1', '0.25', '0.5', '1')
GAIN = 'PACS_Cross_Channel_Gain_Beta'
SCREEN = 'PACS_Cross_Channel_Screening_Beta'
SERIAL = 'Completion_Driven_Replay'


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read(path):
    return json.loads(path.read_text())


def setup(simulator):
    sys.path.insert(0, str(INTENSITY))
    intensity = load_module('intensity_run', INTENSITY / 'run_experiment.py')
    sys.path.pop(0)
    pilot = load_module('wl_ablation', intensity.PILOT / 'run_experiment.py')
    old = read(INTENSITY / 'experiment.json')
    timing = read(INTENSITY / 'trace_manifest.json')
    assert intensity.file_sha256(INTENSITY / 'trace_manifest.json') == old['trace_manifest_sha256']
    traces = {name: {k: info[k] for k in ('1', '50')} for name, info in timing['traces'].items()}
    assert list(traces) == old['trace_names']
    for name, cases in traces.items():
        assert cases['1']['payload_sha256'] == cases['50']['payload_sha256'], name
        for data in cases.values():
            source = Path(data['path'])
            assert (source.stat().st_size, source.stat().st_mtime_ns) == (data['size'], data['mtime_ns']), source
            assert intensity.file_sha256(source) == data['sha256'], source
    TEMPLATE.mkdir(exist_ok=True)
    if not (TEMPLATE / 'MQSim').exists():
        source = simulator or OUTPUT.parents[1] / 'MQSim'
        temporary = TEMPLATE / '.MQSim-copy'
        shutil.copy2(source, temporary)
        temporary.replace(TEMPLATE / 'MQSim')
    if simulator:
        assert filecmp.cmp(simulator, TEMPLATE / 'MQSim', shallow=False), 'Simulator differs from the frozen binary'
    for name in ('ssdconfig.xml', 'workload.xml'):
        if not (TEMPLATE / name).exists():
            shutil.copy2(INTENSITY / 'template' / name, TEMPLATE / name)
        assert intensity.file_sha256(TEMPLATE / name) == old['template_sha256'][name]
    assert ET.parse(TEMPLATE / 'workload.xml').findtext('.//Time_Unit') == 'NANOSECOND'

    beta_options = {beta: {**old['policy_options']['pacs-full'], GAIN: beta, SCREEN: '1'} for beta in BETAS}
    serial_options = {mode: {**flags, **({SCREEN: '1'} if mode.startswith('pacs-') else {})}
                      for mode, flags in old['policy_options'].items()}
    manifest = {
        'trace_names': list(traces), 'trace_manifest_sha256': old['trace_manifest_sha256'], 'settings': old['settings'],
        'template_sha256': {name: intensity.file_sha256(TEMPLATE / name)
                            for name in ('MQSim', 'ssdconfig.xml', 'workload.xml')},
        'screening_beta': 1.0, 'search_budget': 256, 'open_loop_interval_factor': 50,
        'beta_options': beta_options, 'serial_options': serial_options, 'serial_gain_beta': 1.0,
        'serial_replay': 'One outstanding host request. Next request is submitted at the previous host completion.',
    }
    file = OUTPUT / 'experiment.json'
    if file.exists():
        assert read(file) == manifest, 'Experiment inputs changed'
    else:
        intensity.write_json(file, manifest)
    return intensity, pilot, traces, manifest


def template_for(work, name, policy, options, serial):
    template = Path(work) / name
    template.mkdir()
    (template / 'MQSim').symlink_to(TEMPLATE / 'MQSim')
    flow = ET.parse(TEMPLATE / 'workload.xml')
    assert len(flow.findall('.//IO_Flow_Parameter_Set_Trace_Based')) == 1
    if serial:
        ET.SubElement(flow.find('.//IO_Flow_Parameter_Set_Trace_Based'), SERIAL).text = 'true'
    flow.write(template / 'workload.xml', encoding='utf-8')
    device = (TEMPLATE / 'ssdconfig.xml').read_text()
    marker = f'<{policy or "PACS"}_Enabled>false</{policy or "PACS"}_Enabled>'
    assert device.count(marker) == 1 and all(f'<{tag}>' not in device for tag in options)
    tags = ''.join(f'\n\t\t<{tag}>{value}</{tag}>' for tag, value in options.items())
    (template / 'ssdconfig.xml').write_text(device.replace(marker, marker + tags))
    return template


def samples_identical(raw, compressed):
    lhs = raw.open('rb') if raw.exists() else gzip.open(raw.with_suffix('.csv.gz'), 'rb')
    with lhs as actual, gzip.open(compressed, 'rb') as prior:
        for block in iter(lambda: actual.read(2 ** 20), b''):
            if block != prior.read(len(block)):
                return False
        return not prior.read(1)


def measure(case, runner, trace, mode, timing, options, serial, control, intensity, pilot):
    folder = runner.EXPERIMENT
    path = folder / case['result_xml']
    result = ET.parse(path)
    ssd = result.find('./SSDDevice')
    config = ET.parse(path.parent / 'ssdconfig.xml')
    assert all(config.findtext('.//' + name) == value for name, value in options.items())
    base_cfg = ET.parse(TEMPLATE / 'ssdconfig.xml')
    assert config.findtext('.//Wordline_Program_Latencies').split() == base_cfg.findtext('.//Wordline_Program_Latencies').split()
    workload = ET.parse(path.parent / 'workload.xml')
    assert workload.findtext('.//File_Path') == timing['path']
    assert (workload.findtext(f'.//{SERIAL}') == 'true') == serial
    die_count = 1
    for tag in ('Flash_Channel_Count', 'Chip_No_Per_Channel', 'Die_No_Per_Chip'):
        die_count *= int(base_cfg.findtext('.//' + tag))
    load = intensity.read_load(ssd.find('Workload_Intensity'), case, die_count)
    replay = result.find(f'./Host/Host.IO_Flow/{SERIAL}')
    if serial:
        assert replay is not None
        replay_stats = {child.tag: int(child.text) for child in replay}
        assert replay_stats == {'Max_Outstanding_Requests': 1, 'Outstanding_At_End': 0,
                                'Max_Completion_To_Next_Submission_ns': 0}, (trace, mode, replay_stats)
    else:
        assert replay is None and case['simulated_duration_ns'] >= timing['last_timestamp_ns']
        replay_stats = None

    search_stats, decisions = None, None
    if mode.startswith('pacs-'):
        policy = ssd.find('PACS')
        search = policy.find('Adaptive_Search')
        assert float(search.findtext('Gain_Beta')) == float(options[GAIN])
        assert float(search.findtext('Screening_Beta')) == float(options[SCREEN])
        assert search.findtext('Search_Budget') == options['PACS_Cross_Channel_Search_Budget']
        assert policy.findtext('Program_Latency_Source') == ('AVERAGE' if mode == 'pacs-nowl' else 'CONFIGURED')
        assert policy.findtext('Open_Request_Predictions_At_End') == '0'
        search_stats = {
            'beta': float(options[GAIN]), 'screening_beta': float(options[SCREEN]),
            'budget': int(search.findtext('Search_Budget')),
            'budget_exhausted_requests': int(search.findtext('Budget_Exhausted_Requests')),
            'critical_page_counts': {int(row.findtext('Pages')): int(row.findtext('Requests'))
                                     for row in search.findall('Critical_Page_Count')},
            'candidate_plan_counts': {int(row.findtext('Evaluations')): int(row.findtext('Requests'))
                                       for row in search.findall('Candidate_Plan_Count')},
        }
        assert sum(search_stats['critical_page_counts'].values()) == case['multi_page_writes']
        assert sum(search_stats['candidate_plan_counts'].values()) == case['multi_page_writes']
        for key, group, field in (
            ('Single_Page_Cross_Channel_Writes', 'single_page', 'Cross_Channel_Requests'),
            ('Cross_Channel_Multipage_Write_Requests', 'multipage', 'Cross_Channel_Requests'),
            ('Cross_Channel_Write_Pages', 'multipage', 'Cross_Channel_Pages'),
        ):
            assert int(case['policy_counters'][key]) == load[group][field]
        if mode == 'pacs-full':
            stats = policy.find('WL_Decision_Stats')
            single = case['single_page_writes']
            decisions = {
                'single_page': pilot.decision_data(stats.find('Single_Page'), single, single),
                'multipage': pilot.decision_data(stats.find('Multipage'), case['multi_page_writes'],
                                                case['host_write_page_transactions'] - single),
            }
            if options[GAIN] == '0':
                assert all(group['Full_Rejected_By_Beta'] == 0 for group in decisions.values())

    sample_folder = folder / case['latency_samples']
    if control is not None:
        for key in ('latency', 'workload_intensity', 'policy_counters', 'wl_decisions'):
            actual = {'workload_intensity': load, 'wl_decisions': decisions}.get(key, case.get(key))
            assert json.loads(json.dumps(actual)) == control[key], (trace, mode, key)
        previous = INTENSITY / control['latency_samples']
        for i in range(4):
            name = f'{trace}_latency_{i}.csv'
            assert samples_identical(sample_folder / name, previous / (name + '.gz')), (trace, name)
    intensity.archive_samples(sample_folder, trace)

    recorded = {
        **case, 'log': str((folder / case['log']).relative_to(OUTPUT)), 'result_xml': str(path.relative_to(OUTPUT)),
        'latency_samples': str(sample_folder.relative_to(OUTPUT)), 'sample_format': 'gzip',
        'completed_iops': case['requests_completed'] * 1e9 / case['simulated_duration_ns'],
        'workload_intensity': load, 'completion_driven_replay': replay_stats,
    }
    if search_stats is not None:
        recorded['adaptive_search'] = search_stats
    if decisions is not None:
        recorded['wl_decisions'] = decisions
    if control is not None:
        recorded['matches_50x_beta1_reference'] = True
    intensity.write_json(path.parent / 'replay-metrics.json', recorded)
    return recorded


def compare(method, reference):
    return {key: (value / reference['latency'][key] - 1) * 100 for key, value in method['latency'].items()}


def summarize(cases, traces, manifest, reference, intensity):
    report = {}
    for trace in traces:
        control = cases['beta', '1', trace, 'pacs-full']
        reference_50x = reference[trace]['50']['policies']
        sweep = {}
        for beta in BETAS:
            case = cases['beta', beta, trace, 'pacs-full']
            sweep[beta] = {'pacs-full': case, 'change_vs_beta1_percent': compare(case, control),
                           'change_vs_50x_lepa_percent': compare(case, reference_50x['lepa']),
                           'change_vs_50x_wl_aware_lepa_percent': compare(case, reference_50x['lepa-wl-aware'])}
        serial = {mode: cases['serial', '', trace, mode] for mode in manifest['serial_options']}
        report[trace] = {
            'beta_at_50x': sweep,
            'reference_50x': {mode: case['latency'] for mode, case in reference_50x.items()},
            'serial': {
                'policies': serial, 'full_change_percent': {mode: compare(serial['pacs-full'], case)
                                                          for mode, case in serial.items() if mode != 'pacs-full'},
            },
        }
    intensity.write_json(OUTPUT / 'metrics.json', {
        'run_count': len(cases), 'beta_replays': len(traces)*len(BETAS),
        'serial_replays': len(traces)*len(manifest['serial_options']), 'betas': list(BETAS),
        'screening_beta': manifest['screening_beta'], 'search_budget': manifest['search_budget'],
        'settings': manifest['settings'], 'definitions': {
            'beta_at_50x': 'PACS-Full with fixed screening and a variable acceptance gate. Reference LEPA policies use the same 50x traces.',
            'serial': 'Immediately issue the next I/O after the host consumes the previous completion. PACS acceptance and screening beta both equal 1.',
            'percent_changes': 'Per-trace latency ratio minus one, times 100. Negative means a lower latency.',
        }, 'per_trace': report,
    })
    print(f'{len(cases)} verified replays. Results in {OUTPUT / "metrics.json"}', flush=True)


def main():
    parser = argparse.ArgumentParser(description='Run a fixed-screening beta sweep and completion-driven replay.')
    parser.add_argument('--simulator', type=Path)
    parser.add_argument('--jobs', type=int, default=8)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if shutil.which('pigz') is None:
        parser.error('pigz is required to store lossless latency archives')
    intensity, pilot, traces, manifest = setup(args.simulator)
    reference = read(INTENSITY / 'metrics.json')['per_trace']
    order = sorted(traces, key=lambda name: -(traces[name]['1']['reads'] + traces[name]['1']['writes']))
    modes = [('beta', beta) for beta in BETAS] + [('serial', '')]
    runners = {}
    for stage, beta in modes:
        runner = load_module('replay_' + stage + beta.replace('.', '_'), intensity.RELATED / 'run_experiment.py')
        runner.EXPERIMENT = OUTPUT / stage / beta if stage == 'beta' else OUTPUT / stage
        runner.MODES = pilot.MODES if stage == 'serial' else {'pacs-full': 'PACS'}
        runner.verify_inputs(TEMPLATE)
        runners[stage, beta] = runner
    cases, failures = {}, []
    with TemporaryDirectory(prefix='mqsim-beta-and-serial-') as work:
        templates = {('beta', beta, 'pacs-full'): template_for(work, f'beta-{beta}', 'PACS', options, False)
                     for beta, options in manifest['beta_options'].items()}
        templates.update({('serial', '', mode): template_for(work, mode, pilot.MODES[mode], options, True)
                          for mode, options in manifest['serial_options'].items()})

        def replay(stage, beta, trace, mode):
            serial = stage == 'serial'
            runner = runners[stage, beta]
            options = manifest['serial_options'][mode] if serial else manifest['beta_options'][beta]
            trace_input = {**traces[trace]['1' if serial else '50'], 'completion_driven_replay': serial}
            control = reference[trace]['50']['policies']['pacs-full'] if stage == 'beta' and beta == '1' else None
            case = runner.run_case(templates[stage, beta, mode], 'MQSim', trace, mode, trace_input)
            return measure(case, runner, trace, mode, trace_input, options, serial, control, intensity, pilot)

        tasks = [('beta', '1', trace, 'pacs-full') for trace in order]
        tasks += [('serial', '', trace, mode) for trace in order for mode in reversed(pilot.MODES)]
        tasks += [('beta', beta, trace, 'pacs-full') for beta in BETAS if beta != '1' for trace in order]
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            pending = {pool.submit(replay, *key): key for key in tasks}
            for job in as_completed(pending):
                key = pending[job]
                try:
                    cases[key] = job.result()
                    print(f'[{len(cases)}/{len(tasks)}] {key} validated', flush=True)
                except Exception as error:
                    failures.append(key)
                    print(f'FAILED {key}: {error!r}', flush=True)
    if failures:
        raise SystemExit(f'{len(failures)} cases failed: {failures}')
    summarize(cases, traces, manifest, reference, intensity)


if __name__ == '__main__':
    sys.dont_write_bytecode = True
    main()
