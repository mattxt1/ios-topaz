#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static int do_mv(const char *src, const char *dst) {
    if (rename(src, dst) != 0) {
        perror("rename");
        return 1;
    }
    printf("renamed %s -> %s\n", src, dst);
    return 0;
}

static int do_rm(const char *path) {
    if (unlink(path) != 0) {
        perror("unlink");
        return 1;
    }
    printf("removed %s\n", path);
    return 0;
}

static int do_cp(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) { perror(src); return 1; }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { perror(dst); close(in); return 1; }
    char buf[65536];
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, (size_t)n) != n) {
            perror("write");
            close(in);
            close(out);
            return 1;
        }
    }
    close(in);
    close(out);
    printf("copied %s -> %s\n", src, dst);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: fops mv|rm|cp ...\n");
        return 2;
    }
    if (strcmp(argv[1], "mv") == 0 && argc == 4) {
        return do_mv(argv[2], argv[3]);
    } else if (strcmp(argv[1], "rm") == 0 && argc == 3) {
        return do_rm(argv[2]);
    } else if (strcmp(argv[1], "cp") == 0 && argc == 4) {
        return do_cp(argv[2], argv[3]);
    }
    fprintf(stderr, "unknown or invalid arguments\n");
    return 2;
}
