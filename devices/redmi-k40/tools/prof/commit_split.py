"""How often the client (SpringBoard) commits while a finger is on the screen, against how often frames are shown
(tl: a lines: a run-loop turn that committed; tl: i: touch events; tl: v: frames shown), from a touch_lat=1 run:
if the client commits every 16 ms, the server has nothing new to draw in between. usage: commit_split.py LOG..."""
import re
import sys
from collections import Counter

seen = set()
touches, commits, shown = [], [], []
sample = None
for path in sys.argv[1:]:
    for raw in open(path, 'rb'):
        l = raw.decode('utf-8', 'replace')
        at = l.find('tl: ')
        if at < 0:
            continue
        t = l[at:].strip()
        if t in seen:
            continue
        seen.add(t)
        m = re.match(r'tl: i \d+ posted (\d+)', t)
        if m:
            touches.append(int(m.group(1)))
            continue
        m = re.match(r'tl: v \d+ .*?shown (\d+)', t)
        if m:
            shown.append(int(m.group(1)))
            continue
        if t.startswith('tl: a '):
            if sample is None:
                sample = t
            f = t.split()
            # woke, commit, done: only commits that took a while (layers changed), as gesture_tl.py shows them
            if len(f) >= 5 and f[2].isdigit() and f[4].isdigit() and int(f[4]) - int(f[3]) > 300:
                commits.append(int(f[4]))
touches.sort()
print('a sample commit line:', sample)


def finger_down(t):
    lo, hi = 0, len(touches)
    while lo < hi:
        mid = (lo + hi) // 2
        if touches[mid] < t - 25000:
            lo = mid + 1
        else:
            hi = mid
    return lo < len(touches) and touches[lo] <= t


for name, series in (('SpringBoard commits', sorted(set(commits))), ('frames shown', sorted(set(shown)))):
    c = Counter()
    for a, b in zip(series, series[1:]):
        if finger_down(b) and b - a < 60000:
            c[min((b - a) // 2000 * 2, 40)] += 1
    n = sum(c.values())
    print('%s with a finger down: %d intervals, %.0f%% under 12 ms' % (name, n, 100.0 * sum(v for k, v in c.items() if k < 12) / n if n else 0))
    for k in sorted(c):
        print('  %2d-%2d ms: %5d %s' % (k, k + 2, c[k], '#' * min(60, c[k] * 60 // max(c.values()))))
