#!/usr/bin/env python3
"""Exercise the actual UMI mode loader using locally exported vendor tables.

Pass camera-s5khmx-v3.tab and camera-s5khmx-v19.tab. No vendor data is stored
in the test; malformed containers must be rejected before sensor writes.
"""
from pathlib import Path
import re, subprocess, sys, tempfile
if len(sys.argv) != 3:
    raise SystemExit(__doc__)
src = (Path(__file__).resolve().parents[1] / 'overlays/xnu/files/bsd/dev/umi_camera.c').read_text()
u32 = re.search(r'static uint32_t\nutab_u32\(.*?\n}', src, re.S).group()
loader = re.search(r'static int\numode_prepare\(.*?\n}', src, re.S).group()
pre = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
static uint8_t blob[65536];
static uint64_t length;
static unsigned writes, fail_at;
static bool bad_read;
static uint16_t registers[65536];
static void hakios_tl(const char *f,...) {(void)f;}
static void ums(unsigned n) {(void)n;}
static void delay(unsigned n) {(void)n;}
static const uint8_t *hakios_wifi_file(const char *n,uint64_t *s) {(void)n;*s=length;return blob;}
static bool usensor_write(uint16_t r,uint16_t v) {
 assert(r!=0x0100 || !v);++writes;
 if(writes==fail_at)return false;
 registers[r]=v;return true;
}
static bool usensor_read(uint16_t r,uint16_t *v) {*v=registers[r]^(bad_read?1:0);return true;}
'''
post = r'''
static void run(unsigned version) {
 writes=fail_at=0;bad_read=false;
 memset(registers,0,sizeof(registers));
 assert(umode_prepare(version)==0);assert(writes>400);
 assert(registers[0x034c]==3008 && registers[0x034e]==2256 && !registers[0x0100]);
 unsigned n=writes;
 for(unsigned i=1;i<=n;i++){
  writes=0;fail_at=i;assert(umode_prepare(version)==EIO);assert(writes==i);
 }
 fail_at=0;writes=0;bad_read=true;assert(umode_prepare(version)==EIO);bad_read=false;
 /* Every truncation and a poisoned late table count must fail before I/O. */
 uint64_t saved=length;
 for(length=0;length<saved;length++){writes=0;assert(umode_prepare(version)==EINVAL);assert(!writes);}
 length=saved;
 uint64_t o=40+7*48;
 for(unsigned i=0;i<2;i++)o+=12+12ull*utab_u32(blob+o+8);
 unsigned tables=utab_u32(blob+o);o+=4;
 for(unsigned i=0;i<tables;i++){
  uint32_t count=utab_u32(blob+o+8);
  if(!memcmp(blob+o,"init",4)||!memcmp(blob+o,"res3",4)){
   uint8_t entry[12];memcpy(entry,blob+o+12,12);
   /* Inject stream-on and an unsupported width into selected tables. */
   blob[o+12]=0;blob[o+13]=1;blob[o+14]=0;blob[o+15]=1;
   writes=0;assert(umode_prepare(version)==EINVAL);assert(!writes);
   memcpy(blob+o+12,entry,12);blob[o+16]=1;
   writes=0;assert(umode_prepare(version)==EINVAL);assert(!writes);
   memcpy(blob+o+12,entry,12);
  }
  uint8_t save[4];memcpy(save,blob+o+8,4);memset(blob+o+8,255,4);
  writes=0;assert(umode_prepare(version)==EINVAL);assert(!writes);
  memcpy(blob+o+8,save,4);o+=12+12ull*count;
 }
 writes=0;assert(umode_prepare(4)==ENODEV);assert(!writes);
}
int main(int argc,char **argv){
 assert(argc==3);
 for(unsigned i=0;i<2;i++){
  FILE *f=fopen(argv[i+1],"rb");assert(f);
  length=fread(blob,1,sizeof(blob),f);assert(feof(f));fclose(f);
  run(i?19:3);
 }
 puts("PASS: both mode tables, all truncations, table-count overflow, write failures, geometry and no stream-on");
}
'''
with tempfile.TemporaryDirectory(prefix='umi-camera-tables-') as td:
    p=Path(td); (p/'test.c').write_text(pre+u32+'\n'+loader+post)
    subprocess.run(['cc','-std=c11','-O1','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test'),*map(str,map(Path.resolve,map(Path,sys.argv[1:])))],check=True)
