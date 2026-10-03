/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Scene mode and colour effect controls shared by the T23 and T31 drivers
 * (beyond vendor).
 *
 *   0x009a091a V4L2_CID_SCENE_MODE  IMP_ISP_Tuning_SetSceneMode (S/G_CTRL)
 *   0x0098091f V4L2_CID_COLORFX     IMP_ISP_Tuning_SetColorfxMode (S/G_CTRL)
 *
 * The stock tx-isp-t23.ko / tx-isp-t31.ko dispatchers accept both CIDs as
 * no-ops (set returns 0 without effect, get leaves the value untouched) and
 * the stock T23/T31 libimp does not even export the API.  Here:
 *
 *  - Scene: stored and reported, nothing applied (the T20 3.12.0 and T21
 *    drivers apply nothing either).  Values above 14 are rejected.
 *  - Colorfx: AUTO (stock), BW (BCSH saturation 0), VIVID (BCSH saturation
 *    raised by TX_ISP_COLORFX_VIVID_STEP) and NEGATIVE (inverted gamma
 *    LUT).  SEPIA and the other V4L2 effects need a chroma offset these
 *    blocks have no known control for; they are refused (-EINVAL) and the
 *    effect in use stays.  The effect is applied where the user saturation
 *    and the gamma LUT are consumed, so it survives saturation changes and
 *    day/night reloads.
 *
 * In the default state (scene 0, colorfx AUTO) every helper returns its
 * input unchanged, so no register write differs from stock.
 */
#ifndef TX_ISP_SCENE_COLORFX_H
#define TX_ISP_SCENE_COLORFX_H

#define TX_ISP_CID_SCENE_MODE	0x009a091a
#define TX_ISP_CID_COLORFX	0x0098091f

#define TX_ISP_SCENE_MAX	14

#define TX_ISP_COLORFX_AUTO	0
#define TX_ISP_COLORFX_BW	1
#define TX_ISP_COLORFX_NEGATIVE	3
#define TX_ISP_COLORFX_VIVID	9

/* VIVID: steps added to the 8-bit user saturation (128 = neutral).  The
 * BCSH maps 128..255 towards a large gain, so a small step already gives
 * roughly 1.3 to 1.5 times the colour. */
#define TX_ISP_COLORFX_VIVID_STEP	12

static inline int tx_isp_scene_valid(uint32_t scene)
{
	return scene <= TX_ISP_SCENE_MAX;
}

static inline int tx_isp_colorfx_valid(uint32_t fx)
{
	return fx == TX_ISP_COLORFX_AUTO || fx == TX_ISP_COLORFX_BW ||
	       fx == TX_ISP_COLORFX_NEGATIVE || fx == TX_ISP_COLORFX_VIVID;
}

static inline int tx_isp_colorfx_scales_sat(uint32_t fx)
{
	return fx == TX_ISP_COLORFX_BW || fx == TX_ISP_COLORFX_VIVID;
}

/* The 8-bit user saturation the BCSH uses while an effect is active. */
static inline uint8_t tx_isp_colorfx_sat(uint32_t fx, uint8_t sat)
{
	if (fx == TX_ISP_COLORFX_BW)
		return 0;
	if (fx == TX_ISP_COLORFX_VIVID)
		return sat > 255 - TX_ISP_COLORFX_VIVID_STEP ?
		       255 : sat + TX_ISP_COLORFX_VIVID_STEP;
	return sat;
}

/* One gamma register: two 12-bit LUT points, inverted for NEGATIVE. */
static inline uint32_t tx_isp_colorfx_gamma_word(uint32_t lo, uint32_t hi,
						 uint32_t fx)
{
	if (fx == TX_ISP_COLORFX_NEGATIVE) {
		lo = 0xfff - (lo & 0xfff);
		hi = 0xfff - (hi & 0xfff);
	}
	return (hi << 12) | lo;
}

#endif /* TX_ISP_SCENE_COLORFX_H */
