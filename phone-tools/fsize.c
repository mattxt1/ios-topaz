#include <stdio.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) == 0) {
            printf("%lld %s\n", (long long)st.st_size, argv[i]);
        } else {
            perror(argv[i]);
        }
    }
    return 0;
}
