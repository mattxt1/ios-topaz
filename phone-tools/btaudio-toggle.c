#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    const char *active = "/System/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver";
    const char *disabled = "/System/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver.river-disabled";
    const char *src, *dst;
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "disable")) { src = active; dst = disabled; }
    else if (!strcmp(argv[1], "restore")) { src = disabled; dst = active; }
    else return 2;
    if (access(src, F_OK) || !access(dst, F_OK)) {
        fprintf(stderr, "source absent or destination occupied\n");
        return 1;
    }
    if (rename(src, dst)) {
        fprintf(stderr, "rename: %s\n", strerror(errno));
        return 1;
    }
    printf("%s -> %s\n", src, dst);
    return 0;
}
