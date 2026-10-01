/* IRQ context captures luma; sensor I/O runs on the system workqueue. */

struct regtrace_t23_ae_hlil_rung {
    uint32_t packed;
    uint32_t gain_q16;
    uint32_t ev_q10;
    uint32_t it;            /* integration time the sensor allocated, lines */
    uint32_t gain_log2;     /* sensor analog gain allocated, log2 Q16 */
};

#define REGTRACE_T23_AE_HLIL_MAX_STATES 64U
#define REGTRACE_T23_AE_HLIL_GAIN_STEPS 16U

static struct regtrace_t23_ae_hlil_rung
    regtrace_t23_ae_hlil_rungs[REGTRACE_T23_AE_HLIL_MAX_STATES];
static uint32_t regtrace_t23_ae_hlil_state_count;

#define REGTRACE_T23_AE_HLIL_STATE_COUNT regtrace_t23_ae_hlil_state_count

static int regtrace_t23_ae_hlil_append(uint32_t integration,
                                       uint32_t gain_log2_q16)
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
            regtrace_t23_ae_hlil_state_count - 1U].packed == packed)
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
    ev_q10 = (uint64_t)actual_it * gain_q16;
    ev_q10 >>= 6;
    rung->ev_q10 = ev_q10 > 0xffffffffULL ?
        0xffffffffU : (uint32_t)ev_q10;
    return 0;
}

static int regtrace_t23_ae_hlil_build_ladder(void)
{
    uint32_t integration;
    uint32_t next;
    uint32_t gain_step;
    uint32_t gain;
    uint32_t unity_states;
    int ret;

    if (!regtrace_t23_source_sensor_min_it ||
        regtrace_t23_source_sensor_min_it >
        regtrace_t23_source_sensor_max_it)
        return -EINVAL;

    regtrace_t23_ae_hlil_state_count = 0;
    integration = regtrace_t23_source_sensor_min_it;
    for (;;) {
        ret = regtrace_t23_ae_hlil_append(integration, 0U);
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
            ret = regtrace_t23_ae_hlil_append(
                regtrace_t23_source_sensor_max_it, gain);
            if (ret)
                return ret;
            if (gain > 0xffffffffU - gain_step)
                break;
        }
        ret = regtrace_t23_ae_hlil_append(
            regtrace_t23_source_sensor_max_it,
            regtrace_t23_source_sensor_max_again);
        if (ret)
            return ret;
    }

    printk(KERN_INFO
           "tx_isp_t23_recovered: generic AE ladder states=%u it=%u..%u max_again_log2=0x%x initial=0x%08x\n",
           regtrace_t23_ae_hlil_state_count,
           regtrace_t23_source_sensor_min_it,
           regtrace_t23_source_sensor_max_it,
           regtrace_t23_source_sensor_max_again,
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
static uint32_t regtrace_t23_ae_hlil_pending_snapshot;
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
    int ret = 0;

    (void)work;
    spin_lock_irqsave(&regtrace_t23_ae_hlil_lock, flags);
    luma = regtrace_t23_ae_hlil_pending_luma;
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
    next_state = state;
    lower = regtrace_t23_source_ae_hlil_target >
            regtrace_t23_source_ae_hlil_deadband ?
            regtrace_t23_source_ae_hlil_target -
            regtrace_t23_source_ae_hlil_deadband : 0U;
    upper = regtrace_t23_source_ae_hlil_target +
            regtrace_t23_source_ae_hlil_deadband;

    if (!regtrace_t23_sensor_streaming || !regtrace_t23_core_started) {
        ret = -EAGAIN;
    } else if (!luma) {
        next_state = min(state + 4U,
                         REGTRACE_T23_AE_HLIL_STATE_COUNT - 1U);
    } else if (luma < lower || luma > upper) {
        uint32_t current_ev =
            rungs[state].ev_q10 >> 10;

        desired_ev = (uint32_t)div64_u64(
            (uint64_t)current_ev * regtrace_t23_source_ae_hlil_target +
            luma / 2U, luma);
        desired_ev = clamp(desired_ev,
            rungs[0].ev_q10 >> 10,
            rungs[
                REGTRACE_T23_AE_HLIL_STATE_COUNT - 1U].ev_q10 >> 10);
        desired_state = regtrace_t23_ae_hlil_nearest_state(desired_ev);
        if (desired_state > state + 4U)
            next_state = state + 4U;
        else if (state > desired_state + 4U)
            next_state = state - 4U;
        else
            next_state = desired_state;
    }

    if (!ret && next_state != state) {
        rung = &rungs[next_state];
        ret = regtrace_t23_call_sensor_exposure(rung->packed,
                                                "source-ae-hlil");
        if (!ret && rung->gain_q16 != rungs[state].gain_q16)
            ret = regtrace_t23_source_apply_total_gain_value(
                rung->gain_q16, rung->packed >> 16, false, true);
        if (!ret) {
            regtrace_t23_source_ae_hlil_state = next_state;
            regtrace_t23_source_ae_hlil_ev = rung->ev_q10;
            regtrace_t23_source_ae_hlil_updates++;
            if (regtrace_t23_source_ccm_events &&
                regtrace_t23_source_ccm_tuning_init &&
                regtrace_t23_ccm_saturation(rung->ev_q10) !=
                regtrace_t23_source_ccm_last_saturation)
                regtrace_t23_source_ccm_commit(
                    regtrace_t23_source_ccm_runtime_ct, rung->ev_q10);
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
                   "tx_isp_t23_recovered: source AE HLIL run=%u snapshot=%u luma=%u target=%u+-%u state=%u->%u updates=%u ret=%d\n",
                   regtrace_t23_source_ae_hlil_runs, snapshot, luma,
                   regtrace_t23_source_ae_hlil_target,
                   regtrace_t23_source_ae_hlil_deadband, state, next_state,
                   regtrace_t23_source_ae_hlil_updates, ret);
        else
            printk(KERN_INFO
                   "tx_isp_t23_recovered: source AE HLIL run=%u snapshot=%u luma=%u target=%u+-%u state=%u->%u updates=%u ret=%d\n",
                   regtrace_t23_source_ae_hlil_runs, snapshot, luma,
                   regtrace_t23_source_ae_hlil_target,
                   regtrace_t23_source_ae_hlil_deadband, state, next_state,
                   regtrace_t23_source_ae_hlil_updates, ret);
    }

    spin_lock_irqsave(&regtrace_t23_ae_hlil_lock, flags);
    regtrace_t23_ae_hlil_pending = false;
    spin_unlock_irqrestore(&regtrace_t23_ae_hlil_lock, flags);
}

