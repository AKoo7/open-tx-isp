/* SPDX-License-Identifier: GPL-2.0 */
/*
 * T21 tuning controls the stock (lifted) dispatcher accepts but ignores
 * (beyond vendor).  Included once by tx_isp_t21_recovered.c, after the
 * tuning-array declarations; the hooks run from the hand-written glue,
 * the lifted code (tx_isp_t21_adr_oem.inc) is unchanged.
 *
 * Stock oem-t21.ko apical_isp_core_ops_s_ctrl returns 0 without any
 * effect for these CIDs and its g_ctrl leaves the value untouched:
 *
 *   0x009a091a V4L2_CID_SCENE_MODE  SetSceneMode (S_CTRL)
 *   0x0098091f V4L2_CID_COLORFX     SetColorfxMode (S_CTRL)
 *   0x0800002c SinterDnsAttr        112-byte block (tuning ioctl, pointer)
 *   0x08000167 Temper type          TemperDnsAttr/Ctl (S_CTRL)
 *   0x08000083 Temper strength      TemperDnsAttr (tuning ioctl, value)
 *   0x08000082 Temper strength      TemperDnsCtl (tuning ioctl, value)
 *   0x08000025 Expr (get)           IMPISPExpr, 12 bytes (tuning ioctl, pointer)
 *   0x08000062 DPC strength         SetDPC_Strength (tuning ioctl, value)
 *
 * Here they act on existing T21 mechanisms:
 *
 *  - Scene: stored and reported, nothing applied (the T20 3.12.0 driver
 *    returns before applying its scene mode too).
 *  - Colorfx: AUTO, BW (CCM saturation list 0), VIVID (saturation list
 *    x1.5) and NEGATIVE (inverted gamma LUT).  SEPIA and the other V4L2
 *    effects need a chroma offset T21 has no known control for; they are
 *    refused (-1) and the effect in use stays.
 *  - Sinter (2D NR): every SDNS noise-profile field (npv * stren, as
 *    tisp_sdns_y/c_param_cfg write them) is scaled by strength/128
 *    (MANUAL; AUTO = 128), at most up to the 8-bit field limit.  The
 *    stren factors themselves are small integers (0..8 in the jxf23
 *    tuning), too coarse to scale.
 *  - IntegrationTime (same 0x800002c block, [0x52] RANGE): u16 [0x1a]
 *    becomes the AE exposure ceiling (tisp_ae_ctrls max_integration_time,
 *    re-seeded from the sensor by tisp_set_fps), as the T20 3.12.0 AE
 *    caps at stab.global_max_integration_time.  AUTO/MANUAL stay no-ops.
 *  - Temper (3D NR): MANUAL applies the strength through the stock
 *    0x8000085 path, tisp_s_3dns_ratio (128 = tuning file), AUTO puts the
 *    ratio last set through 0x8000085 back (128 if none), DISABLE applies 0.
 *
 * Nothing changes until one of these controls is set: every hook is a
 * no-op in the default state (colorfx AUTO, sinter ratio 128, temper
 * ratio never applied).
 */
#ifndef TX_ISP_T21_TUNING_CTL_H
#define TX_ISP_T21_TUNING_CTL_H

#define T21_CID_SCENE_MODE	0x009a091a
#define T21_CID_COLORFX		0x0098091f
#define T21_CID_SINTER_DNS	0x0800002c
#define T21_CID_TEMPER_TYPE	0x08000167
#define T21_CID_TEMPER_ATTR	0x08000083
#define T21_CID_TEMPER_CTL	0x08000082
#define T21_CID_3DNS_RATIO	0x08000085
#define T21_CID_EXPR		0x08000025	/* GetExpr, get side only */
#define T21_CID_DPC_RATIO	0x08000062

#define T21_COLORFX_AUTO	0
#define T21_COLORFX_BW		1
#define T21_COLORFX_NEGATIVE	3
#define T21_COLORFX_VIVID	9

#define T21_TEMPER_DISABLE	0
#define T21_TEMPER_AUTO		1
#define T21_TEMPER_MANUAL	2

/* IMPISPSinterDenoiseAttr as the T21 1.0.33 libimp sends it on 0x800002c:
 * a zeroed 0x70-byte block, [70] = 1, MANUAL also [10] = [98] = 1 and
 * [43] = strength.  The same CID carries the legacy integration-time
 * block ([0x3f]/[0x52] set, never [70]) and MoveState (a plain 0/1). */
#define T21_SINTER_BLOCK	0x70
#define T21_SINTER_B_MANUAL	10
#define T21_SINTER_B_STRENGTH	43
#define T21_SINTER_B_VALID	70
#define T21_SINTER_B_MANUAL2	98
#define T21_IT_B_AUTO		0x3f
#define T21_IT_B_RANGE		0x52
#define T21_IT_B_MAX		0x1a	/* u16, RANGE max_integration_time */

