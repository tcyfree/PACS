import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from decimal import Decimal
import filecmp
import gzip
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory
from xml.etree import ElementTree as ET

from scale_traces import FACTORS, OUTPUT, PILOT, TRACE_NAMES, file_sha256, prepare_traces, write_json


RELATED = OUTPUT.parent / 'mqsim-soml-read-wl178-no-cache-related-work-all-traces'
TEMPLATE = OUTPUT / 'template'
STATS = 'Workload_Intensity_Stats_Enabled'


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def setup(simulator):
    pilot = load_module('wl_ablation', PILOT / 'run_experiment.py')
    reference = json.loads((PILOT / 'experiment.json').read_text())
    traces = prepare_traces()
    TEMPLATE.mkdir(exist_ok=True)
    frozen = TEMPLATE / 'MQSim'
    if not frozen.exists():
        source = simulator or OUTPUT.parents[1] / 'MQSim'
        temporary = TEMPLATE / '.MQSim-copy'
        shutil.copy2(source, temporary)
        temporary.replace(frozen)
    if simulator:
        assert filecmp.cmp(simulator, frozen, shallow=False), 'Simulator differs from the frozen binary'
    for name in ('ssdconfig.xml', 'workload.xml'):
        if not (TEMPLATE / name).exists():
            shutil.copy2(PILOT / 'template' / name, TEMPLATE / name)
        assert file_sha256(TEMPLATE / name) == reference['template_sha256'][name]
    assert ET.parse(TEMPLATE / 'workload.xml').findtext('.//Time_Unit') == 'NANOSECOND'

    runners = {}
    for factor in FACTORS:
        runner = load_module(f'replay_{factor}x', RELATED / 'run_experiment.py')
        runner.EXPERIMENT = OUTPUT / f'{factor}x'
        runner.MODES = pilot.MODES
        runner.verify_inputs(TEMPLATE)
        runners[factor] = runner
    options = {mode: {**flags, STATS: 'true'} for mode, flags in reference['policy_options'].items()}
    manifest = {
        'factors': traces['factors'], 'trace_names': list(TRACE_NAMES),
        'settings': reference['settings'], 'policy_options': options,
        'template_sha256': {name: file_sha256(TEMPLATE / name) for name in ('MQSim', 'ssdconfig.xml', 'workload.xml')},
        'trace_manifest_sha256': file_sha256(OUTPUT / 'trace_manifest.json'),
        'decision_snapshot_source': reference['decision_snapshot_source'],
    }
    path = OUTPUT / 'experiment.json'
    if path.exists():
        assert json.loads(path.read_text()) == manifest, 'Experiment inputs changed'
    else:
        write_json(path, manifest)
    return pilot, runners, manifest, traces['traces']


def device_template(workdir, mode, policy, options):
    directory = Path(workdir) / mode
    directory.mkdir()
    for name in ('MQSim', 'workload.xml'):
        (directory / name).symlink_to(TEMPLATE / name)
    device = (TEMPLATE / 'ssdconfig.xml').read_text()
    marker = f'<{policy or "PACS"}_Enabled>false</{policy or "PACS"}_Enabled>'
    assert device.count(marker) == 1 and all(f'<{tag}>' not in device for tag in options)
    tags = ''.join(f'\n\t\t<{tag}>{value}</{tag}>' for tag, value in options.items())
    (directory / 'ssdconfig.xml').write_text(device.replace(marker, marker + tags))
    return directory


def read_load(node, case, expected_dies):
    duration = int(node.findtext('Observed_Time_ns'))
    assert duration == case['simulated_duration_ns']
    dies = [{child.tag: Decimal(child.text) if 'Integral' in child.tag else int(child.text)
             for child in row} for row in node.findall('Die')]
    assert len(dies) == expected_dies and all(
        die['Queued_At_End'] == 0 and 0 <= die['NAND_Execution_ns'] <= duration for die in dies
    )
    assert sum(d['Dispatched_User_Writes'] for d in dies) == case['host_write_page_transactions']
    queue_ns = sum(d['Queue_Integral_ns'] for d in dies)
    write_queue_ns = sum(d['User_Write_Queue_Integral_ns'] for d in dies)
    load = {
        'mean_die_utilization_percent': 100 * sum(d['NAND_Execution_ns'] for d in dies) / (duration * len(dies)),
        'mean_tsu_queue_length_per_die': float(queue_ns / (duration * len(dies))),
        'mean_tsu_wait_us': float(queue_ns / sum(d['Dispatched_Transactions'] for d in dies) / 1000),
        'mean_tsu_write_wait_us': float(write_queue_ns / case['host_write_page_transactions'] / 1000),
        'dies': [{key: float(value) if isinstance(value, Decimal) else value for key, value in d.items()} for d in dies],
    }
    single = case['single_page_writes']
    for key, tag, requests, pages in (
        ('single_page', 'Single_Page_Write', single, single),
        ('multipage', 'Multipage_Write', case['multi_page_writes'], case['host_write_page_transactions'] - single),
    ):
        stats = {child.tag: int(child.text) for child in node.find(tag)}
        assert stats['Observed_Requests'] == stats['Placed_Requests'] == requests and stats['Placed_Pages'] == pages
        idle, small_gap = stats['All_Candidate_Dies_Idle_Requests'], stats['Die_Load_Gap_Below_460us_Requests']
        crossed, crossed_pages = stats['Cross_Channel_Requests'], stats['Cross_Channel_Pages']
        assert 0 <= idle <= small_gap <= requests and 0 <= crossed <= min(requests, crossed_pages) <= pages
        assert crossed_pages <= pages
        load[key] = {
            **stats, 'all_candidate_dies_idle_percent': 100 * idle / requests,
            'die_load_gap_below_460us_percent': 100 * small_gap / requests,
            'cross_channel_requests_percent': 100 * crossed / requests,
            'cross_channel_pages_percent': 100 * crossed_pages / pages,
        }
    return load


