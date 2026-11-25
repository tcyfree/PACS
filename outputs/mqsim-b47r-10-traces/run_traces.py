import argparse
import json
import os
import re
import shutil
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from hashlib import sha256
from pathlib import Path
from statistics import median
from xml.etree import ElementTree as ET

import numpy as np


TRACES = (
    '1612_4', '1613_2', '1614_6', '1615_1', '1616_2',
    '1617_3', '1618_3', '1619_0', '1620_2', '1708_0',
)
POLICIES = ('baseline', 'pacs')
TEMPLATE = Path(__file__).resolve().parents[1] / 'mqsim-1612_2/run/b47r-tlc-40c-baseline-latency'


def write_json(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def prepare_trace(source, destination, sectors_per_page, last_lba, timestamp_scale_ns):
    first = previous = None
    reads = writes = multi_page_writes = sectors_total = 0
    temporary = destination.with_suffix('.tmp')
    with source.open('rb') as raw, temporary.open('wb') as trace:
        for line_number, line in enumerate(raw, 1):
            if not line.strip():
                continue
            timestamp, device, lba, sectors, op = map(int, line.split())
            if timestamp < 0 or lba < 0 or sectors <= 0 or op not in (0, 1):
                raise ValueError(f'{source}:{line_number}: invalid trace record')
            if previous is not None and timestamp < previous:
                raise ValueError(f'{source}:{line_number}: timestamps out of order')
            if first is None:
                first = timestamp
            previous = timestamp
            arrival_ns = (timestamp - first) * timestamp_scale_ns + 1
            trace.write(f'{arrival_ns} {device} {lba} {sectors} {op}\n'.encode('ascii'))
            reads += op == 1
            writes += op == 0
            sectors_total += sectors
            if op == 0:
                folded_lba = lba if lba <= last_lba else lba % last_lba
                multi_page_writes += folded_lba % sectors_per_page + sectors > sectors_per_page
    if not reads or not writes:
        raise ValueError(f'{source}: this run requires read and write samples')
    temporary.replace(destination)
    return {
        'source': str(source), 'source_bytes': source.stat().st_size,
        'reads': reads, 'writes': writes, 'multi_page_writes': multi_page_writes,
        'bytes': sectors_total * 512, 'span_ns': (previous - first) * timestamp_scale_ns,
    }


def latency_metrics(path, count):
    values = np.fromstring(path.read_bytes().rstrip(b','), dtype=np.uint64, sep=',')
    if len(values) != count:
        raise ValueError(f'{path}: expected {count} latency samples, got {len(values)}')
    if not count:
        return {'samples': 0}, 0
    ordered = np.sort(values)
    quantiles = {'p50_us': 500, 'p95_us': 950, 'p99_us': 990, 'p99_9_us': 999}
    return ({
        'samples': count, 'mean_us': values.mean().item() / 1000,
        **{label: ordered[(count * permille + 999) // 1000 - 1].item() / 1000
           for label, permille in quantiles.items()},
        'max_us': ordered[-1].item() / 1000,
    }, int(values.sum()))


def run_case(output, name, policy, trace, config, workload, set_count):
    directory = output / 'run' / f'{name}-{policy}'
    metrics_path = directory / 'metrics.json'
    if metrics_path.exists():
        print(f'{name} {policy} already verified', flush=True)
        return json.loads(metrics_path.read_text())

    samples = directory / name
    samples.mkdir(parents=True, exist_ok=True)
    case_config = config.replace('<PACS_Enabled>false</PACS_Enabled>', '<PACS_Enabled>true</PACS_Enabled>') if policy == 'pacs' else config
    (directory / 'ssdconfig.xml').write_text(case_config)
    tree = ET.fromstring(workload)
    tree.find('.//File_Path').text = f'../../sortedTrace/{name}.csv'
    ET.ElementTree(tree).write(directory / 'workload.xml', encoding='utf-8')
    print(f'{name} {policy} running', flush=True)
    command = ['../../MQSim', '-i', 'ssdconfig.xml', '-w', 'workload.xml']
    start = time.monotonic()
    with (directory / 'run.log').open('w') as log:
        run = subprocess.run(command, cwd=directory, stdin=subprocess.DEVNULL, stdout=log,
                             stderr=subprocess.STDOUT, env={**os.environ, 'TZ': 'Asia/Shanghai'})
        elapsed_s = time.monotonic() - start
        log.write(f'\nElapsed wall time: {elapsed_s:.2f} s\nExit status: {run.returncode}\n')
    log = (directory / 'run.log').read_text(errors='replace')
    if run.returncode or re.search(r'ERROR:|open error|stop register', log) or '100% progress' not in log:
        raise RuntimeError(f'{name} {policy} did not finish cleanly. See {directory / "run.log"}')

    result_path = directory / f'w_{name}_setsize{set_count}_reply_1.xml'
    root = ET.parse(result_path).getroot()
    flow = root.find('./Host/Host.IO_Flow')
    ftl = root.find('./SSDDevice/SSDDevice.FTL')
    stream = root.find('./SSDDevice/SSDDevice.HostInterface/SSDDevice.IO_Stream')
    pacs = root.find('./SSDDevice/PACS')
    count = trace['reads'] + trace['writes']
    expected = {
        'Repaly_count': 1,
        'Sys_Total_Generate_Requests': count,
        'Sys_Total_Serviced_Requests': count,
        'Total_Serviced_Read_Requests': trace['reads'],
        'Total_Serviced_Write_Requests': trace['writes'],
    }
    for field, expected_value in expected.items():
        if int(flow.findtext(field)) != expected_value:
            raise ValueError(f'{result_path}: {field} does not match the source trace')
    if (pacs is not None) != (policy == 'pacs'):
        raise ValueError(f'{result_path}: unexpected PACS state')

    read, read_total_ns = latency_metrics(samples / f'{name}_latency_1.csv', trace['reads'])
    write, write_total_ns = latency_metrics(samples / f'{name}_latency_2.csv', trace['writes'])
    multi_page, _ = latency_metrics(samples / f'{name}_latency_3.csv', trace['multi_page_writes'])
    if read_total_ns + write_total_ns != int(flow.findtext('Total_Device_Response_Time')):
        raise ValueError(f'{result_path}: latency samples do not match the simulator totals')
    simulated_ns = int(re.search(rf'\n{count}\n(\d+)\nFlow ', log).group(1))
    if simulated_ns <= trace['span_ns']:
        raise ValueError(f'{result_path}: simulated time ends before the last arrival')

    gc_cycles = int(ftl.get('Total_GC_Executions'))
    gc_pages = round(gc_cycles * float(ftl.get('Average_Page_Movement_For_GC'))) if gc_cycles else 0
    page_programs = int(ftl.get('Issued_Flash_Program_CMD')) + int(ftl.get('Issued_Flash_Copyback_Program_CMD'))
    multiplane_commands = int(ftl.get('Issued_Flash_Multiplane_Program_CMD')) + int(ftl.get('Issued_Flash_Multiplane_Copyback_Program_CMD'))
    data = {
        'requests_completed': count, 'simulated_finish_ns': simulated_ns,
        'iops': count * 1e9 / simulated_ns, 'throughput_mib_s': trace['bytes'] * 1e9 / simulated_ns / 2**20,
        'reads': read, 'writes': write, 'multi_page_writes': multi_page,
        'gc_executions': gc_cycles, 'gc_moved_pages': gc_pages,
        'page_program_amplification': page_programs / int(stream.findtext('Total_Write_Transactions')) if not multiplane_commands else None,
        'mean_write_transaction_wait_us': int(stream.findtext('Average_Write_Transaction_Waiting_Time')),
        'result_xml': str(result_path.relative_to(output)), 'run_log': str((directory / 'run.log').relative_to(output)),
        'latency_sample_directory': str(samples.relative_to(output)), 'wall_seconds': elapsed_s,
    }
    if pacs is not None:
        data['pacs'] = {key: int(pacs.findtext(key)) for key in (
            'Qualified_Program_Observations', 'Discarded_Program_Observations',
            'Ranked_Write_Allocations', 'Redirected_Write_Allocations', 'Outstanding_Programs_At_End',
        )}
        if data['pacs']['Outstanding_Programs_At_End'] != 0:
            raise ValueError(f'{result_path}: outstanding programs at exit')
    write_json(metrics_path, data)
    print(f'{name} {policy} verified, {count:,} requests in {elapsed_s:.1f} s', flush=True)
    return data


def summarize(output, traces, results, page_capacity):
    comparison = {}
    metrics = {
        'mean_write_latency': ('writes', 'mean_us'), 'write_p95': ('writes', 'p95_us'),
        'write_p99': ('writes', 'p99_us'), 'write_p99_9': ('writes', 'p99_9_us'),
        'multi_page_write_p99': ('multi_page_writes', 'p99_us'),
        'multi_page_write_p99_9': ('multi_page_writes', 'p99_9_us'),
        'mean_read_latency': ('reads', 'mean_us'), 'read_p99': ('reads', 'p99_us'),
    }
    for name, trace in traces.items():
        baseline, pacs = (results[name, policy] for policy in POLICIES)
        drops = {metric: 100 * (1 - pacs[group][field] / baseline[group][field])
                 for metric, (group, field) in metrics.items() if baseline[group].get(field, 0)}
        comparison[name] = {
            'trace': trace, 'baseline': baseline, 'pacs': pacs,
            'pacs_latency_reduction_percent': drops,
        }
    aggregate = {}
    for metric in metrics:
        values = [case['pacs_latency_reduction_percent'][metric] for case in comparison.values()
                  if metric in case['pacs_latency_reduction_percent']]
        if values:
            aggregate[metric] = {
                'traces': len(values), 'improved_traces': sum(value > 0 for value in values),
                'median_reduction_percent': median(values),
            }
    write_json(output / 'comparison.json', {
        'percentile_method': 'Nearest rank from host enqueue to completion, by trace and policy.',
        'multi_page_definition': f'A host write spanning more than one {page_capacity}-byte flash page after LBA folding.',
        'latency_reduction_sign': 'Positive means PACS reduced latency.',
        'aggregation': 'Median of paired percentage changes with equal weight per trace.',
        'aggregate': aggregate, 'traces': comparison,
    })
    print(f'Wrote {output / "comparison.json"}', flush=True)


def main():
    parser = argparse.ArgumentParser(description='Replay traces in MQSim with and without PACS.')
    parser.add_argument('--trace-dir', type=Path, default=Path('/home/HDD/abtrace/Trace/sortedTrace'))
    parser.add_argument('--traces', nargs='+', default=TRACES, metavar='NAME', help='Trace filenames or stems')
    parser.add_argument('--timestamp-scale-ns', type=int, default=1, help='Nanoseconds per input timestamp tick')
    parser.add_argument('--template', type=Path, default=TEMPLATE, help='Directory containing MQSim, ssdconfig.xml and workload.xml')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=6)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be at least 1')
    if args.timestamp_scale_ns < 1:
        parser.error('--timestamp-scale-ns must be at least 1')
    trace_names = [name.removesuffix('.csv') for name in args.traces]
    if len(set(trace_names)) != len(trace_names) or any(not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_-]*', name) for name in trace_names):
        parser.error('--traces requires distinct trace filenames without directory components')

    output = args.output.resolve()
    trace_dir = args.trace_dir.resolve()
    template = args.template.resolve()
    binary = template / 'MQSim'
    config = (template / 'ssdconfig.xml').read_text()
    workload = (template / 'workload.xml').read_text()
    if config.count('<PACS_Enabled>false</PACS_Enabled>') != 1:
        raise ValueError(f'{template}: expected a baseline device configuration')
    device = ET.fromstring(config).find('Device_Parameter_Set')
    flash = device.find('Flash_Parameter_Set')
    page_capacity = int(flash.findtext('Page_Capacity'))
    sectors_per_page = page_capacity // 512
    plane_sectors = int(flash.findtext('Block_No_Per_Plane')) * int(flash.findtext('Page_No_Per_Block')) * sectors_per_page
    planes = (int(device.findtext('Flash_Channel_Count')) * int(device.findtext('Chip_No_Per_Channel'))
              * int(flash.findtext('Die_No_Per_Chip')) * int(flash.findtext('Plane_No_Per_Die')))
    last_lba = planes * int(plane_sectors * (1 - float(device.findtext('Overprovisioning_Ratio')))) - 1
    set_count = int(device.findtext('Set_count'))

    sources = {name: trace_dir / f'{name}.csv' for name in trace_names}
    experiment = {
        'binary_sha256': sha256(binary.read_bytes()).hexdigest(),
        'config_sha256': sha256(config.encode()).hexdigest(), 'workload_sha256': sha256(workload.encode()).hexdigest(),
        'sources': {name: {'path': str(path), 'size': path.stat().st_size, 'mtime_ns': path.stat().st_mtime_ns}
                    for name, path in sources.items()},
    }
    if args.timestamp_scale_ns != 1:
        experiment['timestamp_scale_ns'] = args.timestamp_scale_ns
    output.mkdir(parents=True, exist_ok=True)
    manifest = output / 'experiment.json'
    if manifest.exists():
        if json.loads(manifest.read_text()) != experiment:
            raise ValueError(f'{output}: input files changed. Choose a new output directory.')
    else:
        if any(output.iterdir()):
            raise ValueError(f'{output}: choose an empty directory for a new experiment')
        shutil.copy2(binary, output / 'MQSim')
        write_json(manifest, experiment)
    if sha256((output / 'MQSim').read_bytes()).hexdigest() != experiment['binary_sha256']:
        raise ValueError(f'{output}: unexpected MQSim executable')

    normalized = output / 'sortedTrace'
    normalized.mkdir(exist_ok=True)
    traces, results, failures = {}, {}, []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        jobs = {}
        for name, source in sources.items():
            statistics = normalized / f'{name}.json'
            trace = normalized / f'{name}.csv'
            if statistics.exists() and trace.is_file():
                traces[name] = json.loads(statistics.read_text())
            else:
                traces[name] = prepare_trace(source, trace, sectors_per_page, last_lba, args.timestamp_scale_ns)
                write_json(statistics, traces[name])
            print(f'{name} prepared: {traces[name]["reads"]:,} reads, {traces[name]["writes"]:,} writes', flush=True)
            for policy in POLICIES:
                job = pool.submit(run_case, output, name, policy, traces[name], config, workload, set_count)
                jobs[job] = (name, policy)
        for job in as_completed(jobs):
            case = jobs[job]
            try:
                results[case] = job.result()
            except Exception as error:
                failures.append(f'{case[0]} {case[1]}: {error}')
                print(failures[-1], flush=True)
    if failures:
        raise SystemExit(f'{len(failures)} of {len(jobs)} runs failed verification')
    summarize(output, traces, results, page_capacity)


if __name__ == '__main__':
    main()