static uint32_t t21_scene_mode;			/* V4L2 scene, 0 = auto */
static uint32_t t21_colorfx = T21_COLORFX_AUTO;
static uint32_t t21_colorfx_sat_base[9];	/* list before the effect */
static uint32_t t21_sinter_type;		/* 0 auto, 1 manual */
static uint32_t t21_sinter_strength = 128;
static uint32_t t21_sinter_ratio = 128;		/* applied, 128 = identity */
static uint32_t t21_temper_type = T21_TEMPER_AUTO;
static uint32_t t21_temper_strength = 128;
static uint32_t t21_3dns_auto_ratio = 128;	/* last 0x8000085 value */
static uint32_t t21_3dns_applied = 128;		/* ratio in the mdns banks */

/* ---- pure helpers (host-testable) ------------------------------------ */

static inline int t21_colorfx_scales_sat(uint32_t fx)
{
	return fx == T21_COLORFX_BW || fx == T21_COLORFX_VIVID;
}

/* The CCM saturation list (per EV, 256 = 1.0 in cm_control) for an effect. */
static inline void t21_colorfx_sat(uint32_t fx, const uint32_t *in,
				   uint32_t *out)
{
	int i;

	for (i = 0; i < 9; i++) {
		if (fx == T21_COLORFX_BW)
			out[i] = 0;
		else if (fx == T21_COLORFX_VIVID)
			out[i] = in[i] + (in[i] >> 1);
		else
			out[i] = in[i];
	}
}

/* One gamma register: two 12-bit LUT points, inverted for NEGATIVE. */
static inline uint32_t t21_gamma_word(uint32_t lo, uint32_t hi, uint32_t fx)
{
	if (fx == T21_COLORFX_NEGATIVE) {
		lo = 0xfff - (lo & 0xfff);
		hi = 0xfff - (hi & 0xfff);
	}
	return (hi << 12) | lo;
}

/*
 * One SDNS register field (npv * stren, 8 bits, written unmasked) scaled
 * by ratio/128.  A raise stops at 255 (or at the stock value, if that is
 * already larger); 128 returns the stock value unchanged.
 */
static inline uint32_t t21_sinter_px(uint32_t v, uint32_t ratio)
{
	uint32_t s, cap;

	if (ratio == 128)
		return v;
	s = (v * ratio + 64) >> 7;
	if (s <= v)
		return s;
	cap = v > 255 ? v : 255;
	return s > cap ? cap : s;
}

#ifndef T21_TUNING_CTL_HOST_TEST

/* ---- hooks called from the hand-written T21 code ------------------- */

/* tiziano_ccm_params_refresh just loaded cm_sat_list from tparams. */
static void t21_colorfx_after_ccm_refresh(void)
{
	if (!t21_colorfx_scales_sat(t21_colorfx))
		return;
	memcpy(t21_colorfx_sat_base, cm_sat_list, sizeof(t21_colorfx_sat_base));
	t21_colorfx_sat(t21_colorfx, t21_colorfx_sat_base,
			(uint32_t *)cm_sat_list);
}

/* tisp_set_saturation computed a new list: the effect goes on top. */
static const void *t21_colorfx_saturation(const int32_t *eff,
					  uint32_t *scratch)
{
	if (!t21_colorfx_scales_sat(t21_colorfx))
		return eff;
	memcpy(t21_colorfx_sat_base, eff, sizeof(t21_colorfx_sat_base));
	t21_colorfx_sat(t21_colorfx, t21_colorfx_sat_base, scratch);
	return scratch;
}

static int t21_colorfx_set(uint32_t fx)
{
	uint32_t old = t21_colorfx;
	uint32_t list[9];
	int32_t size = sizeof(list);

	if (fx != T21_COLORFX_AUTO && fx != T21_COLORFX_BW &&
	    fx != T21_COLORFX_NEGATIVE && fx != T21_COLORFX_VIVID)
		return -1;
	if (fx == old)
		return 0;

	if (!t21_colorfx_scales_sat(old))
		memcpy(t21_colorfx_sat_base, cm_sat_list,
		       sizeof(t21_colorfx_sat_base));
	t21_colorfx = fx;
	if (t21_colorfx_scales_sat(fx) || t21_colorfx_scales_sat(old)) {
		t21_colorfx_sat(fx, t21_colorfx_sat_base, list);
		tisp_ccm_param_array_set(0x81, list, &size);
	}
	if (fx == T21_COLORFX_NEGATIVE || old == T21_COLORFX_NEGATIVE)
		tiziano_gamma_lut_parameter();
	return 0;
}

