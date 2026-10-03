/* IRQ context captures luma; sensor I/O runs on the system workqueue. */

struct regtrace_t23_ae_hlil_rung {
    uint32_t packed;
    uint32_t gain_q16;
    uint32_t ev_q10;
    uint32_t it;            /* integration time the sensor allocated, lines */
    uint32_t gain_log2;     /* sensor analog gain allocated, log2 Q16 */
    uint32_t dgain_q10;     /* ISP digital gain, linear Q10 (0x400 = 1x) */
};

/* Sensor gain times ISP digital gain, linear Q16. */
static uint32_t regtrace_t23_ae_rung_total_gain(
    const struct regtrace_t23_ae_hlil_rung *rung)
{
    uint64_t total = ((uint64_t)rung->gain_q16 * rung->dgain_q10) >> 10;

    return total > 0xffffffffULL ? 0xffffffffU : (uint32_t)total;
}

/*
 * ISP digital gain (OEM tisp_ae0_process_impl -> JZ_Isp_Ae_Dg2reg): the
 * four GIB channel gains, 0x1030 (R, G) and 0x1034 (B, IR), Q10, latched
 * through 0x1070 (OEM system_reg_write_ae(3, ...)).  The OEM writes the
 * gain itself into all four fields; here it scales the IQ bank's base
 * gains, which is the same for the usual 1x base and leaves the base
 * untouched at 1x.  Only written on change.
 */
#define REGTRACE_T23_AE_DGAIN_STEPS 8U
/* below 64x: the GIB gain fields are 16 bit Q10 */
#define REGTRACE_T23_AE_ISP_DGAIN_MAX_LOG2 ((6U << 16) - 0x100U)

int32_t system_reg_write_gib(int32_t arg1, int32_t arg2, int32_t arg3);

int32_t tisp_adr_ev_update(uint32_t a0, uint32_t a1);
int32_t *tisp_defog_ev_update(uint32_t a0, uint32_t a1, uint32_t a2,
                              uint32_t a3);

/*
 * Stock tisp_long_ev_update, ADR / defog part: tisp_adr_ev_update when ADR
 * is not bypassed (0xc bit 7), then tisp_defog_ev_update when defog is not
 * bypassed (bit 11); both only store ev >> 10 for their algorithm.  With
 * the lifted stock ADR/defog (source_adr_oem=1, default) the bits follow
 * the bank like stock; otherwise both blocks stay bypassed unless
 * source_adr/defog_internal_enable is set.
 */
static void regtrace_t23_ae_ev_adr_defog(uint32_t ev_lo, uint32_t ev_hi)
{
    uint32_t top;

    if (!regtrace_t23_source_adr_internal_enable &&
        !regtrace_t23_source_defog_internal_enable &&
        !regtrace_t23_source_adr_oem)
        return;
    top = (uint32_t)system_reg_read(0xc);
    if (!(top & BIT(7)))
        tisp_adr_ev_update(ev_lo, ev_hi);
    if (!(top & BIT(11)))
        tisp_defog_ev_update(ev_lo, ev_hi, 0, 0);
}

static uint32_t regtrace_t23_ae_user_max_dgain;     /* log2, Q5 (API) */
static uint32_t regtrace_t23_ae_isp_dgain_q10 = 0x400U;   /* in hardware */

static uint32_t regtrace_t23_ae_dgain_field(const unsigned char *base,
                                            uint32_t dgain_q10)
{
    uint32_t value = ((uint64_t)regtrace_t23_get_le32(base) * dgain_q10) >>
                     10;

    return min(value, 0xffffU);
}

static void regtrace_t23_ae_isp_dgain_write(uint32_t dgain_q10)
{
    if (dgain_q10 == regtrace_t23_ae_isp_dgain_q10)
        return;
    system_reg_write_gib(1, 0x1030,
        (regtrace_t23_ae_dgain_field(tiziano_gib_r_g_linear + 4,
                                     dgain_q10) << 16) |
        regtrace_t23_ae_dgain_field(tiziano_gib_r_g_linear, dgain_q10));
    system_reg_write_gib(1, 0x1034,
        (regtrace_t23_ae_dgain_field(tiziano_gib_b_ir_linear + 4,
                                     dgain_q10) << 16) |
        regtrace_t23_ae_dgain_field(tiziano_gib_b_ir_linear, dgain_q10));
    regtrace_t23_ae_isp_dgain_q10 = dgain_q10;
}

/*
 * Program a ladder rung coming from *from: the sensor exposure when its
 * register word changes, the ISP digital gain, and the gain-driven blocks
 * when the total gain changes.
 */
static int regtrace_t23_ae_apply_rung(
    const struct regtrace_t23_ae_hlil_rung *to,
    const struct regtrace_t23_ae_hlil_rung *from, const char *reason)
{
    uint32_t total = regtrace_t23_ae_rung_total_gain(to);
    int ret = 0;

    if (to->packed != from->packed)
        ret = regtrace_t23_call_sensor_exposure(to->packed, reason);
    if (ret)
        return ret;
    regtrace_t23_ae_isp_dgain_write(to->dgain_q10);
    if (total != regtrace_t23_ae_rung_total_gain(from))
        ret = regtrace_t23_source_apply_total_gain_value(
            total, to->packed >> 16, false, true);
    return ret;
}

#define REGTRACE_T23_AE_HLIL_MAX_STATES 64U
#define REGTRACE_T23_AE_HLIL_GAIN_STEPS 16U

static struct regtrace_t23_ae_hlil_rung
    regtrace_t23_ae_hlil_rungs[REGTRACE_T23_AE_HLIL_MAX_STATES];
static uint32_t regtrace_t23_ae_hlil_state_count;

