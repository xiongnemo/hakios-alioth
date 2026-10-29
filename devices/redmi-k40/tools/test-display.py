#!/usr/bin/env python3
"""Exercise the real scan reader with fake registers; no phone or MMIO access."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
source = (root / 'devices/redmi-k40/overlays/xnu/files/bsd/dev/hakios.display.c').read_text()

def function(name):
    start = source.index(name + '(')
    start = source.rfind('\n', 0, start) + 1
    end = source.index('\n}', start) + 2
    return source[start:end]

defines = '\n'.join(re.findall(
    r'^#define (?:GCC_DISP_AHB_CBCR|DISPCC_MDSS_CORE_GDSCR|DISPCC_MDSS_AHB_CBCR|'
    r'CBCR_ENABLE|CBCR_OFF|GDSC_POWER_ON|INTF_FRAME_COUNT|INTF_LINE_COUNT)\s+.*$', source, re.M))
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uintptr_t vm_offset_t;
#define NSEC_PER_MSEC 1000000
#define NSEC_PER_USEC 1000
static bool umi, unstable, clocks_off, command_mode;
static uint32_t te_initial, te_idle_start, te_phase, te_written;
static bool te_wrap_on_read;
static unsigned te_reads;
static uint64_t fake_ns;
static uint64_t longest_delay;
static uint64_t mach_absolute_time(void) { return fake_ns; }
static void absolutetime_to_nanoseconds(uint64_t t, uint64_t *ns) { *ns=t; }
static unsigned frame_reads, interface_reads, writes;
static uint32_t clock_value;
static vm_offset_t gcc_map=1, dispcc_map=2, intf=3;
static struct { int ok; uint32_t lines, shown; uint64_t line_ns, frame_ns; } scan;
static void delay_for_interval(unsigned a, unsigned b) {
    uint64_t ns=(uint64_t)a*b;
    fake_ns+=ns;
    if (ns > longest_delay) longest_delay=ns;
}
static uint32_t rd(vm_offset_t base, uint32_t offset) {
    if (base == 4) { assert(offset == 0); return clock_value; }
    if (base == gcc_map) { assert(offset == (umi ? 0xc : 4)); return clocks_off ? 0 : 1; }
    if (base == dispcc_map) {
        if (offset == (umi ? 0x3000 : 0x1004)) return 0x80000000;
        assert(offset == (umi ? 0x2080 : 0x1050)); return 1;
    }
    assert(base == 3); interface_reads++;
    if (offset == 0x298) {
        uint32_t phase=te_phase;
        if (te_wrap_on_read && te_reads++ == 0) te_phase=1250;
        return te_initial + phase;
    }
    if (offset == 0x2b0) return te_written;
    if (offset == 0xac) return unstable ? frame_reads++ : 42;
    assert(offset == 0xb0); return 77;
}
static void wr(vm_offset_t base, uint32_t offset, uint32_t value) {
    assert(base == 4 && offset == 0); writes++; clock_value = value;
}
'''
program += '\n' + defines + '\n'
program += 'static int\n' + function('regs_alive') + '\n'
program += 'static int\n' + function('clock_on') + '\n'
program += 'uint32_t\n' + function('hakios_display_line') + '\n'
program += 'int hakios_display_command_mode(void) { return scan.ok && command_mode; }\n'
program += 'int\n' + function('hakios_display_wait_copy') + '\n'
program += r'''
int main(void) {
    uint32_t frame=0;
    for (unsigned board=0; board<2; board++) {
        umi=board; scan.ok=1; intf=3; unstable=false; clocks_off=false;
        assert(hakios_display_line(&frame) == 77 && frame == 42);
    }
    puts("PASS: UMI and Yupik use their own clock offsets");
    umi=true; scan.ok=1; intf=3; unstable=true; frame_reads=interface_reads=0;
    (void)hakios_display_line(&frame);
    assert(intf == 0 && scan.ok == 0 && frame_reads == 16 && interface_reads == 24);
    puts("PASS: unstable frame counter exits after eight attempts");
    scan.ok=1; intf=3; clocks_off=true; interface_reads=0;
    assert(hakios_display_line(&frame) == 0 && interface_reads == 0 && !scan.ok);
    puts("PASS: no interface read after a clock stops");
    writes=0; clock_value=UINT32_MAX;
    assert(!clock_on(4, 0, "invalid") && writes == 0);
    clock_value=0x80000001;
    assert(!clock_on(4, 0, "halted"));
    clock_value=0;
    assert(clock_on(4, 0, "running") && clock_value == 1);
    puts("PASS: inaccessible and halted clocks rejected; running clock accepted");
    command_mode=true; clocks_off=false; intf=3; scan.ok=1;
    scan.lines=2352; scan.shown=te_initial=2340; te_idle_start=1250;
    scan.line_ns=7083; scan.frame_ns=16666667; te_phase=1300; te_written=2340;
    assert(hakios_display_wait_copy(2000000, 20000000));
    te_written=1000; fake_ns=0;
    assert(!hakios_display_wait_copy(2000000, 20000000) && fake_ns >= 20000000);
    te_written=2340; te_phase=2300; fake_ns=0;
    assert(!hakios_display_wait_copy(2000000, 20000000));
    te_phase=1500; fake_ns=0;
    assert(hakios_display_wait_copy(2000000, 20000000));
    te_phase=2100; fake_ns=0;
    assert(hakios_display_wait_copy(500000, 20000000));
    assert(!hakios_display_wait_copy(2000000, 20000000));
    te_phase=100; fake_ns=0; longest_delay=0;
    assert(!hakios_display_wait_copy(2000000, 20000000));
    assert(longest_delay > 400000 && fake_ns >= 20000000 && fake_ns < 20001000);
    te_phase=1300; te_reads=0; te_wrap_on_read=true; fake_ns=0;
    assert(!hakios_display_wait_copy(2000000, 0));
    puts("PASS: command copy uses the full safe idle interval, rejects busy DMA, insufficient budget and TE wraps");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='umi-display-') as directory:
    c = Path(directory) / 'test.c'
    binary = Path(directory) / 'test'
    c.write_text(program)
    subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
