/* Temporary on-phone DHCP/IPv4 bring-up helper; no credentials or persistence.
 * A full lease client/rekey lifecycle is outside this bounded diagnostic. */
#include <arpa/inet.h>
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
#include <sys/sockio.h>
#include <time.h>
_Static_assert(sizeof(struct ifaliasreq)==64,"ifaliasreq ABI");
_Static_assert(sizeof(struct rt_msghdr)==92,"route ABI");
_Static_assert(sizeof(struct bpf_hdr)==20,"BPF ABI");
#include <unistd.h>
static uint8_t mac[6],xid[4],offered[4],server[4],mask[4],gateway[4],dns[4];
static uint16_t be16(const uint8_t*p){return ((unsigned)p[0]<<8)|p[1];}
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
 for(unsigned k=240;k<bn;){unsigned id=b[k++];if(!id)continue;if(id==255)break;if(k>=bn)return 0;unsigned z=b[k++];if(z>bn-k)return 0;
  if(id==53&&z==1)type=b[k];if(id==54&&z==4)memcpy(si,b+k,4);if(id==1&&z==4)memcpy(ma,b+k,4);if(id==3&&z>=4)memcpy(gw,b+k,4);if(id==6&&z>=4)memcpy(dn,b+k,4);k+=z;}
 if(type==6){fprintf(stderr,"DHCP NAK\n");return -1;}
 if(type!=expected||!memcmp(si,"\0\0\0\0",4))return 0;
 if(expected==5&&(memcmp(si,server,4)||memcmp(b+16,offered,4)))return 0;
 memcpy(offered,b+16,4);memcpy(server,si,4);memcpy(mask,ma,4);memcpy(gateway,gw,4);memcpy(dns,dn,4);
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
static void default_route(const char*name){
 struct{struct rt_msghdr h;struct sockaddr_in dst,gw,mask;}r={0};
 r.h.rtm_msglen=sizeof(r);r.h.rtm_version=RTM_VERSION;r.h.rtm_type=RTM_ADD;r.h.rtm_index=if_nametoindex(name);r.h.rtm_flags=RTF_UP|RTF_GATEWAY|RTF_STATIC;r.h.rtm_addrs=RTA_DST|RTA_GATEWAY|RTA_NETMASK;r.h.rtm_seq=1;r.h.rtm_pid=getpid();
 r.dst.sin_len=r.gw.sin_len=r.mask.sin_len=sizeof(r.dst);r.dst.sin_family=r.gw.sin_family=r.mask.sin_family=AF_INET;memcpy(&r.gw.sin_addr,gateway,4);
 int fd=socket(PF_ROUTE,SOCK_RAW,AF_INET);if(fd<0)fail("route socket");if(write(fd,&r,sizeof(r))!=sizeof(r))fail("default route write");
 uint8_t buf[4096];for(unsigned w=0;w<10;w++){struct pollfd p={fd,POLLIN,0};if(poll(&p,1,500)<=0)continue;ssize_t n=read(fd,buf,sizeof(buf));if(n<(ssize_t)sizeof(struct rt_msghdr))continue;struct rt_msghdr*h=(void*)buf;if(h->rtm_seq==1&&h->rtm_pid==getpid()){if(h->rtm_errno){errno=h->rtm_errno;fail("default route result");}close(fd);return;}}
 close(fd);fprintf(stderr,"route acknowledgement not seen\n");exit(1);
}
static void dhcp(const char*name,int adddefault){
 struct ifaddrs*head=NULL;if(getifaddrs(&head))fail("getifaddrs");int found=0;
 for(struct ifaddrs*p=head;p;p=p->ifa_next)if(p->ifa_addr&&p->ifa_addr->sa_family==AF_LINK&&!strcmp(p->ifa_name,name)){struct sockaddr_dl*s=(void*)p->ifa_addr;if(s->sdl_alen==6){memcpy(mac,LLADDR(s),6);found=1;}}
 freeifaddrs(head);if(!found){fprintf(stderr,"interface/MAC unavailable\n");exit(1);}arc4random_buf(xid,4);
 int fd=-1;for(unsigned k=0;k<256;k++){char path[32];snprintf(path,sizeof(path),"/dev/bpf%u",k);fd=open(path,O_RDWR);if(fd>=0)break;if(errno!=EBUSY&&errno!=EACCES)break;}if(fd<0)fail("open BPF");
 struct ifreq r={0};snprintf(r.ifr_name,sizeof(r.ifr_name),"%s",name);if(ioctl(fd,BIOCSETIF,&r))fail("BPF attach");unsigned one=1,len;if(ioctl(fd,BIOCIMMEDIATE,&one)||ioctl(fd,BIOCSHDRCMPLT,&one)||ioctl(fd,BIOCGBLEN,&len))fail("BPF configuration");
 for(unsigned phase=0;phase<2;phase++){int ok=0;for(unsigned retry=0;retry<3&&!ok;retry++){send_dhcp(fd,phase?3:1);ok=wait_dhcp(fd,len,phase?5:2);if(ok<0)exit(1);}if(!ok){fprintf(stderr,"DHCP %s timeout\n",phase?"ACK":"OFFER");exit(1);}}
 close(fd);if(!memcmp(mask,"\0\0\0\0",4)){fprintf(stderr,"no DHCP subnet mask\n");exit(1);}addr_config(name);showip("address",offered);showip("mask",mask);showip("gateway",gateway);showip("DNS",dns);
 if(adddefault){if(!memcmp(gateway,"\0\0\0\0",4)){fprintf(stderr,"no DHCP gateway\n");exit(1);}default_route(name);puts("default route installed");}
 puts("DHCP ACK accepted and address configured (diagnostic lease; no renewal daemon)");
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
 if(argc==5&&!strcmp(argv[1],"tcp")){unsigned p=(unsigned)strtoul(argv[4],NULL,10);if(!p||p>65535)return 2;tcp_probe(argv[2],argv[3],p);return 0;}
 if(argc>=3&&!strcmp(argv[1],"dhcp")){dhcp(argv[2],argc==4&&!strcmp(argv[3],"--default"));return 0;}
 fprintf(stderr,"usage: river-net dhcp IFACE [--default] | tcp IFACE IPv4 PORT\n");return 2;
}
