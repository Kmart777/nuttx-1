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

#ifndef __ARCH_ARM_INCLUDE_TSB_PM_H
#define __ARCH_ARM_INCLUDE_TSB_PM_H

#include <nuttx/power/pm.h>

/*
 * All the tsb pm_activity() priority values are defined here, so that when
 * the time for optimization comes, we'll be able to tweak them easily.
 */
#define TSB_UART_ACTIVITY       9
#define TSB_SPI_ACTIVITY        9
#define TSB_SDIO_ACTIVITY       9
#define TSB_UNIPRO_ACTIVITY     9
#define TSB_GPIO_ACTIVITY       9
#define TSB_I2C_ACTIVITY        9

#ifdef CONFIG_PM
int tsb_pm_getstate(void);
void tsb_pm_disable(void);
void tsb_pm_enable(void);
int tsb_pm_wait_for_wakeup(void);
int tsb_pm_driver_state_change(int pmstate);
int tsb_pm_register(pm_prepare_cb prepare, pm_notify_cb notify, void *priv);
#else
static int tsb_pm_getstate(void)
{
    return 0;
}

static void tsb_pm_disable(void)
{

}

static void tsb_pm_enable(void)
{

}

static int tsb_pm_wait_for_wakeup(void)
{
    return 0;
}

static int tsb_pm_driver_state_change(int pmstate)
{
    return 0;
}

static int tsb_pm_register(pm_prepare_cb prepare,
                           pm_notify_cb notify, void *priv)
{
    return 0;
}
#endif

#endif /* __ARCH_ARM_INCLUDE_TSB_PM_H */
