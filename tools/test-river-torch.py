from pathlib import Path
import subprocess, sys, tempfile
if len(sys.argv) != 2:
 raise SystemExit('usage: test-river-torch.py PATCHED_SWEET_KEYS_C')
s=Path(sys.argv[1]).read_text(); a=s.index('static void\nriver_torch_pulse(void)\n{');b=s.index('\nSYSCTL_PROC(_debug, OID_AUTO, river_torch_test',a);fn=s[a:b]
preamble=r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
struct sysctl_req { void *newptr; };
#define SYSCTL_HANDLER_ARGS (void *oidp, void *arg1, int arg2, struct sysctl_req *req)
static int sysctl_handle_int(void *o,int *v,int x,struct sysctl_req *r){(void)o;(void)x;if(r->newptr)*v=*(int*)r->newptr;return 0;}
typedef uintptr_t vm_offset_t;
static int ri_torch_status, ri_torch_request, ri_torch_attempted, bootarg=1, cfg=255, threshold=0, writes, waitms, fault, badread, failwrite, no_vreg, persistent_failure, usb_attached, seq[16];
static bool PE_parse_boot_argn(const char *a,int*b,unsigned c){(void)a;(void)c;*b=bootarg;return bootarg!=0;}
static vm_offset_t ri_map(vm_offset_t x){(void)x;return 1;}
static uint32_t ri_rd(vm_offset_t x,unsigned y){(void)x;(void)y;return 0x20010000;}
static vm_offset_t ri_spmi_obs(vm_offset_t x,unsigned p){(void)x;return p;}
static vm_offset_t ri_spmi_chnl(vm_offset_t x,unsigned p){(void)x;return p;}
static bool ri_spmi_read(vm_offset_t x,unsigned a,unsigned n,uint32_t*v){(void)n;if(badread)return false;*v=0;
 if(x==0x3d3){switch(a){case 4:*v=0x18;break;case 5:*v=5;break;case 0x40:*v=0x93;break;case 0x49:*v=1;break;case 0x76:*v=0x9f;break;}}
 if(x==0x2a6){switch(a){case 0x60:*v=cfg;break;case 0x68:*v=threshold;break;case 7:*v=no_vreg?0:0x10;break;case 8:*v=fault?7:0;break;}}
 if(x==0x213 && a==0x10)*v=usb_attached?0x10:0;
 if(x==0x248){switch(a){case 8:*v=0xc1;break;case 0xbf:*v=150;break;case 0xc0:*v=19000;break;}} return true;}
static bool ri_reset_wr(vm_offset_t x,vm_offset_t y,unsigned a,uint8_t v){(void)x;(void)y;seq[writes++]=(a<<8)|v;if(persistent_failure || failwrite==writes)return false;return true;}
static void ri_wait(unsigned n){waitms+=n;}
'''
main=r'''
int main(void){
 river_torch_pulse();assert(ri_torch_status==-3&&writes==0);
 cfg=0x80;threshold=25;usb_attached=1;river_torch_pulse();assert(ri_torch_status==-3&&writes==0);usb_attached=0;river_torch_pulse();assert(ri_torch_status==1&&writes==4&&waitms==100);
 assert(seq[0]==0x4680&&seq[1]==0x4c01&&seq[2]==0x4c00&&seq[3]==0x4600);
 writes=0;waitms=0;fault=1;river_torch_pulse();assert(ri_torch_status==-4&&writes==3&&seq[1]==0x4c00&&seq[2]==0x4600);
 writes=0;fault=0;failwrite=2;river_torch_pulse();assert(ri_torch_status==-5&&writes==4&&seq[2]==0x4c00&&seq[3]==0x4600);
 writes=0;failwrite=0;no_vreg=1;waitms=0;river_torch_pulse();assert(ri_torch_status==-4&&writes==3&&waitms==300);
 writes=0;no_vreg=0;persistent_failure=1;river_torch_pulse();assert(ri_torch_status==-6&&writes==7);
 writes=0;persistent_failure=0;badread=1;river_torch_pulse();assert(ri_torch_status==-2&&writes==0);
 struct sysctl_req req; int cmd=1; req.newptr=&cmd; bootarg=0;
 assert(river_torch_test_sysctl(0,0,0,&req)==ENXIO && ri_torch_request==0);
 bootarg=1; cmd=2; assert(river_torch_test_sysctl(0,0,0,&req)==EINVAL);
 cmd=1; assert(river_torch_test_sysctl(0,0,0,&req)==0 && ri_torch_request==1 && ri_torch_status==2);
 assert(river_torch_test_sysctl(0,0,0,&req)==EBUSY);
 puts("PASS: refusal, pulse, fault, timeout, cleanup and one-shot request");
}
'''
with tempfile.TemporaryDirectory(prefix='river-torch-test-') as d:
 src=Path(d)/'test.c'; exe=Path(d)/'test'
 src.write_text(preamble+fn+'int main(void){river_torch_pulse();assert(ri_torch_status == -8 && writes == 0);puts("PASS: production interlock makes no writes");}')
 subprocess.run(['clang','-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
 src.write_text(preamble+fn+main)
 subprocess.run(['clang','-DRIVER_TORCH_MODEL_ONLY=1','-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
