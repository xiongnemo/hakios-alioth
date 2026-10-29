"""Why the window server's frames are 16 ms apart while a finger is on the screen (touch_lat=1 run): for each
pair of server frames (tl: g) with a finger down, when did the client's last commit (tl: a: woke commit done)
before the second frame come? Late (within 3 ms of the frame, and long after the one before): the client was
late. Early (the commit came, and the server started its frame only much later): the server waited for
something else (its timer, the previous swap). usage: gap_cause.py LOG..."""
import bisect
import re
import sys
from collections import Counter

seen = set()
touches, commits, frames = [], [], []
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
        f = t.split()
        if t.startswith('tl: i ') and 'posted' in f:
            touches.append(int(f[f.index('posted') + 1]))
        elif t.startswith('tl: a ') and len(f) >= 5 and f[2].isdigit() and f[4].isdigit():
            commits.append(int(f[4]))       # done: the commit is in the server's hands
        elif t.startswith('tl: g ') and 'began' in f:
            frames.append(int(f[f.index('began') + 1]))
touches.sort()
commits.sort()
frames = sorted(set(frames))


def finger_down(t):
    i = bisect.bisect_left(touches, t - 25000)
    return i < len(touches) and touches[i] <= t


kinds = Counter()
waits = Counter()
for a, b in zip(frames, frames[1:]):
    gap = (b - a) / 1000.0
    if not finger_down(b) or gap >= 40:
        continue
    i = bisect.bisect_left(commits, b) - 1
    last = commits[i] if i >= 0 else 0
    lead = (b - last) / 1000.0          # how long before the frame the last commit was in
    if gap < 12:
        kinds['on time (<12 ms)'] += 1
    elif last <= a:
        kinds['no commit since the frame before: the client was late'] += 1
    elif lead < 3:
        kinds['the commit came late (<3 ms before the frame)'] += 1
    else:
        kinds['the commit was there early: the server waited %s' % ('>8 ms' if lead > 8 else '3-8 ms')] += 1
        waits[min(int(lead), 16)] += 1
n = sum(kinds.values())
print('%d server-frame intervals with a finger down' % n)
for k, v in kinds.most_common():
    print('  %5d %4.0f%%  %s' % (v, 100.0 * v / n, k))
if waits:
    print('when the server waited with a commit in hand: how long the commit had been there (ms):')
    for k in sorted(waits):
        print('  %2d: %d' % (k, waits[k]))
