/*
 * RISC-V Control and Status Registers.
 *
 * Copyright (c) 2016-2017 Sagar Karandikar, sagark@eecs.berkeley.edu
 * Copyright (c) 2017-2018 SiFive, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "cpu.h"
#include "pmu.h"
#include "qemu/main-loop.h"
#include "exec/exec-all.h"

/* CSR function table public API */
void riscv_get_csr_ops(int csrno, riscv_csr_operations *ops)
{
    *ops = csr_ops[csrno & (CSR_TABLE_SIZE - 1)];
}

void riscv_set_csr_ops(int csrno, riscv_csr_operations *ops)
{
    csr_ops[csrno & (CSR_TABLE_SIZE - 1)] = *ops;
}

/* Predicates */
static RISCVException fs(CPURISCVState *env, int csrno)
{
#if !defined(CONFIG_USER_ONLY)
    /* loose check condition for fcsr in vector extension */
    if ((csrno == CSR_FCSR) && (env->misa & RVV)) {
        return RISCV_EXCP_NONE;
    }
    if (!env->debugger && !riscv_cpu_fp_enabled(env)) {
        return RISCV_EXCP_ILLEGAL_INST;
    }
#endif
    return RISCV_EXCP_NONE;
}

static RISCVException vs(CPURISCVState *env, int csrno)
{
    if (env->misa & RVV) {
        return RISCV_EXCP_NONE;
    }
    return RISCV_EXCP_ILLEGAL_INST;
}

static RISCVException mctr(CPURISCVState *env, int csrno)
{
#if !defined(CONFIG_USER_ONLY)
    CPUState *cs = env_cpu(env);
    RISCVCPU *cpu = RISCV_CPU(cs);
    int ctr_index;
    int base_csrno = CSR_MHPMCOUNTER3;

    if (riscv_cpu_is_32bit(env) && csrno >= CSR_MCYCLEH) {
        /* Offset for RV32 hpmcounternh counters */
        base_csrno += 0x80;
    }
    ctr_index = csrno - base_csrno;
    if (!cpu->cfg.ext_pmu || ctr_index > cpu->cfg.ext_pmu) {
        /* The Counters extensions is not enabled or out of range*/
        return RISCV_EXCP_ILLEGAL_INST;
    }

    return RISCV_EXCP_NONE;
#endif
}

static RISCVException mctr32(CPURISCVState *env, int csrno)
{
    if (!riscv_cpu_is_32bit(env)) {
        return RISCV_EXCP_ILLEGAL_INST;
    }

    return mctr(env, csrno);
}

static RISCVException sscof(CPURISCVState *env, int csrno)
{
 #if !defined(CONFIG_USER_ONLY)
    CPUState *cs = env_cpu(env);
    RISCVCPU *cpu = RISCV_CPU(cs);

    if (!cpu->cfg.ext_sscof) {
        return RISCV_EXCP_ILLEGAL_INST;
    }
#endif
    return RISCV_EXCP_NONE;
}

static RISCVException ctr(CPURISCVState *env, int csrno)
{
#if !defined(CONFIG_USER_ONLY)
    CPUState *cs = env_cpu(env);
    RISCVCPU *cpu = RISCV_CPU(cs);
    int ctr_index;
    int base_csrno = CSR_CYCLE;
    bool brv32 = riscv_cpu_is_32bit(env);

    if (brv32 && csrno >= CSR_CYCLEH) {
        /* Offset for RV32 hpmcounternh counters */
        base_csrno += 0x80;
    }
    ctr_index = csrno - base_csrno;

    if ((csrno >= CSR_CYCLE && csrno <= CSR_INSTRET) ||
        (csrno >= CSR_CYCLEH && csrno <= CSR_INSTRETH)) {
        goto skip_ext_pmu_check;
    }

    if ((!cpu->cfg.ext_pmu || !(cpu->pmu_avail_ctrs & BIT(ctr_index)))) {
        /* The Counters extension is not enabled or out of range*/
        return RISCV_EXCP_ILLEGAL_INST;
    }

skip_ext_pmu_check:

    if (env->priv == PRV_S) {
        switch (csrno) {
        case CSR_CYCLE:
            if (!get_field(env->mcounteren, HCOUNTEREN_CY)) {
                return RISCV_EXCP_ILLEGAL_INST;
            }
            break;
        case CSR_TIME:
            if (!get_field(env->mcounteren, HCOUNTEREN_TM)) {
                return RISCV_EXCP_ILLEGAL_INST;
            }
            break;
        case CSR_INSTRET:
            if (!get_field(env->mcounteren, HCOUNTEREN_IR)) {
                return RISCV_EXCP_ILLEGAL_INST;
            }
            break;
        case CSR_HPMCOUNTER3...CSR_HPMCOUNTER31:
            ctr_index = csrno - CSR_HPMCOUNTER3 + 3;
            if (!get_field(env->mcounteren, 1 << ctr_index)) {
                return RISCV_EXCP_ILLEGAL_INST;
            }
            break;
        }
        if (brv32) {
            switch (csrno) {
            case CSR_CYCLEH:
                if (!get_field(env->mcounteren, HCOUNTEREN_CY)) {
                    return RISCV_EXCP_ILLEGAL_INST;
                }
                break;
            case CSR_TIMEH:
                if (!get_field(env->mcounteren, HCOUNTEREN_TM)) {
                    return RISCV_EXCP_ILLEGAL_INST;
                }
                break;
            case CSR_INSTRETH:
                if (!get_field(env->mcounteren, HCOUNTEREN_IR)) {
                    return RISCV_EXCP_ILLEGAL_INST;
                }
                break;
            case CSR_HPMCOUNTER3H...CSR_HPMCOUNTER31H:
                ctr_index = csrno - CSR_HPMCOUNTER3H + 3;
                if (!get_field(env->mcounteren, 1 << ctr_index)) {
                    return RISCV_EXCP_ILLEGAL_INST;
                }
                break;
            }
        }
    }

    if (riscv_cpu_virt_enabled(env)) {
        switch (csrno) {
        case CSR_CYCLE:
            if (!get_field(env->hcounteren, HCOUNTEREN_CY) &&
                get_field(env->mcounteren, HCOUNTEREN_CY)) {
                return RISCV_EXCP_VIRT_INSTRUCTION_FAULT;
            }
            break;
        case CSR_TIME:
            if (!get_field(env->hcounteren, HCOUNTEREN_TM) &&
                get_field(env->mcounteren, HCOUNTEREN_TM)) {
                return RISCV_EXCP_VIRT_INSTRUCTION_FAULT;
            }
            break;
        case CSR_INSTRET:
            if (!get_field(env->hcounteren, HCOUNTEREN_IR) &&
                get_field(env->mcounteren, HCOUNTEREN_IR)) {
                return RISCV_EXCP_VIRT_INSTRUCTION_FAULT;
            }
            break;
        case CSR_HPMCOUNTER3...CSR_HPMCOUNTER31:
            ctr_index = csrno - CSR_HPMCOUNTER3 + 3;
            if (!get_field(env->hcounteren, 1 << ctr_index) &&
                get_field(env->mcounteren, 1 << ctr_index)) {
                return RISCV_EXCP_VIRT_INSTRUCTION_FAULT;
            }
            break;
        }
        if (brv32) {
            switch (csrno) {
            case CSR_CYCLEH:
                if (!get_field(env->hcounteren, HCOUNTEREN_CY) &&
                    get_field(env->mcounteren, HCOUNTEREN_CY)) {
                    return RISCV_EXCP_VIRT_INSTRUCTION_FAULT;
                }
                break;
            case CSR_TIMEH:
                if (!get_field(env->hcounteren, HCOUNTEREN_TM) &&
                    get_field(env->mcounteren, HCOUNTEREN_TM)) {
                    return RISCV_EXCP_VIRT_INSTRUCTION_FAULT;
                }
                break;
            case CSR_INSTRETH:
                if (!get_field(env->hcounteren, HCOUNTEREN_IR) &&
                    get_field(env->mcounteren, HCOUNTEREN_IR)) {
                    return RISCV_EXCP_VIRT_INSTRUCTION_FAULT;
                }
                break;
            case CSR_HPMCOUNTER3H...CSR_HPMCOUNTER31H:
                ctr_index = csrno - CSR_HPMCOUNTER3H + 3;
                if (!get_field(env->hcounteren, 1 << ctr_index) &&
                    get_field(env->mcounteren, 1 << ctr_index)) {
                    return RISCV_EXCP_VIRT_INSTRUCTION_FAULT;
                }
                break;
            }
        }
    }
#endif
    return RISCV_EXCP_NONE;
}

static RISCVException ctr32(CPURISCVState *env, int csrno)
{
    if (!riscv_cpu_is_32bit(env)) {
        return RISCV_EXCP_ILLEGAL_INST;
    }

    return ctr(env, csrno);
}

#if !defined(CONFIG_USER_ONLY)
static RISCVException any(CPURISCVState *env, int csrno)
{
    return RISCV_EXCP_NONE;
}

static RISCVException any32(CPURISCVState *env, int csrno)
{
    if (!riscv_cpu_is_32bit(env)) {
        return RISCV_EXCP_ILLEGAL_INST;
    }

    return any(env, csrno);

}

static RISCVException smode(CPURISCVState *env, int csrno)
{
    if (riscv_has_ext(env, RVS)) {
        return RISCV_EXCP_NONE;
    }

    return RISCV_EXCP_ILLEGAL_INST;
}

static RISCVException hmode(CPURISCVState *env, int csrno)
{
    if (riscv_has_ext(env, RVS) &&
        riscv_has_ext(env, RVH)) {
        /* Hypervisor extension is supported */
        if ((env->priv == PRV_S && !riscv_cpu_virt_enabled(env)) ||
            env->priv == PRV_M) {
            return RISCV_EXCP_NONE;
        } else {
            return RISCV_EXCP_VIRT_INSTRUCTION_FAULT;
        }
    }

    return RISCV_EXCP_ILLEGAL_INST;
}

