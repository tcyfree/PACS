from itertools import combinations
import json
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
from matplotlib import pyplot as plt
from matplotlib.backends.backend_pdf import PdfPages
from matplotlib.ticker import MaxNLocator


OUTPUT = Path(__file__).resolve().parent
SERIES = (
    ('baseline', 'Baseline', '#A9B2BC', -0.275),
    ('lepa-wl-aware', 'WL-aware LEPA', '#3474A4', 0),
    ('pacs-adaptive', 'Offline Adaptive PACS', '#B55A32', 0.275),
)
METRICS = (
    ('average_write_latency_ms', 'Average Write Latency'),
    ('average_read_latency_ms', 'Average Read Latency'),
    ('p95_write_latency_ms', 'P95 Write Latency'),
    ('p99_write_latency_ms', 'P99 Write Latency'),
    ('p999_write_latency_ms', 'P99.9 Write Latency'),
    ('read_p99_latency_ms', 'P99 Read Latency'),
    ('single_page_write_p99_ms', 'Single-page Write P99'),
    ('single_page_write_p999_ms', 'Single-page Write P99.9'),
    ('multi_page_write_p99_ms', 'Cross-page Write P99'),
    ('multi_page_write_p999_ms', 'Cross-page Write P99.9'),
)


def percent_label(delta):
    return f'{delta:+.2f}%' if round(delta, 2) else '0.00%'


def draw(per_trace, metric, title):
    labels = list(per_trace)
    figure, axis = plt.subplots(figsize=(19.6, 6.75))
    figure.subplots_adjust(left=0.081, right=0.985, bottom=0.159, top=0.774)
    figure.text(0.081, 0.932, title, fontsize=25, weight='bold', va='top', color='#202934')
    highest = 1
    annotations = []
    for policy, label, color, offset in SERIES:
        values = [row['latency_relative_to_baseline'][policy][metric] for row in per_trace.values()]
        bars = axis.bar([i + offset for i in range(len(labels))], values, width=0.255, label=label, color=color, zorder=3)
        highest = max(highest, max(values))
        if policy == 'baseline':
            assert all(value == 1 for value in values)
            continue
        for bar, row in zip(bars, per_trace.values()):
            delta = row['change_vs_baseline_percent'][policy][metric]
            annotations.append(axis.annotate(
                percent_label(delta), (bar.get_x() + bar.get_width() / 2, bar.get_height()),
                xytext=(0, 5), textcoords='offset points', ha='center', va='bottom',
                rotation=90, fontsize=11, color='#202934', zorder=4,
                bbox={'facecolor': 'white', 'edgecolor': 'none', 'boxstyle': 'square,pad=0.05'},
            ))

    axis.set(xlim=(-0.62, len(labels) - 0.38), ylim=(0, highest / 0.78))
    axis.set_xticks(range(len(labels)), labels)
    axis.set_xlabel('Trace', fontsize=18, labelpad=11)
    axis.set_ylabel('Normalized latency (Baseline = 1)', fontsize=18, labelpad=11)
    axis.tick_params(axis='both', labelsize=15, color='#84909C')
    axis.yaxis.set_major_locator(MaxNLocator(nbins=6, steps=[1, 2, 2.5, 5, 10]))
    axis.grid(axis='y', linewidth=0.65, color='#E3E9EE', zorder=0)
    axis.axhline(1, linewidth=0.9, color='#74818D', linestyle=':', zorder=2)
    axis.spines[['top', 'right']].set_visible(False)
    axis.spines[['left', 'bottom']].set_color('#84909C')
    axis.legend(loc='lower left', bbox_to_anchor=(0, 1.035), frameon=False, ncols=3, fontsize=14, borderaxespad=0)

    figure.canvas.draw()
    renderer = figure.canvas.get_renderer()
    bounds = [text.get_window_extent(renderer) for text in annotations]
    assert all(axis.bbox.contains(box.x0, box.y0) and axis.bbox.contains(box.x1, box.y1) for box in bounds), metric
    assert all(not a.overlaps(b) for a, b in combinations(bounds, 2)), metric
    return figure


def main():
    per_trace = json.loads((OUTPUT / 'metrics-with-baseline.json').read_text())['per_trace']
    plt.rcParams.update({'font.family': 'DejaVu Sans', 'pdf.fonttype': 42})
    destination = OUTPUT / 'figures' / 'normalized-latency-with-baseline.pdf'
    destination.parent.mkdir(exist_ok=True)
    with PdfPages(destination, metadata={'Title': 'Normalized latency across 16 traces'}) as pdf:
        for metric, title in METRICS:
            figure = draw(per_trace, metric, title)
            pdf.savefig(figure)
            plt.close(figure)
    print(destination)


if __name__ == '__main__':
    main()
