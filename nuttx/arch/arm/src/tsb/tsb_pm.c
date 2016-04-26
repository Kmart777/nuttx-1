/*
 * Copyright (c) 2014-2015 Google, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 * * may be used to endorse or promote products derived from this
 * software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <errno.h>

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/power/pm.h>
#include <nuttx/clock.h>

#define WAIT_FOR_WAKEUP_INTERVAL    10000 /* 10ms */
#define WAIT_FOR_WAKEUP_MAX_RETRY   10

static volatile int tsb_pm_curr_state = PM_NORMAL;
static volatile int tsb_pm_enabled = 1;

/*
 * Called from up_idle(). Checks the power state suggested by the power
 * management algorithm, then tries to change the power state of all
 * power-managed drivers. If that succeeds - the bridge-specific power
 * management code is executed.
 */
void up_idlepm(void)
{
    int ret, newstate;
    irqstate_t flags;

    if (!tsb_pm_enabled) {
        return;
    }

    newstate = pm_checkstate();
    if (newstate != tsb_pm_curr_state) {
        flags = irqsave();

        ret = pm_changestate(newstate);
        if (ret < 0) {
            /* Restore previous state on failure. */
            (void)pm_changestate(tsb_pm_curr_state);
        } else {
            tsb_pm_curr_state = newstate;

            /* This is where bridge-specific pm should be done. */
            switch (newstate) {
            case PM_NORMAL:
                break;
            case PM_IDLE:
                break;
            case PM_STANDBY:
                break;
            case PM_SLEEP:
                break;
            default:
                break;
            }
        }

        irqrestore(flags);
    }
}

/**
 * @brief Return current global power-management state.
 * @return Current power state as integer value.
 */
int tsb_pm_getstate(void)
{
    irqstate_t flags;
    int state;

    flags = irqsave();
    state = tsb_pm_curr_state;
    irqrestore(flags);

    return state;
}

/**
 * @brief Disable tsb power management.
 *
 * Bring the system back to PM_NORMAL, then disable any further
 * power-management state changes.
 */
void tsb_pm_disable(void)
{
    irqstate_t flags;

    /* Bring the system back to PM_NORMAL before disabling pm. */
    pm_activity(10);
    up_idlepm();
    for (;;) {
        flags = irqsave();
        if (tsb_pm_curr_state == PM_NORMAL) {
            tsb_pm_enabled = 0;
            irqrestore(flags);
            return;
        }
        irqrestore(flags);
        usleep(TICK2USEC(1));
    }
}

/**
 * @brief Re-enable tsb power-management.
 */
void tsb_pm_enable(void)
{
    irqstate_t flags;

    flags = irqsave();
    tsb_pm_enabled = 1;
    irqrestore(flags);
}

/**
 * @brief Wait for tsb power-management wakeup.
 * @return OK (0) on success, negative error number on failure.
 */
int tsb_pm_wait_for_wakeup(void)
{
    int retry = 0;

    while (tsb_pm_getstate() == PM_SLEEP) {
        usleep(WAIT_FOR_WAKEUP_INTERVAL);
        if (++retry > WAIT_FOR_WAKEUP_MAX_RETRY) {
            return -EIO;
        }
    }

    return OK;
}

/**
 * @brief Force pm state change for drivers.
 * @param pmstate New power state.
 * @return OK (0) on success, negative error number on failure.
 *
 * This routine makes the pm framework execute the prepare and notify callbacks
 * for all drivers, but doesn't actually run the global pm code (as not to
 * actually enter a deeper power state). It's mostly useful for debugging.
 */
int tsb_pm_driver_state_change(int pmstate)
{
    int status;

    status = pm_changestate(pmstate);
    if (status < 0) {
        /* Restore previous state on failure. */
        (void)pm_changestate(tsb_pm_curr_state);
        return status;
    }

    tsb_pm_curr_state = pmstate;

    return OK;
}

/**
 * @brief Register callbacks with the pm framework.
 * @param prepare Prepare callback.
 * @param notify Notify callback.
 * @param priv Pointer to private data to be passed as argument to pm callbacks.
 * @return OK (0) on success, negative error number on failure.
 */
int tsb_pm_register(pm_prepare_cb prepare, pm_notify_cb notify, void *priv)
{
    struct pm_callback_s *pmctx;
    int ret;

    pmctx = malloc(sizeof(struct pm_callback_s));
    if (!pmctx) {
        return -ENOMEM;
    }

    pmctx->prepare = prepare;
    pmctx->notify = notify;
    pmctx->priv = priv;

    ret = pm_register(pmctx);
    if (ret < 0) {
        free(pmctx);
        return ret;
    }

    return OK;
}

void up_pminitialize(void)
{
    pm_initialize();
}