#define REGTRACE_T23_AE_HLIL_STATE_COUNT regtrace_t23_ae_hlil_state_count

static int regtrace_t23_ae_hlil_append(uint32_t integration,
                                       uint32_t gain_log2_q16,
                                       uint32_t dgain_q10)
{
    struct regtrace_t23_ae_hlil_rung *rung;
    uint64_t ev_q10;
    uint32_t packed;
    uint32_t actual_it;
    uint32_t actual_gain_log2;
    uint32_t gain_q16;
    int ret;

    ret = regtrace_t23_sensor_allocate_exposure(
        integration, gain_log2_q16, &packed, &actual_it,
        &actual_gain_log2, &gain_q16);
    if (ret)
        return ret;
    if (regtrace_t23_ae_hlil_state_count &&
        regtrace_t23_ae_hlil_rungs[
            regtrace_t23_ae_hlil_state_count - 1U].packed == packed &&
        regtrace_t23_ae_hlil_rungs[
            regtrace_t23_ae_hlil_state_count - 1U].dgain_q10 == dgain_q10)
        return 0;
    if (regtrace_t23_ae_hlil_state_count >=
        REGTRACE_T23_AE_HLIL_MAX_STATES)
        return -ENOSPC;

    rung = &regtrace_t23_ae_hlil_rungs[
        regtrace_t23_ae_hlil_state_count++];
    rung->packed = packed;
    rung->gain_q16 = gain_q16;
    rung->it = actual_it;
    rung->gain_log2 = actual_gain_log2;
    rung->dgain_q10 = dgain_q10;
    ev_q10 = (uint64_t)actual_it * regtrace_t23_ae_rung_total_gain(rung);
    ev_q10 >>= 6;
    rung->ev_q10 = ev_q10 > 0xffffffffULL ?
        0xffffffffU : (uint32_t)ev_q10;
    return 0;
}

/*
 * Anti-flicker (V4L2 power line frequency: 0 off, 1 50 Hz, 2 60 Hz).  The
 * OEM AE quantises the integration time to whole periods of the light
 * flicker (1 / (2 * mains frequency)) once the exposure reaches one period
 * (tisp_s_antiflick -> tiziano_deflicker_expt_tune).  This driver's AE
 * walks a ladder of exposures, so the ladder itself is quantised: rungs
 * below one period stay (bright scenes), every longer rung is a multiple
 * of the period, and the gain rungs start at the longest such multiple.
 */
static uint32_t regtrace_t23_ae_flicker_mode;   /* 0, 1 (50 Hz), 2 (60 Hz) */
static uint32_t regtrace_t23_ae_flicker_lines;  /* period in use, lines */

static uint32_t regtrace_t23_sensor_held_fps(void);

static uint32_t regtrace_t23_ae_flicker_step(void)
{
    static const uint32_t hz_of_mode[3] = { 0, 50U, 60U };
    unsigned char *attr = regtrace_t23_sensor_owned_attr();
    uint32_t hz;
    uint32_t fps;
    uint32_t num;
    uint32_t den;
    uint32_t vts;

    if (regtrace_t23_ae_flicker_mode >= ARRAY_SIZE(hz_of_mode) || !attr)
        return 0;
    hz = hz_of_mode[regtrace_t23_ae_flicker_mode];
    if (!hz)
        return 0;
    /* No FPS mutex here: callers may hold it (sensor FPS retime). */
    fps = regtrace_t23_sensor_held_fps();
    if (!fps)
        fps = regtrace_t23_sensor_fps_request ?
            regtrace_t23_sensor_fps_request : regtrace_t23_source_sensor_fps;
    num = fps >> 16;
    den = fps & 0xffffU;
    vts = regtrace_t23_get_le16(attr + REGTRACE_T23_ATTR_TOTAL_HEIGHT);
    if (!num || !den || !vts)
        return 0;
    /* lines per second / (2 * hz), rounded */
    return (uint32_t)div64_u64((uint64_t)num * vts + (uint64_t)den * hz,
                               (uint64_t)den * 2U * hz);
}

