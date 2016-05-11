#!/usr/bin/python

import collections
import sys,argparse
import json
import numpy as np
from matplotlib import pyplot as plt
from matplotlib.lines import Line2D


def load_attempts(filename):
    all_attempts = list()
    if filename:
        with open(filename) as f:
            for line in f:
                data = json.loads(line)
                if data['type'] == 'instances':
                    all_attempts.extend(tnx_attempt.make_new(data))
    else:
        for line in sys.stdin:
            data = json.loads(line)
            if data['type'] == 'instances':
                all_attempts.extend(tnx_attempt.make_new(data))
    return all_attempts

class tnx_attempt:
    def __init__(self, pid, attempt_type, start_at, end_at):
        self.pid        = pid
        self.attempt_type= attempt_type
        self.start_at   = start_at
        self.end_at     = end_at
    @staticmethod
    def make_new(data):
        attempts = list()
        pid = data['pid']
        for attempt in data['attempts']:
            attempts.append(tnx_attempt(pid, attempt['type'], attempt['start_at'], attempt['end_at']))
        return attempts

def draw_plot2(infile, outfile, first_count, last_count, shift_x, max_y, title):
    if not last_count:
        last_count = first_count + 1

    all_attempts0 = load_attempts(infile)
    all_attempts = list()
    counts = collections.defaultdict(int)
    (min_x, max_x) = (0, 0)

    for attempt in all_attempts0:
        counts[attempt.pid] += 1
        if counts[attempt.pid] >= first_count and counts[attempt.pid] < last_count:
            all_attempts.append(attempt)
            if min_x == 0:
                min_x = (attempt.start_at - shift_x * 1000) / 1000
            max_x = max(max_x, (attempt.end_at - shift_x * 1000) / 1000)
    print("%d %d" % (len(all_attempts), len(all_attempts0)))
    print("%d %d" % (min_x, max_x))

    fig = plt.figure()
    ax = fig.add_subplot(1,1,1)
    ax.set_title(title)
    ax.set_ylabel('Threads')
    ax.set_xlabel('Time')
    ax.axis([min_x, max_x, 0, max_y+2])

    for attempt in all_attempts:
        line_color = 'green'
        line_width = 2
        if attempt.attempt_type == 'abort':
            line_width = 1
            line_color = 'red'
        elif attempt.attempt_type == 'commit':
            line_width = 1
            line_color = 'blue'
        elif attempt.attempt_type == 'lockQ':
            line_width = 1
            line_color = 'pink'
        elif attempt.attempt_type == 'lock':
            line_width = 1
            line_color = 'black'

        y = (attempt.pid + 1)
        # Convert X axis values to scale (in milliseconds, or 1000-cycle units)
        start_x = (attempt.start_at - shift_x * 1000) / 1000
        end_x = (attempt.end_at - shift_x * 1000) / 1000

        if end_x < min_x:
            pass
        elif start_x > max_x:
            pass
        else:
            ax.plot((start_x, end_x), (y, y), color=line_color, linestyle='solid', linewidth=line_width)

    fig.savefig(outfile, dpi=200)
def draw_plot1(infile, outfile, min_x, max_x, shift_x, max_y, title):
    all_attempts = load_attempts(infile)

    fig = plt.figure()
    ax = fig.add_subplot(1,1,1)
    ax.set_title(title)
    ax.set_ylabel('Threads')
    ax.set_xlabel('Time')
    ax.axis([min_x, max_x, 0, max_y+2])

    for attempt in all_attempts:
        line_color = 'green'
        line_width = 2
        if attempt.attempt_type == 'abort':
            line_width = 1
            line_color = 'red'
        elif attempt.attempt_type == 'commit':
            line_width = 1
            line_color = 'blue'
        elif attempt.attempt_type == 'lockQ':
            line_width = 1
            line_color = 'grey'
        elif attempt.attempt_type == 'lock':
            line_width = 1
            line_color = 'black'

        y = (attempt.pid + 1)
        # Convert X axis values to scale (in milliseconds, or 1000-cycle units)
        start_x = (attempt.start_at - shift_x * 1000) / 1000
        end_x = (attempt.end_at - shift_x * 1000) / 1000

        if end_x < min_x:
            pass
        elif start_x > max_x:
            pass
        else:
            ax.plot((start_x, end_x), (y, y), color=line_color, linestyle='solid', linewidth=line_width)

    fig.savefig(outfile, dpi=200)

def main():
    parser = argparse.ArgumentParser('Converts attempts jsonl file into timeline')
    parser.add_argument('-i', '--infile')
    parser.add_argument('-o', '--outfile', default='a.png')
    parser.add_argument('-m', '--min_x', type=int, default=0)       # In units of 1000-cycles
    parser.add_argument('-M', '--max_x', type=int, default=1000)    # In units of 1000-cycles
    parser.add_argument('-s', '--shift_x', type=int, default=0)     # In units of 1000-cycles
    parser.add_argument('-c', '--first_count', type=int)
    parser.add_argument('-C', '--last_count', type=int)
    parser.add_argument('-Y', '--max_y', type=int, default=16)
    parser.add_argument('-t', '--title', default='Timeline')

    args = parser.parse_args()

    if args.first_count:
        draw_plot2(args.infile, args.outfile, args.first_count, args.last_count, args.shift_x, args.max_y, args.title)
    else:
        draw_plot1(args.infile, args.outfile, args.min_x, args.max_x, args.shift_x, args.max_y, args.title)

if __name__ == "__main__":
    main()

