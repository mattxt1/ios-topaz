#include <notify.h>
/* Temporary on-phone DHCP/IPv4 bring-up helper; no credentials or persistence.
 * Includes DHCP lease client, renewal daemon, system DNS, and SCDynamicStore integration. */
#include <arpa/inet.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/bpf.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/route.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/sockio.h>
#include <time.h>
_Static_assert(sizeof(struct ifaliasreq)==64,"ifaliasreq ABI");
_Static_assert(sizeof(struct rt_msghdr)==92,"route ABI");
_Static_assert(sizeof(struct bpf_hdr)==20,"BPF ABI");
#include <unistd.h>

typedef const struct __SCDynamicStore * SCDynamicStoreRef;
typedef SCDynamicStoreRef (*SCDynamicStoreCreate_t)(CFAllocatorRef, CFStringRef, void*, void*);
typedef Boolean (*SCDynamicStoreSetValue_t)(SCDynamicStoreRef, CFStringRef, CFPropertyListRef);

static uint8_t mac[6],xid[4],offered[4],server[4],mask[4],gateway[4],dns[4];
static uint32_t lease_time = 86400, t1_time = 43200;

static uint16_t be16(const uint8_t*p){return ((unsigned)p[0]<<8)|p[1];}
static uint32_t be32(const uint8_t*p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static void put16(uint8_t*p,unsigned n){p[0]=n>>8;p[1]=n;}
static uint16_t checksum(const uint8_t*p,unsigned n){unsigned sum=0;for(unsigned k=0;k+1<n;k+=2)sum+=be16(p+k);if(n&1)sum+=p[n-1]<<8;while(sum>>16)sum=(sum&65535)+(sum>>16);return ~sum;}
static void fail(const char*s){perror(s);exit(1);}
static void showip(const char*label,const uint8_t*p){printf("%s=%u.%u.%u.%u\n",label,p[0],p[1],p[2],p[3]);}

static int receive_dhcp(const uint8_t*p,unsigned n,unsigned expected){
 if(n<14+20+8+240||be16(p+12)!=0x0800)return 0;
 const uint8_t*ip=p+14;unsigned ihl=(ip[0]&15)*4,total=be16(ip+2);
 if((ip[0]>>4)!=4||ihl<20||total>n-14||total<ihl+8||ip[9]!=17||(ip[6]&63)||ip[7]||checksum(ip,ihl))return 0;
 const uint8_t*u=ip+ihl;unsigned un=be16(u+4);
 if(be16(u)!=67||be16(u+2)!=68||un<248||un>total-ihl)return 0;
 const uint8_t*b=u+8;unsigned bn=un-8;
 if(b[0]!=2||b[1]!=1||b[2]!=6||memcmp(b+4,xid,4)||memcmp(b+28,mac,6)||memcmp(b+236,"\x63\x82\x53\x63",4))return 0;
 unsigned type=0;uint8_t si[4]={0},ma[4]={0},gw[4]={0},dn[4]={0};
 uint32_t lt=0,t1=0;
 for(unsigned k=240;k<bn;){unsigned id=b[k++];if(!id)continue;if(id==255)break;if(k>=bn)return 0;unsigned z=b[k++];if(z>bn-k)return 0;
  if(id==53&&z==1)type=b[k];
  if(id==54&&z==4)memcpy(si,b+k,4);
  if(id==1&&z==4)memcpy(ma,b+k,4);
  if(id==3&&z>=4)memcpy(gw,b+k,4);
  if(id==6&&z>=4)memcpy(dn,b+k,4);
  if(id==51&&z==4)lt=be32(b+k);
  if(id==58&&z==4)t1=be32(b+k);
  k+=z;
 }
 if(type==6){fprintf(stderr,"DHCP NAK\n");return -1;}
 if(type!=expected)return 0;
 if(expected==5){
  if(memcmp(si,"\0\0\0\0",4)&&memcmp(si,server,4)&&memcmp(server,"\0\0\0\0",4))return 0;
  if(memcmp(b+16,offered,4)&&memcmp(b+12,offered,4))return 0;
 } else {
  if(memcmp(si,"\0\0\0\0",4)==0)return 0;
 }
 if(memcmp(b+16,"\0\0\0\0",4))memcpy(offered,b+16,4);
 else if(memcmp(b+12,"\0\0\0\0",4))memcpy(offered,b+12,4);
 if(memcmp(si,"\0\0\0\0",4))memcpy(server,si,4);
 if(memcmp(ma,"\0\0\0\0",4))memcpy(mask,ma,4);
 if(memcmp(gw,"\0\0\0\0",4))memcpy(gateway,gw,4);
 if(memcmp(dn,"\0\0\0\0",4))memcpy(dns,dn,4);
 if(lt){
  lease_time=lt;
  t1_time=t1?t1:(lt/2);
  if(t1_time<30)t1_time=30;
 }
 return 1;
}

static void send_dhcp(int fd,unsigned type){
 uint8_t f[342]={0};memset(f,255,6);memcpy(f+6,mac,6);f[12]=8;
 uint8_t*ip=f+14,*u=ip+20,*b=u+8;ip[0]=0x45;put16(ip+2,sizeof(f)-14);ip[8]=64;ip[9]=17;memset(ip+16,255,4);put16(ip+10,checksum(ip,20));
 put16(u,68);put16(u+2,67);put16(u+4,sizeof(f)-34);b[0]=1;b[1]=1;b[2]=6;memcpy(b+4,xid,4);b[10]=128;memcpy(b+28,mac,6);memcpy(b+236,"\x63\x82\x53\x63",4);
 unsigned k=240;b[k++]=53;b[k++]=1;b[k++]=type;b[k++]=61;b[k++]=7;b[k++]=1;memcpy(b+k,mac,6);k+=6;
 if(type==3){b[k++]=50;b[k++]=4;memcpy(b+k,offered,4);k+=4;b[k++]=54;b[k++]=4;memcpy(b+k,server,4);k+=4;}
 b[k++]=55;b[k++]=4;b[k++]=1;b[k++]=3;b[k++]=6;b[k++]=51;b[k++]=255;
 if(write(fd,f,sizeof(f))!=sizeof(f))fail("BPF DHCP send");
}

/* RFC 2131 renewal request: client IP in ciaddr, destination server or broadcast */
static void send_dhcp_renew(int fd){
 uint8_t f[342]={0};memset(f,255,6);memcpy(f+6,mac,6);f[12]=8;
 uint8_t*ip=f+14,*u=ip+20,*b=u+8;ip[0]=0x45;put16(ip+2,sizeof(f)-14);ip[8]=64;ip[9]=17;
 memcpy(ip+12,offered,4);
 if(!memcmp(server,"\0\0\0\0",4))memset(ip+16,255,4);
 else memcpy(ip+16,server,4);
 put16(ip+10,checksum(ip,20));
 put16(u,68);put16(u+2,67);put16(u+4,sizeof(f)-34);
 b[0]=1;b[1]=1;b[2]=6;memcpy(b+4,xid,4);b[10]=0;
 memcpy(b+12,offered,4); /* ciaddr */
 memcpy(b+28,mac,6);memcpy(b+236,"\x63\x82\x53\x63",4);
 unsigned k=240;b[k++]=53;b[k++]=1;b[k++]=3; /* DHCPREQUEST */
 b[k++]=61;b[k++]=7;b[k++]=1;memcpy(b+k,mac,6);k+=6;
 b[k++]=55;b[k++]=4;b[k++]=1;b[k++]=3;b[k++]=6;b[k++]=51;b[k++]=255;
 if(write(fd,f,sizeof(f))!=sizeof(f))fail("BPF DHCP renew send");
}

static int wait_dhcp(int fd,unsigned buflen,unsigned type){
 uint8_t*buf=malloc(buflen);if(!buf)fail("malloc");int result=0;
 time_t stop=time(NULL)+4;
 while(time(NULL)<stop){struct pollfd p={fd,POLLIN,0};if(poll(&p,1,500)<=0)continue;ssize_t n=read(fd,buf,buflen);if(n<0){if(errno==EINTR)continue;break;}
  for(unsigned off=0;off+sizeof(struct bpf_hdr)<=(unsigned)n;){struct bpf_hdr h;memcpy(&h,buf+off,sizeof(h));if(h.bh_hdrlen<18||h.bh_hdrlen>(unsigned)n-off||h.bh_caplen>(unsigned)n-off-h.bh_hdrlen)break;
   result=receive_dhcp(buf+off+h.bh_hdrlen,h.bh_caplen,type);if(result)goto done;
   unsigned advance=BPF_WORDALIGN(h.bh_hdrlen+h.bh_caplen);if(!advance||advance>(unsigned)n-off)break;off+=advance;}
 }
 done:free(buf);return result;
}

static void addr_config(const char*name){
 int fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0)fail("inet socket");struct ifaliasreq a={0};snprintf(a.ifra_name,sizeof(a.ifra_name),"%s",name);
 struct sockaddr_in *ip=(void*)&a.ifra_addr,*nm=(void*)&a.ifra_mask,*br=(void*)&a.ifra_broadaddr;
 ip->sin_len=nm->sin_len=br->sin_len=sizeof(*ip);ip->sin_family=nm->sin_family=br->sin_family=AF_INET;
 memcpy(&ip->sin_addr,offered,4);memcpy(&nm->sin_addr,mask,4);br->sin_addr.s_addr=ip->sin_addr.s_addr|~nm->sin_addr.s_addr;
 if(ioctl(fd,SIOCAIFADDR,&a))fail("SIOCAIFADDR");close(fd);
}