static int regtrace_t23_ae_hlil_build_ladder(void)
{
    uint32_t integration;
    uint32_t next;
    uint32_t gain_step;
    uint32_t gain;
    uint32_t unity_states;
    uint32_t gain_it = regtrace_t23_source_sensor_max_it;
    uint32_t dgain_max;
    uint32_t step;
    int ret;

    if (!regtrace_t23_source_sensor_min_it ||
        regtrace_t23_source_sensor_min_it >
        regtrace_t23_source_sensor_max_it)
        return -EINVAL;

    regtrace_t23_ae_hlil_state_count = 0;
    step = regtrace_t23_ae_flicker_step();
    if (step && step >= regtrace_t23_source_sensor_min_it &&
        step <= regtrace_t23_source_sensor_max_it) {
        uint32_t periods;
        uint32_t max_periods =
            regtrace_t23_source_sensor_max_it / step;

        integration = regtrace_t23_source_sensor_min_it;
        while (integration < step) {
            ret = regtrace_t23_ae_hlil_append(integration, 0U, 0x400U);
            if (ret)
                return ret;
            next = integration + max(integration >> 1, 1U);
            if (next <= integration)
                break;
            integration = next;
        }
        for (periods = 1; periods <= max_periods;) {
            ret = regtrace_t23_ae_hlil_append(periods * step, 0U, 0x400U);
            if (ret)
                return ret;
            if (periods == max_periods)
                break;
            /* about 1.5x per rung, as the plain ladder */
            next = max(periods + 1U, periods + (periods >> 1));
            periods = min(next, max_periods);
        }
        gain_it = max_periods * step;
        regtrace_t23_ae_flicker_lines = step;
    } else {
        regtrace_t23_ae_flicker_lines = 0;
        integration = regtrace_t23_source_sensor_min_it;
        for (;;) {
            ret = regtrace_t23_ae_hlil_append(integration, 0U, 0x400U);
            if (ret)
                return ret;
            if (integration == regtrace_t23_source_sensor_max_it)
                break;
            next = integration + max(integration >> 1, 1U);
            if (next < integration ||
                next > regtrace_t23_source_sensor_max_it)
                next = regtrace_t23_source_sensor_max_it;
            integration = next;
        }
    }

    unity_states = regtrace_t23_ae_hlil_state_count;
    if (!unity_states)
        return -EINVAL;
    regtrace_t23_source_ae_initial_packed =
        regtrace_t23_ae_hlil_rungs[unity_states - 1U].packed;

    gain_step = DIV_ROUND_UP(regtrace_t23_source_sensor_max_again,
                             REGTRACE_T23_AE_HLIL_GAIN_STEPS);
    if (gain_step) {
        for (gain = gain_step;
             gain < regtrace_t23_source_sensor_max_again;
             gain += gain_step) {
            ret = regtrace_t23_ae_hlil_append(gain_it, gain, 0x400U);
            if (ret)
                return ret;
            if (gain > 0xffffffffU - gain_step)
                break;
        }
        ret = regtrace_t23_ae_hlil_append(
            gain_it, regtrace_t23_source_sensor_max_again, 0x400U);
        if (ret)
            return ret;
    }

    /*
     * ISP digital gain once the analog gain is at its limit (OEM AE:
     * integration time, then sensor gain, then ISP dgain up to
     * ae_exp_th[3] = MAX_DGAIN).  None while MAX_DGAIN is 0 (1x).
     */
    dgain_max = min(regtrace_t23_ae_user_max_dgain << 11,
                    REGTRACE_T23_AE_ISP_DGAIN_MAX_LOG2);
    if (dgain_max) {
        uint32_t dgain_step = DIV_ROUND_UP(dgain_max,
                                           REGTRACE_T23_AE_DGAIN_STEPS);

        for (gain = dgain_step;; gain += dgain_step) {
            if (gain > dgain_max)
                gain = dgain_max;
            ret = regtrace_t23_ae_hlil_append(
                gain_it, regtrace_t23_source_sensor_max_again,
                tisp_math_exp2(gain, 16U, 10U));
            if (ret)
                return ret;
            if (gain == dgain_max)
                break;
        }
    }

    printk(KERN_INFO
           "tx_isp_t23_recovered: generic AE ladder states=%u it=%u..%u flicker=%u lines max_again_log2=0x%x max_isp_dgain_log2=0x%x initial=0x%08x\n",
           regtrace_t23_ae_hlil_state_count,
           regtrace_t23_source_sensor_min_it,
           regtrace_t23_source_sensor_max_it,
           regtrace_t23_ae_flicker_lines,
           regtrace_t23_source_sensor_max_again, dgain_max,
           regtrace_t23_source_ae_initial_packed);
    return 0;
}

static uint32_t regtrace_t23_ae_hlil_nearest_state(uint32_t ev)
{
    uint32_t best = 0;
    uint32_t best_distance = 0xffffffffU;
    uint32_t state;

    for (state = 0; state < REGTRACE_T23_AE_HLIL_STATE_COUNT; ++state) {
        uint32_t rung_ev = regtrace_t23_ae_hlil_rungs[state].ev_q10 >> 10;
        uint32_t distance = rung_ev > ev ? rung_ev - ev : ev - rung_ev;

        if (distance < best_distance) {
            best = state;
            best_distance = distance;
        }
    }
    return best;
}

static uint32_t regtrace_t23_ae_hlil_pending_luma;
static uint32_t regtrace_t23_ae_hlil_pending_bright;    /* clipped, 1/1000 */
static uint32_t regtrace_t23_ae_hlil_bright;            /* last processed */
static uint32_t regtrace_t23_ae_hlil_pending_snapshot;
/* last run saw an over-exposed picture: run at the fast cadence */
static bool regtrace_t23_ae_hlil_over;
static bool regtrace_t23_ae_hlil_pending;
/* Set while the ladder is rebuilt for a new frame length; blocks new work. */
static bool regtrace_t23_ae_hlil_retiming;
/* Set while a parameter-bank switch reloads the blocks; blocks new work. */
static bool regtrace_t23_ae_hlil_paused;
static DEFINE_SPINLOCK(regtrace_t23_ae_hlil_lock);

static void regtrace_t23_source_ae_hlil_work(struct work_struct *work);
static DECLARE_WORK(regtrace_t23_source_ae_hlil_work_item,
                    regtrace_t23_source_ae_hlil_work);

static void regtrace_t23_source_ae_hlil_work_body(struct work_struct *work);

static void regtrace_t23_source_ae_hlil_work(struct work_struct *work)
{
    regtrace_t23_text_check("ae_hlil_work-enter", 0);
    regtrace_t23_source_ae_hlil_work_body(work);
    regtrace_t23_text_check("ae_hlil_work-exit", 0);
}

