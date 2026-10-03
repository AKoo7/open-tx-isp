#!/bin/bash
# Differential check of the T31 privacy mask (CID 0x80000e5) against the
# stock tx-isp-t31.ko: the stock functions run in a small MIPS interpreter
# (tools/t31_mask_stock_emu.py over objdump -dr), the open code is cut out of
# driver/t31 and built for the host.  Register writes, getter and programmed
# copy must match for every generated sequence.
#   STOCK_KO=.../ingenic-sdk-*/tx-isp-t31.ko OBJDUMP=mipsel-linux-objdump \
#   tools/check_t31_mask_oracle.sh [SEEDS]
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
: "${STOCK_KO:?set STOCK_KO to the stock tx-isp-t31.ko}"
OBJDUMP="${OBJDUMP:-mipsel-linux-objdump}"
SEEDS="${1:-400}"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
T="$HERE/driver/t31/tx_isp_tuning.c"; C="$HERE/driver/t31/tx_isp_core.c"

"$OBJDUMP" -dr --start-address=0x62300 --stop-address=0x62350 "$STOCK_KO" > "$W/stock.s"
"$OBJDUMP" -dr --start-address=0x64d64 --stop-address=0x65ae0 "$STOCK_KO" >> "$W/stock.s"
grep -q "<tisp_s_mscaler_mask_attr>:" "$W/stock.s" || { echo "unexpected stock layout"; exit 1; }

cut_fn() { # file start-regex end-regex
	awk -v s="$2" -v e="$3" '$0 ~ s {p=1} p {print} p && $0 ~ e {exit}' "$1"
}
{
cat <<'H'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int32_t s32;
#define GFP_KERNEL 0
#define kmalloc(n, f) malloc(n)
#define kfree(p) free(p)
static uint8_t ds0_attr[0x34], ds1_attr[0x34], ds2_attr[0x34];
uint32_t msca_ch_en;
static uint32_t mmio[0x10000];
void system_reg_write(u32 reg, u32 v) { printf("W %x %x\n", reg, v); mmio[reg / 4] = v; }
uint32_t system_reg_read(u32 reg) { return mmio[reg / 4]; }
static uint8_t *mscaler_mask_saved, *mscaler_mask_active;
H
cut_fn "$C" '^static const uint8_t [*]tisp_channel_attr_store[(]int channel_id[)]$' '^}'
cut_fn "$C" '^static u32 tisp_channel_attr_word[(].*[)]$' '^}'
cut_fn "$C" '^void tisp_mscaler_mask_frame[(].*[)]$' '^}'
cut_fn "$T" '^int system_yvu_or_yuv[(]int arg1, int arg2, int arg3[)]$' '^}'
awk '/^#define MSCA_MASK_ATTR_SIZE/ {p=1} p {print} p && /^int tisp_g_mscaler_mask_attr/ {g=1} g && /^}/ {exit}' "$T"
cat <<'H'
int main(void)
{
	char cmd[16];
	while (scanf("%15s", cmd) == 1) {
		unsigned r, v, w[13]; int c, i; uint8_t b[0xac];
		if (!strcmp(cmd, "rd")) { if (scanf("%x %x", &r, &v) == 2) mmio[r / 4] = v; }
		else if (!strcmp(cmd, "ds")) { if (scanf("%d", &c) != 1) return 1; for (i = 0; i < 13; i++) if (scanf("%x", &w[i]) != 1) return 1; memcpy(c == 0 ? ds0_attr : c == 1 ? ds1_attr : ds2_attr, w, 0x34); }
		else if (!strcmp(cmd, "chen")) { if (scanf("%x", &msca_ch_en) != 1) return 1; }
		else if (!strcmp(cmd, "set")) { for (i = 0; i < 0xac; i++) { if (scanf("%x", &v) != 1) return 1; b[i] = v; } printf("SET\n"); tisp_s_mscaler_mask_attr(b); }
		else if (!strcmp(cmd, "flip")) { if (scanf("%x", &v) != 1) return 1; printf("FLIP %x\n", v); tisp_s_mscaler_hvflip_mask(v); }
		else if (!strcmp(cmd, "get")) {
			tisp_g_mscaler_mask_attr(b); printf("GET"); for (i = 0; i < 0xac; i++) printf(" %02x", b[i]); printf("\n");
			printf("ACT"); for (i = 0; i < 0xac; i++) printf(" %02x", mscaler_mask_active ? mscaler_mask_active[i] : 0); printf("\n");
			printf("CHEN %x\n", msca_ch_en);
		}
	}
	return 0;
}
H
} > "$W/open.c"
cc -O1 -o "$W/open" "$W/open.c"

fail=0
for i in $(seq 1 "$SEEDS"); do
	python3 "$HERE/tools/t31_mask_gen_cases.py" "$i" > "$W/c.in"
	python3 "$HERE/tools/t31_mask_stock_emu.py" "$W/stock.s" < "$W/c.in" > "$W/s.out"
	"$W/open" < "$W/c.in" > "$W/o.out"
	if ! cmp -s "$W/s.out" "$W/o.out"; then
		echo "seed $i differs"; diff "$W/s.out" "$W/o.out" | head -5 || true; fail=$((fail + 1))
	fi
done
echo "t31 mask oracle: $SEEDS sequences, $fail differ"
[ "$fail" -eq 0 ]
