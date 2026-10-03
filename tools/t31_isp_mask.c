/*
 * T31 ISP privacy mask test tool (TISP CID 0x80000e5, the call behind
 * IMP_ISP_Tuning_SetMask/GetMask).  Talks to /dev/isp-m0 like libimp:
 * ioctl 0xc00c56c6 with {direction (0 set, 1 get), cid, user pointer}.
 *
 *   t31_isp_mask get
 *   t31_isp_mask set CH BLK LEFT TOP WIDTH HEIGHT Y U V
 *   t31_isp_mask off CH BLK
 *   t31_isp_mask clear
 *
 * CH 0..2 is the MSCA channel (0 main, 1 sub), BLK 0..3 the block,
 * coordinates are pixels of that channel's output, colour is YUV
 * (mask_type is set to 1 = YUV, so libimp's RGB->YUV step does not apply).
 * "set"/"off" read the current attribute first and change one block.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define TISP_VIDIOC_TUNING 0xc00c56c6UL
#define TISP_CID_MASK 0x80000e5U

struct t31_tuning_control {
	uint32_t direction;
	uint32_t id;
	uint32_t value;
};

struct t31_mask_block {
	uint8_t en;
	uint8_t pad0;
	uint16_t top;
	uint16_t left;
	uint16_t width;
	uint16_t height;
	uint8_t yuv[3];
	uint8_t pad1;
} __attribute__((packed));

struct t31_mask_attr {
	struct t31_mask_block blk[3][4];
	int32_t mask_type;
} __attribute__((packed));

_Static_assert(sizeof(struct t31_mask_block) == 14, "block ABI");
_Static_assert(sizeof(struct t31_mask_attr) == 0xac, "attr ABI");

static int xfer(int fd, int get, struct t31_mask_attr *a)
{
	struct t31_tuning_control c = {
		.direction = get ? 1 : 0,
		.id = TISP_CID_MASK,
		.value = (uint32_t)(uintptr_t)a,
	};

	if (ioctl(fd, TISP_VIDIOC_TUNING, &c) < 0) {
		perror(get ? "get mask" : "set mask");
		return -1;
	}
	return 0;
}

static unsigned long num(const char *s)
{
	char *end;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 0);
	if (errno || *end) {
		fprintf(stderr, "bad number: %s\n", s);
		exit(2);
	}
	return v;
}

static void dump(const struct t31_mask_attr *a)
{
	int c, b;

	for (c = 0; c < 3; c++)
		for (b = 0; b < 4; b++) {
			const struct t31_mask_block *m = &a->blk[c][b];

			printf("ch%d blk%d en=%u left=%u top=%u w=%u h=%u yuv=%02x/%02x/%02x\n",
			       c, b, m->en, m->left, m->top, m->width, m->height,
			       m->yuv[0], m->yuv[1], m->yuv[2]);
		}
	printf("mask_type=%d\n", a->mask_type);
}

int main(int argc, char **argv)
{
	struct t31_mask_attr a;
	int fd, ret = 0;

	if (argc < 2)
		goto usage;
	fd = open("/dev/isp-m0", O_RDWR);
	if (fd < 0) {
		perror("open /dev/isp-m0");
		return 1;
	}
	memset(&a, 0, sizeof(a));

	if (!strcmp(argv[1], "get") && argc == 2) {
		ret = xfer(fd, 1, &a);
		if (!ret)
			dump(&a);
	} else if (!strcmp(argv[1], "clear") && argc == 2) {
		a.mask_type = 1;
		ret = xfer(fd, 0, &a);
	} else if ((!strcmp(argv[1], "set") && argc == 11) ||
		   (!strcmp(argv[1], "off") && argc == 4)) {
		unsigned long c = num(argv[2]), b = num(argv[3]);
		struct t31_mask_block *m;

		if (c > 2 || b > 3)
			goto usage;
		ret = xfer(fd, 1, &a);
		if (ret)
			goto out;
		m = &a.blk[c][b];
		if (argv[1][0] == 's') {
			m->en = 1;
			m->left = num(argv[4]);
			m->top = num(argv[5]);
			m->width = num(argv[6]);
			m->height = num(argv[7]);
			m->yuv[0] = num(argv[8]);
			m->yuv[1] = num(argv[9]);
			m->yuv[2] = num(argv[10]);
		} else {
			m->en = 0;
		}
		a.mask_type = 1;
		ret = xfer(fd, 0, &a);
	} else {
		close(fd);
		goto usage;
	}
out:
	close(fd);
	return ret ? 1 : 0;
usage:
	fprintf(stderr,
		"usage: %s get | clear | off CH BLK |\n"
		"       set CH BLK LEFT TOP WIDTH HEIGHT Y U V\n", argv[0]);
	return 2;
}
