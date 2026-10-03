/*
 * sctl - scene mode / colour effect through the tuning node, no libimp.
 *
 *   sctl get scene|colorfx
 *   sctl set scene <0..14>
 *   sctl set colorfx <0 auto|1 bw|3 negative|9 vivid>
 *   sctl [-d node] ...        (default /dev/isp-m0)
 *
 * Works on the T23 and T31 open-tx-isp drivers (V4L2 S/G_CTRL 0xc008561c /
 * 0xc008561b on /dev/isp-m0, ids 0x9a091a scene, 0x98091f colorfx).
 * Prints the ioctl return value, errno and the value.  Exit 0 = ioctl ok.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct ctl { uint32_t id; int32_t value; };

int main(int argc, char **argv)
{
	const char *node = "/dev/isp-m0";
	struct ctl c;
	int set, fd, ret, i = 1;

	if (argc > 3 && !strcmp(argv[1], "-d")) { node = argv[2]; i = 3; }
	if (argc - i < 2) goto usage;
	set = !strcmp(argv[i], "set");
	if (!set && strcmp(argv[i], "get")) goto usage;
	if (!strcmp(argv[i + 1], "scene")) c.id = 0x009a091a;
	else if (!strcmp(argv[i + 1], "colorfx")) c.id = 0x0098091f;
	else goto usage;
	c.value = -1;
	if (set) {
		if (argc - i != 3) goto usage;
		c.value = (int32_t)strtol(argv[i + 2], NULL, 0);
	}
	fd = open(node, O_RDWR);
	if (fd < 0) { printf("open %s: errno=%d (%s)\n", node, errno, strerror(errno)); return 1; }
	errno = 0;
	ret = ioctl(fd, set ? 0xc008561cUL : 0xc008561bUL, &c);
	printf("%s %s: ret=%d errno=%d (%s) value=%d\n", argv[i], argv[i + 1],
	       ret, ret ? errno : 0, ret ? strerror(errno) : "ok", c.value);
	close(fd);
	return ret ? 1 : 0;
usage:
	fprintf(stderr, "usage: %s [-d node] get scene|colorfx | set scene|colorfx <n>\n", argv[0]);
	return 2;
}