static void regtrace_t23_source_ae_hlil_work_body(struct work_struct *work)
{
    const struct regtrace_t23_ae_hlil_rung *rungs =
        regtrace_t23_ae_hlil_rungs;
    const struct regtrace_t23_ae_hlil_rung *rung;
    unsigned long flags;
    uint32_t luma;
    uint32_t snapshot;
    uint32_t state;
    uint32_t next_state;
    uint32_t lower;
    uint32_t upper;
    uint32_t desired_ev;
    uint32_t desired_state;
    uint32_t bright;
    uint32_t max_down = 4U;
    bool saturated;
    int ret = 0;

    (void)work;
    spin_lock_irqsave(&regtrace_t23_ae_hlil_lock, flags);
    luma = regtrace_t23_ae_hlil_pending_luma;
    bright = regtrace_t23_ae_hlil_pending_bright;
    snapshot = regtrace_t23_ae_hlil_pending_snapshot;
    if (!REGTRACE_T23_AE_HLIL_STATE_COUNT) {
        regtrace_t23_ae_hlil_pending = false;
        spin_unlock_irqrestore(&regtrace_t23_ae_hlil_lock, flags);
        return;
    }
    state = min(regtrace_t23_source_ae_hlil_state,
                REGTRACE_T23_AE_HLIL_STATE_COUNT - 1U);
    spin_unlock_irqrestore(&regtrace_t23_ae_hlil_lock, flags);

    regtrace_t23_source_ae_hlil_runs++;
    regtrace_t23_source_ae_hlil_luma = luma;
    regtrace_t23_ae_hlil_bright = bright;
    next_state = state;
    lower = regtrace_t23_source_ae_hlil_target >
            regtrace_t23_source_ae_hlil_deadband ?
            regtrace_t23_source_ae_hlil_target -
            regtrace_t23_source_ae_hlil_deadband : 0U;
    upper = regtrace_t23_source_ae_hlil_target +
            regtrace_t23_source_ae_hlil_deadband;

    /*
     * Clipped picture (e.g. daylight after a module load, which starts from
     * the longest unity-gain exposure): at least sat_permille of the pixels
     * are above the AE0 bright threshold.  The luma saturates there and
     * understates the over-exposure (it can even land in the deadband), so
     * treat it as over-exposed, aim at half the luma estimate and allow six
     * rungs down instead of four.  Only downwards; dark scenes are unchanged.
     */
    saturated = luma && regtrace_t23_source_ae_hlil_sat_permille &&
                bright >= regtrace_t23_source_ae_hlil_sat_permille;

    if (!regtrace_t23_sensor_streaming || !regtrace_t23_core_started) {
        ret = -EAGAIN;
    } else if (!luma) {
        next_state = min(state + 4U,
                         REGTRACE_T23_AE_HLIL_STATE_COUNT - 1U);
    } else if (saturated || luma < lower || luma > upper) {
        uint32_t current_ev =
            rungs[state].ev_q10 >> 10;
        uint32_t measured = saturated ? max(luma, upper + 1U) : luma;

        desired_ev = (uint32_t)div64_u64(
            (uint64_t)current_ev * regtrace_t23_source_ae_hlil_target +
            measured / 2U, measured);
        if (saturated) {
            desired_ev = max(desired_ev >> 1, 1U);
            max_down = 6U;
        }
        desired_ev = clamp(desired_ev,
            rungs[0].ev_q10 >> 10,
            rungs[
                REGTRACE_T23_AE_HLIL_STATE_COUNT - 1U].ev_q10 >> 10);
        desired_state = regtrace_t23_ae_hlil_nearest_state(desired_ev);
        if (desired_state > state + 4U)
            next_state = state + 4U;
        else if (state > desired_state + max_down)
            next_state = state - max_down;
        else
            next_state = desired_state;
    }
    /* still moving down from an over-exposed picture: fast cadence */
    regtrace_t23_ae_hlil_over = !ret && next_state < state &&
                                (saturated || luma > upper);

    if (!ret && next_state != state) {
        rung = &rungs[next_state];
        ret = regtrace_t23_ae_apply_rung(rung, &rungs[state],
                                         "source-ae-hlil");
        if (!ret) {
            regtrace_t23_source_ae_hlil_state = next_state;
            regtrace_t23_source_ae_hlil_ev = rung->ev_q10;
            regtrace_t23_source_ae_hlil_updates++;
            regtrace_t23_ae_hlil_resume = true;
            if (regtrace_t23_source_ccm_events &&
                regtrace_t23_source_ccm_tuning_init &&
                regtrace_t23_ccm_saturation(rung->ev_q10) !=
                regtrace_t23_source_ccm_last_saturation)
                regtrace_t23_source_ccm_commit(
                    regtrace_t23_source_ccm_runtime_ct, rung->ev_q10);
            regtrace_t23_ae_ev_adr_defog(rung->ev_q10, 0);
            if (regtrace_t23_source_bcsh_events) {
                tisp_bcsh_ev_update(rung->ev_q10, 0);
                regtrace_t23_source_bcsh_ev_events++;
            }
        }
    }

    regtrace_t23_source_ae_hlil_status = ret ? (uint32_t)(-ret) : 0U;
    if (next_state != state || regtrace_t23_source_ae_hlil_runs <= 8U ||
        !(regtrace_t23_source_ae_hlil_runs &
          (regtrace_t23_source_ae_hlil_runs - 1U))) {
        if (ret)
            printk(KERN_WARNING
                   "tx_isp_t23_recovered: source AE HLIL run=%u snapshot=%u luma=%u bright=%u/1000 target=%u+-%u state=%u->%u updates=%u ret=%d\n",
                   regtrace_t23_source_ae_hlil_runs, snapshot, luma, bright,
                   regtrace_t23_source_ae_hlil_target,
                   regtrace_t23_source_ae_hlil_deadband, state, next_state,
                   regtrace_t23_source_ae_hlil_updates, ret);
        else
            printk(KERN_INFO
                   "tx_isp_t23_recovered: source AE HLIL run=%u snapshot=%u luma=%u bright=%u/1000 target=%u+-%u state=%u->%u updates=%u ret=%d\n",
                   regtrace_t23_source_ae_hlil_runs, snapshot, luma, bright,
                   regtrace_t23_source_ae_hlil_target,
                   regtrace_t23_source_ae_hlil_deadband, state, next_state,
                   regtrace_t23_source_ae_hlil_updates, ret);
    }

    spin_lock_irqsave(&regtrace_t23_ae_hlil_lock, flags);
    regtrace_t23_ae_hlil_pending = false;
    spin_unlock_irqrestore(&regtrace_t23_ae_hlil_lock, flags);
}