static RISCVException hmode32(CPURISCVState *env, int csrno)
{
    if (!riscv_cpu_is_32bit(env)) {
        if (riscv_cpu_virt_enabled(env)) {
            return RISCV_EXCP_ILLEGAL_INST;
        } else {
            return RISCV_EXCP_VIRT_INSTRUCTION_FAULT;
        }
    }

    return hmode(env, csrno);

}

static RISCVException pmp(CPURISCVState *env, int csrno)
{
    if (riscv_feature(env, RISCV_FEATURE_PMP)) {
        return RISCV_EXCP_NONE;
    }

    return RISCV_EXCP_ILLEGAL_INST;
}

static RISCVException epmp(CPURISCVState *env, int csrno)
{
    if (env->priv == PRV_M && riscv_feature(env, RISCV_FEATURE_EPMP)) {
        return RISCV_EXCP_NONE;
    }

    return RISCV_EXCP_ILLEGAL_INST;
}
#endif

/* User Floating-Point CSRs */
static RISCVException read_fflags(CPURISCVState *env, int csrno,
                                  target_ulong *val)
{
    *val = riscv_cpu_get_fflags(env);
    return RISCV_EXCP_NONE;
}

static RISCVException write_fflags(CPURISCVState *env, int csrno,
                                   target_ulong val)
{
#if !defined(CONFIG_USER_ONLY)
    env->mstatus |= MSTATUS_FS;
#endif
    riscv_cpu_set_fflags(env, val & (FSR_AEXC >> FSR_AEXC_SHIFT));
    return RISCV_EXCP_NONE;
}

static RISCVException read_frm(CPURISCVState *env, int csrno,
                               target_ulong *val)
{
    *val = env->frm;
    return RISCV_EXCP_NONE;
}

static RISCVException write_frm(CPURISCVState *env, int csrno,
                                target_ulong val)
{
#if !defined(CONFIG_USER_ONLY)
    env->mstatus |= MSTATUS_FS;
#endif
    env->frm = val & (FSR_RD >> FSR_RD_SHIFT);
    return RISCV_EXCP_NONE;
}

static RISCVException read_fcsr(CPURISCVState *env, int csrno,
                                target_ulong *val)
{
    *val = (riscv_cpu_get_fflags(env) << FSR_AEXC_SHIFT)
        | (env->frm << FSR_RD_SHIFT);
    if (vs(env, csrno) >= 0) {
        *val |= (env->vxrm << FSR_VXRM_SHIFT)
                | (env->vxsat << FSR_VXSAT_SHIFT);
    }
    return RISCV_EXCP_NONE;
}

static RISCVException write_fcsr(CPURISCVState *env, int csrno,
                                 target_ulong val)
{
#if !defined(CONFIG_USER_ONLY)
    env->mstatus |= MSTATUS_FS;
#endif
    env->frm = (val & FSR_RD) >> FSR_RD_SHIFT;
    if (vs(env, csrno) >= 0) {
        env->vxrm = (val & FSR_VXRM) >> FSR_VXRM_SHIFT;
        env->vxsat = (val & FSR_VXSAT) >> FSR_VXSAT_SHIFT;
    }
    riscv_cpu_set_fflags(env, (val & FSR_AEXC) >> FSR_AEXC_SHIFT);
    return RISCV_EXCP_NONE;
}

static RISCVException read_vtype(CPURISCVState *env, int csrno,
                                 target_ulong *val)
{
    *val = env->vtype;
    return RISCV_EXCP_NONE;
}

static RISCVException read_vl(CPURISCVState *env, int csrno,
                              target_ulong *val)
{
    *val = env->vl;
    return RISCV_EXCP_NONE;
}

static RISCVException read_vxrm(CPURISCVState *env, int csrno,
                                target_ulong *val)
{
    *val = env->vxrm;
    return RISCV_EXCP_NONE;
}

static RISCVException write_vxrm(CPURISCVState *env, int csrno,
                                 target_ulong val)
{
    env->vxrm = val;
    return RISCV_EXCP_NONE;
}

static RISCVException read_vxsat(CPURISCVState *env, int csrno,
                                 target_ulong *val)
{
    *val = env->vxsat;
    return RISCV_EXCP_NONE;
}

static RISCVException write_vxsat(CPURISCVState *env, int csrno,
                                  target_ulong val)
{
    env->vxsat = val;
    return RISCV_EXCP_NONE;
}

static RISCVException read_vstart(CPURISCVState *env, int csrno,
                                  target_ulong *val)
{
    *val = env->vstart;
    return RISCV_EXCP_NONE;
}

static RISCVException write_vstart(CPURISCVState *env, int csrno,
                                   target_ulong val)
{
    env->vstart = val;
    return RISCV_EXCP_NONE;
}

/* User Timers and Counters */
target_ulong get_icount_ticks(bool brv32)
{
    int64_t val;
    target_ulong result;

#if !defined(CONFIG_USER_ONLY)
    if (icount_enabled()) {
        val = icount_get();
    } else {
        val = cpu_get_host_ticks();
    }
#else
    val = cpu_get_host_ticks();
#endif

    if (brv32) {
        result = val >> 32;
    } else {
        result = val;
    }

    return result;
}

static int read_mhpmevent(CPURISCVState *env, int csrno, target_ulong *val)
{
    int evt_index = csrno - CSR_MCOUNTINHIBIT;

    *val = env->mhpmevent_val[evt_index];

    return RISCV_EXCP_NONE;
}

static int write_mhpmevent(CPURISCVState *env, int csrno, target_ulong val)
{
    int evt_index = csrno - CSR_MCOUNTINHIBIT;

    if (!riscv_cpu_is_32bit(env)) {
        riscv_pmu_update_event_map(env, val, evt_index);
    }
    env->mhpmevent_val[evt_index] = val;

    return RISCV_EXCP_NONE;
}

static int read_mhpmeventh(CPURISCVState *env, int csrno, target_ulong *val)
{
    int evt_index = csrno - CSR_MHPMEVENT3H + 3;

    *val = env->mhpmevent_val[evt_index];

    return RISCV_EXCP_NONE;
}

static int write_mhpmeventh(CPURISCVState *env, int csrno, target_ulong val)
{
    int evt_index = csrno - CSR_MHPMEVENT3H + 3;
    uint64_t mhpmevth_val = val;
    uint64_t mhpmevt_val = env->mhpmevent_val[evt_index];

    mhpmevt_val = mhpmevt_val | (mhpmevth_val << 32);
    riscv_pmu_update_event_map(env, mhpmevt_val, evt_index);
    env->mhpmeventh_val[evt_index] = val;

    return RISCV_EXCP_NONE;
}

static int write_mhpmcounter(CPURISCVState *env, int csrno, target_ulong val)
{
    int ctr_idx = csrno - CSR_MCYCLE;
    PMUCTRState *counter = &env->pmu_ctrs[ctr_idx];

    counter->mhpmcounter_val = val;
    if (riscv_pmu_ctr_monitor_cycles(env, ctr_idx) ||
        riscv_pmu_ctr_monitor_instructions(env, ctr_idx)) {
        counter->mhpmcounter_prev = get_icount_ticks(false);
        if (ctr_idx > 2) {
            riscv_pmu_setup_timer(env, val, ctr_idx);
        }
     } else {
        /* Other counters can keep incrementing from the given value */
        counter->mhpmcounter_prev = val;
     }

     return RISCV_EXCP_NONE;
}

static int write_mhpmcounterh(CPURISCVState *env, int csrno, target_ulong val)
{
    int ctr_idx = csrno - CSR_MCYCLEH;
    PMUCTRState *counter = &env->pmu_ctrs[ctr_idx];
    uint64_t mhpmctr_val = counter->mhpmcounter_val;
    uint64_t mhpmctrh_val = val;

    counter->mhpmcounterh_val = val;
    mhpmctr_val = mhpmctr_val | (mhpmctrh_val << 32);
    if (riscv_pmu_ctr_monitor_cycles(env, ctr_idx) ||
        riscv_pmu_ctr_monitor_instructions(env, ctr_idx)) {
        counter->mhpmcounterh_prev = get_icount_ticks(false);
        if (ctr_idx > 2) {
            riscv_pmu_setup_timer(env, mhpmctr_val, ctr_idx);
        }
    } else {
        counter->mhpmcounterh_prev = val;
    }

    return RISCV_EXCP_NONE;
}

static RISCVException riscv_pmu_read_ctr(CPURISCVState *env, target_ulong *val,
                                    bool is_uh, uint32_t ctr_idx)
{
    PMUCTRState counter = env->pmu_ctrs[ctr_idx];
    target_ulong ctr_prev = is_uh ? counter.mhpmcounterh_prev :
                                    counter.mhpmcounter_prev;
    target_ulong ctr_val = is_uh ? counter.mhpmcounterh_val :
                                   counter.mhpmcounter_val;

    if (get_field(env->mcountinhibit, BIT(ctr_idx))) {
        /**
         * Counter should not increment if inhibit bit is set. We can't really
         * stop the icount counting. Just return the previous value to indicate
         * that counter was not incremented.
         */
        if (!counter.started) {
            *val = ctr_val;
            return RISCV_EXCP_NONE;
        } else {
            /* Mark that the counter has been stopped */
            counter.started = false;
        }
    }
    if (riscv_pmu_ctr_monitor_cycles(env, ctr_idx) ||
        riscv_pmu_ctr_monitor_instructions(env, ctr_idx)) {
        *val = get_icount_ticks(is_uh);
    } else {
        *val = ctr_val;
    }

    /* No need to handle the overflow here */
    *val = *val - ctr_prev + ctr_val;

    return RISCV_EXCP_NONE;
}

static int read_hpmcounter(CPURISCVState *env, int csrno, target_ulong *val)
{
    uint16_t ctr_index;

    if (env->priv == PRV_M) {
        ctr_index = csrno - CSR_MCYCLE;
    } else {
        ctr_index = csrno - CSR_CYCLE;
    }

    return riscv_pmu_read_ctr(env, val, false, ctr_index);
}

