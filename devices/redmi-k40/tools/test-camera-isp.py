#!/usr/bin/env python3
"""Fault-inject the actual XO-clocked UMI ISP diagnostic, without hardware."""
from pathlib import Path
import subprocess, tempfile
src=(Path(__file__).resolve().parents[1]/'overlays/xnu/files/bsd/dev/umi_camera.c').read_text()
a=src.index('static struct {\n\tunsigned cmd;')
b=src.index('static void\nucleanup',a)
body=src[a:b]
pre=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
typedef uintptr_t vm_offset_t;
static struct { vm_offset_t cc,gcc; bool panic; } uc={.cc=0xad00000,.gcc=0x100000};
static uint32_t regs[0x10000/4], gccregs[0x80000/4], iferegs[0x4000/4];
static unsigned fail_reg, writes, reset_failure;
static bool map_failure, ownership_checked;
static uint32_t ur(vm_offset_t p,unsigned o) {
 uintptr_t a=p+o;
 if(a>=0xad00000 && a<0xad10000)return regs[(a-0xad00000)/4];
 if(a>=0x100000 && a<0x180000)return gccregs[(a-0x100000)/4];
 assert(a>=0xacd9000 && a<0xacdd000);
 /* IFE AHB must be enabled for every ISP register access. */
 assert((regs[0xc040/4]&1)&&(regs[0xc018/4]&1)&&(regs[0xc034/4]&1));
 if(a==0xacd9000+0x1f00)ownership_checked=true;
 return iferegs[(a-0xacd9000)/4];
}
static void uw(vm_offset_t,unsigned,uint32_t);
static void umi_camera_dma_quiesce(bool p){(void)p;}
static vm_offset_t umap(vm_offset_t p,unsigned s){(void)s;return map_failure?0:p;}
static void delay(unsigned n){(void)n;}
static void hakios_tl(const char *f,...){(void)f;}
static bool upoll(vm_offset_t p,unsigned o,uint32_t mask,uint32_t want,unsigned us){(void)us;return (ur(p,o)&mask)==want;}
'''
post=r'''
static void uw(vm_offset_t p,unsigned o,uint32_t v){
 uintptr_t a=p+o;++writes;
 if(a>=0xad00000 && a<0xad10000){
  unsigned r=a-0xad00000;
  if(r==0xc114 && (v&1))assert(ownership_checked);
  regs[r/4]=v;
  for(unsigned i=0;i<sizeof(isp_rcgs)/sizeof(isp_rcgs[0]);i++)if(r==isp_rcgs[i].cmd && r!=fail_reg)regs[r/4]&=~1u;
  for(unsigned i=0;i<sizeof(isp_branches)/sizeof(isp_branches[0]);i++)if(!isp_branches[i].gcc && r==isp_branches[i].reg && r!=fail_reg)regs[r/4]=v&~(1u<<31);
  return;
 }
 if(a>=0x100000 && a<0x180000){
  unsigned r=a-0x100000;
  if((r==0xb02c||r==0xb030)&&(v&1))assert(ownership_checked);
  gccregs[r/4]=v;
  if(r!=fail_reg)gccregs[r/4]&=~(1u<<31);
  return;
 }
 assert((regs[0xc040/4]&1)&&(regs[0xc018/4]&1)&&(regs[0xc034/4]&1));
 unsigned r=a-0xacd9000;
 assert(r<0x2200);
 /* This diagnostic never enables the four DMA write masters. */
 assert(r<0x1c00 || r>=0x2000);
 iferegs[r/4]=v;
 if(r==0x210){assert(v==0x1e||v==1);if(reset_failure!=1)iferegs[0x270/4]|=1;}
 if(r==0x278)iferegs[0x270/4]&=~v;
 if(r==0xc){assert(v==2);if(reset_failure!=2)iferegs[0x40/4]|=1u<<17;}
 if(r==0x34)iferegs[0x40/4]&=~v;
}
static void reset(void){
 memset(regs,0,sizeof(regs));memset(gccregs,0,sizeof(gccregs));memset(iferegs,0,sizeof(iferegs));
 writes=fail_reg=reset_failure=0;map_failure=ownership_checked=false;isp_ife=isp_csid=0;isp_resetting=false;
 for(unsigned i=0;i<sizeof(isp_rcgs)/sizeof(isp_rcgs[0]);i++){isp_rcgs[i].changed=false;regs[(isp_rcgs[i].cmd+4)/4]=0xa0000203;}
 for(unsigned i=0;i<sizeof(isp_branches)/sizeof(isp_branches[0]);i++){
  isp_branches[i].changed=false;
  uint32_t *r=isp_branches[i].gcc?gccregs:regs;r[isp_branches[i].reg/4]=1u<<31;
 }
 iferegs[0]=0x10000001;iferegs[0x200/4]=0x20000001;iferegs[0x1a00/4]=0x20000000;
 iferegs[0x28/4]=1u<<17;
 for(unsigned i=0;i<4;i++)iferegs[(0x1c00+0x100*i)/4]=0x10;
}
static void cleanup(void){
 uint32_t expected_mask=isp_resetting?isp_saved_ife_mask:iferegs[0x28/4];
 uisp_down();
 assert(iferegs[0x28/4]==expected_mask);
 for(unsigned i=0;i<sizeof(isp_rcgs)/sizeof(isp_rcgs[0]);i++)assert(regs[(isp_rcgs[i].cmd+4)/4]==0xa0000203);
 for(unsigned i=0;i<sizeof(isp_branches)/sizeof(isp_branches[0]);i++){
  uint32_t *r=isp_branches[i].gcc?gccregs:regs;assert(!(r[isp_branches[i].reg/4]&1));
 }
}
int main(void){
 reset();assert(!uisp_probe());cleanup();
 for(unsigned i=0;i<sizeof(isp_rcgs)/sizeof(isp_rcgs[0]);i++){reset();fail_reg=isp_rcgs[i].cmd;assert(uisp_probe()==ETIMEDOUT);cleanup();}
 for(unsigned i=0;i<sizeof(isp_branches)/sizeof(isp_branches[0]);i++){
  if(isp_branches[i].delay_only)continue;
  reset();fail_reg=isp_branches[i].reg;assert(uisp_probe()==EIO);cleanup();
 }
 for(unsigned i=1;i<=2;i++){reset();reset_failure=i;assert(uisp_probe()==ETIMEDOUT);cleanup();}
 reset();map_failure=true;assert(uisp_probe()==ENOMEM);cleanup();
 reset();regs[0xc018/4]|=1;assert(uisp_probe()==EBUSY);assert(!writes);
 reset();iferegs[0x1c00/4]=1;assert(uisp_probe()==EBUSY);cleanup();
 reset();iferegs[0x28/4]=1;assert(uisp_probe()==EBUSY);cleanup();
 puts("PASS: ISP clock failures, reset timeouts, inherited ownership, no DMA writes and reverse cleanup");
}
'''
with tempfile.TemporaryDirectory(prefix='umi-camera-isp-') as td:
 p=Path(td);(p/'test.c').write_text(pre+body+post)
 subprocess.run(['cc','-std=c11','-O1','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
