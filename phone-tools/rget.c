/* rget PATH OFFSET LENGTH: bounded read-only file/partition copy to stdout. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc != 4) {
		fprintf(stderr, "usage: rget PATH OFFSET LENGTH\n");
		return 2;
	}
	char *end;
	errno = 0;
	unsigned long long off = strtoull(argv[2], &end, 0);
	if (errno || *end) return 2;
	errno = 0;
	unsigned long long length = strtoull(argv[3], &end, 0);
	if (errno || *end || length > 1024ull * 1024 * 1024) return 2;
	if (strncmp(argv[1], "/dev/remmc", 10) == 0 && ((off | length) & 511)) {
		fprintf(stderr, "raw eMMC reads require 512-byte aligned offset and length\n");
		return 2;
	}
	int fd = open(argv[1], O_RDONLY);
	if (fd < 0) { perror(argv[1]); return 1; }
	char buf[65536];
	while (length) {
		size_t want = length < sizeof(buf) ? (size_t)length : sizeof(buf);
		ssize_t n = pread(fd, buf, want, (off_t)off);
		if (n <= 0) { perror("pread"); close(fd); return 1; }
		for (ssize_t done = 0; done < n;) {
			ssize_t w = write(1, buf + done, (size_t)(n - done));
			if (w <= 0) { perror("write"); close(fd); return 1; }
			done += w;
		}
		off += (unsigned long long)n;
		length -= (unsigned long long)n;
	}
	close(fd);
	return 0;
}
