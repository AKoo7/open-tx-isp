/*
 * T21 ABI adapters for the shared TX-ISP math library.
 *
 * Stock T21 and T31 use the same implementations for these entry points.
 * Keep the generation-facing names and o32 argument layout here while the
 * arithmetic lives in the host-tested common header.
 */

#include <linux/types.h>

#include "../include/tx_isp/tx_isp_math.h"

#define TX_ISP_T21_INTERPOLATION_LAST_INDEX	8U

extern s32 isp_printf(u32 level, const char *fmt, ...);

u32 private_math_exp2(s32 value, char input_precision,
		      char output_precision)
{
	return tx_isp_exp2_u32((u32)value, (u8)input_precision,
			       (u8)output_precision);
}

s32 private_log2_int_to_fixed(u32 value, char precision, char output_shift)
{
	return (s32)tx_isp_log2_int_u32(value, (u8)precision,
				       (u8)output_shift);
}

s32 private_log2_fixed_to_fixed(u32 value, s32 input_precision,
				char output_precision)
{
	return (s32)tx_isp_log2_fixed_u32(value, (u32)input_precision,
					 (u8)output_precision);
}

u32 tisp_math_exp2(s32 value, char input_precision, char output_precision)
{
	return tx_isp_exp2_u32((u32)value, (u8)input_precision,
			       (u8)output_precision);
}

s32 fix_point_mult3_32(u32 point_pos, u32 first, u32 second, u32 third)
{
	u32 pair = tx_isp_fixmul_wrapped_u32_unchecked(point_pos, first,
						      second);

	return (s32)tx_isp_fixmul_wrapped_u32_unchecked(point_pos, pair,
						       third);
}

s32 fix_point_div_32(s32 point_pos, s32 numerator, s32 denominator)
{
	return (s32)tx_isp_fixdiv_oem_u32((u32)point_pos, (u32)numerator,
					 (u32)denominator);
}

s32 tisp_simple_intp(s32 index, s32 fraction, void *table_address)
{
	const s32 *table = table_address;

	if (!table)
		return 0;

	return (s32)tx_isp_lerp_s32((u32)index, (u32)fraction, table,
				    TX_ISP_T21_INTERPOLATION_LAST_INDEX);
}

s32 tisp_log2_fixed_to_fixed(u32 value, s32 input_precision,
			     char output_precision)
{
	return (s32)tx_isp_log2_fixed_u32(value, (u32)input_precision,
					 (u8)output_precision);
}