/* Verify a route directly when route-socket acknowledgement is unavailable. */
static int route_matches(const uint8_t *buf,size_t len,unsigned index,const uint8_t want[4]){
 for(size_t off=0;off+sizeof(struct rt_msghdr)<=len;){
  struct rt_msghdr h;memcpy(&h,buf+off,sizeof(h));
  if(h.rtm_msglen<sizeof(h)||h.rtm_msglen>len-off)return 0;
  const uint8_t *dst=NULL,*gw=NULL;unsigned dlen=0,glen=0;int zero_mask=1,valid=1;
  size_t pos=off+sizeof(h),end=off+h.rtm_msglen;
  for(unsigned bit=1;bit<=RTA_BRD;bit<<=1){if(!(h.rtm_addrs&bit))continue;
   if(pos+2>end){valid=0;break;}unsigned n=buf[pos],step=n?(n+3)&~3u:4;
   if(n>end-pos||step>end-pos){valid=0;break;}
   if(bit==RTA_DST){dst=buf+pos;dlen=n;}if(bit==RTA_GATEWAY){gw=buf+pos;glen=n;}
   if(bit==RTA_NETMASK)for(unsigned k=4;k<n;k++)if(buf[pos+k])zero_mask=0;
   pos+=step;
  }
  if(valid&&h.rtm_version==RTM_VERSION&&h.rtm_index==index&&
     (h.rtm_flags&(RTF_UP|RTF_GATEWAY|RTF_HOST))==(RTF_UP|RTF_GATEWAY)&&
     dst&&gw&&dlen>=8&&glen>=8&&dst[1]==AF_INET&&gw[1]==AF_INET&&
     !memcmp(dst+4,"\0\0\0\0",4)&&!memcmp(gw+4,want,4)&&zero_mask)return 1;
  off+=h.rtm_msglen;
 }return 0;
}

