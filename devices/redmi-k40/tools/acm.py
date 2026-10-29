"""The phone's USB serial port (hakios.usb.c, usb=2: CDC ACM 1209:316d "HakiOS K40"): wait for it to
appear, open it (DTR on: the phone sends only to an open port), and keep what comes in a file.

usage: acm.py OUT [--wait S] [--secs S] [--send TEXT --send-at S]...
  --wait   how long to wait for the port to appear (default 180 s)
  --secs   how long to read once it is open (default 60 s; the port going away ends it too)
  --send   text to send (\\n for a newline) --send-at seconds after the port opened (repeatable, in pairs)
Says when the port appeared, how much came, and the first and last lines."""
import argparse
import time

import serial
import serial.tools.list_ports

VID, PID = 0x1209, 0x316d

ap = argparse.ArgumentParser()
ap.add_argument('out')
ap.add_argument('--wait', type=float, default=180)
ap.add_argument('--secs', type=float, default=60)
ap.add_argument('--send', action='append', default=[])
ap.add_argument('--send-at', action='append', type=float, default=[])
a = ap.parse_args()
sends = sorted(zip(a.send_at, [s.encode().decode('unicode_escape').encode() for s in a.send]))

t0 = time.time()
port = None
while time.time() - t0 < a.wait and port is None:
    for p in serial.tools.list_ports.comports():
        if p.vid == VID and p.pid == PID:
            port = p
            break
    else:
        time.sleep(0.5)
if port is None:
    raise SystemExit('acm: no %04x:%04x port in %.0f s' % (VID, PID, a.wait))
print('acm: %s appeared after %.1f s: %s, serial %s' % (port.device, time.time() - t0, port.description, port.serial_number))
s = None
for attempt in range(20):
    try:
        s = serial.Serial(port.device, 115200, timeout=0.2)
        break
    except serial.SerialException as e:
        time.sleep(0.5)
if s is None:
    raise SystemExit('acm: %s cannot be opened' % port.device)
s.dtr = True
opened = time.time()
data = bytearray()
with open(a.out, 'wb') as f:
    while time.time() - opened < a.secs:
        while sends and time.time() - opened >= sends[0][0]:
            s.write(sends.pop(0)[1])
        try:
            chunk = s.read(65536)
        except serial.SerialException as e:
            print('acm: the port went away after %.1f s (%s)' % (time.time() - opened, e))
            break
        if chunk:
            data += chunk
            f.write(chunk)
            f.flush()
try:
    s.close()
except Exception:
    pass
lines = data.decode('utf-8', 'replace').splitlines()
print('acm: %d bytes, %d lines in %.1f s' % (len(data), len(lines), time.time() - opened))
for l in lines[:5] + (['...'] if len(lines) > 10 else []) + lines[-5:]:
    print('  ' + l[:160])