static void regtrace_t23_source_ae_hlil_capture(uint32_t luma,
                                               uint32_t bright_permille,
                                               uint32_t snapshot)
{
    unsigned long flags;
    uint32_t interval = regtrace_t23_source_ae_hlil_interval;

    if (!regtrace_t23_source_ae_hlil)
        return;
    /* over-exposed: shorter cadence (never longer than the normal one) */
    if (ACCESS_ONCE(regtrace_t23_ae_hlil_over) &&
        regtrace_t23_source_ae_hlil_fast_interval &&
        (!interval || regtrace_t23_source_ae_hlil_fast_interval < interval))
        interval = regtrace_t23_source_ae_hlil_fast_interval;
    if (interval && snapshot % interval)
        return;

    spin_lock_irqsave(&regtrace_t23_ae_hlil_lock, flags);
    if (regtrace_t23_ae_hlil_pending || regtrace_t23_ae_hlil_retiming ||
        regtrace_t23_ae_hlil_paused) {
        regtrace_t23_source_ae_hlil_drops++;
        spin_unlock_irqrestore(&regtrace_t23_ae_hlil_lock, flags);
        return;
    }
    regtrace_t23_ae_hlil_pending_luma = luma;
    regtrace_t23_ae_hlil_pending_bright = bright_permille;
    regtrace_t23_ae_hlil_pending_snapshot = snapshot;
    regtrace_t23_ae_hlil_pending = true;
    spin_unlock_irqrestore(&regtrace_t23_ae_hlil_lock, flags);
    schedule_work(&regtrace_t23_source_ae_hlil_work_item);
}

static void regtrace_t23_source_ae_hlil_trigger(void)
{
    unsigned long flags;

    if (!regtrace_t23_source_ae_hlil || !regtrace_t23_sensor_streaming ||
        !regtrace_t23_core_started || regtrace_t23_source_ae_oem)
        return;

    spin_lock_irqsave(&regtrace_t23_ae_hlil_lock, flags);
    if (regtrace_t23_ae_hlil_pending || regtrace_t23_ae_hlil_retiming ||
        regtrace_t23_ae_hlil_paused) {
        regtrace_t23_source_ae_hlil_drops++;
        spin_unlock_irqrestore(&regtrace_t23_ae_hlil_lock, flags);
        return;
    }
    regtrace_t23_ae_hlil_pending_luma = regtrace_t23_source_ae_hlil_luma;
    regtrace_t23_ae_hlil_pending_bright = regtrace_t23_ae_hlil_bright;
    regtrace_t23_ae_hlil_pending_snapshot =
        regtrace_t23_source_ae_stats_snapshots;
    regtrace_t23_ae_hlil_pending = true;
    spin_unlock_irqrestore(&regtrace_t23_ae_hlil_lock, flags);
    schedule_work(&regtrace_t23_source_ae_hlil_work_item);
}

/*
 * Exposure the sensor gets at stream start.  The OEM AE state lives from
 * tisp_init (sensor enable) on, and the OEM sc2336 s_stream only writes
 * 0x0100, so a framesource disable/enable keeps the last exposure; the
 * AE continues from there.  The ladder bootstrap rung (longest unity-gain
 * integration time) is only used before the AE has moved at all.
 */
/*
 * The resume point is the exposure value, not the rung index: if the ladder
 * was rebuilt since the AE last moved (frame rate / AE_IT_MAX / MAX_AGAIN
 * change, another sensor), the old index names a different exposure.
 * Re-resolve the rung nearest to the last EV, as the retime path does, and
 * keep state and EV consistent.  Returns REGTRACE_T23_AE_HLIL_STATE_COUNT
 * when there is nothing to resume.
 */
static uint32_t regtrace_t23_ae_hlil_resume_state(void)
{
    uint32_t state = regtrace_t23_source_ae_hlil_state;
    uint32_t ev = regtrace_t23_source_ae_hlil_ev;

    if (!regtrace_t23_ae_hlil_resume || !REGTRACE_T23_AE_HLIL_STATE_COUNT)
        return REGTRACE_T23_AE_HLIL_STATE_COUNT;
    if (state < REGTRACE_T23_AE_HLIL_STATE_COUNT &&
        regtrace_t23_ae_hlil_rungs[state].ev_q10 == ev)
        return state;
    state = regtrace_t23_ae_hlil_nearest_state(ev >> 10);
    printk(KERN_INFO
           "tx_isp_t23_recovered: AE resume re-resolved ev=%u -> state=%u ev=%u\n",
           ev, state, regtrace_t23_ae_hlil_rungs[state].ev_q10);
    regtrace_t23_source_ae_hlil_state = state;
    regtrace_t23_source_ae_hlil_ev = regtrace_t23_ae_hlil_rungs[state].ev_q10;
    return state;
}

static uint32_t regtrace_t23_ae_hlil_stream_packed(void)
{
    uint32_t state;

    if (regtrace_t23_source_ae_force_packed)
        return regtrace_t23_source_ae_force_packed;
    state = regtrace_t23_ae_hlil_resume_state();
    if (state < REGTRACE_T23_AE_HLIL_STATE_COUNT)
        return regtrace_t23_ae_hlil_rungs[state].packed;
    return regtrace_t23_source_ae_initial_packed;
}