static int route_check(const char*name,const uint8_t want[4]){
 int mib[]={CTL_NET,PF_ROUTE,0,AF_INET,NET_RT_DUMP,0};size_t n=0;
 if(sysctl(mib,6,NULL,&n,NULL,0)||!n||n>2*1024*1024)return 0;
 uint8_t*b=malloc(n);if(!b)return 0;
 int ok=!sysctl(mib,6,b,&n,NULL,0)&&route_matches(b,n,if_nametoindex(name),want);free(b);return ok;
}

static void default_route(const char*name){
 if(route_check(name,gateway)){puts("default route already present");return;}
 struct{struct rt_msghdr h;struct sockaddr_in dst,gw,mask;}r={0};
 r.h.rtm_msglen=sizeof(r);r.h.rtm_version=RTM_VERSION;r.h.rtm_type=RTM_ADD;r.h.rtm_index=if_nametoindex(name);r.h.rtm_flags=RTF_UP|RTF_GATEWAY|RTF_STATIC;r.h.rtm_addrs=RTA_DST|RTA_GATEWAY|RTA_NETMASK;r.h.rtm_seq=1;r.h.rtm_pid=getpid();
 r.dst.sin_len=r.gw.sin_len=r.mask.sin_len=sizeof(r.dst);r.dst.sin_family=r.gw.sin_family=r.mask.sin_family=AF_INET;memcpy(&r.gw.sin_addr,gateway,4);
 int fd=socket(PF_ROUTE,SOCK_RAW,AF_INET);if(fd<0)fail("route socket");
 if(write(fd,&r,sizeof(r))!=sizeof(r)){if(errno==EEXIST){close(fd);puts("default route already exists");return;}fail("default route write");}
 uint8_t buf[4096];for(unsigned w=0;w<10;w++){struct pollfd p={fd,POLLIN,0};if(poll(&p,1,500)<=0)continue;ssize_t n=read(fd,buf,sizeof(buf));if(n<(ssize_t)sizeof(struct rt_msghdr))continue;struct rt_msghdr*h=(void*)buf;if(h->rtm_seq==1&&h->rtm_pid==getpid()){if(h->rtm_errno){errno=h->rtm_errno;fail("default route result");}close(fd);return;}}
 close(fd);if(route_check(name,gateway)){puts("default route verified from routing table (socket acknowledgement unavailable)");return;}fprintf(stderr,"route acknowledgement/table verification failed\n");exit(1);
}

