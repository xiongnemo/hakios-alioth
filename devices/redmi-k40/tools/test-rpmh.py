#!/usr/bin/env python3
"""Check actual UMI RPMh code against immediate/stale hardware completions."""
from pathlib import Path
import re
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'overlays/xnu/files/bsd/dev/umi_rpmh.c').read_text()
source = re.sub(r'^#include .*\n', '', source, flags=re.M)
source = re.sub(r'static uint32_t\nrh_read\([^\n]*\)\n\{.*?\n\}',
                'static uint32_t\nrh_read(vm_offset_t p, uint32_t o) { return mock_read(p+o); }', source, flags=re.S)
source = re.sub(r'static void\nrh_write\([^\n]*\)\n\{.*?\n\}',
                'static void\nrh_write(vm_offset_t p, uint32_t o, uint32_t v) { mock_write(p+o,v); }', source, flags=re.S)
prelude = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
typedef uintptr_t vm_offset_t;
typedef unsigned long vm_size_t;
typedef int lck_mtx_t;
#define __printflike(a,b)
static unsigned mutex_depth, writes;
static bool mutex_busy, interrupts_on;
static uint32_t mock_read(uintptr_t);
static void mock_write(uintptr_t,uint32_t);
static bool ml_get_interrupts_enabled(void) { return interrupts_on; }
static void lck_mtx_lock(lck_mtx_t *p) { (void)p;assert(!mutex_depth&&!mutex_busy);mutex_depth=1; }
static bool lck_mtx_try_lock(lck_mtx_t *p) { (void)p;if(mutex_busy)return false;lck_mtx_lock(p);return true; }
static void lck_mtx_unlock(lck_mtx_t *p) { (void)p;assert(mutex_depth==1);mutex_depth=0; }
static void delay(unsigned n) { (void)n; }
'''
post = r'''
lck_mtx_t hakios_rsc_lock;
static struct { uintptr_t address; uint32_t value; } registers[512];
static unsigned used, trigger_count, last_mask;
static bool instant, pending;
static const uintptr_t slot = RH_TCS_PA + RH_TCS_STRIDE;
static uint32_t get(uintptr_t a) {
 for(unsigned i=0;i<used;i++)if(registers[i].address==a)return registers[i].value;
 return 0;
}
static void set(uintptr_t a,uint32_t v) {
 for(unsigned i=0;i<used;i++)if(registers[i].address==a){registers[i].value=v;return;}
 assert(used<512);registers[used].address=a;registers[used++].value=v;
}
static void complete(void) {
 unsigned mask=get(slot+RH_ENABLE);
 for(unsigned c=0;c<16;c++)if(mask&(1u<<c))set(slot+RH_CMD_STATUS+c*RH_CMD_STRIDE,0x10101);
 set(slot+RH_STATUS,1);
 if(get(RH_TCS_PA+RH_IRQ_EN)&2)set(RH_TCS_PA+RH_IRQ_STATUS,get(RH_TCS_PA+RH_IRQ_STATUS)|2);
 pending=false;
}
static uint32_t mock_read(uintptr_t a) {
 if(a==slot+RH_STATUS && pending){complete();return 0;}
 return get(a);
}
static void mock_write(uintptr_t a,uint32_t v) {
 assert(mutex_depth==1);writes++;
 if(a==RH_TCS_PA+RH_IRQ_CLEAR){set(RH_TCS_PA+RH_IRQ_STATUS,get(RH_TCS_PA+RH_IRQ_STATUS)&~v);return;}
 set(a,v);
 if(a==slot+RH_CONTROL && (v&RH_TRIGGER)) {
  trigger_count++;last_mask=get(slot+RH_ENABLE);
  if(instant)complete();else {pending=true;set(slot+RH_STATUS,0);}
 }
}
vm_offset_t ml_io_map(vm_offset_t p,vm_size_t n){(void)n;return p;}
void hakios_tl(const char *format,...){(void)format;}
static void fixture(void) {
 used=writes=trigger_count=last_mask=mutex_depth=0;pending=false;instant=true;
 mutex_busy=false;interrupts_on=true;rh_poisoned=false;rh_uncertain_irq=0;
 rh_tcs=RH_TCS_PA;rh_gic=0x17a00000;rh_ncpt=16;
 set(RH_TCS_PA+RH_IRQ_EN,1);set(RH_TCS_PA+RH_IRQ_STATUS,1);
 /* ADSP's slot 0 and its pending IRQ must remain untouched. */
 set(RH_TCS_PA+RH_STATUS,1);set(RH_TCS_PA+RH_WAIT,3);
 set(slot+RH_STATUS,1);set(slot+RH_CONTROL,RH_AMC_EN);
 for(unsigned c=0;c<3;c++)set(slot+RH_CMD_STATUS+c*RH_CMD_STRIDE,0x10101);
 set(rh_gic+0x84,1u<<5); /* INTID37: nonsecure Group1, level, masked */
}
static void released(void) {
 assert(!rh_poisoned&&!rh_uncertain_irq&&!mutex_depth);
 assert(get(slot+RH_CONTROL)==RH_AMC_EN&&!get(slot+RH_ENABLE)&&!get(slot+RH_WAIT));
 assert(get(RH_TCS_PA+RH_IRQ_EN)==1&&get(RH_TCS_PA+RH_IRQ_STATUS)==1);
 assert(get(RH_TCS_PA+RH_WAIT)==3&&!get(RH_TCS_PA+RH_CONTROL)&&!get(RH_TCS_PA+RH_ENABLE));
 assert(trigger_count==1);
}
int main(void) {
 fixture();assert(rh_send(0x42000,1800,true)==0);released();assert(last_mask==7);
 assert(get(slot+RH_ADDR)==0x42000&&get(slot+RH_DATA)==1800);
 assert(get(slot+RH_ADDR+RH_CMD_STRIDE)==0x42008&&get(slot+RH_DATA+RH_CMD_STRIDE)==7);
 assert(get(slot+RH_ADDR+2*RH_CMD_STRIDE)==0x42004&&get(slot+RH_DATA+2*RH_CMD_STRIDE)==1);
 fixture();assert(rh_send_state(0x42000,2850,true,false)==0);released();
 assert(get(slot+RH_DATA+2*RH_CMD_STRIDE)==0);
 fixture();assert(rh_send(0x42000,950,false)==0);released();assert(last_mask==3);
 assert(get(slot+RH_ADDR+RH_CMD_STRIDE)==0x42004&&get(slot+RH_DATA+RH_CMD_STRIDE)==1);
 /* No fresh IRQ and no observed busy transition cannot prove completion. */
 fixture();set(rh_gic+0x104,1u<<5);
 assert(rh_send(0x42000,1800,true)==ETIMEDOUT&&rh_poisoned&&!mutex_depth);
 assert(get(slot+RH_CONTROL)&RH_TRIGGER);assert(get(slot+RH_ENABLE)==7);
 assert(get(RH_TCS_PA+RH_IRQ_EN)==1&&get(RH_TCS_PA+RH_IRQ_STATUS)==1);
 /* If the GIC line is enabled, retain the bounded busy/status fallback. */
 fixture();set(rh_gic+0x104,1u<<5);instant=false;assert(rh_send(0x42000,1800,true)==0);released();
 fixture();set(rh_gic+0x84,0);instant=false;assert(rh_send(0x42000,1800,true)==0);released();
 fixture();set(rh_gic+0xc08,2u<<10);instant=false;assert(rh_send(0x42000,1800,true)==0);released();
 fixture();interrupts_on=false;mutex_busy=true;
 assert(rh_send(0x42000,1800,true)==EBUSY&&!writes&&!mutex_depth);
 fixture();interrupts_on=false;assert(rh_send(0x42000,1800,true)==0);released();
 puts("PASS: immediate completion, stale-status rejection, IRQ ownership, on/off votes, shared lock and panic try-lock");
}
'''
with tempfile.TemporaryDirectory(prefix='umi-rpmh-check-') as directory:
    path = Path(directory)
    (path / 'rpmh.c').write_text(prelude + source + post)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(path / 'rpmh.c'), '-o', str(path / 'rpmh')], check=True)
    subprocess.run([str(path / 'rpmh')], check=True)
