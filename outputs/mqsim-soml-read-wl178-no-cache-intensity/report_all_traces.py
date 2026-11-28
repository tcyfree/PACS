import json
from statistics import median

import matplotlib

from run_all_traces import OUTPUT


matplotlib.use('Agg')
from matplotlib import pyplot as plt
from matplotlib.ticker import FuncFormatter, MaxNLocator


METRICS = (
    ('average_write_latency_ms', '平均写', 'Average write latency'),
    ('average_read_latency_ms', '平均读', 'Average read latency'),
    ('p95_write_latency_ms', '写 P95', 'P95 write latency'),
    ('p99_write_latency_ms', '写 P99', 'P99 write latency'),
    ('p999_write_latency_ms', '写 P99.9', 'P99.9 write latency'),
    ('read_p99_latency_ms', '读 P99', 'P99 read latency'),
)
MODES = {
    'baseline': 'Baseline', 'lepa': 'LEPA', 'lepa-wl-aware': 'WL-aware LEPA',
    'pacs-nowl': 'PACS-NoWL', 'pacs-full': 'PACS-Full',
}


def table(header, rows, labels=1):
    separator = ['---'] * labels + ['---:'] * (len(header) - labels)
    return ['| ' + ' | '.join(row) + ' |' for row in (header, separator, *rows)]


def signed(value):
    return f'{0.0 if abs(value) < 0.005 else value:+.2f}%'


def write_share(case, field):
    return 100 * sum(case['workload_intensity'][group][field] for group in ('single_page', 'multipage')) / case['writes']


def write_report(data, output=OUTPUT):
    factors = [str(factor) for factor in data['factors']]
    traces = data['per_trace']
    archived = data.get('archived_case_count', data['archived_1x_count'])
    rows = []
    for mode, label in MODES.items():
        for factor in factors[1:]:
            changes = [row['factors'][factor]['latency_change_vs_1x_percent'][mode] for row in traces.values()]
            rows.append([label, f'{factor}×', *(signed(median(change[key] for change in changes)) for key, _, _ in METRICS)])
    doc = [
        '# 16 条 trace 的到达间隔倍率对照', '',
        f"共 {data['run_count']} 组结果，包含 {data['new_replays']} 组新回放和 {archived} 组已验证的归档对照。",
        '设备为 4 通道、16 KiB TLC，关闭写缓存。PACS 的接受系数 β 为 1。',
        '开环回放用 `t0 + k × (t − t0)` 放大到达间隔，同时到达的请求以原顺序提交。', '',
        '## 各策略相对 1× 的延迟变化', '',
        '每个百分比先在同一 trace、同一策略内与 1× 比较，再取 16 条 trace 的中位数。负值表示时延降低。', '',
        *table(['策略', '倍率', *(label for _, label, _ in METRICS)], rows, labels=2), '',
        '## PACS-Full 相对其他策略的平均写延迟', '',
        '百分比为相同 trace、相同倍率下的逐 trace 变化中位数。括号内为 PACS-Full 平均写延迟更低的 trace 数。', '',
    ]
    rows = []
    for factor in factors:
        row = [f'{factor}×']
        for reference in ('baseline', 'pacs-nowl', 'lepa', 'lepa-wl-aware'):
            changes = [trace['factors'][factor]['latency_change_percent'][f'pacs-full_vs_{reference}']
                       ['average_write_latency_ms'] for trace in traces.values()]
            row.append(f'{signed(median(changes))}（{sum(value < 0 for value in changes)}/{len(traces)}）')
        rows.append(row)
    doc += table(['倍率', 'Baseline', 'PACS-NoWL', 'LEPA', 'WL-aware LEPA'], rows)

    for trace, block in traces.items():
        tie_rate = 100 * block['zero_interval_pairs'] / (block['request_count'] - 1)
        doc += ['', f'## {trace}', '',
                f"{block['request_count']:,} 笔请求。相邻请求同时间戳占 {tie_rate:.3f}%。",
                '延迟单位为 ms，括号内为该策略在这条 trace 上相对 1× 的变化。', '']
        rows = []
        for mode, name in MODES.items():
            for key, label, _ in METRICS:
                values = []
                for factor in factors:
                    batch = block['factors'][factor]
                    value = batch['policies'][mode]['latency'][key]
                    change = batch['latency_change_vs_1x_percent'][mode][key]
                    values.append(f'{value:.4f}' if factor == '1' else f'{value:.4f} ({signed(change)})')
                rows.append([name, label, *values])
        doc += table(['策略', '指标', *(f'{factor}×' for factor in factors)], rows, labels=2)
        doc += ['', 'PACS-Full 的选址与调度统计。', '']
        rows = []
        for factor in factors:
            case = block['factors'][factor]['policies']['pacs-full']
            idle = write_share(case, 'All_Candidate_Dies_Idle_Requests')
            cross = write_share(case, 'Cross_Channel_Requests')
            wait = case['workload_intensity']['mean_tsu_write_wait_us']
            rows.append([f'{factor}×', f'{idle:.3f}%', f'{wait:.2f}', f'{cross:.4f}%'])
        doc += table(['倍率', '写入选址时候选 Die 均空闲', '平均写 TSU 等待（µs）', '跨通道写请求占比'], rows)
    (output / 'comparison.md').write_text('\n'.join(doc) + '\n', encoding='utf-8')