/* Total gain of the resumed rung, 0 when the AE starts from bootstrap. */
static uint32_t regtrace_t23_ae_hlil_resume_gain(uint32_t *sensor_again)
{
    const struct regtrace_t23_ae_hlil_rung *rung;
    uint32_t state;

    if (regtrace_t23_source_ae_force_packed)
        return 0;
    state = regtrace_t23_ae_hlil_resume_state();
    if (state >= REGTRACE_T23_AE_HLIL_STATE_COUNT)
        return 0;
    rung = &regtrace_t23_ae_hlil_rungs[state];
    *sensor_again = rung->packed >> 16;
    return regtrace_t23_ae_rung_total_gain(rung);
}

static void regtrace_t23_source_ae_hlil_reset(void)
{
    const struct regtrace_t23_ae_hlil_rung *rungs =
        regtrace_t23_ae_hlil_rungs;
    unsigned long flags;
    uint32_t initial_packed = regtrace_t23_source_ae_force_packed ?
        regtrace_t23_source_ae_force_packed :
        regtrace_t23_source_ae_initial_packed;
    uint32_t state;

    cancel_work_sync(&regtrace_t23_source_ae_hlil_work_item);
    spin_lock_irqsave(&regtrace_t23_ae_hlil_lock, flags);
    regtrace_t23_ae_hlil_pending = false;
    regtrace_t23_ae_hlil_pending_luma = 0;
    regtrace_t23_ae_hlil_pending_bright = 0;
    regtrace_t23_ae_hlil_pending_snapshot = 0;
    regtrace_t23_ae_hlil_over = false;
    spin_unlock_irqrestore(&regtrace_t23_ae_hlil_lock, flags);

    if (!REGTRACE_T23_AE_HLIL_STATE_COUNT) {
        regtrace_t23_source_ae_hlil_state = 0U;
        regtrace_t23_source_ae_hlil_status = ENODEV;
        return;
    }
    if (!regtrace_t23_source_ae_force_packed &&
        regtrace_t23_ae_hlil_resume_state() <
        REGTRACE_T23_AE_HLIL_STATE_COUNT) {
        /* keep the rung the stream start just programmed */
    } else {
        regtrace_t23_ae_hlil_resume = false;
        regtrace_t23_source_ae_hlil_state = 0U;
        for (state = 0; state < REGTRACE_T23_AE_HLIL_STATE_COUNT; ++state) {
            if (rungs[state].packed == initial_packed) {
                regtrace_t23_source_ae_hlil_state = state;
                break;
            }
        }
        if (state == REGTRACE_T23_AE_HLIL_STATE_COUNT)
            regtrace_t23_source_ae_hlil_state =
                regtrace_t23_ae_hlil_nearest_state(initial_packed & 0xffffU);
    }
    regtrace_t23_source_ae_hlil_ev =
        rungs[regtrace_t23_source_ae_hlil_state].ev_q10;
    /* the sensor exposure is programmed by the stream start */
    regtrace_t23_ae_isp_dgain_write(
        rungs[regtrace_t23_source_ae_hlil_state].dgain_q10);
    regtrace_t23_source_ae_hlil_runs = 0;
    regtrace_t23_source_ae_hlil_updates = 0;
    regtrace_t23_source_ae_hlil_drops = 0;
    regtrace_t23_source_ae_hlil_luma = 0;
    regtrace_t23_ae_hlil_bright = 0;
    regtrace_t23_source_ae_hlil_status = 0;
    memset(regtrace_t23_source_ae_scene_luma, 0,
           sizeof(regtrace_t23_source_ae_scene_luma));
    regtrace_t23_source_ae_scene_luma_valid = false;
    regtrace_t23_source_bcsh_ev_events = 0;

    printk(KERN_WARNING
           "tx_isp_t23_recovered: source AE HLIL reset target=%u+-%u interval=%u state=%u ev=%u resume=%u enabled=%u\n",
           regtrace_t23_source_ae_hlil_target,
           regtrace_t23_source_ae_hlil_deadband,
           regtrace_t23_source_ae_hlil_interval,
           regtrace_t23_source_ae_hlil_state,
           regtrace_t23_source_ae_hlil_ev,
           regtrace_t23_ae_hlil_resume ? 1U : 0U,
           regtrace_t23_source_ae_hlil ? 1U : 0U);
}

/*
 * A sensor FPS change moves VTS and with it the longest integration time.
 * Stock tisp_set_fps refreshes the AE limits from the re-synced sensor
 * attributes; the generic ladder is built from those limits, so rebuild it
 * and continue from the rung closest to the exposure value in use.  When
 * the frame gets shorter, the current rung may exceed the new limit and is
 * reprogrammed at once.
 */
