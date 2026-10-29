#!/usr/bin/env python3
"""Exercise the actual UMI TPG/SMMU code with a host MMIO model.

This simulates a completed frame for state/cleanup tests; it does not validate
real ISP output, which must be checked separately on the phone.
"""
from pathlib import Path
import re, subprocess, tempfile
src=(Path(__file__).resolve().parents[1]/'overlays/xnu/files/bsd/dev/umi_camera_dma.c').read_text()
src=re.sub(r'^#include .*\n','',src,flags=re.M)
src=re.sub(r'static uint32_t\ndr\(.*?\n}', 'static uint32_t\ndr(vm_offset_t p,unsigned o){return mock_read(p+o);}',src,flags=re.S)
src=re.sub(r'static void\ndw\(.*?\n}', 'static void\ndw(vm_offset_t p,unsigned o,uint32_t v){mock_write(p+o,v);}',src,flags=re.S)
src=re.sub(r'static void\ndcache\(.*?\n}', 'static void\ndcache(vm_offset_t p,vm_size_t s){(void)p;(void)s;}',src,flags=re.S)
pre=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
typedef int lck_mtx_t;
#define __printflike(a,b)
#define PAGE_MASK 16383
#define KMA_KOBJECT 0
#define KMA_ZERO 0
#define KMA_DATA 0
#define VM_KERN_MEMORY_IOKIT 0
#define KERN_SUCCESS 0
#define NSEC_PER_MSEC 1000000
#define round_page(v) (((v)+16383)&~16383ul)
static int kernel_map;
static unsigned lock_depth, allocations, fail_alloc, mapping_fail, ticks, fail_slot;
static bool no_frame, device_fault, tlb_stuck, route_fail, ttbr_fail, guard_damage;
static uint32_t smmu_regs[0x100000/4], ife_regs[0x4000/4];
static uint32_t saved_cbar, saved_cba2r, saved_cb[9];
static uintptr_t allocated[8];
static uint32_t mock_read(uintptr_t);
static void mock_write(uintptr_t,uint32_t);
static void mock_tick(void);
static void delay(unsigned n){(void)n;}
static void delay_for_interval(unsigned n,unsigned u){(void)n;(void)u;mock_tick();}
static void lck_mtx_lock(lck_mtx_t *p){(void)p;assert(!lock_depth);lock_depth=1;}
static void lck_mtx_unlock(lck_mtx_t *p){(void)p;assert(lock_depth==1);lock_depth=0;}
static size_t strlcpy(char *d,const char *s,size_t n){size_t l=strlen(s);if(n){size_t c=l<n-1?l:n-1;memcpy(d,s,c);d[c]=0;}return l;}
static int kmem_alloc(int m,vm_offset_t *p,vm_size_t n,unsigned f,unsigned t){
 (void)m;(void)f;(void)t;++allocations;if(allocations==fail_alloc)return 1;
 void *v=aligned_alloc(16384,round_page(n));assert(v);memset(v,0,round_page(n));*p=(uintptr_t)v;
 assert(allocations<8);allocated[allocations]=(uintptr_t)v;return 0;
}
static int kmem_alloc_contig(int m,vm_offset_t *p,vm_size_t n,unsigned mask,unsigned a,unsigned b,unsigned f,unsigned t){
 (void)mask;(void)a;(void)b;return kmem_alloc(m,p,n,f,t);
}
'''
post=r'''
lck_mtx_t hakios_smmu_lock;
uint64_t loader_recording[3];
uint64_t kvtophys(vm_offset_t v){return 0xa0000000ull+(v&0xffffff);}
vm_offset_t ml_io_map(vm_offset_t p,vm_size_t s){(void)s;return mapping_fail?0:p;}
void hakios_tl(const char *f,...){(void)f;}
static uint32_t mock_read(uintptr_t a){
 if(a>=0x15000000 && a<0x15100000){
  unsigned r=a-0x15000000;
  if(r>=0x80000 && (r&0xffff)==0x7f4 && tlb_stuck)return 1;
  return smmu_regs[r/4];
 }
 assert(a>=0xacd9000 && a<0xacdd000);return ife_regs[(a-0xacd9000)/4];
}
static void mock_write(uintptr_t a,uint32_t v){
 if(a>=0x15000000 && a<0x15100000){
  unsigned r=a-0x15000000;assert(lock_depth || !ud.context_changed);
  if(r>=0x800 && r<0x810 && (r-0x800)/4==fail_slot && route_fail && (v>>31))return;
  if(r>=0x80000 && (r&0xffff)==0x20 && ttbr_fail)return;
  if(r>=0x80000 && (r&0xffff)==0x58)smmu_regs[r/4]&=~v;
  else smmu_regs[r/4]=v;
  return;
 }
 assert(a>=0xacd9000 && a<0xacdd000);
 unsigned r=a-0xacd9000;
 if(r==0x1c00 && (v&1)){
  assert(ud.mapped && ud.context_changed && ud.installed==2);
  assert((smmu_regs[(0x80000+ud.context*0x10000)/4]&1));
 }
 if(r==0x1a20)ife_regs[0x1a28/4]&=~v;
 else ife_regs[r/4]=v;
}
static void mock_tick(void){
 ++ticks;
 if(!ud.running || no_frame)return;
 assert(ife_regs[0x1c04/4]==UD_IOVA+4096);
 assert((ife_regs[0x1c00/4]&1)&&ud.mapped);
 if(device_fault){smmu_regs[(0x80000+ud.context*0x10000+0x58)/4]=2;return;}
 unsigned char *p=(void *)(ud.memory+4096);
 for(unsigned i=0;i<UD_BYTES;i++)p[i]=(unsigned char)(i%251);
 if(guard_damage)p[UD_BYTES]=0xff;
 ife_regs[0x1a28/4]|=1u<<4;
}
static void reset(void){
 for(unsigned i=0;i<8;i++)if(allocated[i])free((void *)allocated[i]);
 memset(allocated,0,sizeof(allocated));memset(&ud,0,sizeof(ud));
 memset(smmu_regs,0,sizeof(smmu_regs));memset(ife_regs,0,sizeof(ife_regs));
 memset(loader_recording,0,sizeof(loader_recording));
 lock_depth=allocations=fail_alloc=mapping_fail=ticks=fail_slot=0;
 no_frame=device_fault=tlb_stuck=route_fail=ttbr_fail=guard_damage=false;
 smmu_regs[0x20/4]=(1u<<27)|8;smmu_regs[0x24/4]=0xa0000004; /* 64K pages, 8+8 banks, 4 CBs */
 saved_cbar=0x1200;saved_cba2r=0;
 smmu_regs[0x10000/4]=saved_cbar;smmu_regs[0x10800/4]=saved_cba2r;
 for(unsigned i=0;i<9;i++){saved_cb[i]=i?0x120+i:0;smmu_regs[(0x80000+ud_cbregs[i])/4]=saved_cb[i];}
}
static int capture(void){return umi_camera_tpg(0xacd9000,0xacd9200);}
static void clean(void){
 assert(!lock_depth&&!ud.mapped&&!ud.context_changed&&!ud.running);
 assert(!ife_regs[0x1c00/4]&&!ife_regs[0x800/4]);
 for(unsigned i=0;i<4;i++)assert(!(smmu_regs[(0x800+4*i)/4]>>31));
 assert(smmu_regs[0x10000/4]==saved_cbar && smmu_regs[0x10800/4]==saved_cba2r);
 for(unsigned i=0;i<9;i++)assert(smmu_regs[(0x80000+ud_cbregs[i])/4]==saved_cb[i]);
}
int main(void){
 reset();assert(capture()==0);clean();assert(((struct ud_header *)ud.memory)->frames==1);assert(loader_recording[1]==ud.size);
 for(unsigned i=1;i<=2;i++){reset();fail_alloc=i;assert(capture()==ENOMEM);clean();}
 reset();mapping_fail=1;assert(capture()==ENOMEM);clean();
 reset();smmu_regs[0x800/4]=(1u<<31)|UD_SID;assert(capture()==EBUSY);assert(!ud.context_changed);
 reset();smmu_regs[0]=1u<<3;assert(capture()==ENODEV);assert(!ud.context_changed);
 reset();smmu_regs[0x24/4]=0;assert(capture()==ENODEV);assert(!ud.context_changed);
 reset();tlb_stuck=true;assert(capture()==ETIMEDOUT);clean();
 reset();smmu_regs[(0x80000+0x58)/4]=0x400;assert(capture()==0);clean();assert(smmu_regs[(0x80000+0x58)/4]==0x400);
 reset();for(unsigned i=0;i<4;i++)smmu_regs[(0x80000+i*0x10000+0x58)/4]=2;assert(capture()==EBUSY);assert(!ud.context_changed);
 for(unsigned i=0;i<2;i++){reset();route_fail=true;fail_slot=i;assert(capture()==EIO);clean();}
 reset();ttbr_fail=true;assert(capture()==EIO);clean();
 reset();guard_damage=true;assert(capture()==EIO);clean();
 reset();device_fault=true;assert(capture()==EIO);clean();assert(!smmu_regs[(0x80000+0x58)/4]);
 reset();no_frame=true;assert(capture()==EIO);clean();assert(ticks==200);
 reset();assert(capture()==0);assert(capture()==EBUSY);clean();
 reset();
 puts("PASS: DMA allocation/mapping/translation failures, SID conflict, timeouts, simulated completion and SMMU restoration");
}
'''
with tempfile.TemporaryDirectory(prefix='umi-camera-dma-') as td:
 p=Path(td);(p/'test.c').write_text(pre+src+post)
 subprocess.run(['cc','-std=c11','-O1','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
