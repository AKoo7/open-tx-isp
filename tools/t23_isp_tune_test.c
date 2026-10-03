/*
 * t23_isp_tune_test - exercise T23 tuning controls through /dev/isp-m0.
 *
 * Sends the 16-byte extended control {direction, id, value, sensor 0} with ioctl
 * 0xc01056c6 (direction 0 = set, 1 = get).  For pointer-valued controls
 * the value field carries the address of a local buffer.
 *
 *   t23_isp_tune_test set    ID VALUE          scalar set
 *   t23_isp_tune_test get    ID                scalar get, prints value
 *   t23_isp_tune_test setptr ID W0[,W1,...]    set, buffer of 32-bit words
 *   t23_isp_tune_test setb   ID B0[,B1,...]    set, buffer of bytes
 *   t23_isp_tune_test getptr ID SIZE           get into SIZE-byte buffer, hexdump
 *   t23_isp_tune_test expr                     decode GetExpr (0x8000025)
 *   t23_isp_tune_test vset ID VALUE / vget ID  V4L2 S_CTRL/G_CTRL (0xc008561c/b)
 *
 * Numbers accept 0x.. hex.  Exit status 0 when the ioctl returned 0.
 * Build: mipsel-linux-gcc -static -O2 -o t23_isp_tune_test t23_isp_tune_test.c
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define TISP_VIDIOC_TUNING 0xc01056c6UL
#define MAX_BUF 2048

struct t23_ext_control {
	uint32_t direction;
	uint32_t id;
	uint32_t value;
	uint32_t sensor;
};

static int isp_fd = -1;

static uint32_t num(const char *s)
{
	char *end;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 0);
	if (errno || *end != '\0') {
		fprintf(stderr, "invalid number: %s\n", s);
		exit(2);
	}
	return (uint32_t)v;
}

static int xfer(uint32_t dir, uint32_t id, uint32_t *value)
{
	struct t23_ext_control c = { dir, id, *value, 0 };
	int ret = ioctl(isp_fd, TISP_VIDIOC_TUNING, &c);

	if (ret < 0) {
		fprintf(stderr, "ioctl %s 0x%08x: ret=%d errno=%d (%s)\n",
			dir ? "get" : "set", id, ret, errno, strerror(errno));
		return 1;
	}
	*value = c.value;
	return 0;
}

static void hexdump(const uint8_t *b, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		printf("%02x%s", b[i], (i % 16 == 15 || i + 1 == n) ? "\n" : " ");
}

static unsigned int parse_list(const char *s, uint8_t *buf, int word)
{
	char tmp[1024];
	char *tok, *save = NULL;
	unsigned int n = 0;

	snprintf(tmp, sizeof(tmp), "%s", s);
	for (tok = strtok_r(tmp, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		uint32_t v = num(tok);

		if (word) {
			if (n + 4 > MAX_BUF)
				break;
			memcpy(buf + n, &v, 4);
			n += 4;
		} else {
			if (n + 1 > MAX_BUF)
				break;
			buf[n++] = (uint8_t)v;
		}
	}
	return n;
}

int main(int argc, char **argv)
{
	static uint8_t buf[MAX_BUF];
	uint32_t v;
	int ret;

	if (argc < 2) {
		fprintf(stderr,
			"usage: %s set ID VALUE | get ID | setptr ID W0,W1.. |\n"
			"          setb ID B0,B1.. | getptr ID SIZE | expr\n", argv[0]);
		return 2;
	}

	isp_fd = open("/dev/isp-m0", O_RDWR);
	if (isp_fd < 0) {
		perror("open /dev/isp-m0");
		return 1;
	}

	if (!strcmp(argv[1], "set") && argc == 4) {
		v = num(argv[3]);
		ret = xfer(0, num(argv[2]), &v);
	} else if (!strcmp(argv[1], "get") && argc == 3) {
		v = 0;
		ret = xfer(1, num(argv[2]), &v);
		if (!ret)
			printf("%u (0x%x)\n", v, v);
	} else if ((!strcmp(argv[1], "setptr") || !strcmp(argv[1], "setb")) && argc == 4) {
		unsigned int n = parse_list(argv[3], buf, argv[1][3] == 'p');

		printf("sending %u bytes\n", n);
		v = (uint32_t)(uintptr_t)buf;
		ret = xfer(0, num(argv[2]), &v);
	} else if (!strcmp(argv[1], "getptr") && argc == 4) {
		unsigned int n = num(argv[3]);

		if (n > MAX_BUF)
			n = MAX_BUF;
		memset(buf, 0xa5, sizeof(buf));	/* shows bytes the driver did not write */
		v = (uint32_t)(uintptr_t)buf;
		ret = xfer(1, num(argv[2]), &v);
		if (!ret)
			hexdump(buf, n);
	} else if ((!strcmp(argv[1], "vset") && argc == 4) ||
		   (!strcmp(argv[1], "vget") && argc == 3)) {
		struct { uint32_t id; int32_t value; } c;
		int set = argv[1][1] == 's';

		c.id = num(argv[2]);
		c.value = set ? (int32_t)num(argv[3]) : 0;
		ret = ioctl(isp_fd, set ? 0xc008561cUL : 0xc008561bUL, &c);
		if (ret < 0)
			fprintf(stderr, "v%s 0x%08x: errno=%d (%s)\n", set ? "set" : "get",
				c.id, errno, strerror(errno));
		else if (!set)
			printf("%d (0x%x)\n", c.value, c.value);
		ret = ret < 0;
	} else if (!strcmp(argv[1], "expr") && argc == 2) {
		struct {
			uint32_t mode;
			uint16_t it, it_min, it_max, line_us;
		} e;

		memset(&e, 0, sizeof(e));
		v = (uint32_t)(uintptr_t)&e;
		ret = xfer(1, 0x8000025, &v);
		if (!ret)
			printf("mode=%u it=%u it_min=%u it_max=%u line_us=%u\n",
			       e.mode, e.it, e.it_min, e.it_max, e.line_us);
	} else {
		fprintf(stderr, "bad arguments\n");
		ret = 2;
	}

	close(isp_fd);
	return ret;
}
