from pathlib import Path
import subprocess, sys, tempfile
if len(sys.argv)!=2: raise SystemExit('usage: test-river-charge-sample.py PATCHED_SWEET_KEYS_C')
s=Path(sys.argv[1]).read_text();a=s.index('static int\nriver_charge_sample_sysctl');b=s.index('\nSYSCTL_PROC(_debug, OID_AUTO, river_charge_sample',a);fn=s[s.index('static bool\nri_power_read('):s.index('\n/* On-demand',s.index('static bool\nri_power_read('))]+s[a:b]
pre=r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
typedef uintptr_t vm_offset_t;
struct sysctl_req { const char *newptr; };
#define SYSCTL_HANDLER_ARGS (void *oidp, void *arg1, int arg2, struct sysctl_req *req)
static int ri_power_ready, ri_power_busy, calls, failat;
static vm_offset_t ri_charge_chgr=1,ri_charge_dcdc=2;
static bool ri_spmi_read(vm_offset_t p,unsigned reg,unsigned n,uint32_t*v){(void)p;(void)n;calls++;if(calls==failat)return false;*v=0xaabb0000u|reg;return true;}
static int sysctl_handle_string(void *o,char*s,int l,struct sysctl_req*r){(void)o;(void)l;(void)r;assert(strstr(s,"power_path=0xb"));return 0;}
'''
main=r'''int main(void){uint32_t masked;assert(ri_power_read(1,0xc0,1,&masked)&&masked==0xc0);assert(ri_power_read(1,0xc0,2,&masked)&&masked==0xc0);assert(ri_power_read(1,0xc0,4,&masked)&&masked==0xaabb00c0);assert(!ri_power_read(1,0xc0,3,&masked));calls=0;struct sysctl_req r={0};assert(river_charge_sample_sysctl(0,0,0,&r)==ENXIO);ri_power_ready=1;ri_charge_chgr=0;assert(river_charge_sample_sysctl(0,0,0,&r)==ENXIO);ri_charge_chgr=1;ri_power_busy=1;assert(river_charge_sample_sysctl(0,0,0,&r)==EBUSY);ri_power_busy=0;failat=4;assert(river_charge_sample_sysctl(0,0,0,&r)==EIO&&ri_power_busy==0);calls=0;failat=0;assert(river_charge_sample_sysctl(0,0,0,&r)==0&&calls==8&&ri_power_busy==0);puts("PASS: unavailable, busy, read error and charge snapshot");}'''
with tempfile.TemporaryDirectory(prefix='river-charge-test-') as d:
 src=Path(d)/'test.c'; exe=Path(d)/'test';src.write_text(pre+fn+main)
 subprocess.run(['clang','-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
