#include <sys/types.h>
#include <sys/sysctl.h>
#include <stdio.h>
int main(void) {
 char data[512]; size_t n=sizeof(data);
 if (sysctlbyname("debug.river_flash_survey",data,&n,0,0)) { perror("debug.river_flash_survey"); return 1; }
 printf("%s\n",data); return 0;
}
