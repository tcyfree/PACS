import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import filecmp
from hashlib import sha256
import json
from pathlib import Path
import re
import shutil
from statistics import median
import subprocess
import time
from xml.etree import ElementTree as ET

import numpy as np


EXPERIMENT = Path(__file__).resolve().parent
MQSIM_DIR = EXPERIMENT.parents[1]
HISTORY = EXPERIMENT.parent / 'mqsim-soml-read-wl178-no-cache'
MODES = {'baseline': None, 'pacs': 'PACS', 'qstr-med': 'QSTR_MED', 'lepa': 'LEPA'}
LATENCY_METRICS = (
    'average_write_latency_ms', 'average_read_latency_ms', 'p95_write_latency_ms',
    'p99_write_latency_ms', 'p999_write_latency_ms', 'read_p99_latency_ms',
    'single_page_write_p99_ms', 'single_page_write_p999_ms',
    'multi_page_write_p99_ms', 'multi_page_write_p999_ms',
)
MULTIPLANE_COMMANDS = (
    'Issued_Flash_Multiplane_Read_CMD', 'Issued_Flash_Multiplane_Program_CMD',
    'Issued_Flash_Multiplane_Erase_CMD', 'Issued_Flash_Multiplane_Copyback_Program_CMD',
    'Issued_Flash_Interleaved_Multiplane_Read_CMD', 'Issued_Flash_Interleaved_Multiplane_Program_CMD',
    'Issued_Flash_Interleaved_Multiplane_Erase_CMD',
)


def read_json(path):
    return json.loads(path.read_text())


def write_json(path, data):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(data, indent=2, allow_nan=False) + '\n')
    temporary.replace(path)


def verify_inputs(template):
    device = ET.parse(template / 'ssdconfig.xml')
    for name, expected in (
        ('Flash_Channel_Count', '4'), ('Page_Capacity', '16384'), ('Page_No_Per_Block', '768'),
        ('Flash_Technology', 'TLC'), ('Independent_Wordline_Multiplane', 'false'),
        ('PACS_Enabled', 'false'), ('QSTR_MED_Enabled', 'false'), ('LEPA_Enabled', 'false'),
    ):
        assert device.findtext('.//' + name) == expected, name
    assert len(device.findtext('.//Wordline_Program_Latencies').split()) == 256
    assert not device.findtext('.//Block_Latency_Profile_File')
    workload = ET.parse(template / 'workload.xml')
    assert workload.findtext('.//Device_Level_Data_Caching_Mode') == 'TURNED_OFF'
    assert workload.findtext('.//Relay_Count') == workload.findtext('.//Turn_on') == '1'


def prepare():
    template = EXPERIMENT / 'template'
    if not template.exists():
        template.mkdir()
        for item in ('ssdconfig.xml', 'workload.xml', 'MQSim'):
            shutil.copy2(MQSIM_DIR / item, template / item)
    verify_inputs(template)
    known = read_json(HISTORY / 'requested_metrics.json')['per_trace']
    traces = {}
    for batch in ('lun', 'msrc'):
        path = HISTORY / batch
        cases = read_json(path / 'comparison.json')['traces']
        for name, case in cases.items():
            stats = case['trace']
            assert name not in traces
            assert stats['reads'] == known[name]['baseline']['reads']
            assert stats['writes'] == known[name]['baseline']['writes']
            trace_path = path / 'sortedTrace' / (name + '.csv')
            file_info = trace_path.stat()
            traces[name] = {
                'path': str(trace_path), 'size': file_info.st_size, 'mtime_ns': file_info.st_mtime_ns,
                'reads': stats['reads'], 'writes': stats['writes'], 'multi_page_writes': stats['multi_page_writes'],
                'bytes': stats['bytes'], 'span_ns': stats['span_ns'], 'history_batch': str(path),
            }

    assert len(traces) == len(known) == 16 and set(traces) == set(known)
    assert sum(t['reads'] + t['writes'] for t in traces.values()) == 30_560_937
    binaries = {mode: 'MQSim-lepa' if mode == 'lepa' and (template / 'MQSim-lepa').is_file() else 'MQSim'
                for mode in MODES}
    manifest = {
        'policies': list(MODES),
        'policy_binaries': binaries,
        'template_sha256': {name: sha256((template / name).read_bytes()).hexdigest()
                            for name in sorted(set(binaries.values()) | {'ssdconfig.xml', 'workload.xml'})},
        'traces': {name: traces[name] for name in known},
    }
    path = EXPERIMENT / 'experiment.json'
    if path.exists():
        recorded = read_json(path)
        recorded_traces = recorded['traces']
        # Treat paths through the original outputs symlink as the same inputs.
        for name in recorded_traces.keys() & traces.keys():
            for field in ('path', 'history_batch'):
                expected = traces[name][field]
                if Path(recorded_traces[name][field]).resolve() == Path(expected).resolve():
                    recorded_traces[name][field] = expected
        assert recorded == manifest, 'Inputs changed since this experiment was created'
    else:
        write_json(path, manifest)
    return template, manifest


