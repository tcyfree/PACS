import json
from statistics import median

from report_all_traces import METRICS, MODES, write_report
from run_missing_factors import FACTORS, OUTPUT

from matplotlib import pyplot as plt
from matplotlib.ticker import FuncFormatter, MaxNLocator, NullLocator


STYLES = {
    'baseline': ('#6D7785', None, '--'),
    'lepa': ('#B36A26', 's', '-'),
    'lepa-wl-aware': ('#7955A6', 'D', '-'),
    'pacs-nowl': ('#008076', '^', '-'),
    'pacs-full': ('#2360A6', 'o', '-'),
}


def change(trace, factor, mode, metric):
    cases = trace['factors'][str(factor)]['policies']
    return 100 * (cases[mode]['latency'][metric] / cases['baseline']['latency'][metric] - 1)


def draw(ax, series, title):
    ax.set_title(title, loc='left')
    ax.set_xscale('log')
    ax.set_xticks(FACTORS, ['1', '5', '10', '50', '100', '1k', '10k'])
    ax.xaxis.set_minor_locator(NullLocator())
    ax.yaxis.set_major_locator(MaxNLocator(nbins=5))
    ax.yaxis.set_major_formatter(FuncFormatter(lambda value, _: f'{value:g}%'))
    ax.grid(axis='y', color='#DBE3EC', linewidth=0.9)
    ax.set_axisbelow(True)
    ax.margins(x=0.047, y=0.14)
    for mode, name in MODES.items():
        color, marker, linestyle = STYLES[mode]
        ax.plot(FACTORS, series[mode], label=name, color=color, marker=marker, linestyle=linestyle,
                markersize=5.2, markeredgecolor='white', markeredgewidth=0.43,
                linewidth=2.25 if mode == 'pacs-full' else 1.65)


def finish_figure(figure, axes, title, subtitle, file):
    figure.suptitle(title, x=0.086, y=0.977, ha='left', fontsize=23, fontweight='semibold')
    if subtitle:
        figure.text(0.086, 0.935, subtitle, fontsize=12.4, color='#45556A', ha='left')
    handles, labels = axes.flat[0].get_legend_handles_labels()
    figure.legend(handles, labels, loc='upper left', bbox_to_anchor=(0.086, 0.902), ncol=5,
                  frameon=False, borderaxespad=0, columnspacing=1.7, handlelength=1.8)
    figure.supxlabel('Arrival interval multiplier', fontsize=14, y=0.018)
    figure.supylabel('Latency change relative to Baseline at the same multiplier', fontsize=14, x=0.020)
    figure.savefig(OUTPUT / file, dpi=185)
    plt.close(figure)


def plot(data):
    plt.rcParams.update({
        'font.family': 'DejaVu Sans', 'font.size': 11.5, 'axes.titlesize': 15,
        'axes.titleweight': 'semibold', 'axes.titlepad': 7, 'text.color': '#152335',
        'axes.labelcolor': '#202D3D', 'xtick.color': '#28384C', 'ytick.color': '#28384C',
        'axes.edgecolor': '#BCC7D3', 'axes.spines.top': False, 'axes.spines.right': False,
        'figure.facecolor': '#FFFFFF', 'savefig.facecolor': '#FFFFFF',
    })
    traces = data['per_trace']
    figure, axes = plt.subplots(3, 2, sharex=True, figsize=(14.2, 10.2))
    figure.subplots_adjust(left=0.086, right=0.982, bottom=0.086, top=0.819, wspace=0.21, hspace=0.35)
    for ax, (key, _, title) in zip(axes.flat, METRICS):
        series = {mode: [median(change(trace, factor, mode, key) for trace in traces.values())
                         for factor in FACTORS] for mode in MODES}
        draw(ax, series, title)
    finish_figure(figure, axes, 'SSD latency across seven workload intensities',
                  'Median of the per-trace changes across 16 traces', 'latency-by-factor.png')

    figure, axes = plt.subplots(4, 4, sharex=True, figsize=(19.2, 12.2))
    figure.subplots_adjust(left=0.086, right=0.984, bottom=0.077, top=0.825, wspace=0.32, hspace=0.37)
    for ax, (name, trace) in zip(axes.flat, traces.items()):
        series = {mode: [change(trace, factor, mode, 'average_write_latency_ms') for factor in FACTORS]
                  for mode in MODES}
        draw(ax, series, name)
        ax.yaxis.set_major_locator(MaxNLocator(nbins=4))
        ax.tick_params(axis='x', labelsize=10)
    finish_figure(figure, axes, 'Average write latency by trace',
                  'Each panel uses the Baseline results of that trace', 'average-write-by-trace.png')


if __name__ == '__main__':
    data = json.loads((OUTPUT / 'metrics.json').read_text())
    assert data['factors'] == FACTORS and data['trace_count'] == 16 and data['run_count'] == 560
    write_report(data, OUTPUT)
    plot(data)
