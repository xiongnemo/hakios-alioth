#!/usr/bin/env python3
"""Exercise UMI's actual torch sequencing and fault rollback with fake registers."""
from pathlib import Path
import re
import subprocess
import tempfile
source = (Path(__file__).resolve().parents[1] / 'overlays/xnu/files/bsd/dev/hakios.torch.c').read_text()
def function(name):
    start = source.rfind('\n', 0, source.index(name + '(')) + 1
    return source[start:source.index('\n}', start) + 2]
program = r'''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
static bool broken, configured, lit, fault;
static unsigned writes, fail_at;
static uint8_t regs[256];
static const char *trouble(void) { return fault ? "fault" : NULL; }
#define note(...) ((void)0)
static void give_up(const char *why) { (void)why; broken=true; }
static bool put(uint8_t reg, uint8_t value) {
    if (broken) return false;
    assert(reg==0x40 || reg==0x41 || reg==0x43 || reg==0x44 || reg==0x46 ||
           reg==0x47 || reg==0x50 || reg==0x52 || reg==0x49 || reg==0x4a || reg==0x4c);
    regs[reg]=value;
    if (++writes==fail_at) { broken=true; return false; }
    return true;
}
static bool put_bits(uint8_t reg, uint8_t mask, uint8_t value) {
    return put(reg, (regs[reg] & ~mask) | (value & mask));
}
'''
program += '\n'.join(re.findall(r'^#define (?:FLASH_\w+|ANDROID_TORCH_MA|IRES_UA|ANDROID_ITARGET)\s+.*$', source, re.M)) + '\n'
for name in ('off_write','dark','configure','light'):
    program += 'static bool\n' + function(name) + '\n'
program += r'''
int main(void) {
    assert(light(1000));
    assert(regs[0x50]==0 && regs[0x52]==3);
    assert(lit && regs[0x4c]==1 && regs[0x46]==0x80);
    assert(regs[0x43]==9 && regs[0x44]==0);
    assert(regs[0x40]==0x13 && regs[0x41]==0);
    assert(dark() && !lit && !regs[0x4c] && !regs[0x46]);
    assert(light(1) && regs[0x43]==1 && regs[0x44]==0);
    assert(light(UINT32_MAX) && regs[0x43]==9 && regs[0x44]==0);
    assert(dark());
    for (unsigned fail=1; fail<=8; fail++) {
        broken=configured=lit=false; writes=0; fail_at=fail;
        for (unsigned i=0;i<256;i++) regs[i]=0;
        assert(!light(1000) && broken);
        assert(!regs[0x4c] && !regs[0x46] && !regs[0x43] && !regs[0x44]);
        assert(!light(1000));
    }
    broken=configured=lit=false; fail_at=0; fault=true;
    assert(!light(1000) && broken && !regs[0x4c] && !regs[0x46]);
    puts("PASS: working channel only, current bounds, timer disable, partial-write rollback and fault shutoff");
}
'''
with tempfile.TemporaryDirectory(prefix='umi-torch-') as d:
    c=Path(d)/'test.c'; binary=Path(d)/'test'; c.write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