static int read_hpmcounterh(CPURISCVState *env, int csrno, target_ulong *val)
{
    uint16_t ctr_index;

    if (env->priv == PRV_M) {
        ctr_index = csrno - CSR_MCYCLEH;
    } else {
        ctr_index = csrno - CSR_CYCLEH;
    }

    return riscv_pmu_read_ctr(env, val, true, ctr_index);
}

static int read_scountovf(CPURISCVState *env, int csrno, target_ulong *val)
{
    int mhpmevt_start = CSR_MHPMEVENT3 - CSR_MCOUNTINHIBIT;
    int i;
    *val = 0;
    target_ulong *mhpm_evt_val;
    uint64_t of_bit_mask;

    if (riscv_cpu_is_32bit(env)) {
        mhpm_evt_val = env->mhpmeventh_val;
        of_bit_mask = MHPMEVENTH_BIT_OF;
    } else {
        mhpm_evt_val = env->mhpmevent_val;
        of_bit_mask = MHPMEVENT_BIT_OF;
    }

    for (i = mhpmevt_start; i < RV_MAX_MHPMEVENTS; i++) {
        if ((get_field(env->mcounteren, BIT(i))) &&
            (mhpm_evt_val[i] & of_bit_mask)) {
                    *val |= BIT(i);
            }
    }

    return RISCV_EXCP_NONE;
}

#if defined(CONFIG_USER_ONLY)
static RISCVException read_time(CPURISCVState *env, int csrno,
                                target_ulong *val)
{
    *val = cpu_get_host_ticks();
    return RISCV_EXCP_NONE;
}

static RISCVException read_timeh(CPURISCVState *env, int csrno,
                                 target_ulong *val)
{
    *val = cpu_get_host_ticks() >> 32;
    return RISCV_EXCP_NONE;
}

#else /* CONFIG_USER_ONLY */

static RISCVException read_time(CPURISCVState *env, int csrno,
                                target_ulong *val)
{
    uint64_t delta = riscv_cpu_virt_enabled(env) ? env->htimedelta : 0;

    if (!env->rdtime_fn) {
        return RISCV_EXCP_ILLEGAL_INST;
    }

    *val = env->rdtime_fn(env->rdtime_fn_arg) + delta;
    return RISCV_EXCP_NONE;
}

static RISCVException read_timeh(CPURISCVState *env, int csrno,
                                 target_ulong *val)
{
    uint64_t delta = riscv_cpu_virt_enabled(env) ? env->htimedelta : 0;

    if (!env->rdtime_fn) {
        return RISCV_EXCP_ILLEGAL_INST;
    }

    *val = (env->rdtime_fn(env->rdtime_fn_arg) + delta) >> 32;
    return RISCV_EXCP_NONE;
}

/* Machine constants */

#define M_MODE_INTERRUPTS  (MIP_MSIP | MIP_MTIP | MIP_MEIP)
#define S_MODE_INTERRUPTS  (MIP_SSIP | MIP_STIP | MIP_SEIP | MIP_LCOFIP)
#define VS_MODE_INTERRUPTS (MIP_VSSIP | MIP_VSTIP | MIP_VSEIP)

static const target_ulong delegable_ints = S_MODE_INTERRUPTS |
                                           VS_MODE_INTERRUPTS;
static const target_ulong vs_delegable_ints = VS_MODE_INTERRUPTS;
static const target_ulong all_ints = M_MODE_INTERRUPTS | S_MODE_INTERRUPTS |
                                     VS_MODE_INTERRUPTS;
#define DELEGABLE_EXCPS ((1ULL << (RISCV_EXCP_INST_ADDR_MIS)) | \
                         (1ULL << (RISCV_EXCP_INST_ACCESS_FAULT)) | \
                         (1ULL << (RISCV_EXCP_ILLEGAL_INST)) | \
                         (1ULL << (RISCV_EXCP_BREAKPOINT)) | \
                         (1ULL << (RISCV_EXCP_LOAD_ADDR_MIS)) | \
                         (1ULL << (RISCV_EXCP_LOAD_ACCESS_FAULT)) | \
                         (1ULL << (RISCV_EXCP_STORE_AMO_ADDR_MIS)) | \
                         (1ULL << (RISCV_EXCP_STORE_AMO_ACCESS_FAULT)) | \
                         (1ULL << (RISCV_EXCP_U_ECALL)) | \
                         (1ULL << (RISCV_EXCP_S_ECALL)) | \
                         (1ULL << (RISCV_EXCP_VS_ECALL)) | \
                         (1ULL << (RISCV_EXCP_M_ECALL)) | \
                         (1ULL << (RISCV_EXCP_INST_PAGE_FAULT)) | \
                         (1ULL << (RISCV_EXCP_LOAD_PAGE_FAULT)) | \
                         (1ULL << (RISCV_EXCP_STORE_PAGE_FAULT)) | \
                         (1ULL << (RISCV_EXCP_INST_GUEST_PAGE_FAULT)) | \
                         (1ULL << (RISCV_EXCP_LOAD_GUEST_ACCESS_FAULT)) | \
                         (1ULL << (RISCV_EXCP_VIRT_INSTRUCTION_FAULT)) | \
                         (1ULL << (RISCV_EXCP_STORE_GUEST_AMO_ACCESS_FAULT)))
static const target_ulong vs_delegable_excps = DELEGABLE_EXCPS &
    ~((1ULL << (RISCV_EXCP_S_ECALL)) |
      (1ULL << (RISCV_EXCP_VS_ECALL)) |
      (1ULL << (RISCV_EXCP_M_ECALL)) |
      (1ULL << (RISCV_EXCP_INST_GUEST_PAGE_FAULT)) |
      (1ULL << (RISCV_EXCP_LOAD_GUEST_ACCESS_FAULT)) |
      (1ULL << (RISCV_EXCP_VIRT_INSTRUCTION_FAULT)) |
      (1ULL << (RISCV_EXCP_STORE_GUEST_AMO_ACCESS_FAULT)));
static const target_ulong sstatus_v1_10_mask = SSTATUS_SIE | SSTATUS_SPIE |
    SSTATUS_UIE | SSTATUS_UPIE | SSTATUS_SPP | SSTATUS_FS | SSTATUS_XS |
    SSTATUS_SUM | SSTATUS_MXR;
static const target_ulong sip_writable_mask = SIP_SSIP | MIP_USIP | MIP_UEIP |
                                              SIP_LCOFIP;
static const target_ulong hip_writable_mask = MIP_VSSIP;
static const target_ulong hvip_writable_mask = MIP_VSSIP | MIP_VSTIP | MIP_VSEIP;
static const target_ulong vsip_writable_mask = MIP_VSSIP;

static const char valid_vm_1_10_32[16] = {
    [VM_1_10_MBARE] = 1,
    [VM_1_10_SV32] = 1
};

static const char valid_vm_1_10_64[16] = {
    [VM_1_10_MBARE] = 1,
    [VM_1_10_SV39] = 1,
    [VM_1_10_SV48] = 1,
    [VM_1_10_SV57] = 1
};

/* Machine Information Registers */
static RISCVException read_zero(CPURISCVState *env, int csrno,
                                target_ulong *val)
{
    *val = 0;
    return RISCV_EXCP_NONE;
}

static RISCVException read_mhartid(CPURISCVState *env, int csrno,
                                   target_ulong *val)
{
    *val = env->mhartid;
    return RISCV_EXCP_NONE;
}

/* Machine Trap Setup */
static RISCVException read_mstatus(CPURISCVState *env, int csrno,
                                   target_ulong *val)
{
    *val = env->mstatus;
    return RISCV_EXCP_NONE;
}

static int validate_vm(CPURISCVState *env, target_ulong vm)
{
    if (riscv_cpu_is_32bit(env)) {
        return valid_vm_1_10_32[vm & 0xf];
    } else {
        return valid_vm_1_10_64[vm & 0xf];
    }
}

static RISCVException write_mstatus(CPURISCVState *env, int csrno,
                                    target_ulong val)
{
    uint64_t mstatus = env->mstatus;
    uint64_t mask = 0;
    int dirty;

    /* flush tlb on mstatus fields that affect VM */
    if ((val ^ mstatus) & (MSTATUS_MXR | MSTATUS_MPP | MSTATUS_MPV |
            MSTATUS_MPRV | MSTATUS_SUM)) {
        tlb_flush(env_cpu(env));
    }
    mask = MSTATUS_SIE | MSTATUS_SPIE | MSTATUS_MIE | MSTATUS_MPIE |
        MSTATUS_SPP | MSTATUS_FS | MSTATUS_MPRV | MSTATUS_SUM |
        MSTATUS_MPP | MSTATUS_MXR | MSTATUS_TVM | MSTATUS_TSR |
        MSTATUS_TW;

    if (!riscv_cpu_is_32bit(env)) {
        /*
         * RV32: MPV and GVA are not in mstatus. The current plan is to
         * add them to mstatush. For now, we just don't support it.
         */
        mask |= MSTATUS_MPV | MSTATUS_GVA;
    }

    mstatus = (mstatus & ~mask) | (val & mask);

    dirty = ((mstatus & MSTATUS_FS) == MSTATUS_FS) |
            ((mstatus & MSTATUS_XS) == MSTATUS_XS);
    if (riscv_cpu_is_32bit(env)) {
        mstatus = set_field(mstatus, MSTATUS32_SD, dirty);
    } else {
        mstatus = set_field(mstatus, MSTATUS64_SD, dirty);
    }
    env->mstatus = mstatus;

    return RISCV_EXCP_NONE;
}

static RISCVException read_mstatush(CPURISCVState *env, int csrno,
                                    target_ulong *val)
{
    *val = env->mstatus >> 32;
    return RISCV_EXCP_NONE;
}

