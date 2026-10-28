#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>

int main(int argc, char **argv) {
    int ms = 50;
    if (argc > 1) {
        ms = atoi(argv[1]);
    }
    int ret = sysctlbyname("debug.river_vibrate", NULL, NULL, &ms, sizeof(ms));
    if (ret != 0) {
        perror("sysctlbyname debug.river_vibrate");
        return 1;
    }
    printf("Triggered River haptic vibration: %d ms\n", ms);
    return 0;
}