static void t21_sinter_set_ratio(uint32_t ratio)
{
	if (ratio == t21_sinter_ratio)
		return;
	t21_sinter_ratio = ratio;
	/* Re-interpolate at the gain in use, like the stock day/night
	 * refresh; before the first refresh the next frame does it. */
	if (sdns_gain_old != 0xffffffffU)
		tisp_sdns_intp_reg_refresh(sdns_gain_old);
}

static void t21_3dns_apply(uint32_t ratio)
{
	if (ratio > 255 || ratio == t21_3dns_applied)
		return;
	tisp_s_3dns_ratio(ratio);
	t21_3dns_applied = ratio;
}

static void t21_temper_update(void)
{
	switch (t21_temper_type) {
	case T21_TEMPER_MANUAL:
		t21_3dns_apply(t21_temper_strength);
		break;
	case T21_TEMPER_DISABLE:
		t21_3dns_apply(0);
		break;
	default:
		t21_3dns_apply(t21_3dns_auto_ratio);
		break;
	}
}

/*
 * SetIntegrationTime(MODE_RANGE): the AE exposure ceiling.  Stock
 * oem-t21.ko ignores the block (0x800002c is a no-op), so the cap read
 * back but had no effect.  T21 already has the field the T20 3.12.0 AE
 * caps at (stab.global_max_integration_time there): tisp_ae_ctrls
 * max_integration_time, seeded from the sensor by tisp_set_fps (ISP
 * start and every fps change, like the T20 SYNC_VIDEO_IN re-seed) and
 * copied into _exp_parameter by tisp_ae_ctrls_update each frame, which
 * also clamps it to the sensor maximum.  Never below the sensor minimum.
 */
static void t21_ae_set_it_max(uint32_t lines)
{
	const struct t21_sensor_ctrl_view *sctrl =
		(const struct t21_sensor_ctrl_view *)sensor_ctrl;
	struct t21_ae_ctrls_view *ae = (struct t21_ae_ctrls_view *)tisp_ae_ctrls;
	uint32_t lo = sctrl->min_integration_time ? sctrl->min_integration_time : 1;
	uint32_t hi = sctrl->max_integration_time;

	if (lines < lo)
		lines = lo;
	if (hi && lines > hi)
		lines = hi;
	ACCESS_ONCE(ae->max_integration_time) = lines;
}

/*
 * Set side.  Returns 1 when the control was handled here (*ret holds the
 * result), 0 to pass it on to the stock dispatcher.
 */
/*
 * DPC strength (0x8000062, value 0..255, 128 = tuning file).  Stock T21
 * has the DPC block (tiziano_dpc, regs 0x204-0x23c) but no strength
 * control.  The OEM T23/T31 tisp_s_dpc_str_internal scales the m1/m3
 * detection thresholds; T21's equivalents are the m1 level-0 thresholds:
 * up to 128 the "f" threshold scales linearly (at least 5) and the "d"
 * threshold moves towards 1000, above 128 "f" moves towards 1200 and "d"
 * towards 5.  Applied to the gain-interpolated values (the mapping is
 * affine, so this equals scaling the bank arrays), every refresh.
 */
static uint32_t t21_dpc_ratio = 128;

static void t21_dpc_scale_intp(void)
{
	int32_t s = (int32_t)t21_dpc_ratio;
	int32_t f = (int32_t)dpc_d_m1_l0_fthres_intp;
	int32_t d = (int32_t)dpc_d_m1_l0_dthres_intp;

	if (s == 128)
		return;
	if (s < 128) {
		f = (f * s) >> 7;
		if (f < 5)
			f = 5;
		d = 1000 + (((d - 1000) * s) >> 7);
	} else {
		f += ((1200 - f) * (s - 128)) >> 7;
		d = ((256 - s) * d + 5 * (s - 128)) >> 7;
	}
	dpc_d_m1_l0_fthres_intp = (uintptr_t)(f < 0 ? 0 : f);
	dpc_d_m1_l0_dthres_intp = (uintptr_t)(d < 0 ? 0 : d);
}

static void t21_dpc_set_ratio(uint32_t ratio)
{
	t21_dpc_ratio = ratio;
	/* re-interpolate at the gain in use (as the day/night refresh) */
	if (dpc_gain_old != 0xffffffffU)
		tisp_dpc_intp_reg_refresh(dpc_gain_old);
}