static int regtrace_t23_ae_hlil_retime_force(uint32_t max_it,
                                             const char *reason, bool force)
{
    static struct regtrace_t23_ae_hlil_rung
        saved[REGTRACE_T23_AE_HLIL_MAX_STATES];
    const struct regtrace_t23_ae_hlil_rung *rung;
    unsigned long flags;
    uint32_t saved_count;
    uint32_t saved_max_it;
    uint32_t saved_initial;
    struct regtrace_t23_ae_hlil_rung old_rung;
    uint32_t old_ev;
    uint32_t state;
    int ret;

    if (!regtrace_t23_source_sensor_configured)
        return 0;   /* resolve_sensor_config reads the new limit later */
    if (!max_it || max_it < regtrace_t23_source_sensor_min_it)
        return -EINVAL;
    if (max_it == regtrace_t23_source_sensor_max_it && !force)
        return 0;

    spin_lock_irqsave(&regtrace_t23_ae_hlil_lock, flags);
    regtrace_t23_ae_hlil_retiming = true;
    spin_unlock_irqrestore(&regtrace_t23_ae_hlil_lock, flags);
    cancel_work_sync(&regtrace_t23_source_ae_hlil_work_item);

    saved_count = regtrace_t23_ae_hlil_state_count;
    saved_max_it = regtrace_t23_source_sensor_max_it;
    saved_initial = regtrace_t23_source_ae_initial_packed;
    memcpy(saved, regtrace_t23_ae_hlil_rungs, sizeof(saved));
    old_ev = regtrace_t23_source_ae_hlil_ev;
    memset(&old_rung, 0, sizeof(old_rung));
    old_rung.dgain_q10 = regtrace_t23_ae_isp_dgain_q10;
    if (saved_count) {
        state = min(regtrace_t23_source_ae_hlil_state, saved_count - 1U);
        old_rung = saved[state];
        if (!old_ev)
            old_ev = saved[state].ev_q10;
    }

    regtrace_t23_source_sensor_max_it = max_it;
    ret = regtrace_t23_ae_hlil_build_ladder();
    if (ret || !regtrace_t23_ae_hlil_state_count) {
        memcpy(regtrace_t23_ae_hlil_rungs, saved, sizeof(saved));
        regtrace_t23_ae_hlil_state_count = saved_count;
        regtrace_t23_source_sensor_max_it = saved_max_it;
        regtrace_t23_source_ae_initial_packed = saved_initial;
        if (!ret)
            ret = -EINVAL;
    } else {
        state = regtrace_t23_ae_hlil_nearest_state(old_ev >> 10);
        rung = &regtrace_t23_ae_hlil_rungs[state];
        regtrace_t23_source_ae_hlil_state = state;
        regtrace_t23_source_ae_hlil_ev = rung->ev_q10;
        /* the lifted stock AE owns the sensor exposure (source_ae_oem) */
        if (regtrace_t23_sensor_streaming && regtrace_t23_core_started &&
            !regtrace_t23_source_ae_oem)
            ret = regtrace_t23_ae_apply_rung(rung, &old_rung,
                                             "source-ae-retime");
    }

    spin_lock_irqsave(&regtrace_t23_ae_hlil_lock, flags);
    regtrace_t23_ae_hlil_pending = false;
    regtrace_t23_ae_hlil_retiming = false;
    spin_unlock_irqrestore(&regtrace_t23_ae_hlil_lock, flags);

    printk(KERN_INFO
           "tx_isp_t23_recovered: AE retime max_it=%u->%u states=%u state=%u ev=%u ret=%d reason=%s\n",
           saved_max_it, regtrace_t23_source_sensor_max_it,
           regtrace_t23_ae_hlil_state_count,
           regtrace_t23_source_ae_hlil_state,
           regtrace_t23_source_ae_hlil_ev, ret, reason ? reason : "?");
    return ret;
}

/*
 * AE limits set through the tuning API (OEM tisp_s_ae_it_max,
 * tiziano_ae_s_max_again).  The ladder is built from the smaller of the
 * sensor's own limit and the user limit; the sensor limit is kept so a
 * frame-rate change or clearing the user limit restores it.  0 = no user
 * limit.
 */
static uint32_t regtrace_t23_ae_sensor_max_it;      /* lines */
static uint32_t regtrace_t23_ae_user_max_it;        /* lines */
static uint32_t regtrace_t23_ae_sensor_max_again;   /* log2, Q16 */
static uint32_t regtrace_t23_ae_user_max_again;     /* log2, Q16 */

static uint32_t regtrace_t23_ae_cap_it(uint32_t sensor_max_it)
{
    uint32_t cap = regtrace_t23_ae_user_max_it;

    if (cap && cap < sensor_max_it &&
        cap >= regtrace_t23_source_sensor_min_it)
        return cap;
    return sensor_max_it;
}

static uint32_t regtrace_t23_ae_cap_again(uint32_t sensor_max_again)
{
    uint32_t cap = regtrace_t23_ae_user_max_again;

    return (cap && cap < sensor_max_again) ? cap : sensor_max_again;
}

/* Sensor limits as resolved from the sensor attributes. */
static void regtrace_t23_ae_sensor_limits(uint32_t max_it, uint32_t max_again)
{
    regtrace_t23_ae_sensor_max_it = max_it;
    regtrace_t23_ae_sensor_max_again = max_again;
    regtrace_t23_source_sensor_max_it = regtrace_t23_ae_cap_it(max_it);
    regtrace_t23_source_sensor_max_again =
        regtrace_t23_ae_cap_again(max_again);
}

static int regtrace_t23_ae_hlil_retime(uint32_t max_it, const char *reason)
{
    regtrace_t23_ae_sensor_max_it = max_it;
    return regtrace_t23_ae_hlil_retime_force(regtrace_t23_ae_cap_it(max_it),
                                             reason, false);
}

/* AE_IT_MAX: integration-time ceiling in lines (OEM tisp_s_ae_it_max). */
static int regtrace_t23_ae_set_max_it(uint32_t lines)
{
    uint32_t old = regtrace_t23_ae_user_max_it;
    int ret;

    if (lines && regtrace_t23_source_sensor_configured &&
        lines < regtrace_t23_source_sensor_min_it)
        return -EINVAL;
    regtrace_t23_ae_user_max_it = lines;
    if (!regtrace_t23_source_sensor_configured)
        return 0;   /* resolve_sensor_config applies it */
    ret = regtrace_t23_ae_hlil_retime_force(
        regtrace_t23_ae_cap_it(regtrace_t23_ae_sensor_max_it), "it-max",
        true);
    if (ret)
        regtrace_t23_ae_user_max_it = old;
    return ret;
}

