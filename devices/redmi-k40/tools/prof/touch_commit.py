"""From a touch move handed on (tl: p 2 ... at US) to the client's commit (tl: a woke commit done) and to the
server's next frame (tl: g began): how long each takes, and where in the TE period (tl: v shown = a TE) the
moves land and the frames begin. To choose touch_sync_lead. usage: touch_commit.py LOG..."""
import bisect
import re
import sys
from collections import Counter

seen = set()
moves, commits, frames, shown = [], [], [], []
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
        m = re.match(r'tl: p 2 \d+ \d+ \d+ at (\d+)', t)
        if m:
            moves.append(int(m.group(1)))
        elif t.startswith('tl: a ') and len(f) >= 5 and f[2].isdigit() and f[4].isdigit():
            commits.append(int(f[4]))
        elif t.startswith('tl: g ') and 'began' in f:
            frames.append(int(f[f.index('began') + 1]))
        else:
            m = re.match(r'tl: v \d+ .*?shown (\d+)', t)
            if m:
                shown.append(int(m.group(1)))
for s in (moves, commits, frames, shown):
    s.sort()


def hist(name, values, step=0.5, top=12):
    c = Counter(min(int(v / step) * step, top) for v in values)
    print('%s (%d):' % (name, len(values)))
    for k in sorted(c):
        print('  %5.1f ms: %5d %s' % (k, c[k], '#' * min(50, c[k] * 50 // max(c.values()))))


to_commit, to_frame, move_phase, frame_phase = [], [], [], []
for m in moves:
    i = bisect.bisect_left(commits, m)
    if i < len(commits) and commits[i] - m < 20000:
        to_commit.append((commits[i] - m) / 1000.0)
    j = bisect.bisect_left(frames, m)
    if j < len(frames) and frames[j] - m < 20000:
        to_frame.append((frames[j] - m) / 1000.0)
    k = bisect.bisect_right(shown, m) - 1
    if k >= 0 and m - shown[k] < 9000:
        move_phase.append((m - shown[k]) / 1000.0)
for g in frames:
    k = bisect.bisect_right(shown, g) - 1
    if k >= 0 and g - shown[k] < 9000:
        frame_phase.append((g - shown[k]) / 1000.0)
hist('move handed on -> the next commit done', to_commit)
hist('move handed on -> the next server frame begins', to_frame, 1.0, 20)
hist('where moves land after a frame went up (ms into the TE period)', move_phase)
hist('where server frames begin after a frame went up', frame_phase)