static int t21_tuning_ctl_s(int32_t *ctl, int32_t *ret)
{
	uint8_t blk[T21_SINTER_BLOCK];
	uint32_t v = (uint32_t)ctl[1];

	switch ((uint32_t)ctl[0]) {
	case T21_CID_SCENE_MODE:
		if (v > 14) {
			*ret = -1;
			return 1;
		}
		t21_scene_mode = v;
		*ret = 0;
		return 1;
	case T21_CID_COLORFX:
		*ret = t21_colorfx_set(v);
		return 1;
	case T21_CID_DPC_RATIO:
		if (v > 255) {
			*ret = -1;
			return 1;
		}
		t21_dpc_set_ratio(v);
		*ret = 0;
		return 1;
	case T21_CID_SINTER_DNS:
		/* MoveState passes 0/1, not a pointer: the copy fails and the
		 * stock no-op answers, as before. */
		if (private_copy_from_user(blk, (const void __user *)(uintptr_t)v,
					   sizeof(blk)))
			return 0;
		if (!blk[T21_SINTER_B_VALID] && blk[T21_IT_B_RANGE]) {
			t21_ae_set_it_max(blk[T21_IT_B_MAX] |
					  (blk[T21_IT_B_MAX + 1] << 8));
			*ret = 0;
			return 1;
		}
		if (!blk[T21_SINTER_B_VALID] || blk[T21_IT_B_AUTO] ||
		    blk[T21_IT_B_RANGE])
			return 0;
		if (blk[T21_SINTER_B_MANUAL]) {
			t21_sinter_type = 1;
			t21_sinter_strength = blk[T21_SINTER_B_STRENGTH];
			t21_sinter_set_ratio(t21_sinter_strength);
		} else {
			t21_sinter_type = 0;
			t21_sinter_set_ratio(128);
		}
		*ret = 0;
		return 1;
	case T21_CID_TEMPER_TYPE:
		if (v > T21_TEMPER_MANUAL) {
			*ret = -1;
			return 1;
		}
		t21_temper_type = v;
		t21_temper_update();
		*ret = 0;
		return 1;
	case T21_CID_TEMPER_ATTR:
	case T21_CID_TEMPER_CTL:
		if (v > 255) {
			*ret = -1;
			return 1;
		}
		t21_temper_strength = v;
		if (t21_temper_type == T21_TEMPER_MANUAL)
			t21_3dns_apply(v);
		*ret = 0;
		return 1;
	case T21_CID_3DNS_RATIO:
		/* stock path (tisp_s_3dns_ratio for < 256); remember it as
		 * the AUTO ratio and as what the banks now hold */
		if (v < 256) {
			t21_3dns_auto_ratio = v;
			t21_3dns_applied = v;
		}
		return 0;
	}
	return 0;
}

/* Get side, same convention; the value goes to ctl[1]. */
static int t21_tuning_ctl_g(int32_t *ctl, int32_t *ret)
{
	uint8_t blk[T21_SINTER_BLOCK];
	uint32_t v = (uint32_t)ctl[1];

	switch ((uint32_t)ctl[0]) {
	case T21_CID_SCENE_MODE:
		ctl[1] = (int32_t)t21_scene_mode;
		break;
	case T21_CID_COLORFX:
		ctl[1] = (int32_t)t21_colorfx;
		break;
	case T21_CID_DPC_RATIO:
		ctl[1] = (int32_t)t21_dpc_ratio;
		break;
	case T21_CID_TEMPER_TYPE:
		ctl[1] = (int32_t)t21_temper_type;
		break;
	case T21_CID_TEMPER_ATTR:
	case T21_CID_TEMPER_CTL:
		ctl[1] = (int32_t)t21_temper_strength;
		break;
	case T21_CID_EXPR:
		/* Stock g_ctrl acknowledges 0x8000025 without writing the
		 * record; the pre-lift glue answered it from the live AE
		 * (t21_g_expr), lost when the stock dispatcher was lifted.
		 * GetExpr's maximum and line time are what a caller converts
		 * an integration-time cap with. */
		*ret = t21_g_expr((void __user *)(uintptr_t)v);
		return 1;
	case T21_CID_SINTER_DNS:
		/* fill only the sinter bytes; the integration-time getter
		 * reads other bytes of the same block, which stay as given */
		if (private_copy_from_user(blk, (const void __user *)(uintptr_t)v,
					   sizeof(blk)))
			return 0;
		blk[T21_SINTER_B_VALID] = 1;
		blk[T21_SINTER_B_MANUAL] = t21_sinter_type ? 1 : 0;
		blk[T21_SINTER_B_MANUAL2] = t21_sinter_type ? 1 : 0;
		blk[T21_SINTER_B_STRENGTH] = (uint8_t)t21_sinter_strength;
		*ret = private_copy_to_user((void __user *)(uintptr_t)v, blk,
					    sizeof(blk)) ? -14 : 0;
		return 1;
	default:
		return 0;
	}
	*ret = 0;
	return 1;
}

#endif /* !T21_TUNING_CTL_HOST_TEST */
#endif /* TX_ISP_T21_TUNING_CTL_H */