def plot(data):
    factors = data['factors'][1:]
    traces = data['per_trace']
    positions = list(range(len(traces)))
    plt.rcParams.update({
        'font.family': 'DejaVu Sans', 'font.size': 13, 'axes.labelsize': 14,
        'axes.titlesize': 16, 'axes.titleweight': 'semibold', 'axes.titlepad': 9,
        'text.color': '#152335', 'axes.labelcolor': '#202D3D', 'xtick.color': '#253347', 'ytick.color': '#253347',
        'axes.edgecolor': '#BDC6D2', 'axes.spines.top': False, 'axes.spines.right': False,
        'figure.facecolor': '#FFFFFF', 'savefig.facecolor': '#FFFFFF',
    })
    fig, axes = plt.subplots(3, 2, figsize=(17.8, 11.0), sharex=True)
    fig.subplots_adjust(left=0.071, right=0.978, bottom=0.124, top=0.837, wspace=0.22, hspace=0.38)
    fig.suptitle('PACS-Full latency under scaled arrivals', x=0.071, y=0.971, ha='left',
                 fontsize=25, fontweight='semibold')
    for ax, (metric, _, title) in zip(axes.flat, METRICS):
        ax.set_title(title, loc='left')
        ax.set_ylabel('Change vs 1×')
        ax.yaxis.set_major_locator(MaxNLocator(nbins=5))
        ax.yaxis.set_major_formatter(FuncFormatter(lambda value, _: f'{value:g}%'))
        ax.axhline(0, color='#687482', linewidth=1.05, label='1× reference', zorder=3)
        ax.grid(axis='y', color='#E1E7EF', linewidth=0.85)
        ax.set_axisbelow(True)
        ax.set_xlim(-0.55, len(traces) - 0.45)
        width = 0.25
        for index, (factor, color) in enumerate(zip(factors, ('#386FAC', '#087F75', '#C27B2F'))):
            values = [data['factors'][str(factor)]['latency_change_vs_1x_percent']['pacs-full'][metric]
                      for data in traces.values()]
            ax.bar([n + (index - 1) * width for n in positions], values,
                   width=0.93 * width, color=color, label=f'{factor}×', zorder=2)
    for ax in axes[-1]:
        ax.set_xticks(positions, traces, rotation=37, ha='right', rotation_mode='anchor')
        ax.set_xlabel('Trace')
    handles, labels = axes[0, 0].get_legend_handles_labels()
    fig.legend(handles, labels, loc='upper left', bbox_to_anchor=(0.071, 0.912), ncol=4,
               frameon=False, borderaxespad=0, columnspacing=2.05)
    fig.savefig(OUTPUT / 'latency-vs-1x.png', dpi=185)
    plt.close(fig)


if __name__ == '__main__':
    data = json.loads((OUTPUT / 'metrics.json').read_text())
    write_report(data)
    plot(data)
