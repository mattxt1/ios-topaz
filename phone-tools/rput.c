/*
 * rput PATH MODE: write stdin to PATH (through PATH.rput, then rename) and
 * chmod it to octal MODE. River's iOS has no cat or chmod, so files go in as
 *   tools/river-ssh 'rput /usr/local/bin/x 755' < x
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: rput PATH MODE < data\n");
		return 2;
	}
	char tmp[1024];
	snprintf(tmp, sizeof(tmp), "%s.rput", argv[1]);
	mode_t mode = (mode_t)strtoul(argv[2], NULL, 8);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		perror(tmp);
		return 1;
	}
	char buf[65536];
	ssize_t n;
	long long total = 0;
	while ((n = read(0, buf, sizeof(buf))) > 0) {
		for (ssize_t off = 0; off < n;) {
			ssize_t w = write(fd, buf + off, (size_t)(n - off));
			if (w < 0) {
				perror("write");
				return 1;
			}
			off += w;
		}
		total += n;
	}
	if (n < 0 || fsync(fd) != 0 || fchmod(fd, mode) != 0 || close(fd) != 0 || rename(tmp, argv[1]) != 0) {
		perror(argv[1]);
		unlink(tmp);
		return 1;
	}
	printf("%s: %lld bytes, mode %o\n", argv[1], total, mode);
	return 0;
}