def sample_array(path, count):
    arr = np.fromstring(path.read_bytes().rstrip(b','), sep=',', dtype=np.uint64)
    assert arr.size == count, (path, arr.size, count)
    return arr


def percentiles(arr, *permilles):
    if not arr.size:
        return (None,) * len(permilles)
    positions = [(arr.size * p + 999) // 1000 - 1 for p in permilles]
    selected = np.partition(arr, positions)
    return tuple(int(selected[pos]) / 1e6 for pos in positions)


def separate_single_page(all_writes, multi_page):
    values, counts = np.unique(all_writes, return_counts=True)
    multi_values, multi_counts = np.unique(multi_page, return_counts=True)
    pos = np.searchsorted(values, multi_values)
    assert (pos < values.size).all() and np.array_equal(values[pos], multi_values)
    counts[pos] -= multi_counts
    assert (counts >= 0).all()
    return np.repeat(values, counts)


def compare_archived_samples(trace, mode, sample_directory, details):
    original = Path(details['history_batch']) / 'run' / f'{trace}-{mode}' / trace
    return all(filecmp.cmp(sample_directory / f'{trace}_latency_{i}.csv',
                           original / f'{trace}_latency_{i}.csv', shallow=False) for i in range(4))


def run_case(template, binary, trace, mode, details):
    case = EXPERIMENT / 'run' / f'{trace}-{mode}'
    finalized = case / 'metrics.json'
    if finalized.is_file():
        data = read_json(finalized)
        assert data['requests_completed'] == details['reads'] + details['writes']
        assert data.get('simulator_binary', 'MQSim') == binary
        print(f'{trace} {mode}: already verified', flush=True)
        return data

    samples = case / trace
    samples.mkdir(parents=True, exist_ok=True)
    device = (template / 'ssdconfig.xml').read_text()
    policy = MODES[mode]
    if policy:
        tag = policy + '_Enabled'
        device, count = re.subn(fr'<{tag}>false</{tag}>', f'<{tag}>true</{tag}>', device)
        assert count == 1, mode
    (case / 'ssdconfig.xml').write_text(device)
    workload = ET.parse(template / 'workload.xml')
    workload.find('.//File_Path').text = details['path']
    workload.write(case / 'workload.xml', encoding='utf-8')
    log_path = case / 'run.log'
    start = time.monotonic()
    print(f'{trace} {mode}: running', flush=True)
    with log_path.open('w') as log_file:
        command = [str(template / binary), '-i', 'ssdconfig.xml', '-w', 'workload.xml']
        process = subprocess.run(command, cwd=case, stdin=subprocess.DEVNULL, stdout=log_file, stderr=subprocess.STDOUT)

    log = log_path.read_text(errors='replace')
    if process.returncode or '100% progress' not in log or re.search(r'ERROR:|open error|stop register', log):
        raise RuntimeError(f'{trace} {mode} exited with {process.returncode}. See {log_path}')
    assert 'Using program latencies for 256 wordlines' in log
    result_path = case / f'w_{trace}_setsize4_reply_1.xml'
    result = ET.parse(result_path)
    flow = result.find('./Host/Host.IO_Flow')
    reads, writes = details['reads'], details['writes']
    completed = reads + writes
    for name, expected in (
        ('Repaly_count', 1), ('Sys_Total_Generate_Requests', completed), ('Sys_Total_Serviced_Requests', completed),
        ('Total_Serviced_Read_Requests', reads), ('Total_Serviced_Write_Requests', writes),
    ):
        assert int(flow.findtext(name)) == expected, (trace, mode, name)
    time_match = re.search(rf'\n{completed}\n(\d+)\nFlow ', log)
    assert time_match is not None
    simulation_ns = int(time_match.group(1))
    assert simulation_ns > details['span_ns']

    read, write, multi = [
        sample_array(samples / f'{trace}_latency_{index}.csv', expected)
        for index, expected in ((1, reads), (2, writes), (3, details['multi_page_writes']))
    ]
    assert int(read.sum()) + int(write.sum()) == int(flow.findtext('Total_Device_Response_Time'))
    single = separate_single_page(write, multi)
    p95, p99, p999 = percentiles(write, 950, 990, 999)
    single_p99, single_p999 = percentiles(single, 990, 999)
    multi_p99, multi_p999 = percentiles(multi, 990, 999)
    stats = (
        int(write.sum()) / writes / 1e6, int(read.sum()) / reads / 1e6, p95, p99, p999,
        percentiles(read, 990)[0], single_p99, single_p999, multi_p99, multi_p999,
    )
    ftl = result.find('./SSDDevice/SSDDevice.FTL').attrib
    multi_commands = {tag: int(ftl[tag]) for tag in MULTIPLANE_COMMANDS if tag in ftl}
    assert 'Issued_Flash_Multiplane_Program_CMD' in multi_commands and not any(multi_commands.values()), (trace, mode)
    stream = result.find('./SSDDevice/SSDDevice.HostInterface/SSDDevice.IO_Stream')
    write_transactions = int(stream.findtext('Total_Write_Transactions'))
    programmed = int(ftl['Issued_Flash_Program_CMD']) + int(ftl['Issued_Flash_Copyback_Program_CMD'])
    issued_gc = int(ftl['Total_GC_Executions'])
    gc_moved = round(issued_gc * float(ftl['Average_Page_Movement_For_GC'])) if issued_gc else 0

    present = [result.find('./SSDDevice/' + key) for key in MODES.values() if key]
    present = [node for node in present if node is not None]
    assert [node.tag for node in present] == ([policy] if policy else [])
    counters = {child.tag: child.text for node in present for child in node if not len(child)}
    if policy == 'LEPA':
        assert counters['Independent_Wordline_Multiplane'] == 'false'
        assert all(counters['Outstanding_' + kind] == '0' for kind in ('Reads', 'Writes', 'Erases'))
    if policy == 'PACS':
        assert counters['Outstanding_Programs_At_End'] == '0'
    if policy == 'QSTR_MED':
        assert counters['Profiled_Blocks'] == '0'

    data = {
        'requests_completed': completed, 'reads': reads, 'writes': writes,
        'single_page_writes': single.size, 'multi_page_writes': multi.size,
        'latency': dict(zip(LATENCY_METRICS, stats)),
        'simulated_duration_ns': simulation_ns,
        'replay_throughput_mib_s': details['bytes'] / (2 ** 20) * 1e9 / simulation_ns,
        'gc_executions': issued_gc, 'gc_moved_pages': gc_moved,
        'host_write_page_transactions': write_transactions, 'page_programs': programmed,
        'page_program_amplification': programmed / write_transactions,
        'mean_write_transaction_wait_us': int(stream.findtext('Average_Write_Transaction_Waiting_Time')),
        'multiplane_commands': multi_commands, 'policy_counters': counters,
        'result_xml': str(result_path.relative_to(EXPERIMENT)), 'log': str(log_path.relative_to(EXPERIMENT)),
        'latency_samples': str(samples.relative_to(EXPERIMENT)),
        'simulator_binary': binary,
        'wall_seconds': time.monotonic() - start,
    }
    if mode in ('baseline', 'pacs'):
        data['samples_match_previous_experiment'] = compare_archived_samples(trace, mode, samples, details)
    write_json(finalized, data)
    print(f'{trace} {mode}: {completed:,} requests verified in {data["wall_seconds"]:.1f} s', flush=True)
    return data


def summarize(results, manifest):
    traces = {}
    comparisons = [('pacs', 'baseline'), ('qstr-med', 'baseline'), ('lepa', 'baseline'), ('lepa', 'pacs')]
    for trace in manifest['traces']:
        cases = {mode: results[(trace, mode)] for mode in MODES}
        changes = {}
        for method, reference in comparisons:
            baseline = cases[reference]['latency']
            compared = cases[method]['latency']
            changes[f'{method}_vs_{reference}'] = {
                metric: (compared[metric] / baseline[metric] - 1) * 100
                if baseline[metric] and compared[metric] is not None else None
                for metric in LATENCY_METRICS
            }
        traces[trace] = {'policies': cases, 'change_percent': changes}

    relative_summary = {}
    for method, reference in comparisons:
        name = f'{method}_vs_{reference}'
        relative_summary[name] = {}
        for metric in LATENCY_METRICS:
            values = [row['change_percent'][name][metric] for row in traces.values()
                      if row['change_percent'][name][metric] is not None]
            relative_summary[name][metric] = {
                'median_percent': median(values), 'lower': sum(value < 0 for value in values),
                'equal': sum(value == 0 for value in values), 'higher': sum(value > 0 for value in values),
                'min_percent': min(values), 'max_percent': max(values),
            }
    outcome = {
        'trace_count': len(traces), 'run_count': len(results),
        'requests_per_policy': sum(t['reads'] + t['writes'] for t in manifest['traces'].values()),
        'settings': {'page_capacity_bytes': 16384, 'flash_channels': 4, 'simulated_wordlines': 256,
                     'device_data_caching': 'TURNED_OFF', 'independent_wordline_multiplane': False,
                     'block_latency_profiles': 0},
        'metrics': list(LATENCY_METRICS),
        'policy_binaries': manifest['policy_binaries'],
        'definitions': {
            'latency': 'Device response time measured per host request, in ms. Percentiles use nearest rank.',
            'write_subsets': 'Single-page samples equal all writes minus the multiset of cross-page writes.',
            'relative_summary': 'Unweighted median and sign counts of the per-trace percent changes.',
        },
        'per_trace': traces, 'relative_summary': relative_summary,
    }
    write_json(EXPERIMENT / 'metrics.json', outcome)
    print(f'{len(results)} verified cases. Results in {EXPERIMENT / "metrics.json"}')


def main():
    parser = argparse.ArgumentParser(description='Compare four allocation policies on the same single-plane trace replays.')
    parser.add_argument('--jobs', type=int, default=6)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    template, manifest = prepare()
    traces = manifest['traces']
    ordered = sorted(traces, key=lambda name: -(traces[name]['reads'] + traces[name]['writes']))
    results = {}
    failures = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        tasks = {pool.submit(run_case, template, manifest['policy_binaries'][mode], trace, mode, traces[trace]): (trace, mode)
                 for trace in ordered for mode in MODES}
        for task in as_completed(tasks):
            name = tasks[task]
            try:
                results[name] = task.result()
            except Exception as error:
                failures.append(name)
                print(f'FAILED {name}: {error!r}', flush=True)
    if failures:
        raise SystemExit(f'{len(failures)} cases did not pass validation')
    summarize(results, manifest)


if __name__ == '__main__':
    main()