static RISCVException write_mstatush(CPURISCVState *env, int csrno,
                                     target_ulong val)
{
    uint64_t valh = (uint64_t)val << 32;
    uint64_t mask = MSTATUS_MPV | MSTATUS_GVA;

    if ((valh ^ env->mstatus) & (MSTATUS_MPV)) {
        tlb_flush(env_cpu(env));
    }

    env->mstatus = (env->mstatus & ~mask) | (valh & mask);

    return RISCV_EXCP_NONE;
}

static RISCVException read_misa(CPURISCVState *env, int csrno,
                                target_ulong *val)
{
    *val = env->misa;
    return RISCV_EXCP_NONE;
}

static RISCVException write_misa(CPURISCVState *env, int csrno,
                                 target_ulong val)
{
    if (!riscv_feature(env, RISCV_FEATURE_MISA)) {
        /* drop write to misa */
        return RISCV_EXCP_NONE;
    }

    /* 'I' or 'E' must be present */
    if (!(val & (RVI | RVE))) {
        /* It is not, drop write to misa */
        return RISCV_EXCP_NONE;
    }

    /* 'E' excludes all other extensions */
    if (val & RVE) {
        /* when we support 'E' we can do "val = RVE;" however
         * for now we just drop writes if 'E' is present.
         */
        return RISCV_EXCP_NONE;
    }

    /* Mask extensions that are not supported by this hart */
    val &= env->misa_mask;

    /* Mask extensions that are not supported by QEMU */
    val &= (RVI | RVE | RVM | RVA | RVF | RVD | RVC | RVS | RVU);

    /* 'D' depends on 'F', so clear 'D' if 'F' is not present */
    if ((val & RVD) && !(val & RVF)) {
        val &= ~RVD;
    }

    /* Suppress 'C' if next instruction is not aligned
     * TODO: this should check next_pc
     */
    if ((val & RVC) && (GETPC() & ~3) != 0) {
        val &= ~RVC;
    }

    /* misa.MXL writes are not supported by QEMU */
    if (riscv_cpu_is_32bit(env)) {
        val = (env->misa & MISA32_MXL) | (val & ~MISA32_MXL);
    } else {
        val = (env->misa & MISA64_MXL) | (val & ~MISA64_MXL);
    }

    /* flush translation cache */
    if (val != env->misa) {
        tb_flush(env_cpu(env));
    }

    env->misa = val;

    return RISCV_EXCP_NONE;
}

static RISCVException read_medeleg(CPURISCVState *env, int csrno,
                                   target_ulong *val)
{
    *val = env->medeleg;
    return RISCV_EXCP_NONE;
}

static RISCVException write_medeleg(CPURISCVState *env, int csrno,
                                    target_ulong val)
{
    env->medeleg = (env->medeleg & ~DELEGABLE_EXCPS) | (val & DELEGABLE_EXCPS);
    return RISCV_EXCP_NONE;
}

static RISCVException read_mideleg(CPURISCVState *env, int csrno,
                                   target_ulong *val)
{
    *val = env->mideleg;
    return RISCV_EXCP_NONE;
}

static RISCVException write_mideleg(CPURISCVState *env, int csrno,
                                    target_ulong val)
{
    env->mideleg = (env->mideleg & ~delegable_ints) | (val & delegable_ints);
    if (riscv_has_ext(env, RVH)) {
        env->mideleg |= VS_MODE_INTERRUPTS;
    }
    return RISCV_EXCP_NONE;
}

static RISCVException read_mie(CPURISCVState *env, int csrno,
                               target_ulong *val)
{
    *val = env->mie;
    return RISCV_EXCP_NONE;
}

static RISCVException write_mie(CPURISCVState *env, int csrno,
                                target_ulong val)
{
    env->mie = (env->mie & ~all_ints) | (val & all_ints);
    return RISCV_EXCP_NONE;
}

static RISCVException read_mtvec(CPURISCVState *env, int csrno,
                                 target_ulong *val)
{
    *val = env->mtvec;
    return RISCV_EXCP_NONE;
}

static RISCVException write_mtvec(CPURISCVState *env, int csrno,
                                  target_ulong val)
{
    /* bits [1:0] encode mode; 0 = direct, 1 = vectored, 2 >= reserved */
    if ((val & 3) < 2) {
        env->mtvec = val;
    } else {
        qemu_log_mask(LOG_UNIMP, "CSR_MTVEC: reserved mode not supported\n");
    }
    return RISCV_EXCP_NONE;
}

static RISCVException read_mcountinhibit(CPURISCVState *env, int csrno,
                                         target_ulong *val)
{
    if (env->priv_ver < PRIV_VERSION_1_11_0) {
        return -RISCV_EXCP_ILLEGAL_INST;
    }

    *val = env->mcountinhibit;
    return RISCV_EXCP_NONE;
}

static RISCVException write_mcountinhibit(CPURISCVState *env, int csrno,
                                          target_ulong val)
{
    int cidx;
    PMUCTRState *counter;

    if (env->priv_ver < PRIV_VERSION_1_11_0) {
        return -RISCV_EXCP_ILLEGAL_INST;
    }

    env->mcountinhibit = val;

    /* Check if any other counter is also monitoring cycles/instructions */
    for (cidx = 0; cidx < RV_MAX_MHPMCOUNTERS; cidx++) {
        if (!get_field(env->mcountinhibit, BIT(cidx))) {
            counter = &env->pmu_ctrs[cidx];
            counter->started = true;
        }
    }

    return RISCV_EXCP_NONE;
}

static RISCVException read_mcounteren(CPURISCVState *env, int csrno,
                                      target_ulong *val)
{
    *val = env->mcounteren;
    return RISCV_EXCP_NONE;
}

static RISCVException write_mcounteren(CPURISCVState *env, int csrno,
                                       target_ulong val)
{
    env->mcounteren = val;
    return RISCV_EXCP_NONE;
}

/* Machine Trap Handling */
static RISCVException read_mscratch(CPURISCVState *env, int csrno,
                                    target_ulong *val)
{
    *val = env->mscratch;
    return RISCV_EXCP_NONE;
}

static RISCVException write_mscratch(CPURISCVState *env, int csrno,
                                     target_ulong val)
{
    env->mscratch = val;
    return RISCV_EXCP_NONE;
}

static RISCVException read_mepc(CPURISCVState *env, int csrno,
                                     target_ulong *val)
{
    *val = env->mepc;
    return RISCV_EXCP_NONE;
}

static RISCVException write_mepc(CPURISCVState *env, int csrno,
                                     target_ulong val)
{
    env->mepc = val;
    return RISCV_EXCP_NONE;
}

static RISCVException read_mcause(CPURISCVState *env, int csrno,
                                     target_ulong *val)
{
    *val = env->mcause;
    return RISCV_EXCP_NONE;
}

static RISCVException write_mcause(CPURISCVState *env, int csrno,
                                     target_ulong val)
{
    env->mcause = val;
    return RISCV_EXCP_NONE;
}

static RISCVException read_mtval(CPURISCVState *env, int csrno,
                                 target_ulong *val)
{
    *val = env->mtval;
    return RISCV_EXCP_NONE;
}

static RISCVException write_mtval(CPURISCVState *env, int csrno,
                                  target_ulong val)
{
    env->mtval = val;
    return RISCV_EXCP_NONE;
}

static RISCVException rmw_mip(CPURISCVState *env, int csrno,
                              target_ulong *ret_value,
                              target_ulong new_value, target_ulong write_mask)
{
    RISCVCPU *cpu = env_archcpu(env);
    /* Allow software control of delegable interrupts not claimed by hardware */
    target_ulong mask = write_mask & delegable_ints & ~env->miclaim;
    uint32_t old_mip;

    if (mask) {
        old_mip = riscv_cpu_update_mip(cpu, mask, (new_value & mask));
    } else {
        old_mip = env->mip;
    }

    if (ret_value) {
        *ret_value = old_mip;
    }

    return RISCV_EXCP_NONE;
}

/* Supervisor Trap Setup */
static RISCVException read_sstatus(CPURISCVState *env, int csrno,
                                   target_ulong *val)
{
    target_ulong mask = (sstatus_v1_10_mask);

    if (riscv_cpu_is_32bit(env)) {
        mask |= SSTATUS32_SD;
    } else {
        mask |= SSTATUS64_SD;
    }

    *val = env->mstatus & mask;
    return RISCV_EXCP_NONE;
}

static RISCVException write_sstatus(CPURISCVState *env, int csrno,
                                    target_ulong val)
{
    target_ulong mask = (sstatus_v1_10_mask);
    target_ulong newval = (env->mstatus & ~mask) | (val & mask);
    return write_mstatus(env, CSR_MSTATUS, newval);
}

static RISCVException read_vsie(CPURISCVState *env, int csrno,
                                target_ulong *val)
{
    /* Shift the VS bits to their S bit location in vsie */
    *val = (env->mie & env->hideleg & VS_MODE_INTERRUPTS) >> 1;
    return RISCV_EXCP_NONE;
}

static RISCVException read_sie(CPURISCVState *env, int csrno,
                               target_ulong *val)
{
    if (riscv_cpu_virt_enabled(env)) {
        read_vsie(env, CSR_VSIE, val);
    } else {
        *val = env->mie & env->mideleg;
    }
    return RISCV_EXCP_NONE;
}

static RISCVException write_vsie(CPURISCVState *env, int csrno,
                                 target_ulong val)
{
    /* Shift the S bits to their VS bit location in mie */
    target_ulong newval = (env->mie & ~VS_MODE_INTERRUPTS) |
                          ((val << 1) & env->hideleg & VS_MODE_INTERRUPTS);
    return write_mie(env, CSR_MIE, newval);
}

static int write_sie(CPURISCVState *env, int csrno, target_ulong val)
{
    if (riscv_cpu_virt_enabled(env)) {
        write_vsie(env, CSR_VSIE, val);
    } else {
        target_ulong newval = (env->mie & ~S_MODE_INTERRUPTS) |
                              (val & S_MODE_INTERRUPTS);
        write_mie(env, CSR_MIE, newval);
    }

    return RISCV_EXCP_NONE;
}