/*
 * MAX_AGAIN: analog-gain ceiling, log2 in 1/32 steps (32 = 2x) as the OEM
 * tiziano_ae_s_max_again takes it and GetMaxAgain reports it (tisp_g_ev_attr
 * word 11).  The OEM rejects values above the sensor's maximum.
 */
static int regtrace_t23_ae_set_max_again(uint32_t log2_q5)
{
    uint32_t q16 = log2_q5 << 11;
    uint32_t old_user;
    uint32_t old_max;
    int ret;

    if (log2_q5 > 0xffffU ||
        (regtrace_t23_ae_sensor_max_again &&
         q16 > regtrace_t23_ae_sensor_max_again))
        return -EINVAL;
    old_user = regtrace_t23_ae_user_max_again;
    old_max = regtrace_t23_source_sensor_max_again;
    regtrace_t23_ae_user_max_again = q16;
    if (!regtrace_t23_source_sensor_configured)
        return 0;
    regtrace_t23_source_sensor_max_again =
        regtrace_t23_ae_cap_again(regtrace_t23_ae_sensor_max_again);
    ret = regtrace_t23_ae_hlil_retime_force(
        regtrace_t23_source_sensor_max_it, "max-again", true);
    if (ret) {
        regtrace_t23_ae_user_max_again = old_user;
        regtrace_t23_source_sensor_max_again = old_max;
    }
    return ret;
}

static uint32_t regtrace_t23_ae_get_max_again(void)
{
    return regtrace_t23_source_sensor_max_again >> 11;
}

/*
 * MAX_DGAIN: ISP digital-gain ceiling, log2 in 1/32 steps (32 = 2x), as
 * the OEM tiziano_ae_s_max_isp_dgain takes it (ae_exp_th[3] = 2^value).
 * The ladder gets ISP digital-gain rungs above the analog-gain limit up to
 * this ceiling (capped below 64x, the GIB gain field); 0 = none.
 */
static int regtrace_t23_ae_set_max_dgain(uint32_t log2_q5)
{
    uint32_t old = regtrace_t23_ae_user_max_dgain;
    int ret;

    if (log2_q5 > 0xffffU)
        return -EINVAL;
    regtrace_t23_ae_user_max_dgain = log2_q5;
    if (!regtrace_t23_source_sensor_configured || old == log2_q5)
        return 0;   /* resolve_sensor_config builds the ladder */
    ret = regtrace_t23_ae_hlil_retime_force(
        regtrace_t23_source_sensor_max_it, "max-dgain", true);
    if (ret)
        regtrace_t23_ae_user_max_dgain = old;
    return ret;
}

static uint32_t regtrace_t23_ae_get_max_dgain(void)
{
    return regtrace_t23_ae_user_max_dgain;
}

/* Anti-flicker change: rebuild the ladder for the same frame length. */
static int regtrace_t23_ae_set_antiflicker(uint32_t mode)
{
    uint32_t old = regtrace_t23_ae_flicker_mode;
    int ret;

    if (mode > 2U)
        return -EINVAL;
    regtrace_t23_ae_flicker_mode = mode;
    if (mode == old)
        return 0;
    ret = regtrace_t23_ae_hlil_retime_force(
        regtrace_t23_source_sensor_max_it, "antiflicker", true);
    if (ret)
        regtrace_t23_ae_flicker_mode = old;
    return ret;
}

/*
 * Exposure the sensor was last programmed with, for the read-back paths
 * (GetExpr/GetEVAttr/GetTotalGain and /proc/jz/isp/isp-m0).  Every sensor
 * exposure goes through regtrace_t23_call_sensor_exposure(), which reports a
 * successful write here; the values come from the ladder rung that produced
 * the packed register word.  A word that is no rung (source_ae_force_packed
 * set by hand) keeps its integration time and is reported at unity gain,
 * since the sensor's gain register cannot be mapped back to a gain.
 *
 * Writers run in process context (AE work, ioctl, stream-on); readers too,
 * but the spinlock keeps the snapshot consistent without sleeping.
 */
struct regtrace_t23_expo_live {
    uint32_t packed;
    uint32_t it;            /* lines */
    uint32_t gain_log2;     /* sensor analog gain, log2 Q16 */
    uint32_t gain_q16;      /* sensor analog gain, linear Q16 */
    bool valid;
    bool gain_known;
};

static struct regtrace_t23_expo_live regtrace_t23_expo_live;
static DEFINE_SPINLOCK(regtrace_t23_expo_lock);

static void regtrace_t23_expo_note(uint32_t packed)
{
    struct regtrace_t23_expo_live live;
    unsigned long flags;
    uint32_t state;

    memset(&live, 0, sizeof(live));
    live.packed = packed;
    live.it = packed & 0xffffU;
    live.gain_q16 = 0x10000U;
    for (state = 0; state < REGTRACE_T23_AE_HLIL_STATE_COUNT; ++state) {
        const struct regtrace_t23_ae_hlil_rung *rung =
            &regtrace_t23_ae_hlil_rungs[state];

        if (rung->packed != packed)
            continue;
        live.it = rung->it;
        live.gain_log2 = rung->gain_log2;
        live.gain_q16 = rung->gain_q16;
        live.gain_known = true;
        break;
    }
    live.valid = live.it != 0;

    spin_lock_irqsave(&regtrace_t23_expo_lock, flags);
    regtrace_t23_expo_live = live;
    spin_unlock_irqrestore(&regtrace_t23_expo_lock, flags);
}

static void regtrace_t23_expo_get_live(struct regtrace_t23_expo_live *out)
{
    unsigned long flags;

    spin_lock_irqsave(&regtrace_t23_expo_lock, flags);
    *out = regtrace_t23_expo_live;
    spin_unlock_irqrestore(&regtrace_t23_expo_lock, flags);
}