def samples_match(raw, prior):
    if raw.is_file():
        return filecmp.cmp(raw, prior, shallow=False)
    with gzip.open(raw.with_suffix('.csv.gz'), 'rb') as left, prior.open('rb') as right:
        for block in iter(lambda: left.read(2 ** 20), b''):
            if block != right.read(len(block)):
                return False
        return not right.read(1)


def archive_samples(folder, trace):
    paths = [folder / f'{trace}_latency_{i}.csv' for i in range(4)]
    raw = [str(path) for path in paths if path.is_file()]
    if raw:
        subprocess.run(['pigz', '-1', '-n', '-p', '1', '--', *raw], check=True)
    archives = [str(path.with_suffix('.csv.gz')) for path in paths]
    assert all(Path(archive).is_file() for archive in archives)
    subprocess.run(['pigz', '-t', '-p', '1', '--', *archives], check=True)


def verify_case(case, runner, trace, factor, mode, timing, prior, pilot, manifest, artifact_root=OUTPUT):
    folder = runner.EXPERIMENT
    result_path = folder / case['result_xml']
    actual = ET.parse(result_path).find('./SSDDevice')
    actual_cfg = ET.parse(result_path.parent / 'ssdconfig.xml')
    ref_cfg = ET.parse(TEMPLATE / 'ssdconfig.xml')
    for tag, value in manifest['policy_options'][mode].items():
        assert actual_cfg.findtext('.//' + tag) == value, (trace, factor, mode, tag)
    assert actual_cfg.findtext('.//Wordline_Program_Latencies').split() == ref_cfg.findtext('.//Wordline_Program_Latencies').split()
    die_count = 1
    for tag in ('Flash_Channel_Count', 'Chip_No_Per_Channel', 'Die_No_Per_Chip'):
        die_count *= int(ref_cfg.findtext('.//' + tag))
    load = read_load(actual.find('Workload_Intensity'), case, die_count)
    assert case['simulated_duration_ns'] >= timing['last_timestamp_ns']
    assert ET.parse(result_path.parent / 'workload.xml').findtext('.//File_Path') == timing['path']
    decisions = None
    if mode == 'baseline':
        assert load['single_page']['Cross_Channel_Pages'] == load['multipage']['Cross_Channel_Pages'] == 0
    if mode in ('pacs-full', 'pacs-nowl'):
        counters = case['policy_counters']
        assert int(counters['Single_Page_Cross_Channel_Writes']) == load['single_page']['Cross_Channel_Requests']
        assert int(counters['Cross_Channel_Multipage_Write_Requests']) == load['multipage']['Cross_Channel_Requests']
        assert int(counters['Cross_Channel_Write_Pages']) == load['multipage']['Cross_Channel_Pages']
        assert counters['Program_Latency_Source'] == ('CONFIGURED' if mode == 'pacs-full' else 'AVERAGE')
    if mode == 'pacs-full':
        stats = actual.find('./PACS/WL_Decision_Stats')
        single = case['single_page_writes']
        decisions = {
            'single_page': pilot.decision_data(stats.find('Single_Page'), single, single),
            'multipage': pilot.decision_data(stats.find('Multipage'), case['multi_page_writes'],
                                            case['host_write_page_transactions'] - single),
        }
    samples = folder / case['latency_samples']
    match_reference = factor == 1 and prior is not None
    if match_reference:
        assert case['latency'] == prior['latency'] and case['policy_counters'] == prior['policy_counters'], (trace, mode)
        for i in range(4):
            name = f'{trace}_latency_{i}.csv'
            assert samples_match(samples / name, PILOT / prior['latency_samples'] / name), (trace, mode, name)
    archive_samples(samples, trace)
    out = {
        **case, 'result_xml': str(result_path.relative_to(artifact_root)),
        'log': str((folder / case['log']).relative_to(artifact_root)),
        'latency_samples': str(samples.relative_to(artifact_root)), 'sample_format': 'gzip',
        'arrival_rate_iops': case['requests_completed'] * 1e9 / timing['span_ns'],
        'reference_samples_match': match_reference, 'workload_intensity': load,
    }
    if decisions is not None:
        out['wl_decisions'] = decisions
    write_json(result_path.parent / 'intensity-metrics.json', out)
    return out