/* SystemConfiguration dynamic store integration for system DNS and primary network service */
static int sc_config(const char *name, const uint8_t *ip, const uint8_t *netmask, const uint8_t *gw, const uint8_t *dn) {
 void *h = dlopen("/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration", RTLD_LAZY);
 if (!h) {
  fprintf(stderr, "dlopen SystemConfiguration: %s\n", dlerror());
  return 0;
 }
 SCDynamicStoreCreate_t fn_Create = (SCDynamicStoreCreate_t)dlsym(h, "SCDynamicStoreCreate");
 SCDynamicStoreSetValue_t fn_SetValue = (SCDynamicStoreSetValue_t)dlsym(h, "SCDynamicStoreSetValue");
 if (!fn_Create || !fn_SetValue) {
  fprintf(stderr, "SCDynamicStore symbols unavailable\n");
  dlclose(h);
  return 0;
 }
 SCDynamicStoreRef ds = fn_Create(NULL, CFSTR("river-net"), NULL, NULL);
 if (!ds) {
  fprintf(stderr, "SCDynamicStoreCreate failed\n");
  dlclose(h);
  return 0;
 }
 char ip_str[INET_ADDRSTRLEN], mask_str[INET_ADDRSTRLEN], gw_str[INET_ADDRSTRLEN], dns_str[INET_ADDRSTRLEN];
 snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
 snprintf(mask_str, sizeof(mask_str), "%u.%u.%u.%u", netmask[0], netmask[1], netmask[2], netmask[3]);
 snprintf(gw_str, sizeof(gw_str), "%u.%u.%u.%u", gw[0], gw[1], gw[2], gw[3]);
 if (!memcmp(dn, "\0\0\0\0", 4)) snprintf(dns_str, sizeof(dns_str), "%s", gw_str);
 else snprintf(dns_str, sizeof(dns_str), "%u.%u.%u.%u", dn[0], dn[1], dn[2], dn[3]);

 CFStringRef cf_ifname = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
 CFStringRef cf_ip = CFStringCreateWithCString(NULL, ip_str, kCFStringEncodingUTF8);
 CFStringRef cf_mask = CFStringCreateWithCString(NULL, mask_str, kCFStringEncodingUTF8);
 CFStringRef cf_gw = CFStringCreateWithCString(NULL, gw_str, kCFStringEncodingUTF8);
 CFStringRef cf_dns = CFStringCreateWithCString(NULL, dns_str, kCFStringEncodingUTF8);
 CFStringRef cf_pubdns = CFSTR("1.1.1.1");

 const void *dns_addrs[2] = { cf_dns, cf_pubdns };
 CFArrayRef cf_dns_arr = CFArrayCreate(NULL, dns_addrs, 2, &kCFTypeArrayCallBacks);
 const void *dns_k[1] = { CFSTR("ServerAddresses") };
 const void *dns_v[1] = { cf_dns_arr };
 CFDictionaryRef cf_dns_dict = CFDictionaryCreate(NULL, dns_k, dns_v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
 fn_SetValue(ds, CFSTR("State:/Network/Global/DNS"), cf_dns_dict);

 const void *gipv4_k[3] = { CFSTR("PrimaryInterface"), CFSTR("PrimaryService"), CFSTR("Router") };
 const void *gipv4_v[3] = { cf_ifname, cf_ifname, cf_gw };
 CFDictionaryRef cf_gipv4_dict = CFDictionaryCreate(NULL, gipv4_k, gipv4_v, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
 fn_SetValue(ds, CFSTR("State:/Network/Global/IPv4"), cf_gipv4_dict);

 const void *addrs_v[1] = { cf_ip };
 CFArrayRef cf_addrs_arr = CFArrayCreate(NULL, addrs_v, 1, &kCFTypeArrayCallBacks);
 const void *masks_v[1] = { cf_mask };
 CFArrayRef cf_masks_arr = CFArrayCreate(NULL, masks_v, 1, &kCFTypeArrayCallBacks);

 const void *sipv4_k[4] = { CFSTR("Addresses"), CFSTR("SubnetMasks"), CFSTR("InterfaceName"), CFSTR("Router") };
 const void *sipv4_v[4] = { cf_addrs_arr, cf_masks_arr, cf_ifname, cf_gw };
 CFDictionaryRef cf_sipv4_dict = CFDictionaryCreate(NULL, sipv4_k, sipv4_v, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

 CFStringRef cf_svc_ipv4 = CFStringCreateWithFormat(NULL, NULL, CFSTR("State:/Network/Service/%s/IPv4"), name);
 fn_SetValue(ds, cf_svc_ipv4, cf_sipv4_dict);

 CFStringRef cf_svc_dns = CFStringCreateWithFormat(NULL, NULL, CFSTR("State:/Network/Service/%s/DNS"), name);
 fn_SetValue(ds, cf_svc_dns, cf_dns_dict);

 int lq_val = 100;
 CFNumberRef cf_lq = CFNumberCreate(NULL, kCFNumberIntType, &lq_val);
 const void *lq_k[1] = { CFSTR("LinkQuality") };
 const void *lq_v[1] = { cf_lq };
 CFDictionaryRef cf_lq_dict = CFDictionaryCreate(NULL, lq_k, lq_v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
 CFStringRef cf_if_lq = CFStringCreateWithFormat(NULL, NULL, CFSTR("State:/Network/Interface/%s/LinkQuality"), name);
 fn_SetValue(ds, cf_if_lq, cf_lq_dict);

 CFRelease(cf_if_lq); CFRelease(cf_lq_dict); CFRelease(cf_lq);

 /* AirPort & Link state for status bar and UI indicators */
 CFMutableDictionaryRef cf_ap = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
 CFDictionarySetValue(cf_ap, CFSTR("Power Status"), kCFBooleanTrue);
 CFDictionarySetValue(cf_ap, CFSTR("Active"), kCFBooleanTrue);
 CFDictionarySetValue(cf_ap, CFSTR("SSID_STR"), CFSTR("Wi-Fi"));
 CFDictionarySetValue(cf_ap, CFSTR("OP_MODE"), CFSTR("Infrastructure"));
 int ap_chan = 11;
 CFNumberRef cf_chan = CFNumberCreate(NULL, kCFNumberIntType, &ap_chan);
 CFDictionarySetValue(cf_ap, CFSTR("CHANNEL"), cf_chan);
 CFRelease(cf_chan);
 int ap_rssi = -50;
 CFNumberRef cf_rssi = CFNumberCreate(NULL, kCFNumberIntType, &ap_rssi);
 CFDictionarySetValue(cf_ap, CFSTR("RSSI"), cf_rssi);
 CFRelease(cf_rssi);
 int ap_bars = 3;
 CFNumberRef cf_bars = CFNumberCreate(NULL, kCFNumberIntType, &ap_bars);
 CFDictionarySetValue(cf_ap, CFSTR("Bars"), cf_bars);
 CFRelease(cf_bars);

 CFMutableDictionaryRef cf_link = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
 CFDictionarySetValue(cf_link, CFSTR("Active"), kCFBooleanTrue);

 CFStringRef cf_if_ap = CFStringCreateWithFormat(NULL, NULL, CFSTR("State:/Network/Interface/%s/AirPort"), name);
 CFStringRef cf_if_link = CFStringCreateWithFormat(NULL, NULL, CFSTR("State:/Network/Interface/%s/Link"), name);
 fn_SetValue(ds, cf_if_ap, cf_ap);
 fn_SetValue(ds, cf_if_link, cf_link);
 fn_SetValue(ds, CFSTR("State:/Network/Interface/en0/AirPort"), cf_ap);
 fn_SetValue(ds, CFSTR("State:/Network/Interface/en0/Link"), cf_link);
 CFRelease(cf_if_ap); CFRelease(cf_if_link);
 CFRelease(cf_ap); CFRelease(cf_link);

 (void)notify_post("com.apple.system.config.network_change");
 (void)notify_post("com.apple.wifi.link-changed");
 (void)notify_post("com.apple.wifi.power-changed");

 CFRelease(cf_svc_dns); CFRelease(cf_svc_ipv4); CFRelease(cf_sipv4_dict);
 CFRelease(cf_masks_arr); CFRelease(cf_addrs_arr);
 CFRelease(cf_gipv4_dict); CFRelease(cf_dns_dict); CFRelease(cf_dns_arr);
 CFRelease(cf_dns); CFRelease(cf_gw); CFRelease(cf_mask); CFRelease(cf_ip); CFRelease(cf_ifname);
 CFRelease(ds); dlclose(h);
 puts("SCDynamicStore system DNS and primary IPv4 service published");
 return 1;
}

static int open_bpf_attached(const char *name, unsigned *buflen) {
 int fd = -1;
 for(unsigned k=0;k<256;k++){
  char path[32];snprintf(path,sizeof(path),"/dev/bpf%u",k);
  fd=open(path,O_RDWR);if(fd>=0)break;
  if(errno!=EBUSY&&errno!=EACCES)break;
 }
 if(fd<0)return -1;
 struct ifreq r={0};snprintf(r.ifr_name,sizeof(r.ifr_name),"%s",name);
 if(ioctl(fd,BIOCSETIF,&r)){close(fd);return -1;}
 unsigned one=1,len=0;
 if(ioctl(fd,BIOCIMMEDIATE,&one)||ioctl(fd,BIOCSHDRCMPLT,&one)||ioctl(fd,BIOCGBLEN,&len)){
  close(fd);return -1;
 }
 if(buflen)*buflen=len;
 return fd;
}

static void get_mac(const char *name) {
 struct ifaddrs*head=NULL;if(getifaddrs(&head))fail("getifaddrs");int found=0;
 for(struct ifaddrs*p=head;p;p=p->ifa_next)if(p->ifa_addr&&p->ifa_addr->sa_family==AF_LINK&&!strcmp(p->ifa_name,name)){
  struct sockaddr_dl*s=(void*)p->ifa_addr;
  if(s->sdl_alen==6){memcpy(mac,LLADDR(s),6);found=1;}
 }
 freeifaddrs(head);
 if(!found){fprintf(stderr,"interface/MAC unavailable\n");exit(1);}
}

static int get_interface_ip(const char *name) {
 struct ifaddrs *head = NULL;
 if (getifaddrs(&head)) return 0;
 int found = 0;
 for (struct ifaddrs *p = head; p; p = p->ifa_next) {
  if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET && !strcmp(p->ifa_name, name)) {
   struct sockaddr_in *sin = (void*)p->ifa_addr;
   memcpy(offered, &sin->sin_addr, 4);
   if (p->ifa_netmask) {
    struct sockaddr_in *nm = (void*)p->ifa_netmask;
    memcpy(mask, &nm->sin_addr, 4);
   }
   found = 1;
   break;
  }
 }
 freeifaddrs(head);
 return found;
}

static int dhcp_renew_attempt(const char *name) {
 get_mac(name);
 if (!get_interface_ip(name)) {
  fprintf(stderr, "No configured IPv4 address on %s to renew\n", name);
  return 0;
 }
 printf("Renewing lease for %u.%u.%u.%u on %s...\n",
        offered[0], offered[1], offered[2], offered[3], name);
 unsigned len=0;int fd=open_bpf_attached(name,&len);
 if(fd<0){perror("BPF attach for renew");return 0;}
 arc4random_buf(xid,4);
 int ok=0;
 for(unsigned retry=0;retry<3&&!ok;retry++){
  send_dhcp_renew(fd);
  ok=wait_dhcp(fd,len,5);
 }
 close(fd);
 if(ok>0){
  printf("DHCP renewal accepted: %u.%u.%u.%u, lease=%u s, T1=%u s\n",
         offered[0],offered[1],offered[2],offered[3],lease_time,t1_time);
  sc_config(name,offered,mask,gateway,dns);
  return 1;
 }
 fprintf(stderr,"DHCP renewal request timed out\n");
 return 0;
}

static void dhcp(const char*name,int adddefault){
 get_mac(name);
 arc4random_buf(xid,4);
 unsigned len=0;int fd=open_bpf_attached(name,&len);
 if(fd<0)fail("open/attach BPF");
 for(unsigned phase=0;phase<2;phase++){
  int ok=0;
  for(unsigned retry=0;retry<3&&!ok;retry++){
   send_dhcp(fd,phase?3:1);
   ok=wait_dhcp(fd,len,phase?5:2);
   if(ok<0)exit(1);
  }
  if(!ok){fprintf(stderr,"DHCP %s timeout\n",phase?"ACK":"OFFER");exit(1);}
 }
 close(fd);
 if(!memcmp(mask,"\0\0\0\0",4)){fprintf(stderr,"no DHCP subnet mask\n");exit(1);}
 addr_config(name);
 showip("address",offered);showip("mask",mask);showip("gateway",gateway);showip("DNS",dns);
 printf("lease=%u s (renewal T1=%u s)\n", lease_time, t1_time);
 if(adddefault){
  if(!memcmp(gateway,"\0\0\0\0",4)){fprintf(stderr,"no DHCP gateway\n");exit(1);}
  default_route(name);
  puts("default route installed");
 }
 sc_config(name,offered,mask,gateway,dns);
 puts("DHCP ACK accepted and address configured");
}

static void dhcp_daemon_loop(const char *name, int adddefault) {
 FILE *log = freopen("/tmp/river-net.log", "a", stdout);
 if (log) setvbuf(stdout, NULL, _IOLBF, 0);
 (void)freopen("/tmp/river-net.log", "a", stderr);
 (void)daemon(1, 1);
 int nullfd = open("/dev/null", O_RDONLY);
 if (nullfd >= 0) { dup2(nullfd, 0); close(nullfd); }
 dhcp(name, adddefault);
 puts("Entering DHCP lease renewal daemon loop");
 fflush(stdout);
 while(1) {
  uint32_t wait_sec = t1_time;
  if (wait_sec > 60) wait_sec -= 30; /* renew slightly before T1 */
  printf("DHCP renewal sleeping for %u seconds...\n", wait_sec);
  fflush(stdout);
  sleep(wait_sec);
  printf("DHCP lease renewal timer fired; renewing...\n");
  fflush(stdout);
  if (!dhcp_renew_attempt(name)) {
   fprintf(stderr, "Renewal failed; will retry in 30 seconds\n");
   sleep(30);
   if (!dhcp_renew_attempt(name)) {
    fprintf(stderr, "Secondary renewal failed; performing full DHCP re-bind\n");
    dhcp(name, adddefault);
   }
  }
  fflush(stdout);
 }
}

static void tcp_probe(const char*name,const char*address,unsigned port){
 unsigned index=if_nametoindex(name);if(!index){fprintf(stderr,"interface unavailable\n");exit(1);}
 int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)fail("TCP socket");
 if(setsockopt(fd,IPPROTO_IP,IP_BOUND_IF,&index,sizeof(index)))fail("bind interface");
 int flags=fcntl(fd,F_GETFL,0);fcntl(fd,F_SETFL,flags|O_NONBLOCK);
 struct sockaddr_in a={0};a.sin_len=sizeof(a);a.sin_family=AF_INET;a.sin_port=htons(port);
 if(inet_pton(AF_INET,address,&a.sin_addr)!=1){fprintf(stderr,"invalid IPv4 address\n");exit(2);}
 if(connect(fd,(void*)&a,sizeof(a))&&errno!=EINPROGRESS)fail("TCP connect");
 struct pollfd p={fd,POLLOUT,0};if(poll(&p,1,5000)<=0){fprintf(stderr,"TCP connect timeout\n");exit(1);}
 int err=0;socklen_t z=sizeof(err);getsockopt(fd,SOL_SOCKET,SO_ERROR,&err,&z);if(err){errno=err;fail("TCP connection result");}
 fcntl(fd,F_SETFL,flags);struct timeval timeout={5,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
 printf("TCP connected via %s to %s:%u\n",name,address,port);
 if(port==80){char request[256];int n=snprintf(request,sizeof(request),"GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",address);if(send(fd,request,n,0)!=n)fail("HTTP request");char reply[1024];ssize_t got=recv(fd,reply,sizeof(reply)-1,0);if(got<=0)fail("HTTP read");reply[got]=0;char*end=strstr(reply,"\r\n");if(end)*end=0;printf("HTTP response: %.200s\n",reply);}
 close(fd);
}

int main(int argc,char**argv){
 if(argc==4&&!strcmp(argv[1],"route-check")){uint8_t ip[4];if(inet_pton(AF_INET,argv[3],ip)!=1)return 2;if(!route_check(argv[2],ip)){fprintf(stderr,"matching default route absent\n");return 1;}puts("matching default route verified");return 0;}
 if(argc==5&&!strcmp(argv[1],"tcp")){unsigned p=(unsigned)strtoul(argv[4],NULL,10);if(!p||p>65535)return 2;tcp_probe(argv[2],argv[3],p);return 0;}
 if(argc>=3&&!strcmp(argv[1],"dhcp")){dhcp(argv[2],argc==4&&!strcmp(argv[3],"--default"));return 0;}
 if(argc>=3&&!strcmp(argv[1],"daemon")){dhcp_daemon_loop(argv[2],argc==4&&!strcmp(argv[3],"--default"));return 0;}
 if(argc==3&&!strcmp(argv[1],"renew")){return dhcp_renew_attempt(argv[2])?0:1;}
 if(argc>=5&&!strcmp(argv[1],"sc-setup")){
  uint8_t ip[4]={192,168,1,232},m[4]={255,255,255,0},gw[4],dn[4];
  if(inet_pton(AF_INET,argv[3],gw)!=1||inet_pton(AF_INET,argv[4],dn)!=1)return 2;
  if(argc>=7&&(inet_pton(AF_INET,argv[5],ip)!=1||inet_pton(AF_INET,argv[6],m)!=1))return 2;
  return sc_config(argv[2],ip,m,gw,dn)?0:1;
 }
 fprintf(stderr,"usage: river-net dhcp IFACE [--default] | daemon IFACE [--default] | renew IFACE | tcp IFACE IPv4 PORT | route-check IFACE GATEWAY | sc-setup IFACE GATEWAY DNS [IP MASK]\n");return 2;
}