static void regtrace_t23_source_ae_hlil_capture(uint32_t luma,
                                               uint32_t snapshot)
{
    unsigned long flags;

    if (!regtrace_t23_source_ae_hlil)
        return;
    if (regtrace_t23_source_ae_hlil_interval &&
        snapshot % regtrace_t23_source_ae_hlil_interval)
        return;

    spin_lock_irqsave(&regtrace_t23_ae_hlil_lock, flags);
    if (regtrace_t23_ae_hlil_pending || regtrace_t23_ae_hlil_retiming ||
        regtrace_t23_ae_hlil_paused) {
        regtrace_t23_source_ae_hlil_drops++;
        spin_unlock_irqrestore(&regtrace_t23_ae_hlil_lock, flags);
        return;
    }
    regtrace_t23_ae_hlil_pending_luma = luma;
    regtrace_t23_ae_hlil_pending_snapshot = snapshot;
    regtrace_t23_ae_hlil_pending = true;
    spin_unlock_irqrestore(&regtrace_t23_ae_hlil_lock, flags);
    schedule_work(&regtrace_t23_source_ae_hlil_work_item);
}

static void regtrace_t23_source_ae_hlil_trigger(void)
{
    unsigned long flags;

    if (!regtrace_t23_source_ae_hlil || !regtrace_t23_sensor_streaming ||
        !regtrace_t23_core_started)
        return;

    spin_lock_irqsave(&regtrace_t23_ae_hlil_lock, flags);
    if (regtrace_t23_ae_hlil_pending || regtrace_t23_ae_hlil_retiming ||
        regtrace_t23_ae_hlil_paused) {
        regtrace_t23_source_ae_hlil_drops++;
        spin_unlock_irqrestore(&regtrace_t23_ae_hlil_lock, flags);
        return;
    }
    regtrace_t23_ae_hlil_pending_luma = regtrace_t23_source_ae_hlil_luma;
    regtrace_t23_ae_hlil_pending_snapshot =
        regtrace_t23_source_ae_stats_snapshots;
    regtrace_t23_ae_hlil_pending = true;
    spin_unlock_irqrestore(&regtrace_t23_ae_hlil_lock, flags);
    schedule_work(&regtrace_t23_source_ae_hlil_work_item);
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
    regtrace_t23_ae_hlil_pending_snapshot = 0;
    spin_unlock_irqrestore(&regtrace_t23_ae_hlil_lock, flags);

    regtrace_t23_source_ae_hlil_state = 0U;
    if (!REGTRACE_T23_AE_HLIL_STATE_COUNT) {
        regtrace_t23_source_ae_hlil_status = ENODEV;
        return;
    }
    for (state = 0; state < REGTRACE_T23_AE_HLIL_STATE_COUNT; ++state) {
        if (rungs[state].packed == initial_packed) {
            regtrace_t23_source_ae_hlil_state = state;
            break;
        }
    }
    if (state == REGTRACE_T23_AE_HLIL_STATE_COUNT)
        regtrace_t23_source_ae_hlil_state =
            regtrace_t23_ae_hlil_nearest_state(initial_packed & 0xffffU);
    regtrace_t23_source_ae_hlil_ev =
        rungs[regtrace_t23_source_ae_hlil_state].ev_q10;
    regtrace_t23_source_ae_hlil_runs = 0;
    regtrace_t23_source_ae_hlil_updates = 0;
    regtrace_t23_source_ae_hlil_drops = 0;
    regtrace_t23_source_ae_hlil_luma = 0;
    regtrace_t23_source_ae_hlil_status = 0;
    memset(regtrace_t23_source_ae_scene_luma, 0,
           sizeof(regtrace_t23_source_ae_scene_luma));
    regtrace_t23_source_ae_scene_luma_valid = false;
    regtrace_t23_source_bcsh_ev_events = 0;

    printk(KERN_WARNING
           "tx_isp_t23_recovered: source AE HLIL reset target=%u+-%u interval=%u state=%u ev=%u enabled=%u\n",
           regtrace_t23_source_ae_hlil_target,
           regtrace_t23_source_ae_hlil_deadband,
           regtrace_t23_source_ae_hlil_interval,
           regtrace_t23_source_ae_hlil_state,
           regtrace_t23_source_ae_hlil_ev,
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
static int regtrace_t23_ae_hlil_retime(uint32_t max_it, const char *reason)
{
    static struct regtrace_t23_ae_hlil_rung
        saved[REGTRACE_T23_AE_HLIL_MAX_STATES];
    const struct regtrace_t23_ae_hlil_rung *rung;
    unsigned long flags;
    uint32_t saved_count;
    uint32_t saved_max_it;
    uint32_t saved_initial;
    uint32_t old_packed = 0;
    uint32_t old_gain = 0;
    uint32_t old_ev;
    uint32_t state;
    int ret;

    if (!regtrace_t23_source_sensor_configured)
        return 0;   /* resolve_sensor_config reads the new limit later */
    if (!max_it || max_it < regtrace_t23_source_sensor_min_it)
        return -EINVAL;
    if (max_it == regtrace_t23_source_sensor_max_it)
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
    if (saved_count) {
        state = min(regtrace_t23_source_ae_hlil_state, saved_count - 1U);
        old_packed = saved[state].packed;
        old_gain = saved[state].gain_q16;
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
        if (regtrace_t23_sensor_streaming && regtrace_t23_core_started &&
            rung->packed != old_packed) {
            ret = regtrace_t23_call_sensor_exposure(rung->packed,
                                                    "source-ae-retime");
            if (!ret && rung->gain_q16 != old_gain)
                ret = regtrace_t23_source_apply_total_gain_value(
                    rung->gain_q16, rung->packed >> 16, false, true);
        }
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
