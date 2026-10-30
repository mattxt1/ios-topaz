from pathlib import Path
import subprocess,sys,tempfile
if len(sys.argv)!=2:raise SystemExit('usage: test-river-charge-timing.py PATCHED_SWEET_KEYS_C')
s=Path(sys.argv[1]).read_text();a=s.index('static bool\nri_charge_timing_read');b=s.index('\nSYSCTL_PROC(_debug, OID_AUTO, river_charge_timing',a);fn=s[a:b]
pre=r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
typedef uintptr_t vm_offset_t;
struct sysctl_req { const char *newptr; };
#define SYSCTL_HANDLER_ARGS (void *oidp, void *arg1, int arg2, struct sysctl_req *req)
static int ri_power_ready,ri_power_busy,delayed,writes,fail;
static vm_offset_t ri_charge_chgr=1;
static void ri_wr(vm_offset_t p,unsigned r,uint32_t v){(void)p;assert(r==0&&v==((1u<<27)|(4u<<4)));delayed=0;writes++;}
static uint32_t ri_rd(vm_offset_t p,unsigned r){(void)p;if(r==8)return fail?3:1;assert(r==0x18);return delayed?2:0;}
static void delay(unsigned n){if(n==10)delayed=1;}
static int sysctl_handle_string(void *o,char*s,int l,struct sysctl_req*r){(void)o;(void)l;(void)r;assert(strstr(s,fail?"immediate_error=64":"immediate_zero=64"));assert(strstr(s,fail?"delayed_error=64":"delayed_two=64"));return 0;}
'''
main=r'''int main(void){struct sysctl_req r={0};assert(river_charge_timing_sysctl(0,0,0,&r)==ENXIO);ri_power_ready=1;ri_power_busy=1;assert(river_charge_timing_sysctl(0,0,0,&r)==EBUSY);ri_power_busy=0;assert(river_charge_timing_sysctl(0,0,0,&r)==0&&writes==128&&ri_power_busy==0);writes=0;fail=1;assert(river_charge_timing_sysctl(0,0,0,&r)==0&&writes==128&&ri_power_busy==0);puts("PASS: unavailable, busy, timing comparison and error accounting");}'''
with tempfile.TemporaryDirectory(prefix='river-charge-timing-') as d:
 src=Path(d)/'test.c';exe=Path(d)/'test';src.write_text(pre+fn+main)
 subprocess.run(['clang','-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
