"""Frame intervals (tl: v, frames shown) split by whether a finger was on the screen (a tl: i touch event within the
last 25 ms) or not (animations after the finger lifted): to tell frames lost from frames never asked for.
usage: touch_split.py LOG..."""
import re
import sys
from collections import Counter

seen = set()
touches, shown = [], []
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
touches.sort()
shown = sorted(set(shown))
groups = {'finger down': Counter(), 'after lift': Counter()}
j = 0
for a, b in zip(shown, shown[1:]):
    d = (b - a) / 1000.0
    if d >= 60:
        continue
    while j < len(touches) and touches[j] < b - 25000:
        j += 1
    down = j < len(touches) and touches[j] <= b
    groups['finger down' if down else 'after lift'][min(int(d // 2) * 2, 40)] += 1
print('%d touch events, %d frames shown' % (len(touches), len(shown)))
for name, c in groups.items():
    n = sum(c.values())
    fast = sum(v for k, v in c.items() if k < 12)
    print('%s: %d intervals, %.0f%% under 12 ms (120 Hz pace)' % (name, n, 100.0 * fast / n if n else 0))
    for k in sorted(c):
        print('  %2d-%2d ms: %5d %s' % (k, k + 2, c[k], '#' * min(70, c[k] * 70 // max(c.values()))))