static RISCVException read_stvec(CPURISCVState *env, int csrno,
                                 target_ulong *val)
{
    *val = env->stvec;
    return RISCV_EXCP_NONE;
}

static RISCVException write_stvec(CPURISCVState *env, int csrno,
                                  target_ulong val)
{
    /* bits [1:0] encode mode; 0 = direct, 1 = vectored, 2 >= reserved */
    if ((val & 3) < 2) {
        env->stvec = val;
    } else {
        qemu_log_mask(LOG_UNIMP, "CSR_STVEC: reserved mode not supported\n");
    }
    return RISCV_EXCP_NONE;
}

static RISCVException read_scounteren(CPURISCVState *env, int csrno,
                                      target_ulong *val)
{
    *val = env->scounteren;
    return RISCV_EXCP_NONE;
}

static RISCVException write_scounteren(CPURISCVState *env, int csrno,
                                       target_ulong val)
{
    env->scounteren = val;
    return RISCV_EXCP_NONE;
}

/* Supervisor Trap Handling */
static RISCVException read_sscratch(CPURISCVState *env, int csrno,
                                    target_ulong *val)
{
    *val = env->sscratch;
    return RISCV_EXCP_NONE;
}

static RISCVException write_sscratch(CPURISCVState *env, int csrno,
                                     target_ulong val)
{
    env->sscratch = val;
    return RISCV_EXCP_NONE;
}

static RISCVException read_sepc(CPURISCVState *env, int csrno,
                                target_ulong *val)
{
    *val = env->sepc;
    return RISCV_EXCP_NONE;
}

static RISCVException write_sepc(CPURISCVState *env, int csrno,
                                 target_ulong val)
{
    env->sepc = val;
    return RISCV_EXCP_NONE;
}

static RISCVException read_scause(CPURISCVState *env, int csrno,
                                  target_ulong *val)
{
    *val = env->scause;
    return RISCV_EXCP_NONE;
}

static RISCVException write_scause(CPURISCVState *env, int csrno,
                                   target_ulong val)
{
    env->scause = val;
    return RISCV_EXCP_NONE;
}

static RISCVException read_stval(CPURISCVState *env, int csrno,
                                 target_ulong *val)
{
    *val = env->stval;
    return RISCV_EXCP_NONE;
}

static RISCVException write_stval(CPURISCVState *env, int csrno,
                                  target_ulong val)
{
    env->stval = val;
    return RISCV_EXCP_NONE;
}

static RISCVException rmw_vsip(CPURISCVState *env, int csrno,
                               target_ulong *ret_value,
                               target_ulong new_value, target_ulong write_mask)
{
    /* Shift the S bits to their VS bit location in mip */
    int ret = rmw_mip(env, 0, ret_value, new_value << 1,
                      (write_mask << 1) & vsip_writable_mask & env->hideleg);
    *ret_value &= VS_MODE_INTERRUPTS;
    /* Shift the VS bits to their S bit location in vsip */
    *ret_value >>= 1;
    return ret;
}

static RISCVException rmw_sip(CPURISCVState *env, int csrno,
                              target_ulong *ret_value,
                              target_ulong new_value, target_ulong write_mask)
{
    int ret;

    if (riscv_cpu_virt_enabled(env)) {
        ret = rmw_vsip(env, CSR_VSIP, ret_value, new_value, write_mask);
    } else {
        ret = rmw_mip(env, CSR_MSTATUS, ret_value, new_value,
                      write_mask & env->mideleg & sip_writable_mask);
    }

    *ret_value &= env->mideleg;
    return ret;
}

/* Supervisor Protection and Translation */
static RISCVException read_satp(CPURISCVState *env, int csrno,
                                target_ulong *val)
{
    if (!riscv_feature(env, RISCV_FEATURE_MMU)) {
        *val = 0;
        return RISCV_EXCP_NONE;
    }

    if (env->priv == PRV_S && get_field(env->mstatus, MSTATUS_TVM)) {
        return RISCV_EXCP_ILLEGAL_INST;
    } else {
        *val = env->satp;
    }

    return RISCV_EXCP_NONE;
}

static RISCVException write_satp(CPURISCVState *env, int csrno,
                                 target_ulong val)
{
    int vm, mask, asid;

    if (!riscv_feature(env, RISCV_FEATURE_MMU)) {
        return RISCV_EXCP_NONE;
    }

    if (riscv_cpu_is_32bit(env)) {
        vm = validate_vm(env, get_field(val, SATP32_MODE));
        mask = (val ^ env->satp) & (SATP32_MODE | SATP32_ASID | SATP32_PPN);
        asid = (val ^ env->satp) & SATP32_ASID;
    } else {
        vm = validate_vm(env, get_field(val, SATP64_MODE));
        mask = (val ^ env->satp) & (SATP64_MODE | SATP64_ASID | SATP64_PPN);
        asid = (val ^ env->satp) & SATP64_ASID;
    }

    if (vm && mask) {
        if (env->priv == PRV_S && get_field(env->mstatus, MSTATUS_TVM)) {
            return RISCV_EXCP_ILLEGAL_INST;
        } else {
            if (asid) {
                tlb_flush(env_cpu(env));
            }
            env->satp = val;
        }
    }
    return RISCV_EXCP_NONE;
}

/* Hypervisor Extensions */
static RISCVException read_hstatus(CPURISCVState *env, int csrno,
                                   target_ulong *val)
{
    *val = env->hstatus;
    if (!riscv_cpu_is_32bit(env)) {
        /* We only support 64-bit VSXL */
        *val = set_field(*val, HSTATUS_VSXL, 2);
    }
    /* We only support little endian */
    *val = set_field(*val, HSTATUS_VSBE, 0);
    return RISCV_EXCP_NONE;
}

static RISCVException write_hstatus(CPURISCVState *env, int csrno,
                                    target_ulong val)
{
    env->hstatus = val;
    if (!riscv_cpu_is_32bit(env) && get_field(val, HSTATUS_VSXL) != 2) {
        qemu_log_mask(LOG_UNIMP, "QEMU does not support mixed HSXLEN options.");
    }
    if (get_field(val, HSTATUS_VSBE) != 0) {
        qemu_log_mask(LOG_UNIMP, "QEMU does not support big endian guests.");
    }
    return RISCV_EXCP_NONE;
}

static RISCVException read_hedeleg(CPURISCVState *env, int csrno,
                                   target_ulong *val)
{
    *val = env->hedeleg;
    return RISCV_EXCP_NONE;
}

static RISCVException write_hedeleg(CPURISCVState *env, int csrno,
                                    target_ulong val)
{
    env->hedeleg = val & vs_delegable_excps;
    return RISCV_EXCP_NONE;
}

static RISCVException read_hideleg(CPURISCVState *env, int csrno,
                                   target_ulong *val)
{
    *val = env->hideleg;
    return RISCV_EXCP_NONE;
}

static RISCVException write_hideleg(CPURISCVState *env, int csrno,
                                    target_ulong val)
{
    env->hideleg = val & vs_delegable_ints;
    return RISCV_EXCP_NONE;
}

static RISCVException rmw_hvip(CPURISCVState *env, int csrno,
                               target_ulong *ret_value,
                               target_ulong new_value, target_ulong write_mask)
{
    int ret = rmw_mip(env, 0, ret_value, new_value,
                      write_mask & hvip_writable_mask);

    *ret_value &= hvip_writable_mask;

    return ret;
}

static RISCVException rmw_hip(CPURISCVState *env, int csrno,
                              target_ulong *ret_value,
                              target_ulong new_value, target_ulong write_mask)
{
    int ret = rmw_mip(env, 0, ret_value, new_value,
                      write_mask & hip_writable_mask);

    *ret_value &= hip_writable_mask;

    return ret;
}

static RISCVException read_hie(CPURISCVState *env, int csrno,
                               target_ulong *val)
{
    *val = env->mie & VS_MODE_INTERRUPTS;
    return RISCV_EXCP_NONE;
}

static RISCVException write_hie(CPURISCVState *env, int csrno,
                                target_ulong val)
{
    target_ulong newval = (env->mie & ~VS_MODE_INTERRUPTS) | (val & VS_MODE_INTERRUPTS);
    return write_mie(env, CSR_MIE, newval);
}

static RISCVException read_hcounteren(CPURISCVState *env, int csrno,
                                      target_ulong *val)
{
    *val = env->hcounteren;
    return RISCV_EXCP_NONE;
}

static RISCVException write_hcounteren(CPURISCVState *env, int csrno,
                                       target_ulong val)
{
    env->hcounteren = val;
    return RISCV_EXCP_NONE;
}

static RISCVException read_hgeie(CPURISCVState *env, int csrno,
                                 target_ulong *val)
{
    qemu_log_mask(LOG_UNIMP, "No support for a non-zero GEILEN.");
    return RISCV_EXCP_NONE;
}

static RISCVException write_hgeie(CPURISCVState *env, int csrno,
                                  target_ulong val)
{
    qemu_log_mask(LOG_UNIMP, "No support for a non-zero GEILEN.");
    return RISCV_EXCP_NONE;
}

static RISCVException read_htval(CPURISCVState *env, int csrno,
                                 target_ulong *val)
{
    *val = env->htval;
    return RISCV_EXCP_NONE;
}

static RISCVException write_htval(CPURISCVState *env, int csrno,
                                  target_ulong val)
{
    env->htval = val;
    return RISCV_EXCP_NONE;
}

static RISCVException read_htinst(CPURISCVState *env, int csrno,
                                  target_ulong *val)
{
    *val = env->htinst;
    return RISCV_EXCP_NONE;
}

static RISCVException write_htinst(CPURISCVState *env, int csrno,
                                   target_ulong val)
{
    return RISCV_EXCP_NONE;
}

static RISCVException read_hgeip(CPURISCVState *env, int csrno,
                                 target_ulong *val)
{
    qemu_log_mask(LOG_UNIMP, "No support for a non-zero GEILEN.");
    return RISCV_EXCP_NONE;
}

