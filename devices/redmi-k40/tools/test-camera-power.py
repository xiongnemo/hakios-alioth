#!/usr/bin/env python3
"""Fault-inject the actual UMI sensor probe against a host register model.

The model leaves the sensor unacknowledged, exercising the full power cycle
and its cleanup without pretending to validate a real sensor or CSI capture.
"""
from pathlib import Path
import argparse, re, subprocess, tempfile
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--object', type=Path, help='also reject SIMD/FP instructions in the compiled ARM64 probe object')
args = parser.parse_args()
source=(Path(__file__).resolve().parents[1] / 'overlays/xnu/files/bsd/dev/umi_camera.c').read_text()
source=re.sub(r'^#include .*\n','',source,flags=re.M)
source=re.sub(r'static uint32_t\nur\([^\n]*\)\n\{.*?\n\}', 'static uint32_t\nur(vm_offset_t p, unsigned o) { return mock_read(p+o); }',source,flags=re.S)
source=re.sub(r'static void\nuw\([^\n]*\)\n\{.*?\n\}', 'static void\nuw(vm_offset_t p, unsigned o, uint32_t v) { mock_write(p+o,v); }',source,flags=re.S)
prelude=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
typedef uintptr_t vm_offset_t;
typedef unsigned long vm_size_t;
#define __printflike(a,b)
#define NSEC_PER_MSEC 1000000
static uint32_t mock_read(uintptr_t);
static void mock_write(uintptr_t,uint32_t);
static void delay(unsigned n) { (void)n; }
static void delay_for_interval(unsigned a,unsigned b) { (void)a;(void)b; }
static void hakios_spmi_lock(void) {}
static void hakios_spmi_unlock(void) {}
'''
post=r'''
static struct { uintptr_t a; uint32_t v; } registers[2048];
static unsigned used, write_count, fail_write;
static int fail_rail, fail_clock;
static bool rail_state[3];
static uint64_t mapping_free;
static uint32_t initial_pin_cfg[4], initial_gcc[2];
static uint8_t gpio_regs[3][256], initial_gpio[3][256];
static const unsigned ids[3]={0x4c2,0x4c0,0x4c3};
static uint32_t get(uintptr_t a) {
 for(unsigned i=0;i<used;i++)if(registers[i].a==a)return registers[i].v;
 return 0;
}
static void set(uintptr_t a,uint32_t v) {
 for(unsigned i=0;i<used;i++)if(registers[i].a==a){registers[i].v=v;return;}
 assert(used<2048);registers[used].a=a;registers[used++].v=v;
}
static uint32_t mock_read(uintptr_t a) {
 if(a>=0x0ad00000 && a<0x0ad10000)assert(get(0x0010b008)&1);
 for(unsigned p=0;p<4;p++)if(a==uc.pin[p]+4){
  unsigned cfg=get(uc.pin[p]),out=get(a)&2;
  assert(!(cfg&0x800)||(cfg&0x1000));
  return out | ((cfg&512)?(out>>1):1);
 }
 return get(a);
}
static void mock_write(uintptr_t a,uint32_t v) {
 if(a>=0x0ad00000 && a<0x0ad10000)assert(get(0x0010b008)&1);
 for(unsigned p=0;p<4;p++)if(a==uc.pin[p]&&uc.reset_changed&&(uc.pin_cfg[p]&0x800))assert(v&0x1000);
 set(a,v);
 if(a==0x0010b008 && fail_clock==3){set(a,0);return;}
 if(a==0x0010b040 && fail_clock==4){set(a,0);return;}
 if(a==0x0ad0c144){set(a,(v&~0x80000000u)|((!(v&1)&&fail_clock!=1)?0x80000000u:0));return;}
 if(a==0x0ad0501c){if(fail_clock!=2)set(a,v&~1u);return;}
 if(a==0x0ad05034){set(a,(v&~0x80000000u)|((v&1)?0:0x80000000u));return;}
 if(a>=0x0e600000 && a<0x0e610000 && !(a&127)){
  unsigned c=(a-0x0e600000)/128,reg=(v>>4)&255;
  assert(c>=100 && c<103);set(a+8,1);set(a+0x18,gpio_regs[c-100][reg]);return;
 }
 if(a>=0x0c600000 && a<0x0e600000 && !(a&65535)){
  unsigned c=(a-0x0c600000)/65536,reg=(v>>4)&255;
  assert(c>=100 && c<103);++write_count;
  if(write_count==fail_write){set(a+8,3);return;}
  gpio_regs[c-100][reg]=get(a+0x10);set(a+8,1);
 }
}
vm_offset_t ml_io_map(vm_offset_t p,vm_size_t s){(void)s;return p;}
uint64_t umi_camera_iomap_free(void){return mapping_free;}
void hakios_tl(const char *f,...){(void)f;}
const uint8_t *hakios_wifi_file(const char *n,uint64_t *s){(void)n;*s=0;return NULL;}
int umi_camera_tpg(vm_offset_t a,vm_offset_t b){(void)a;(void)b;return ENODEV;}
void umi_camera_dma_quiesce(bool p){(void)p;}
int umi_rpmh_camera_vreg(unsigned r,bool enabled){assert(r<3);if(enabled&&(int)r==fail_rail)return EIO;rail_state[r]=enabled;return 0;}
static void reset_fixture(void){
 memset(&uc,0,sizeof(uc));used=write_count=fail_write=0;fail_rail=-1;fail_clock=0;
 mapping_free=32ull<<20;
 memset(initial_pin_cfg,0,sizeof(initial_pin_cfg));memset(initial_gcc,0,sizeof(initial_gcc));
 memset(rail_state,0,sizeof(rail_state));memset(gpio_regs,0,sizeof(gpio_regs));
 for(unsigned g=0;g<3;g++){
  gates[g].changed=false;gates[g].write=0;
  gpio_regs[g][4]=0x10;gpio_regs[g][5]=0x10;
  gpio_regs[g][0x40]=2;gpio_regs[g][0x42]=4;gpio_regs[g][0x45]=1;gpio_regs[g][0x46]=0x80;
  set(0x0c440900+4*(100+g),ids[g]<<8);
 }
 memcpy(initial_gpio,gpio_regs,sizeof(gpio_regs));
 set(0x0c440000,0x50000000);set(0x0c440004,512);
 set(0x0ad05020,0x12340000);set(0x0ad05034,0x80000000);set(0x0ad0c144,1);
}
static void check_cleanup(void){
 assert(!uc.active);assert(!rail_state[0]&&!rail_state[1]&&!rail_state[2]);
 assert(!memcmp(initial_gpio,gpio_regs,sizeof(gpio_regs)));
 assert(get(0x0ad05020)==0x12340000);assert(get(0x0ad05034)==0x80000000);assert(get(0x0ad0c144)==1);
 assert(get(0x0010b008)==initial_gcc[0]&&get(0x0010b040)==initial_gcc[1]);
 for(unsigned p=0;p<4;p++)assert(get(uc.pin[p])==initial_pin_cfg[p]);
}
int main(void){
 reset_fixture();assert(umi_camera_probe()==EIO);check_cleanup();
 for(unsigned n=1;n<=24;n++){reset_fixture();fail_write=n;assert(umi_camera_probe()==EIO);check_cleanup();}
 for(int n=0;n<3;n++){reset_fixture();fail_rail=n;assert(umi_camera_probe()==EIO);check_cleanup();}
 for(int n=1;n<=4;n++){reset_fixture();fail_clock=n;assert(umi_camera_probe()==EIO);check_cleanup();}
 reset_fixture();set(0x0ad05034,1);assert(umi_camera_probe()==EBUSY);assert(!write_count);
 reset_fixture();gpio_regs[0][0x44]=0x80;assert(umi_camera_probe()==EBUSY);assert(!write_count);
 reset_fixture();mapping_free=0;assert(umi_camera_probe()==ENOMEM);assert(!write_count);
 reset_fixture();set(0x0010b008,1);set(0x0010b040,1);initial_gcc[0]=initial_gcc[1]=1;
 assert(umi_camera_probe()==EIO);check_cleanup();
 reset_fixture();const uint32_t pages[4]={0x0f53e000,0x0f95f000,0x0f967000,0x0f968000};
 for(unsigned p=0;p<4;p++){set(pages[p],0x800);initial_pin_cfg[p]=0x800;}
 assert(umi_camera_probe()==EIO);check_cleanup();
 reset_fixture();set(0x0c440000,0x50010000);assert(umi_camera_probe()==EIO);check_cleanup();
 reset_fixture();set(0x0c440000,0x70000000);assert(umi_camera_probe()==EBUSY);assert(!write_count);check_cleanup();
 puts("PASS: GPIO/supply/clock failure cleanup, gated-CamCC access, extended GPIO, inherited clocks, I2C NACK and resource refusal");
}
'''
with tempfile.TemporaryDirectory(prefix='umi-camera-check-') as temp:
 p=Path(temp);(p/'probe.c').write_text(prelude+source+post)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'probe.c'),'-o',str(p/'probe')],check=True)
 subprocess.run([str(p/'probe')],check=True)
if args.object:
 disassembly=subprocess.check_output(['llvm-objdump','-d',str(args.object)],text=True)
 offenders=[line for line in disassembly.splitlines() if re.search(r'\b(?:[qds]\d{1,2}|v\d{1,2}\.\d*[bhsd])\b',line.partition(':')[2])]
 if offenders: raise SystemExit('Probe contains SIMD/FP register use:\n'+'\n'.join(offenders))
 print('PASS: compiled ARM64 probe uses only general registers')
