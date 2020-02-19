#!/usr/bin/env python3

import sys
import argparse

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
plt.rcParams['font.family'] = 'Times New Roman'


prettify = {
    'puffer_ttp_cl/bbr': ['Fugu', 'C3'],
    'linear_bba/bbr': ['BBA', 'C2'],
    'mpc/bbr': ['MPC-HM', 'C0'],
    'robust_mpc/bbr': ['RobustMPC-HM', 'C5'],
    'pensieve/bbr': ['Pensieve', 'C4']
}


def plot_data(data, output_figure):
    fig, ax = plt.subplots()
    ax.set_xlabel('Time spent stalled (%)')
    ax.set_ylabel('Average SSIM (dB)')

    for name in data:
        x = data[name]['stall'][2]
        y = data[name]['ssim'][2]

        pretty_name = prettify[name][0]
        pretty_color = prettify[name][1]

        ax.scatter(x, y, color=pretty_color)
        ax.errorbar(x, y,
            xerr=[[x - data[name]['stall'][0]], [data[name]['stall'][1] - x]],
            yerr=[[y - data[name]['ssim'][1]], [data[name]['ssim'][0] - y]],
            ecolor=pretty_color,
            capsize=4.0)
        ax.annotate(pretty_name, (x, y),
                    xytext=(4,5), textcoords='offset pixels')

    ax.invert_xaxis()

    # Hide the right and top spines
    ax.spines['right'].set_visible(False)
    ax.spines['top'].set_visible(False)

    fig.savefig(output_figure)
    sys.stderr.write('Saved plot to {}\n'.format(output_figure))


def parse_data(input_data_path):
    data = {}

    with open(input_data_path) as fh:
        for line in fh:
            if line[0] == '#':
                continue
            line = line.replace(',', '').replace(';', '').replace('%', '')

            items = line.split()

            name = items[0]
            data[name] = {}

            # stall_low, stall_high, stall_mean
            data[name]['stall'] = [float(x) for x in items[5:11:2]]

            # ssim_low, ssim_high, ssim_mean
            data[name]['ssim'] = [float(x) for x in items[13:19:2]]

    return data


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('input_data', help='input data (output of confint)')
    parser.add_argument('output_figure', help='output figure')
    args = parser.parse_args()

    data = parse_data(args.input_data)
    plot_data(data, args.output_figure)


if __name__ == '__main__':
    main()