def summarize(cases, manifest):
    report = {}
    comparisons = (('pacs-full', 'baseline'), ('pacs-full', 'pacs-nowl'), ('pacs-full', 'lepa'),
                   ('pacs-full', 'lepa-wl-aware'), ('pacs-nowl', 'baseline'))
    for trace in TRACE_NAMES:
        report[trace] = {}
        for factor in FACTORS:
            policies = {mode: cases[(trace, factor, mode)] for mode in manifest['policy_options']}
            report[trace][str(factor)] = {'policies': policies, 'latency_change_percent': {
                f'{method}_vs_{reference}': {
                    key: (ms / policies[reference]['latency'][key] - 1) * 100
                    for key, ms in policies[method]['latency'].items()
                } for method, reference in comparisons
            }}
    write_json(OUTPUT / 'metrics.json', {
        'trace_count': len(TRACE_NAMES), 'run_count': len(cases), 'factors': list(FACTORS),
        'settings': manifest['settings'], 'experiment': 'experiment.json', 'trace_manifest': 'trace_manifest.json',
        'decision_snapshot_source': manifest['decision_snapshot_source'],
        'definitions': {
            'mean_die_utilization_percent': 'Executed NAND array time divided by total Die time across the complete replay.',
            'mean_tsu_queue_length_per_die': 'Time weighted TSU occupancy per Die, including all flash transaction types.',
            'mean_tsu_write_wait_us': 'Time from TSU submission to command dispatch for user write page transactions.',
            'write_load_snapshot': 'At entry of the complete host write into address translation, before allocation. The common eligible Die pool uses CWDP Die indices with at least one writable Plane.',
            'all_candidate_dies_idle_percent': 'No active command or queued flash transactions on any eligible Die.',
            'die_load_gap_below_460us_percent': 'Max minus min of predicted Die ready time on the common pool, strictly below 460 us.',
            'cross_channel_requests_percent': 'Share of successfully placed write requests with at least one page on a Channel different from the CWDP target.',
        },
        'per_trace': report,
    })
    print(f'{len(cases)} verified replays. Results in {OUTPUT / "metrics.json"}', flush=True)


def main():
    parser = argparse.ArgumentParser(description='Replay all four traces, six interval factors and five allocation methods.')
    parser.add_argument('--simulator', type=Path)
    parser.add_argument('--jobs', type=int, default=8)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if shutil.which('pigz') is None:
        parser.error('pigz is required to store lossless latency archives')
    pilot, runners, manifest, traces = setup(args.simulator)
    prior = json.loads((PILOT / 'metrics.json').read_text())['per_trace']
    order = sorted(TRACE_NAMES, key=lambda trace: -(traces[trace]['1']['reads'] + traces[trace]['1']['writes']))
    cases = {}
    with TemporaryDirectory(prefix='mqsim-workload-intensity-') as workdir:
        templates = {mode: device_template(workdir, mode, policy, manifest['policy_options'][mode])
                     for mode, policy in pilot.MODES.items()}

        def run(trace, factor, mode):
            runner = runners[factor]
            details = traces[trace][str(factor)]
            case = runner.run_case(templates[mode], 'MQSim', trace, mode, details)
            return verify_case(case, runner, trace, factor, mode, details, prior[trace]['policies'][mode], pilot, manifest)

        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            pending = {pool.submit(run, trace, factor, mode): (trace, factor, mode)
                       for factor in FACTORS for trace in order for mode in reversed(pilot.MODES)}
            failures = []
            for job in as_completed(pending):
                key = pending[job]
                try:
                    cases[key] = job.result()
                    print(f'[{len(cases)}/{len(pending)}] {key[0]} {key[1]}x {key[2]} validated', flush=True)
                except Exception as error:
                    failures.append(key)
                    print(f'FAILED {key}: {error!r}', flush=True)
    if failures:
        raise SystemExit(f'{len(failures)} failed cases: {failures}')
    summarize(cases, manifest)


if __name__ == '__main__':
    sys.dont_write_bytecode = True
    main()
