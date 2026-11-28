import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import shutil
from tempfile import TemporaryDirectory

import run_experiments as previous


OUTPUT = previous.OUTPUT / 'qd1-beta-sweep'
BETAS = ('0', '0.05', '0.1', '0.25')
MODE = 'pacs-full'


def acceptance_stats(case):
    load = case['workload_intensity']
    groups = (load['single_page'], load['multipage'])
    crossed = sum(group['Cross_Channel_Requests'] for group in groups)
    idle = sum(group['All_Candidate_Dies_Idle_Requests'] for group in groups)
    decisions = case['wl_decisions'].values()
    below = sum(group['Full_Requests_Only_Below_Beta'] for group in decisions)
    changed = sum(group['WL_Decision_Changed_Requests'] for group in decisions)
    return {
        'cross_channel_write_requests': crossed,
        'cross_channel_writes_percent': 100 * crossed / case['writes'],
        'all_candidate_dies_idle_percent': 100 * idle / case['writes'],
        'requests_only_below_beta_percent': 100 * below / case['writes'],
        'wl_decision_change_percent': 100 * changed / case['writes'],
    }


def summarize(cases, traces, archived, manifest, intensity):
    report = {}
    for trace in traces:
        controls = archived[trace]['serial']['policies']
        sweep = {}
        for beta in (*BETAS, '1'):
            case = controls[MODE] if beta == '1' else cases[beta, trace]
            sweep[beta] = {
                MODE: case, 'archived_control': beta == '1',
                **acceptance_stats(case),
                'change_vs_beta1_percent': previous.compare(case, controls[MODE]),
                'change_vs_lepa_percent': previous.compare(case, controls['lepa']),
                'change_vs_wl_aware_lepa_percent': previous.compare(case, controls['lepa-wl-aware']),
            }
        report[trace] = {
            'qd1_beta_sweep': sweep,
            'reference_qd1': {mode: case['latency'] for mode, case in controls.items()},
        }
    intensity.write_json(OUTPUT / 'metrics.json', {
        'new_replays': len(cases), 'beta1_controls_reused': len(traces),
        'betas': [*BETAS, '1'], 'case_paths_relative_to': '..',
        'latency_unit': 'ms', 'per_trace': report, 'experiment': manifest,
    })


def main():
    parser = argparse.ArgumentParser(description='Sweep the Adaptive PACS acceptance beta at queue depth one.')
    parser.add_argument('--jobs', type=int, default=8)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if shutil.which('pigz') is None:
        parser.error('pigz is required to store lossless latency archives')

    intensity, pilot, traces, parent = previous.setup(None)
    prior_path = previous.OUTPUT / 'metrics.json'
    archived = previous.read(prior_path)['per_trace']
    base_options = parent['serial_options'][MODE]
    assert base_options[previous.GAIN] == base_options[previous.SCREEN] == '1'
    assert base_options['PACS_Cross_Channel_Search_Budget'] == '256'
    for trace, data in traces.items():
        reference = archived[trace]['serial']['policies'][MODE]
        expected = data['1']['reads'] + data['1']['writes']
        assert reference['requests_completed'] == expected
        assert reference['adaptive_search']['beta'] == reference['adaptive_search']['screening_beta'] == 1
        assert reference['completion_driven_replay'] == {
            'Max_Outstanding_Requests': 1, 'Outstanding_At_End': 0,
            'Max_Completion_To_Next_Submission_ns': 0,
        }

    options = {beta: {**base_options, previous.GAIN: beta} for beta in BETAS}
    manifest = {
        'trace_names': list(traces), 'trace_manifest_sha256': parent['trace_manifest_sha256'],
        'settings': parent['settings'], 'template_sha256': parent['template_sha256'],
        'beta_options': options, 'beta1_reference_sha256': intensity.file_sha256(prior_path),
        'beta1_reference': '../metrics.json', 'serial_replay': parent['serial_replay'],
    }
    OUTPUT.mkdir(exist_ok=True)
    record = OUTPUT / 'experiment.json'
    if record.exists():
        assert previous.read(record) == manifest, 'Experiment inputs changed'
    else:
        intensity.write_json(record, manifest)

    runners = {}
    for beta in BETAS:
        runner = previous.load_module('qd1_' + beta.replace('.', '_'), intensity.RELATED / 'run_experiment.py')
        runner.EXPERIMENT = OUTPUT / beta
        runner.MODES = {MODE: 'PACS'}
        runner.verify_inputs(previous.TEMPLATE)
        runners[beta] = runner

    cases, failures = {}, []
    with TemporaryDirectory(prefix='mqsim-qd1-beta-') as work:
        templates = {beta: previous.template_for(work, beta, 'PACS', flags, True) for beta, flags in options.items()}

        def replay(beta, trace):
            runner = runners[beta]
            timing = {**traces[trace]['1'], 'completion_driven_replay': True}
            case = runner.run_case(templates[beta], 'MQSim', trace, MODE, timing)
            return previous.measure(case, runner, trace, MODE, timing, options[beta], True, None, intensity, pilot)

        # Dispatch the longer replays first to keep the workers supplied with work.
        order = sorted(traces, key=lambda name: -(traces[name]['1']['reads'] + traces[name]['1']['writes']))
        tasks = [(beta, trace) for trace in order for beta in BETAS]
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            pending = {pool.submit(replay, *key): key for key in tasks}
            for job in as_completed(pending):
                key = pending[job]
                try:
                    cases[key] = job.result()
                    print(f'[{len(cases)}/{len(tasks)}] beta={key[0]} {key[1]} verified', flush=True)
                except Exception as error:
                    failures.append(key)
                    print(f'FAILED {key}: {error!r}', flush=True)
    if failures:
        raise SystemExit(f'{len(failures)} cases failed: {failures}')
    summarize(cases, traces, archived, manifest, intensity)
    print(f'{len(cases)} new QD1 replays verified. Results in {OUTPUT / "metrics.json"}', flush=True)


if __name__ == '__main__':
    main()
