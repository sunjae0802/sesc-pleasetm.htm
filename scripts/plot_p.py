#!/usr/bin/python

import collections
import sys,argparse
import json
from matplotlib import pyplot as plt
from matplotlib.lines import Line2D


def load_attempts(filename):
    all_attempts = collections.defaultdict(list)
    if filename:
        with open(filename) as f:
            for line in f:
                data = json.loads(line)
                if data['type'] == 'instances':
                    new_instance = atomic_instance.make_new(data)
                    all_attempts[new_instance.pid].append(new_instance)
    else:
        for line in sys.stdin:
            data = json.loads(line)
            if data['type'] == 'instances':
                new_instance = atomic_instance.make_new(data)
                all_attempts[new_instance.pid].append(new_instance)
    return all_attempts

class atomic_attempt:
    def __init__(self, attempt_type, start_at, end_at):
        self.attempt_type= attempt_type
        self.start_at   = start_at
        self.end_at     = end_at
    @staticmethod
    def make_new(attempts_data):
        attempts = list()
        for attempt in attempts_data:
            attempts.append(atomic_attempt(attempt['type'], attempt['start_at'], attempt['end_at']))
        return attempts

class atomic_instance:
    def __init__(self, pid, start_at, end_at, attempts, rset, wset):
        self.pid        = pid
        self.start_at   = start_at
        self.end_at     = end_at
        self.attempts   = attempts
        self.rset       = rset
        self.wset       = wset
    @staticmethod
    def make_new(data):
        pid     = data['pid']
        start_at= data['start_at']
        end_at  = data['end_at']
        attempts= atomic_attempt.make_new(data['attempts'])
        rset    = set(data['rset'])
        wset    = set(data['wset'])
        return atomic_instance(pid, start_at, end_at, attempts, rset, wset)

def compute_my_p(pids, conflicts):
    csets= list()
    left_p = set(pids)
    while len(left_p) > 0:
        cset = set()
        pid = left_p.pop()
        cset.add(pid)
        also_add = set(conflicts[pid]).difference(cset)
        while len(also_add) > 0:
            a = also_add.pop()
            cset.add(a)
            left_p.remove(a)
            also_add.update(set(conflicts[a]).difference(cset))
        csets.append(cset)
    return len(csets)

def compute_my_p0(pids, conflicts):
    return len(pids) - max([len(c) for c in conflicts.values()])

def compute_p(all_attempts):
    pids = list(all_attempts.keys())
    max_attempts = 0
    for pid, attempts in all_attempts.iteritems():
        max_attempts = max(max_attempts, len(attempts))

    # Compute P
    computed_p = list()
    for attempt_id in range(0, max_attempts):
        conflicts = dict()
        for pid1 in pids:
            conflicts[pid1] = list()
            if attempt_id < len(all_attempts[pid1]):
                rset1 = all_attempts[pid1][attempt_id].rset
                wset1 = all_attempts[pid1][attempt_id].wset
                for pid2 in pids:
                    if pid1 != pid2 and attempt_id < len(all_attempts[pid2]):
                        rset2 = all_attempts[pid2][attempt_id].rset
                        wset2 = all_attempts[pid2][attempt_id].wset
                        if rset1.isdisjoint(wset2) and wset1.isdisjoint(rset2):
                            pass
                        else:
                            conflicts[pid1].append(pid2)
        my_p = compute_my_p(pids, conflicts)
        computed_p.append(my_p)

    return computed_p

def draw_cplot(infile, outfile, title):
    all_attempts = load_attempts(infile)
    computed_p = compute_p(all_attempts)

    pids = list(all_attempts.keys())
    max_attempts = 0
    for pid, attempts in all_attempts.iteritems():
        print("%d %d" % (pid, len(attempts)))
        max_attempts = max(max_attempts, len(attempts))

    threads_alive = list()
    for attempt_id in range(0, max_attempts):
        alive = 0
        for pid in pids:
            if attempt_id < len(all_attempts[pid]):
                alive += 1
        threads_alive.append(alive)

    # Now actually draw the plot
    fig = plt.figure()
    ax1 = fig.add_subplot(1,1,1)

    ax1.set_title(title)
    ax1.set_ylabel('Parallelism')
    ax1.set_xlabel("Time (# instances)")
    ax1.axis([0, max_attempts, 0, len(pids) + 1])

    x = list(range(0, max_attempts))
    y = computed_p
    ax1.plot(x, y, color='black')
    ax1.fill_between(x, y, color='0.8')

    ax2 = ax1.twinx()
    ax2.axis([0, max_attempts, 0, len(pids) + 1])
    ax2.set_ylabel('Threads Alive', color='red')
    ax2.plot(x, threads_alive, color='red')

    fig.savefig(outfile, dpi=200)

def main():
    parser = argparse.ArgumentParser('Converts attempts jsonl file into timeline')
    parser.add_argument('-i', '--infile')
    parser.add_argument('-o', '--outfile', default='a.png')
    parser.add_argument('-t', '--title', default='Timeline')

    args = parser.parse_args()
    draw_cplot(args.infile, args.outfile, args.title)

if __name__ == "__main__":
    main()

