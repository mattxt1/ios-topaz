#!/usr/bin/env python3
"""Run actual haptics functions from a patched sweet_keys.c under ASan/UBSan.
Usage: python3 tools/test-river-haptics.py PATH/TO/patched/bsd/dev/sweet_keys.c
No phone access; the register model checks completion, error and timeout paths.
"""
import pathlib, subprocess, sys, tempfile
source = pathlib.Path(sys.argv[1]).read_text()
def function(name):
    pos = source.index('\n' + name + '(')
    start = source.rfind('\n', 0, pos - 1) + 1
    brace = source.index('{', pos)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + '\n'
# The sysctl function uses XNU's argument macro rather than parentheses.
def sysctl_function():
    start=source.index('static int\nriver_vibrate_sysctl ')
    return source[start:source.index('\nSYSCTL_PROC',start)]
preamble=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <stddef.h>
typedef uintptr_t vm_offset_t;
static vm_offset_t ri_qup2=2;
static bool ri_haptics_ok;
static uint64_t ri_vib_deadline;
static int ri_vib_request=-1;
static unsigned op, status, spmi_status, loops, command, status_writes, clk, reg, value;
static unsigned aw_count, aw_fail;
static bool readback=true;
static uint32_t ri_rd(vm_offset_t base,unsigned off) {
 if(base==3) return spmi_status;
 if(off==0x18) return op;
 if(off==0x404) return status;
 if(off==0x218) return 0x00910187; /* tag, count, chip ID */
 return 0;
}
static void ri_wr(vm_offset_t base,unsigned off,uint32_t v) {
 if(base==3 && off==0) command=v;
 if(base==3 && off==8) status_writes++;
 if(off==0x400) clk=v;
}
static void delay(unsigned n) {(void)n;loops++;}
static bool ri_qup_state(vm_offset_t q,unsigned st) {(void)q;(void)st;return true;}
static bool ri_spmi_read(vm_offset_t obs,unsigned off,unsigned count,uint32_t *v) {
 (void)obs;(void)off;(void)count;*v=value;return readback;
}
static bool ri_aw_wr(uint8_t r,uint8_t v) {reg=r;value=v;return ++aw_count!=aw_fail;}
#define NSEC_PER_MSEC 1000000ULL
static void nanoseconds_to_absolutetime(uint64_t ns,uint64_t *out) {*out=ns;}
static uint64_t mach_absolute_time(void) {return 1000;}
struct request {void *newptr;};
static int requested;
#define SYSCTL_HANDLER_ARGS (void *oidp, void *arg1, int arg2, struct request *req)
static int sysctl_handle_int(void *o,int *v,int a,struct request *r) {
 (void)o;(void)a;(void)r;*v=requested;return 0;
}
'''
main=r'''
int main(void) {
 uint8_t tx=0,rx=0;
 op=0x100; loops=0; /* service needed is NOT write completion */
 assert(!ri_qup_xfer(2,0x5b,&tx,1,NULL,0)); assert(loops==2000);
 op=0x400; assert(ri_qup_xfer(2,0x5b,&tx,1,NULL,0)); assert(clk==0x35d);
 assert(ri_qup_xfer(1,1,&tx,1,NULL,0)); assert(clk==((62u<<16)|124u));
 status=8; assert(!ri_qup_xfer(2,0x5b,&tx,1,NULL,0));status=0;
 op=0x820; assert(ri_qup_xfer(2,0x5b,&tx,1,&rx,1));assert(rx==0x91);
 assert(!ri_qup_xfer(2,0x5b,&tx,17,NULL,0));
 spmi_status=1;assert(ri_spmi_write(3,0x45,3));assert(command==0x450);
 spmi_status=5;assert(!ri_spmi_write(3,0x45,3)); /* DONE + DENIED */
 spmi_status=3;assert(!ri_spmi_write(3,0x45,3)); /* DONE + FAILURE */
 spmi_status=9;assert(!ri_spmi_write(3,0x45,3)); /* DONE + DROPPED */
 spmi_status=0;loops=0;assert(!ri_spmi_write(3,0x45,3));assert(loops==2000);
 assert(status_writes==0); /* arbiter STATUS is not W1C */
 spmi_status=1;value=3;assert(ri_reset_wr(3,4,0x45,3));
 value=0x30;assert(!ri_reset_wr(3,4,0x45,3));
 value=3;readback=false;assert(!ri_reset_wr(3,4,0x45,3));readback=true;
 ri_haptics_ok=true;aw_count=0;aw_fail=0;
 river_vibrate(25);assert(ri_vib_deadline==25001000);
 river_vibrate(0);assert(ri_vib_deadline==0 && reg==4 && value==1);
 aw_fail=aw_count+1;river_vibrate(25);assert(!ri_haptics_ok && ri_vib_deadline==0);
 struct request req={.newptr=&requested};
 requested=-1;assert(river_vibrate_sysctl(NULL,NULL,0,&req)==EINVAL);
 requested=1001;assert(river_vibrate_sysctl(NULL,NULL,0,&req)==EINVAL);
 requested=25;assert(river_vibrate_sysctl(NULL,NULL,0,&req)==ENXIO);
 ri_haptics_ok=true;unsigned before=aw_count;
 assert(river_vibrate_sysctl(NULL,NULL,0,&req)==0);assert(aw_count==before);
 assert(__atomic_exchange_n(&ri_vib_request,-1,__ATOMIC_ACQUIRE)==25);
 requested=0;assert(river_vibrate_sysctl(NULL,NULL,0,&req)==0);assert(ri_vib_request==0);
 puts("PASS: QUP completion/NACK/read framing, SPMI opcode/errors/timeout/readback, pulse deadline, sysctl bounds/mailbox");
}
'''
code=preamble+''.join(function(n) for n in ['ri_qup_xfer','ri_spmi_write','ri_reset_wr','river_vibrate'])+sysctl_function()+main
with tempfile.TemporaryDirectory(prefix='river-haptics-test-') as temp:
    c=pathlib.Path(temp)/'test.c';c.write_text(code)
    exe=pathlib.Path(temp)/'test'
    subprocess.run(['clang','-std=c11','-O1','-g','-fsanitize=address,undefined',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