static RISCVException write_hgeip(CPURISCVState *env, int csrno,
                                  target_ulong val)
{
    qemu_log_mask(LOG_UNIMP, "No support for a non-zero GEILEN.");
    return RISCV_EXCP_NONE;
}

static RISCVException read_hgatp(CPURISCVState *env, int csrno,
                                 target_ulong *val)
{
    *val = env->hgatp;
    return RISCV_EXCP_NONE;
}

static RISCVException write_hgatp(CPURISCVState *env, int csrno,
                                  target_ulong val)
{
    env->hgatp = val;
    return RISCV_EXCP_NONE;
}

static RISCVException read_htimedelta(CPURISCVState *env, int csrno,
                                      target_ulong *val)
{
    if (!env->rdtime_fn) {
        return RISCV_EXCP_ILLEGAL_INST;
    }

    *val = env->htimedelta;
    return RISCV_EXCP_NONE;
}

static RISCVException write_htimedelta(CPURISCVState *env, int csrno,
                                       target_ulong val)
{
    if (!env->rdtime_fn) {
        return RISCV_EXCP_ILLEGAL_INST;
    }

    if (riscv_cpu_is_32bit(env)) {
        env->htimedelta = deposit64(env->htimedelta, 0, 32, (uint64_t)val);
    } else {
        env->htimedelta = val;
    }
    return RISCV_EXCP_NONE;
}

static RISCVException read_htimedeltah(CPURISCVState *env, int csrno,
                                       target_ulong *val)
{
    if (!env->rdtime_fn) {
        return RISCV_EXCP_ILLEGAL_INST;
    }

    *val = env->htimedelta >> 32;
    return RISCV_EXCP_NONE;
}

static RISCVException write_htimedeltah(CPURISCVState *env, int csrno,
                                        target_ulong val)
{
    if (!env->rdtime_fn) {
        return RISCV_EXCP_ILLEGAL_INST;
    }

    env->htimedelta = deposit64(env->htimedelta, 32, 32, (uint64_t)val);
    return RISCV_EXCP_NONE;
}

/* Virtual CSR Registers */
static RISCVException read_vsstatus(CPURISCVState *env, int csrno,
                                    target_ulong *val)
{
    *val = env->vsstatus;
    return RISCV_EXCP_NONE;
}

static RISCVException write_vsstatus(CPURISCVState *env, int csrno,
                                     target_ulong val)
{
    uint64_t mask = (target_ulong)-1;
    env->vsstatus = (env->vsstatus & ~mask) | (uint64_t)val;
    return RISCV_EXCP_NONE;
}

static int read_vstvec(CPURISCVState *env, int csrno, target_ulong *val)
{
    *val = env->vstvec;
    return RISCV_EXCP_NONE;
}

static RISCVException write_vstvec(CPURISCVState *env, int csrno,
                                   target_ulong val)
{
    env->vstvec = val;
    return RISCV_EXCP_NONE;
}

static RISCVException read_vsscratch(CPURISCVState *env, int csrno,
                                     target_ulong *val)
{
    *val = env->vsscratch;
    return RISCV_EXCP_NONE;
}

static RISCVException write_vsscratch(CPURISCVState *env, int csrno,
                                      target_ulong val)
{
    env->vsscratch = val;
    return RISCV_EXCP_NONE;
}

static RISCVException read_vsepc(CPURISCVState *env, int csrno,
                                 target_ulong *val)
{
    *val = env->vsepc;
    return RISCV_EXCP_NONE;
}

static RISCVException write_vsepc(CPURISCVState *env, int csrno,
                                  target_ulong val)
{
    env->vsepc = val;
    return RISCV_EXCP_NONE;
}

static RISCVException read_vscause(CPURISCVState *env, int csrno,
                                   target_ulong *val)
{
    *val = env->vscause;
    return RISCV_EXCP_NONE;
}

static RISCVException write_vscause(CPURISCVState *env, int csrno,
                                    target_ulong val)
{
    env->vscause = val;
    return RISCV_EXCP_NONE;
}

static RISCVException read_vstval(CPURISCVState *env, int csrno,
                                  target_ulong *val)
{
    *val = env->vstval;
    return RISCV_EXCP_NONE;
}

static RISCVException write_vstval(CPURISCVState *env, int csrno,
                                   target_ulong val)
{
    env->vstval = val;
    return RISCV_EXCP_NONE;
}

static RISCVException read_vsatp(CPURISCVState *env, int csrno,
                                 target_ulong *val)
{
    *val = env->vsatp;
    return RISCV_EXCP_NONE;
}

static RISCVException write_vsatp(CPURISCVState *env, int csrno,
                                  target_ulong val)
{
    env->vsatp = val;
    return RISCV_EXCP_NONE;
}

static RISCVException read_mtval2(CPURISCVState *env, int csrno,
                                  target_ulong *val)
{
    *val = env->mtval2;
    return RISCV_EXCP_NONE;
}

static RISCVException write_mtval2(CPURISCVState *env, int csrno,
                                   target_ulong val)
{
    env->mtval2 = val;
    return RISCV_EXCP_NONE;
}

static RISCVException read_mtinst(CPURISCVState *env, int csrno,
                                  target_ulong *val)
{
    *val = env->mtinst;
    return RISCV_EXCP_NONE;
}

static RISCVException write_mtinst(CPURISCVState *env, int csrno,
                                   target_ulong val)
{
    env->mtinst = val;
    return RISCV_EXCP_NONE;
}

/* Physical Memory Protection */
static RISCVException read_mseccfg(CPURISCVState *env, int csrno,
                                   target_ulong *val)
{
    *val = mseccfg_csr_read(env);
    return RISCV_EXCP_NONE;
}

static RISCVException write_mseccfg(CPURISCVState *env, int csrno,
                         target_ulong val)
{
    mseccfg_csr_write(env, val);
    return RISCV_EXCP_NONE;
}

static RISCVException read_pmpcfg(CPURISCVState *env, int csrno,
                                  target_ulong *val)
{
    *val = pmpcfg_csr_read(env, csrno - CSR_PMPCFG0);
    return RISCV_EXCP_NONE;
}

static RISCVException write_pmpcfg(CPURISCVState *env, int csrno,
                                   target_ulong val)
{
    pmpcfg_csr_write(env, csrno - CSR_PMPCFG0, val);
    return RISCV_EXCP_NONE;
}

static RISCVException read_pmpaddr(CPURISCVState *env, int csrno,
                                   target_ulong *val)
{
    *val = pmpaddr_csr_read(env, csrno - CSR_PMPADDR0);
    return RISCV_EXCP_NONE;
}

static RISCVException write_pmpaddr(CPURISCVState *env, int csrno,
                                    target_ulong val)
{
    pmpaddr_csr_write(env, csrno - CSR_PMPADDR0, val);
    return RISCV_EXCP_NONE;
}

#endif

/*
 * riscv_csrrw - read and/or update control and status register
 *
 * csrr   <->  riscv_csrrw(env, csrno, ret_value, 0, 0);
 * csrrw  <->  riscv_csrrw(env, csrno, ret_value, value, -1);
 * csrrs  <->  riscv_csrrw(env, csrno, ret_value, -1, value);
 * csrrc  <->  riscv_csrrw(env, csrno, ret_value, 0, value);
 */

RISCVException riscv_csrrw(CPURISCVState *env, int csrno,
                           target_ulong *ret_value,
                           target_ulong new_value, target_ulong write_mask)
{
    RISCVException ret;
    target_ulong old_value;
    RISCVCPU *cpu = env_archcpu(env);

    /* check privileges and return -1 if check fails */
#if !defined(CONFIG_USER_ONLY)
    int effective_priv = env->priv;
    int read_only = get_field(csrno, 0xC00) == 3;

    if (riscv_has_ext(env, RVH) &&
        env->priv == PRV_S &&
        !riscv_cpu_virt_enabled(env)) {
        /*
         * We are in S mode without virtualisation, therefore we are in HS Mode.
         * Add 1 to the effective privledge level to allow us to access the
         * Hypervisor CSRs.
         */
        effective_priv++;
    }

    if ((write_mask && read_only) ||
        (!env->debugger && (effective_priv < get_field(csrno, 0x300)))) {
        return RISCV_EXCP_ILLEGAL_INST;
    }
#endif

    /* ensure the CSR extension is enabled. */
    if (!cpu->cfg.ext_icsr) {
        return RISCV_EXCP_ILLEGAL_INST;
    }

    /* check predicate */
    if (!csr_ops[csrno].predicate) {
        return RISCV_EXCP_ILLEGAL_INST;
    }
    ret = csr_ops[csrno].predicate(env, csrno);
    if (ret != RISCV_EXCP_NONE) {
        return ret;
    }

    /* execute combined read/write operation if it exists */
    if (csr_ops[csrno].op) {
        return csr_ops[csrno].op(env, csrno, ret_value, new_value, write_mask);
    }

    /* if no accessor exists then return failure */
    if (!csr_ops[csrno].read) {
        return RISCV_EXCP_ILLEGAL_INST;
    }
    /* read old value */
    ret = csr_ops[csrno].read(env, csrno, &old_value);
    if (ret != RISCV_EXCP_NONE) {
        return ret;
    }

    /* write value if writable and write mask set, otherwise drop writes */
    if (write_mask) {
        new_value = (old_value & ~write_mask) | (new_value & write_mask);
        if (csr_ops[csrno].write) {
            ret = csr_ops[csrno].write(env, csrno, new_value);
            if (ret != RISCV_EXCP_NONE) {
                return ret;
            }
        }
    }

    /* return old value */
    if (ret_value) {
        *ret_value = old_value;
    }

    return RISCV_EXCP_NONE;
}

/*
 * Debugger support.  If not in user mode, set env->debugger before the
 * riscv_csrrw call and clear it after the call.
 */
RISCVException riscv_csrrw_debug(CPURISCVState *env, int csrno,
                                 target_ulong *ret_value,
                                 target_ulong new_value,
                                 target_ulong write_mask)
{
    RISCVException ret;
#if !defined(CONFIG_USER_ONLY)
    env->debugger = true;
#endif
    ret = riscv_csrrw(env, csrno, ret_value, new_value, write_mask);
#if !defined(CONFIG_USER_ONLY)
    env->debugger = false;
#endif
    return ret;
}

