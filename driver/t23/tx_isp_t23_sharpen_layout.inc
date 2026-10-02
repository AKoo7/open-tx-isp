/*
 * T23 sharpen tuning fields, in the order the OEM tiziano_sharpen_params_refresh
 * copies them from the active bank (OEM tparams+0x1e4e0 = active bank +0xb3e0;
 * IQ file offset 0x18 + 0xb3e0), taken from the OEM T23 module.  The OEM
 * copies y_sp_fl_sl_3_array last, from +0x6b4 behind the WDR curves.
 */
#define REGTRACE_T23_SHARPEN_TUNING_OFFSET 0xb3f8U
#define REGTRACE_T23_SHARPEN_TUNING_SIZE 0x6d8U
#define REGTRACE_T23_SHARPEN_TUNING_FIELDS(X) \
    X(y_sp_out_opt_array, 0x0000U, 0x4U) \
    X(y_sp_sl_exp_thres_array, 0x0004U, 0x24U) \
    X(y_sp_sl_exp_num_array, 0x0028U, 0x24U) \
    X(y_sp_std_cfg_array, 0x004cU, 0x8U) \
    X(y_sp_uu_min_stren_array, 0x0054U, 0x24U) \
    X(y_sp_uu_min_thres_array, 0x0078U, 0x24U) \
    X(y_sp_uu_thres_array, 0x009cU, 0x24U) \
    X(y_sp_mv_uu_thres_array, 0x00c0U, 0x24U) \
    X(y_sp_mv_uu_stren_array, 0x00e4U, 0x24U) \
    X(y_sp_uu_stren_array, 0x0108U, 0x24U) \
    X(y_sp_uu_par_cfg_array, 0x012cU, 0x10U) \
    X(y_sp_fl_std_thres_array, 0x013cU, 0x24U) \
    X(y_sp_mv_fl_std_thres_array, 0x0160U, 0x24U) \
    X(y_sp_fl_thres_array, 0x0184U, 0x24U) \
    X(y_sp_fl_min_thres_array, 0x01a8U, 0x24U) \
    X(y_sp_mv_fl_thres_array, 0x01ccU, 0x24U) \
    X(y_sp_mv_fl_min_thres_array, 0x01f0U, 0x24U) \
    X(y_sp_fl_par_cfg_array, 0x0214U, 0x8U) \
    X(y_sp_v2_win5_thres_array, 0x021cU, 0x24U) \
    X(y_sp_v1_v2_coef_par_cfg_array, 0x0240U, 0x30U) \
    X(y_sp_w_b_ll_par_cfg_array, 0x0270U, 0x24U) \
    X(y_sp_uu_np_array, 0x0294U, 0x40U) \
    X(y_sp_w_wei_np_array, 0x02d4U, 0x40U) \
    X(y_sp_b_wei_np_array, 0x0314U, 0x40U) \
    X(y_sp_w_sl_stren_0_array, 0x0354U, 0x24U) \
    X(y_sp_w_sl_stren_1_array, 0x0378U, 0x24U) \
    X(y_sp_w_sl_stren_2_array, 0x039cU, 0x24U) \
    X(y_sp_w_sl_stren_3_array, 0x03c0U, 0x24U) \
    X(y_sp_b_sl_stren_0_array, 0x03e4U, 0x24U) \
    X(y_sp_b_sl_stren_1_array, 0x0408U, 0x24U) \
    X(y_sp_b_sl_stren_2_array, 0x042cU, 0x24U) \
    X(y_sp_b_sl_stren_3_array, 0x0450U, 0x24U) \
    X(y_sp_uu_sl_0_array, 0x0474U, 0x24U) \
    X(y_sp_uu_sl_1_array, 0x0498U, 0x24U) \
    X(y_sp_uu_sl_2_array, 0x04bcU, 0x24U) \
    X(y_sp_uu_sl_3_array, 0x04e0U, 0x24U) \
    X(y_sp_fl_sl_0_array, 0x0504U, 0x24U) \
    X(y_sp_fl_sl_1_array, 0x0528U, 0x24U) \
    X(y_sp_fl_sl_2_array, 0x054cU, 0x24U) \
    X(y_sp_uu_thres_wdr_array, 0x0570U, 0x24U) \
    X(y_sp_w_sl_stren_0_wdr_array, 0x0594U, 0x24U) \
    X(y_sp_w_sl_stren_1_wdr_array, 0x05b8U, 0x24U) \
    X(y_sp_w_sl_stren_2_wdr_array, 0x05dcU, 0x24U) \
    X(y_sp_w_sl_stren_3_wdr_array, 0x0600U, 0x24U) \
    X(y_sp_b_sl_stren_0_wdr_array, 0x0624U, 0x24U) \
    X(y_sp_b_sl_stren_1_wdr_array, 0x0648U, 0x24U) \
    X(y_sp_b_sl_stren_2_wdr_array, 0x066cU, 0x24U) \
    X(y_sp_b_sl_stren_3_wdr_array, 0x0690U, 0x24U) \
    X(y_sp_fl_sl_3_array, 0x06b4U, 0x24U)
