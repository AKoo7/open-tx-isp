#include "tx-isp-core-local.h"

/*
 * jxf23_t20 (and most other T20 sensors) never populate
 * attr->one_line_expr_in_us, so the vendor GetExpr handler always reports
 * line_us = 0 to userspace.  The sensor source isn't in this tree to patch
 * directly, so rename the vendor v4l2 entry point and wrap it: if the
 * active sensor left the field at 0, derive the line period from the
 * current timing instead (line time = 1e6 / (fps * vts), matching the
 * lines_per_second convention already used by the compact AE in
 * sensor_drv.c) and patch just that field in the copy already sent to
 * userspace.
 */
#define isp_core_ops_g_ctrl isp_core_ops_g_ctrl_vendor
#include "source/apical-isp/tx-isp-core-tuning.c"
#undef isp_core_ops_g_ctrl

static unsigned short t20_core_fallback_one_line_expr_in_us(struct tx_isp_core_device *core)
{
	struct tx_isp_sensor_attribute *attr = core->vin.attr;
	unsigned int fps = core->vin.fps;
	uint64_t lines_per_second;
	uint64_t line_us;

	if (!attr || !attr->total_height || !fps || (fps & 0xffff) == 0)
		return 0;

	lines_per_second = div_u64((uint64_t)attr->total_height * (fps >> 16),
				    fps & 0xffff);
	if (!lines_per_second)
		return 0;

	line_us = div_u64(1000000ULL, lines_per_second);
	if (!line_us)
		line_us = 1;
	if (line_us > 0xffff)
		line_us = 0xffff;
	return (unsigned short)line_us;
}

int isp_core_ops_g_ctrl(struct v4l2_subdev *sd, struct v4l2_control *ctrl)
{
	struct tx_isp_core_device *core = sd_to_tx_isp_core_device(sd);
	struct tx_isp_sensor_attribute *attr;
	unsigned short line_us;
	int ret;

	ret = isp_core_ops_g_ctrl_vendor(sd, ctrl);
	if (ret)
		return ret;

	if (ctrl->id != IMAGE_TUNING_CID_EXPR_ATTR)
		return ret;

	attr = core->vin.attr;
	if (attr && attr->one_line_expr_in_us == 0) {
		line_us = t20_core_fallback_one_line_expr_in_us(core);
		if (line_us) {
			union isp_core_expr_attr __user *uattr =
				(union isp_core_expr_attr __user *)ctrl->value;

			if (copy_to_user(&uattr->g_attr.one_line_expr_in_us,
					  &line_us, sizeof(line_us)))
				return -EFAULT;
		}
	}
	return ret;
}