/* Control and Status Register function table */
riscv_csr_operations csr_ops[CSR_TABLE_SIZE] = {
    /* User Floating-Point CSRs */
    [CSR_FFLAGS]   = { "fflags",   fs,     read_fflags,  write_fflags },
    [CSR_FRM]      = { "frm",      fs,     read_frm,     write_frm    },
    [CSR_FCSR]     = { "fcsr",     fs,     read_fcsr,    write_fcsr   },
    /* Vector CSRs */
    [CSR_VSTART]   = { "vstart",   vs,     read_vstart,  write_vstart },
    [CSR_VXSAT]    = { "vxsat",    vs,     read_vxsat,   write_vxsat  },
    [CSR_VXRM]     = { "vxrm",     vs,     read_vxrm,    write_vxrm   },
    [CSR_VL]       = { "vl",       vs,     read_vl                    },
    [CSR_VTYPE]    = { "vtype",    vs,     read_vtype                 },
    /* User Timers and Counters */
    [CSR_CYCLE]    = { "cycle",    ctr,    read_hpmcounter  },
    [CSR_INSTRET]  = { "instret",  ctr,    read_hpmcounter  },
    [CSR_CYCLEH]   = { "cycleh",   ctr32,  read_hpmcounterh },
    [CSR_INSTRETH] = { "instreth", ctr32,  read_hpmcounterh },

    /*
     * In privileged mode, the monitor will have to emulate TIME CSRs only if
     * rdtime callback is not provided by machine/platform emulation.
     */
    [CSR_TIME]  = { "time",  ctr,   read_time  },
    [CSR_TIMEH] = { "timeh", ctr32, read_timeh },

#if !defined(CONFIG_USER_ONLY)
    /* Machine Timers and Counters */
    [CSR_MCYCLE]    = { "mcycle",    any,   read_hpmcounter, write_mhpmcounter},
    [CSR_MINSTRET]  = { "minstret",  any,   read_hpmcounter, write_mhpmcounter},
    [CSR_MCYCLEH]   = { "mcycleh",   any32, read_hpmcounterh, write_mhpmcounterh},
    [CSR_MINSTRETH] = { "minstreth", any32, read_hpmcounterh, write_mhpmcounterh},

    /* Machine Information Registers */
    [CSR_MVENDORID] = { "mvendorid", any,   read_zero    },
    [CSR_MARCHID]   = { "marchid",   any,   read_zero    },
    [CSR_MIMPID]    = { "mimpid",    any,   read_zero    },
    [CSR_MHARTID]   = { "mhartid",   any,   read_mhartid },

    /* Machine Trap Setup */
    [CSR_MSTATUS]     = { "mstatus",    any,   read_mstatus,     write_mstatus     },
    [CSR_MISA]        = { "misa",       any,   read_misa,        write_misa        },
    [CSR_MIDELEG]     = { "mideleg",    any,   read_mideleg,     write_mideleg     },
    [CSR_MEDELEG]     = { "medeleg",    any,   read_medeleg,     write_medeleg     },
    [CSR_MIE]         = { "mie",        any,   read_mie,         write_mie         },
    [CSR_MTVEC]       = { "mtvec",      any,   read_mtvec,       write_mtvec       },
    [CSR_MCOUNTEREN]  = { "mcounteren", any,   read_mcounteren,  write_mcounteren  },

    [CSR_MSTATUSH]    = { "mstatush",   any32, read_mstatush,    write_mstatush    },

    /* Machine Trap Handling */
    [CSR_MSCRATCH] = { "mscratch", any,  read_mscratch, write_mscratch },
    [CSR_MEPC]     = { "mepc",     any,  read_mepc,     write_mepc     },
    [CSR_MCAUSE]   = { "mcause",   any,  read_mcause,   write_mcause   },
    [CSR_MTVAL]    = { "mtval",    any,  read_mtval,    write_mtval    },
    [CSR_MIP]      = { "mip",      any,  NULL,    NULL, rmw_mip        },

    /* Supervisor Trap Setup */
    [CSR_SSTATUS]    = { "sstatus",    smode, read_sstatus,    write_sstatus    },
    [CSR_SIE]        = { "sie",        smode, read_sie,        write_sie        },
    [CSR_STVEC]      = { "stvec",      smode, read_stvec,      write_stvec      },
    [CSR_SCOUNTEREN] = { "scounteren", smode, read_scounteren, write_scounteren },

    /* Supervisor Trap Handling */
    [CSR_SSCRATCH] = { "sscratch", smode, read_sscratch, write_sscratch },
    [CSR_SEPC]     = { "sepc",     smode, read_sepc,     write_sepc     },
    [CSR_SCAUSE]   = { "scause",   smode, read_scause,   write_scause   },
    [CSR_STVAL]    = { "stval",    smode, read_stval,   write_stval   },
    [CSR_SIP]      = { "sip",      smode, NULL,    NULL, rmw_sip        },

    /* Supervisor Protection and Translation */
    [CSR_SATP]     = { "satp",     smode, read_satp,    write_satp      },

    [CSR_HSTATUS]     = { "hstatus",     hmode,   read_hstatus,     write_hstatus     },
    [CSR_HEDELEG]     = { "hedeleg",     hmode,   read_hedeleg,     write_hedeleg     },
    [CSR_HIDELEG]     = { "hideleg",     hmode,   read_hideleg,     write_hideleg     },
    [CSR_HVIP]        = { "hvip",        hmode,   NULL,   NULL,     rmw_hvip          },
    [CSR_HIP]         = { "hip",         hmode,   NULL,   NULL,     rmw_hip           },
    [CSR_HIE]         = { "hie",         hmode,   read_hie,         write_hie         },
    [CSR_HCOUNTEREN]  = { "hcounteren",  hmode,   read_hcounteren,  write_hcounteren  },
    [CSR_HGEIE]       = { "hgeie",       hmode,   read_hgeie,       write_hgeie       },
    [CSR_HTVAL]       = { "htval",       hmode,   read_htval,       write_htval       },
    [CSR_HTINST]      = { "htinst",      hmode,   read_htinst,      write_htinst      },
    [CSR_HGEIP]       = { "hgeip",       hmode,   read_hgeip,       write_hgeip       },
    [CSR_HGATP]       = { "hgatp",       hmode,   read_hgatp,       write_hgatp       },
    [CSR_HTIMEDELTA]  = { "htimedelta",  hmode,   read_htimedelta,  write_htimedelta  },
    [CSR_HTIMEDELTAH] = { "htimedeltah", hmode32, read_htimedeltah, write_htimedeltah },

    [CSR_VSSTATUS]    = { "vsstatus",    hmode,   read_vsstatus,    write_vsstatus    },
    [CSR_VSIP]        = { "vsip",        hmode,   NULL,    NULL,    rmw_vsip          },
    [CSR_VSIE]        = { "vsie",        hmode,   read_vsie,        write_vsie        },
    [CSR_VSTVEC]      = { "vstvec",      hmode,   read_vstvec,      write_vstvec      },
    [CSR_VSSCRATCH]   = { "vsscratch",   hmode,   read_vsscratch,   write_vsscratch   },
    [CSR_VSEPC]       = { "vsepc",       hmode,   read_vsepc,       write_vsepc       },
    [CSR_VSCAUSE]     = { "vscause",     hmode,   read_vscause,     write_vscause     },
    [CSR_VSTVAL]      = { "vstval",      hmode,   read_vstval,      write_vstval      },
    [CSR_VSATP]       = { "vsatp",       hmode,   read_vsatp,       write_vsatp       },

    [CSR_MTVAL2]      = { "mtval2",      hmode,   read_mtval2,      write_mtval2      },
    [CSR_MTINST]      = { "mtinst",      hmode,   read_mtinst,      write_mtinst      },

    /* Physical Memory Protection */
    [CSR_MSECCFG]    = { "mseccfg",  epmp, read_mseccfg, write_mseccfg },
    [CSR_PMPCFG0]    = { "pmpcfg0",   pmp, read_pmpcfg,  write_pmpcfg  },
    [CSR_PMPCFG1]    = { "pmpcfg1",   pmp, read_pmpcfg,  write_pmpcfg  },
    [CSR_PMPCFG2]    = { "pmpcfg2",   pmp, read_pmpcfg,  write_pmpcfg  },
    [CSR_PMPCFG3]    = { "pmpcfg3",   pmp, read_pmpcfg,  write_pmpcfg  },
    [CSR_PMPADDR0]   = { "pmpaddr0",  pmp, read_pmpaddr, write_pmpaddr },
    [CSR_PMPADDR1]   = { "pmpaddr1",  pmp, read_pmpaddr, write_pmpaddr },
    [CSR_PMPADDR2]   = { "pmpaddr2",  pmp, read_pmpaddr, write_pmpaddr },
    [CSR_PMPADDR3]   = { "pmpaddr3",  pmp, read_pmpaddr, write_pmpaddr },
    [CSR_PMPADDR4]   = { "pmpaddr4",  pmp, read_pmpaddr, write_pmpaddr },
    [CSR_PMPADDR5]   = { "pmpaddr5",  pmp, read_pmpaddr, write_pmpaddr },
    [CSR_PMPADDR6]   = { "pmpaddr6",  pmp, read_pmpaddr, write_pmpaddr },
    [CSR_PMPADDR7]   = { "pmpaddr7",  pmp, read_pmpaddr, write_pmpaddr },
    [CSR_PMPADDR8]   = { "pmpaddr8",  pmp, read_pmpaddr, write_pmpaddr },
    [CSR_PMPADDR9]   = { "pmpaddr9",  pmp, read_pmpaddr, write_pmpaddr },
    [CSR_PMPADDR10]  = { "pmpaddr10", pmp, read_pmpaddr, write_pmpaddr },
    [CSR_PMPADDR11]  = { "pmpaddr11", pmp, read_pmpaddr, write_pmpaddr },
    [CSR_PMPADDR12]  = { "pmpaddr12", pmp, read_pmpaddr, write_pmpaddr },
    [CSR_PMPADDR13]  = { "pmpaddr13", pmp, read_pmpaddr, write_pmpaddr },
    [CSR_PMPADDR14] =  { "pmpaddr14", pmp, read_pmpaddr, write_pmpaddr },
    [CSR_PMPADDR15] =  { "pmpaddr15", pmp, read_pmpaddr, write_pmpaddr },

    /* Performance Counters */
    [CSR_HPMCOUNTER3]    = { "hpmcounter3",    ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER4]    = { "hpmcounter4",    ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER5]    = { "hpmcounter5",    ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER6]    = { "hpmcounter6",    ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER7]    = { "hpmcounter7",    ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER8]    = { "hpmcounter8",    ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER9]    = { "hpmcounter9",    ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER10]   = { "hpmcounter10",   ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER11]   = { "hpmcounter11",   ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER12]   = { "hpmcounter12",   ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER13]   = { "hpmcounter13",   ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER14]   = { "hpmcounter14",   ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER15]   = { "hpmcounter15",   ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER16]   = { "hpmcounter16",   ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER17]   = { "hpmcounter17",   ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER18]   = { "hpmcounter18",   ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER19]   = { "hpmcounter19",   ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER20]   = { "hpmcounter20",   ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER21]   = { "hpmcounter21",   ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER22]   = { "hpmcounter22",   ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER23]   = { "hpmcounter23",   ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER24]   = { "hpmcounter24",   ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER25]   = { "hpmcounter25",   ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER26]   = { "hpmcounter26",   ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER27]   = { "hpmcounter27",   ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER28]   = { "hpmcounter28",   ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER29]   = { "hpmcounter29",   ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER30]   = { "hpmcounter30",   ctr,    read_hpmcounter },
    [CSR_HPMCOUNTER31]   = { "hpmcounter31",   ctr,    read_hpmcounter },

    [CSR_MHPMCOUNTER3]   = { "mhpmcounter3",   mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER4]   = { "mhpmcounter4",   mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER5]   = { "mhpmcounter5",   mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER6]   = { "mhpmcounter6",   mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER7]   = { "mhpmcounter7",   mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER8]   = { "mhpmcounter8",   mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER9]   = { "mhpmcounter9",   mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER10]  = { "mhpmcounter10",  mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER11]  = { "mhpmcounter11",  mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER12]  = { "mhpmcounter12",  mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER13]  = { "mhpmcounter13",  mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER14]  = { "mhpmcounter14",  mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER15]  = { "mhpmcounter15",  mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER16]  = { "mhpmcounter16",  mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER17]  = { "mhpmcounter17",  mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER18]  = { "mhpmcounter18",  mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER19]  = { "mhpmcounter19",  mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER20]  = { "mhpmcounter20",  mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER21]  = { "mhpmcounter21",  mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER22]  = { "mhpmcounter22",  mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER23]  = { "mhpmcounter23",  mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER24]  = { "mhpmcounter24",  mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER25]  = { "mhpmcounter25",  mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER26]  = { "mhpmcounter26",  mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER27]  = { "mhpmcounter27",  mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER28]  = { "mhpmcounter28",  mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER29]  = { "mhpmcounter29",  mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER30]  = { "mhpmcounter30",  mctr,    read_hpmcounter,
                                                       write_mhpmcounter },
    [CSR_MHPMCOUNTER31]  = { "mhpmcounter31",  mctr,    read_hpmcounter,
                                                       write_mhpmcounter },

    [CSR_MCOUNTINHIBIT]  = { "mcountinhibit",  any, read_mcountinhibit,
                                                    write_mcountinhibit },

    [CSR_MHPMEVENT3]     = { "mhpmevent3",     any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT4]     = { "mhpmevent4",     any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT5]     = { "mhpmevent5",     any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT6]     = { "mhpmevent6",     any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT7]     = { "mhpmevent7",     any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT8]     = { "mhpmevent8",     any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT9]     = { "mhpmevent9",     any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT10]    = { "mhpmevent10",    any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT11]    = { "mhpmevent11",    any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT12]    = { "mhpmevent12",    any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT13]    = { "mhpmevent13",    any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT14]    = { "mhpmevent14",    any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT15]    = { "mhpmevent15",    any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT16]    = { "mhpmevent16",    any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT17]    = { "mhpmevent17",    any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT18]    = { "mhpmevent18",    any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT19]    = { "mhpmevent19",    any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT20]    = { "mhpmevent20",    any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT21]    = { "mhpmevent21",    any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT22]    = { "mhpmevent22",    any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT23]    = { "mhpmevent23",    any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT24]    = { "mhpmevent24",    any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT25]    = { "mhpmevent25",    any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT26]    = { "mhpmevent26",    any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT27]    = { "mhpmevent27",    any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT28]    = { "mhpmevent28",    any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT29]    = { "mhpmevent29",    any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT30]    = { "mhpmevent30",    any,    read_mhpmevent,
                                                       write_mhpmevent },
    [CSR_MHPMEVENT31]    = { "mhpmevent31",    any,    read_mhpmevent,
                                                       write_mhpmevent },

    [CSR_MHPMEVENT3H]    = { "mhpmevent3h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT4H]    = { "mhpmevent4h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT5H]    = { "mhpmevent5h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT6H]    = { "mhpmevent6h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT7H]    = { "mhpmevent7h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT8H]    = { "mhpmevent8h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT9H]    = { "mhpmevent9h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT10H]   = { "mhpmevent10h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT11H]   = { "mhpmevent11h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT12H]   = { "mhpmevent12h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT13H]   = { "mhpmevent13h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT14H]   = { "mhpmevent14h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT15H]   = { "mhpmevent15h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT16H]   = { "mhpmevent16h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT17H]   = { "mhpmevent17h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT18H]   = { "mhpmevent18h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT19H]   = { "mhpmevent19h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT20H]   = { "mhpmevent20h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT21H]   = { "mhpmevent21h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT22H]   = { "mhpmevent22h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT23H]   = { "mhpmevent23h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT24H]   = { "mhpmevent24h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT25H]   = { "mhpmevent25h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT26H]   = { "mhpmevent26h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT27H]   = { "mhpmevent27h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT28H]   = { "mhpmevent28h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT29H]   = { "mhpmevent29h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT30H]   = { "mhpmevent30h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},
    [CSR_MHPMEVENT31H]   = { "mhpmevent31h",    sscof,  read_mhpmeventh,
                                                       write_mhpmeventh},

    [CSR_HPMCOUNTER3H]   = { "hpmcounter3h",   ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER4H]   = { "hpmcounter4h",   ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER5H]   = { "hpmcounter5h",   ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER6H]   = { "hpmcounter6h",   ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER7H]   = { "hpmcounter7h",   ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER8H]   = { "hpmcounter8h",   ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER9H]   = { "hpmcounter9h",   ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER10H]  = { "hpmcounter10h",  ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER11H]  = { "hpmcounter11h",  ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER12H]  = { "hpmcounter12h",  ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER13H]  = { "hpmcounter13h",  ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER14H]  = { "hpmcounter14h",  ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER15H]  = { "hpmcounter15h",  ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER16H]  = { "hpmcounter16h",  ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER17H]  = { "hpmcounter17h",  ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER18H]  = { "hpmcounter18h",  ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER19H]  = { "hpmcounter19h",  ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER20H]  = { "hpmcounter20h",  ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER21H]  = { "hpmcounter21h",  ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER22H]  = { "hpmcounter22h",  ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER23H]  = { "hpmcounter23h",  ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER24H]  = { "hpmcounter24h",  ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER25H]  = { "hpmcounter25h",  ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER26H]  = { "hpmcounter26h",  ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER27H]  = { "hpmcounter27h",  ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER28H]  = { "hpmcounter28h",  ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER29H]  = { "hpmcounter29h",  ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER30H]  = { "hpmcounter30h",  ctr32,  read_hpmcounterh },
    [CSR_HPMCOUNTER31H]  = { "hpmcounter31h",  ctr32,  read_hpmcounterh },

    [CSR_MHPMCOUNTER3H]  = { "mhpmcounter3h",  mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER4H]  = { "mhpmcounter4h",  mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER5H]  = { "mhpmcounter5h",  mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER6H]  = { "mhpmcounter6h",  mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER7H]  = { "mhpmcounter7h",  mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER8H]  = { "mhpmcounter8h",  mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER9H]  = { "mhpmcounter9h",  mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER10H] = { "mhpmcounter10h", mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER11H] = { "mhpmcounter11h", mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER12H] = { "mhpmcounter12h", mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER13H] = { "mhpmcounter13h", mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER14H] = { "mhpmcounter14h", mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER15H] = { "mhpmcounter15h", mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER16H] = { "mhpmcounter16h", mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER17H] = { "mhpmcounter17h", mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER18H] = { "mhpmcounter18h", mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER19H] = { "mhpmcounter19h", mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER20H] = { "mhpmcounter20h", mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER21H] = { "mhpmcounter21h", mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER22H] = { "mhpmcounter22h", mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER23H] = { "mhpmcounter23h", mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER24H] = { "mhpmcounter24h", mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER25H] = { "mhpmcounter25h", mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER26H] = { "mhpmcounter26h", mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER27H] = { "mhpmcounter27h", mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER28H] = { "mhpmcounter28h", mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER29H] = { "mhpmcounter29h", mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER30H] = { "mhpmcounter30h", mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_MHPMCOUNTER31H] = { "mhpmcounter31h", mctr32,  read_hpmcounterh,
                                                       write_mhpmcounterh },
    [CSR_SCOUNTOVF]      = { "scountovf", sscof,  read_scountovf },

#endif /* !CONFIG_USER_ONLY */
};
