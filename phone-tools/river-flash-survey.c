#include <sys/types.h>
#include <sys/sysctl.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
 const char *name = "debug.river_flash_survey";
 if (argc == 2 && strcmp(argv[1], "--live") == 0) name = "debug.river_power_sample";
 else if (argc == 2 && strcmp(argv[1], "--charge") == 0) name = "debug.river_charge_sample";
 else if (argc == 2 && strcmp(argv[1], "--timing") == 0) name = "debug.river_charge_timing";
 else if (argc != 1) { fprintf(stderr, "usage: river-flash-survey [--live|--charge|--timing]\n"); return 2; }
 char data[512]; size_t n=sizeof(data);
 if (sysctlbyname(name,data,&n,0,0)) { perror(name); return 1; }
 printf("%s\n",data); return 0;
}
