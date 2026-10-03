/* Host test: defaults of the T23/T31 scene/colorfx helpers are identity.
 * gcc -I driver/common -o /tmp/t tests/scene_colorfx_host_test.c && /tmp/t */
#include <stdint.h>
#include <stdio.h>
#include "tx_isp_scene_colorfx.h"

int main(void)
{
	uint32_t lo, hi, s;
	int bad = 0;

	for (s = 0; s < 256; s++)
		bad += tx_isp_colorfx_sat(TX_ISP_COLORFX_AUTO, s) != s;
	for (lo = 0; lo < 0x1000; lo += 7)
		for (hi = 0; hi < 0x1000; hi += 13)
			bad += tx_isp_colorfx_gamma_word(lo, hi, TX_ISP_COLORFX_AUTO) !=
			       ((hi << 12) | lo);
	bad += tx_isp_colorfx_sat(TX_ISP_COLORFX_BW, 200) != 0;
	bad += tx_isp_colorfx_sat(TX_ISP_COLORFX_VIVID, 128) != 128 + TX_ISP_COLORFX_VIVID_STEP;
	bad += tx_isp_colorfx_sat(TX_ISP_COLORFX_VIVID, 250) != 255;
	bad += tx_isp_colorfx_gamma_word(0, 0xfff, TX_ISP_COLORFX_NEGATIVE) != 0xfff;
	bad += !tx_isp_scene_valid(14) || tx_isp_scene_valid(15);
	bad += tx_isp_colorfx_valid(2) || tx_isp_colorfx_valid(4) || !tx_isp_colorfx_valid(9);
	printf("%s (%d)\n", bad ? "FAIL" : "ok", bad);
	return bad != 0;
}
