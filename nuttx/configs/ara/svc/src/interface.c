/*
 * Copyright (c) 2015-2016 Google Inc.
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
 * contributors may be used to endorse or promote products derived from this
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

/**
 * @brief: Functions and definitions for interface block management.
 */

#define DBG_COMP ARADBG_SVC     /* DBG_COMP macro of the component */

#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/gpio.h>
#include <nuttx/power/pm.h>

#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <pthread.h>

#include <ara_debug.h>
#include "interface.h"
#include "vreg.h"
#include "string.h"
#include "svc.h"
#include "tsb_switch.h"
#include "tsb_switch_event.h"
#include "svc_pm.h"
#include "ara_board.h"

#define POWER_OFF_TIME_IN_US                                (500000)
#define MODULE_PORT_WAKEOUT_PULSE_DURATION_IN_US            (500000)
#define MODULE_PORT_WAKEOUT_PULSE_DURATION_IN_MS            \
        (MODULE_PORT_WAKEOUT_PULSE_DURATION_IN_US / 1000)
#define LINKUP_WD_DELAY_IN_MS                               \
        (200 + MODULE_PORT_WAKEOUT_PULSE_DURATION_IN_MS)
#define LINKUP_WD_DELAY                                     \
        ((LINKUP_WD_DELAY_IN_MS * CLOCKS_PER_SEC) / 1000)

static struct interface **interfaces;
static unsigned int nr_interfaces;
static unsigned int nr_spring_interfaces;
static struct vreg *vlatch_vdd;
static struct vreg *latch_ilim;
static pthread_mutex_t latch_ilim_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t mod_sense;

static void interface_power_cycle(void *data);
static void interface_uninstall_wd_handler(struct interface *iface,
                                           struct wd_data *wd);
static int interface_install_wd_handler(struct interface *iface, bool);

/*
 * Debug macros to verify the interface mutex logic - this should be removed
 * in the long term. A thread locking a mutex that it already owns is a bug and
 * we should fix those bugs.
 */
#define pthread_mutex_lock_debug(mutex) {                     \
    int ret;                                                  \
                                                              \
    ret = pthread_mutex_lock(mutex);                          \
    if (ret) {                                                \
        PANIC();                                              \
        while(1){};                                           \
    }                                                         \
}

#define pthread_mutex_unlock_debug(mutex) {                   \
    int ret;                                                  \
                                                              \
    ret = pthread_mutex_unlock(mutex);                        \
    if (ret) {                                                \
        PANIC();                                              \
        while(1){};                                           \
    }                                                         \
}

/**
 * @brief Configure all the voltage regulators associated with an interface
 * to their default states.
 * @param iface interface to configure
 */
static int interface_config(struct interface *iface)
{
    int rc_pwr, rc_clk;

    dbg_verbose("Configuring interface %s.\n",
                iface->name ? iface->name : "unknown");

    /* Configure default state for the regulator pins */
    rc_pwr = vreg_config(iface->vsys_vreg);
    rc_clk = vreg_config(iface->refclk_vreg);

    /* Configure the interfaces pin according to the interface type */
    switch (iface->if_type) {
    case ARA_IFACE_TYPE_MODULE_PORT:
    case ARA_IFACE_TYPE_MODULE_PORT2:
        /*
         * DB3 module port:
         * The WAKEOUT pin is configured as interrupt input at handler
         * installation time
         */

        /* Configure the release line */
        if (iface->ejectable) {
            gpio_activate(iface->release_gpio);
            gpio_direction_out(iface->release_gpio, 0);
        }
        break;
    default:
        break;
    }

    /* Init power state */
    atomic_init(&iface->power_state,
                rc_pwr ? ARA_IFACE_PWR_ERROR : ARA_IFACE_PWR_DOWN);
    atomic_init(&iface->refclk_state,
                rc_clk ? ARA_IFACE_PWR_ERROR : ARA_IFACE_PWR_DOWN);

    return rc_pwr ? rc_pwr : rc_clk ? rc_clk : 0;
}

/**
 * @brief Supply the reference clock to an interface
 *
 * This function attempts to apply the reference clock to the
 * interface, and updates the interface's refclk state accordingly.
 * This affects the value returned by interface_get_refclk_state(iface).
 *
 * @param iface Interface whose reference clock to enable
 * @return 0 on success, <0 on error
 */
int interface_refclk_enable(struct interface *iface)
{
    int rc;

    if (!iface) {
        return -EINVAL;
    }

    rc = vreg_get(iface->refclk_vreg);
    if (rc) {
        dbg_error("Failed to enable the reference clock for interface %s: %d\n",
                  iface->name, rc);
        atomic_init(&iface->refclk_state, ARA_IFACE_PWR_ERROR);
    } else {
        atomic_init(&iface->refclk_state, ARA_IFACE_PWR_UP);
    }

    return rc;
}


/**
 * @brief Disable the reference clock supply to this interface
 *
 * This function attempts to remove the reference clock from the
 * interface, and updates the interface's refclk state accordingly.
 * This affects the value returned by interface_get_refclk_state(iface).
 *
 * @param iface Interface whose reference clock to disable
 * @return 0 on success, <0 on error
 */
int interface_refclk_disable(struct interface *iface)
{
    int rc;

    if (!iface) {
        return -EINVAL;
    }

    rc = vreg_put(iface->refclk_vreg);
    if (rc) {
        dbg_error("Failed to disable the reference clock for interface %s: %d\n",
                  iface->name, rc);
        atomic_init(&iface->refclk_state, ARA_IFACE_PWR_ERROR);
    } else {
        atomic_init(&iface->refclk_state, ARA_IFACE_PWR_DOWN);
    }

    return rc;
}

/**
 * @brief Turn on VSYS power to this interface
 *
 * This function attempts to apply VSYS power to the interface, and
 * updates the interface's power state accordingly.  This affects the
 * value returned by interface_get_pwr_state(iface).
 *
 * @param iface Interface whose VSYS power to enable
 * @return 0 on success, <0 on error
 */
int interface_vsys_enable(struct interface *iface)
{
    int rc;

    if (!iface) {
        dbg_verbose("%s: called with null interface\n", __func__);
        return -EINVAL;
    }

    rc = vreg_get(iface->vsys_vreg);
    if (rc) {
        dbg_error("Failed to enable interface %s: %d\n", iface->name, rc);
        atomic_init(&iface->power_state, ARA_IFACE_PWR_ERROR);
    } else {
        atomic_init(&iface->power_state, ARA_IFACE_PWR_UP);
    }

    return rc;
}


/**
 * @brief Turn off VSYS power to this interface
 *
 * This function attempts to remove VSYS power from the interface, and
 * updates the interface's power state accordingly.  This affects the
 * value returned by interface_get_pwr_state(iface).
 *
 * @param iface Interface whose VSYS power to disable
 * @return 0 on success, <0 on error
 */
int interface_vsys_disable(struct interface *iface)
{
    int rc;

    if (!iface) {
        dbg_verbose("%s: called with null interface\n", __func__);
        return -EINVAL;
    }

    rc = vreg_put(iface->vsys_vreg);
    if (rc) {
        dbg_error("Failed to disable interface %s: %d\n", iface->name, rc);
        atomic_init(&iface->power_state, ARA_IFACE_PWR_ERROR);
    } else {
        atomic_init(&iface->power_state, ARA_IFACE_PWR_DOWN);
    }

    return rc;
}

/*
 * @brief Handle the end of the WAKEOUT pulse on an interface
 * @param data pointer to interface struct
 *
 * Runs in workqueue context
 * Requires caller to hold iface->mutex
 */
static int interface_wakeout_timeout(struct interface *iface)
{
    int rc = 0;

    if (!iface) {
        dbg_error("%s: called with null interface\n", __func__);
        return -ENODEV;
    }

    dbg_verbose("Wakeout pulse timeout on %s\n", iface->name);

    switch (iface->if_type) {
    case ARA_IFACE_TYPE_MODULE_PORT:
        /* Put the WAKE/DETECT line back to default state */
        gpio_direction_out(iface->detect_in.gpio, iface->detect_in.polarity);
        /* Finally re-install the interrupt handler on the pin */
        rc = interface_install_wd_handler(iface, true);
        break;
    case ARA_IFACE_TYPE_MODULE_PORT2:
        /* Put the WAKEOUT line back to default state */
        gpio_direction_out(iface->wake_gpio, !iface->wake_gpio_pol);
        break;
    case ARA_IFACE_TYPE_BUILTIN:
    default:
        dbg_error("%s: unsupported interface type: %d\n", __func__,
                  iface->if_type);
        rc = -ENOTSUP;
        break;
    }

    return rc;
}

/*
 * @brief Handle the end of the WAKEOUT pulse on an interface
 * @param data pointer to interface struct
 *
 * Runs in workqueue context
 */
static void interface_wakeout_timeout_atomic(void *data)
{
    struct interface *iface = data;

    pthread_mutex_lock_debug(&iface->mutex);
    interface_wakeout_timeout(iface);
    pthread_mutex_unlock_debug(&iface->mutex);
}

/*
 * @brief Called by the timesync layer when timesync operations are commencing
 *
 * For the duration of timesync all specified interfaces will be reserved.
 */
uint32_t interfaces_timesync_init(uint32_t strobe_mask)
{
    int intf_id;
    uint32_t pin_strobe_mask = 0;
    struct wd_data *wd;
    struct interface *iface;

    for (intf_id = 0; intf_id < nr_interfaces; intf_id++) {

        iface = interfaces[intf_id];
        if (iface->dev_id && strobe_mask & (1 << iface->dev_id)) {
            /* Wait - release the interface in fini() */
            pthread_mutex_lock(&iface->mutex);

            wd = &iface->detect_in;
            if (iface->ejectable && wd->db_state != WD_ST_ACTIVE_STABLE) {
                dbg_error("%s state %d is not WD_ST_ACTIVE_STABLE\n",
                          iface->name, wd->db_state);
                #if 0
                /*
                 * TODO: remove if 0 when SW-4053 gets fixed
                 * https://projectara.atlassian.net/browse/SW-4053
                 */
                pthread_mutex_unlock(&iface->mutex);
                continue;
                #endif
            }

            /* Uninstall the WD handler for input */
            interface_uninstall_wd_handler(iface, &iface->detect_in);

            /* Set interface state */
            iface->state = ARA_IFACE_STATE_WD_TIMESYNC;

            /* Set initial state to low */
            gpio_direction_out(iface->detect_in.gpio, 0);

            /* Add the pin to the return mask */
            pin_strobe_mask |= 1 << iface->detect_in.gpio;
        }
    }
    return pin_strobe_mask;
}

/*
 * @brief Release interfaces from timesync
 *
 */
void interfaces_timesync_fini(void)
{
    int intf_id;
    struct interface *iface;

    for (intf_id = 0; intf_id < nr_interfaces; intf_id++) {

        iface = interfaces[intf_id];
        if (iface->state == ARA_IFACE_STATE_WD_TIMESYNC) {

            /* Reinstall the WD handler for input */
            interface_install_wd_handler(iface, true);

            /* Post - release the interface taken in timesync_init() */
            pthread_mutex_unlock(&iface->mutex);
        }
    }
}

/*
 * @brief Generate a WAKEOUT signal to wake-up/power-up modules.
 * If assert is true, keep the WAKEOUT lines asserted.
 *
 * The corresponding power supplies must already be enabled.
 * Requires caller to hold iface->mutex
 */
static int interface_generate_wakeout(struct interface *iface,
                                             bool assert, int length)
{
    uint32_t gpio;
    bool polarity;
    int rc = 0;

    if (!iface) {
        dbg_error("%s: called with null interface\n", __func__);
        return -ENODEV;
    }

    if (length <= 0) {
        dbg_info("Generating WAKEOUT on interface %s\n",
                 iface->name ? iface->name : "unknown");
    } else {
        dbg_info("Generating WAKEOUT on interface %s (%d us)\n",
                 iface->name ? iface->name : "unknown", length);
    }

    switch (iface->if_type) {
    case ARA_IFACE_TYPE_MODULE_PORT:
        gpio = iface->detect_in.gpio;
        polarity = !iface->detect_in.polarity;
        break;

    case ARA_IFACE_TYPE_MODULE_PORT2:
        gpio = iface->wake_gpio;
        polarity = iface->wake_gpio_pol;
        break;

    case ARA_IFACE_TYPE_BUILTIN:
    default:
        dbg_error("%s: unsupported interface type: %d\n", __func__,
                  iface->if_type);
        return -ENOTSUP;
    }

    /* Generate a WAKEOUT pulse
     *
     * DB3 module port:
     * Generate a pulse on the WD line. The polarity is reversed
     * from the DETECT_IN polarity.
     */
    if (gpio) {
        int pulse_len = (length > 0) ?
                        length : MODULE_PORT_WAKEOUT_PULSE_DURATION_IN_US;

        switch (iface->if_type) {
        case ARA_IFACE_TYPE_MODULE_PORT:
            /* First uninstall the interrupt handler on the pin */
            interface_uninstall_wd_handler(iface, &iface->detect_in);
            break;

        case ARA_IFACE_TYPE_MODULE_PORT2:
        case ARA_IFACE_TYPE_BUILTIN:
            break;

        default:
            dbg_error("%s(): unsupported interface port type: %d\n", __func__,
                      iface->if_type);
            return -ENOTSUP;
        }

        /* Then configure the pin as output and assert it */
        gpio_direction_out(gpio, polarity);

        /*
         * Keep the line asserted for the given duration. After timeout
         * de-assert the line.
         */
        if (!work_available(&iface->wakeout_work)) {
            rc = work_cancel(HPWORK, &iface->wakeout_work);
            /*
             * work_cancel() doesn't fail in the current
             * implementation. And if it did, we'd be dead in the water
             * anyway.
             */
            DEBUGASSERT(!rc);
        }
        rc = work_queue(HPWORK, &iface->wakeout_work,
                        interface_wakeout_timeout_atomic,
                        iface, USEC2TICK(pulse_len));
        if (rc) {
            dbg_error("%s: Could not schedule WAKEOUT pulse completion work for %s\n",
                      __func__, iface->name);
            interface_wakeout_timeout(iface);
        }
    }
    return rc;
}

/*
 * @brief Cancel the WAKEOUT pulse on an interface
 * @param data Pointer to interface struct
 *
 * Requires caller to hold iface->mutex
 */
static int interface_cancel_wakeout(struct interface *iface)
{
    int rc = 0;

    if (!iface) {
        dbg_error("%s: called with null interface\n", __func__);
        return -ENODEV;
    }

    /* Cancel the work queue if already started */
    if (!work_available(&iface->wakeout_work)) {
        rc = work_cancel(HPWORK, &iface->wakeout_work);
        /*
         * work_cancel() doesn't fail in the current
         * implementation. And if it did, we'd be dead in the water
         * anyway.
         */
        DEBUGASSERT(!rc);
    }

    /*
     * Re-install the interrupt handler on the pin.
     * Since we are canceling the WAKEOUT pulse do not check the hotplug
     * state here. Depending on the case (power off, power cycle etc.)
     * the caller checks the new state.
     */
    rc = interface_wakeout_timeout(iface);

    return rc;
}

/*
 * @brief helper function for interface_generate_wakeout ensures calling
 *        context holds the interface mutex when calling into
 *        interface_generate_wakeout().
 */
int interface_generate_wakeout_atomic(struct interface *iface, bool assert,
                                      int length)
{
    int rc;

    pthread_mutex_lock_debug(&iface->mutex);
    rc = interface_generate_wakeout(iface, assert, length);
    pthread_mutex_unlock_debug(&iface->mutex);

    return rc;
}

/*
 * @brief Cancel a WAKEOUT signal on an interface.
 */
int interface_cancel_wakeout_atomic(struct interface *iface)
{
    int rc;

    pthread_mutex_lock_debug(&iface->mutex);
    rc = interface_cancel_wakeout(iface);
    pthread_mutex_unlock_debug(&iface->mutex);

    return rc;
}

/**
 * @brief Get interface power supply state
 * @param iface Interface whose power state to retrieve
 * @return iface's power state, or ARA_IFACE_PWR_ERROR if iface == NULL.
 */
enum ara_iface_pwr_state interface_get_vsys_state(struct interface *iface)
{
    if (!iface) {
        return ARA_IFACE_PWR_ERROR;
    }

    return atomic_get(&iface->power_state);
}

/**
 * @brief Get interface refclk supply state
 * @param iface Interface whose refclk state to retrieve
 * @return iface's refclk state, or ARA_IFACE_PWR_ERROR if iface == NULL.
 */
enum ara_iface_pwr_state interface_get_refclk_state(struct interface *iface)
{
    if (!iface) {
        return ARA_IFACE_PWR_ERROR;
    }

    return atomic_get(&iface->refclk_state);
}


/*
 * @brief Interface power control helper, to be used by the DETECT_IN/hotplug
 * mechanism.
 * @param Interface to power off
 *
 * Power OFF the interface
 * Requires caller to hold iface->mutex
 */
static int interface_power_off(struct interface *iface)
{
    int rc;

    if (!iface) {
        return -EINVAL;
    }

    /* Cancel LinkUp and WAKEOUT pulse for the interface */
    iface->linkup_req_sent = false;
    wd_cancel(&iface->linkup_wd);
    rc = interface_cancel_wakeout(iface);

    /* Disable Switch port IRQs */
    switch_port_irq_enable(svc->sw, iface->switch_portid, false);

    /* Disable Switch port */
    rc = switch_enable_port(svc->sw, iface->switch_portid, false);
    if (rc && (rc != -EOPNOTSUPP)) {
        dbg_error("Failed to disable switch port for interface %s: %d.\n",
                  iface->name, rc);
    }

    /* Power off the interface */
    rc = interface_vsys_disable(iface);
    if (rc < 0) {
        return rc;
    }

    rc = interface_refclk_disable(iface);
    if (rc < 0) {
        return rc;
    }

    return 0;
}

/*
 * @brief interface_power_off_atomic - non-static external helper function
 *        calls interface_power_off holding iface->mutex, then releases mutex
 * @param Interface to power off
 */
int interface_power_off_atomic(struct interface *iface)
{
    int rc;

    pthread_mutex_lock_debug(&iface->mutex);
    rc = interface_power_off(iface);
    pthread_mutex_unlock_debug(&iface->mutex);

    return rc;
}

/*
 * @brief linkup timeout callback
 *        context watchdog IRQ - increment of linkup timeout accomplished
 *        in workqueue context @ interface_power_cycle
 */
static void interface_linkup_timeout(int argc, uint32_t arg1, ...)
{
    struct interface *iface = (struct interface*) arg1;

    DEBUGASSERT(sizeof(struct interface*) == sizeof(uint32_t));

    dbg_warn("Link-up took more than %d ms, turning interface '%s' OFF and ON again\n",
             LINKUP_WD_DELAY_IN_MS, iface->name);

    work_queue(HPWORK, &iface->linkup_work, interface_power_cycle, iface, 0);
}

/*
 * Requires calling context to hold iface->mutex
 * Takes and releases latch_lim_lock
 */
static int interface_detect_order(struct interface *iface)
{
    int retval;

    if (iface->if_type != ARA_IFACE_TYPE_MODULE_PORT2) {
        iface->if_order = ARA_IFACE_ORDER_UNKNOWN;
        return -ENOTSUP;
    }

    pthread_mutex_lock_debug(&latch_ilim_lock);
    retval = vreg_get(vlatch_vdd);
    if (retval) {
        dbg_error("couldn't enable VLATCH_VDD_EN, aborting order detection...\n");
        goto error_get_vlatch_vdd;
    }

    gpio_set_value(iface->release_gpio, 1);

    /* since we got the latch_ilim_lock it means LATCH_ILIM_EN = 0 */
    iface->if_order = gpio_get_value(mod_sense) ? ARA_IFACE_ORDER_SECONDARY
                                                : ARA_IFACE_ORDER_PRIMARY;

    gpio_set_value(iface->release_gpio, 0);

    retval = vreg_put(vlatch_vdd);
    goto done;

error_get_vlatch_vdd:
    iface->if_order = ARA_IFACE_ORDER_UNKNOWN;
done:
    pthread_mutex_unlock_debug(&latch_ilim_lock);
    return retval;
}

/*
 * Interface power control helper, to be used by the DETECT_IN/hotplug
 * mechanism.
 *
 * Power ON the interface in order to cleanly reboot the interface
 * module(s). Then an initial handshake between the module(s) and the
 * interface can take place.
 * Requires calling context to hold iface->mutex
 */
static int interface_power_on(struct interface *iface)
{
    int rc;

    if (!iface) {
        return -EINVAL;
    }

    iface->linkup_req_sent = false;

    rc = interface_detect_order(iface);
    if (rc && rc != -ENOTSUP) {
        dbg_error("failed to detect interface order for %s: %d\n",
                  iface->name ? iface->name : "unknown", rc);
        return rc;
    }

    /* If powered OFF, power it ON now */
    if (!interface_get_vsys_state(iface)) {
        rc = interface_vsys_enable(iface);
        if (rc < 0) {
            return rc;
        }
    }

    if (!interface_get_refclk_state(iface)) {
        rc = interface_refclk_enable(iface);
        if (rc < 0) {
            goto out_power;
        }
    }

    /* Enable Switch port */
    rc = switch_enable_port(svc->sw, iface->switch_portid, true);
    if (rc && (rc != -EOPNOTSUPP)) {
        dbg_error("Failed to enable switch port for interface %s: %d.\n",
                  iface->name, rc);
        goto out_refclk;
    }

    /* Enable interrupts for Unipro port */
    rc = switch_port_irq_enable(svc->sw, iface->switch_portid, true);
    if (rc) {
        dbg_error("Failed to enable port IRQs for interface %s: %d.\n",
                  iface->name, rc);
        goto out_port;
    }

    /*
     * HACK (SW-2591)
     *
     * There are issues with cold boot support for built-in
     * (non-ejectable) interfaces which are leading to a significant
     * percentage of boots failing to result in a working UniPro
     * network.
     *
     * Skip the watchdog and linkup retries while those are being
     * debugged. The race condition which this watchdog is intended to
     * avoid doesn't happen as often, so not dealing with it actually
     * leads to better behavior for now.
     */
    if (iface->ejectable) {
        wd_start(&iface->linkup_wd, LINKUP_WD_DELAY, interface_linkup_timeout,
                 1, iface);
    } else {
        dbg_info("%s: skipping linkup watchdog for interface %s\n",
                 __func__, iface->name);
    }

    /* Generate WAKEOUT */
    rc = interface_generate_wakeout(iface, false, -1);
    if (rc) {
        dbg_error("Failed to generate wakeout on interface %s\n", iface->name);
        goto out_port_irq;
    }

    return 0;

out_port_irq:
    switch_port_irq_enable(svc->sw, iface->switch_portid, false);
out_port:
    switch_enable_port(svc->sw, iface->switch_portid, false);
out_refclk:
    interface_refclk_disable(iface);
out_power:
    interface_vsys_disable(iface);

    return rc;
}

/*
 * interface_power_on_atomic - non-static external helper function
 *                             calls interface_power_on holding iface->mutex
 *                             then releases mutex
 */
int interface_power_on_atomic(struct interface *iface)
{
    int rc;

    pthread_mutex_lock_debug(&iface->mutex);
    rc = interface_power_on(iface);
    pthread_mutex_unlock_debug(&iface->mutex);

    return rc;
}

/*
 * Cancel a linkup - relies on the state of iface->ejectable
 *                   and requires calling context to hold
 *                   iface->mutex to protect iface->ejectable.
 */
static void interface_cancel_linkup_wd(struct interface *iface)
{
    if (!iface->ejectable) {
        /*
         * HACK (SW-2591): see interface_power_on() comment with this
         * issue tag.
         */
        return;
    }
    dbg_verbose("Canceling linkup watchdog for '%s'\n", iface->name);
    wd_cancel(&iface->linkup_wd);
}

/*
 * interface_cancel_linkup_wd_atomic - non-static external helper function
 *                                     calls interface_cancel_linkup_wd holding
 *                                     iface->mutex, then releases mutex
 */
void interface_cancel_linkup_wd_atomic(struct interface *iface)
{
    pthread_mutex_lock_debug(&iface->mutex);
    interface_cancel_linkup_wd(iface);
    pthread_mutex_unlock_debug(&iface->mutex);
}

/*
 * interface_power_cycle - workqueue context
 */
static void interface_power_cycle(void *data)
{
    struct interface *iface = data;
    uint8_t retries;

    pthread_mutex_lock_debug(&iface->mutex);
    interface_power_off(iface);

    if (++iface->linkup_retries >= INTERFACE_MAX_LINKUP_TRIES) {
        dbg_error("Could not link-up with '%s' in less than %d ms, aborting after %d tries\n",
                  iface->name, LINKUP_WD_DELAY_IN_MS,
                  INTERFACE_MAX_LINKUP_TRIES);
        goto done;
    }

    retries = iface->linkup_retries;
    interface_power_on(iface);
    iface->linkup_retries = retries;
done:
    pthread_mutex_unlock_debug(&iface->mutex);
}

/**
 * @brief           Return the name of the interface
 * @return          Interface name (string), NULL in case of error.
 * @param[in]       iface: configured interface structure
 */
const char *interface_get_name(struct interface *iface)
{
    if (!iface) {
        return NULL;
    }

    return iface->name;
}

/**
 * @brief           Return the switch port ID of the interface
 * @return          ID of the interface's switch port; INVALID_PORT on error.
 * @param[in]       iface: configured interface structure
 */
unsigned int interface_get_portid(const struct interface *iface)
{
    if (!iface) {
        return INVALID_PORT;
    }

    return iface->switch_portid;
}

/**
 * @brief Get the interface struct from the index, as specified in the MDK.
 *        Index 0 is for the first interface (aka 'A').
 * @return interface* on success, NULL on error
 */
struct interface* interface_get(uint8_t index)
{
    if ((!interfaces) || (index >= nr_interfaces))
        return NULL;

    return interfaces[index];
}

/**
 * @brief           Return the interface struct from the name
 * @return interface* on success, NULL on error
 */
struct interface* interface_get_by_name(const char *name)
{
    struct interface *iface;
    int i;

    interface_foreach(iface, i) {
      if (!strcmp(iface->name, name)) {
        return iface;
      }
    }

    return NULL;
}

/**
 * @brief           Return the interface struct from the port_id
 * @return interface* on success, NULL on error
 */
struct interface* interface_get_by_portid(uint8_t port_id)
{
    int iface_idx = interface_get_id_by_portid(port_id);

    if (iface_idx <= 0) {
        return NULL;
    }

    return interface_get(iface_idx - 1);
}

/*
 * Interface numbering is defined as it's position in the interface table + 1.
 *
 * By convention, the AP module should be interface number 1.
 */

/**
 * @brief find an intf_id given a portid
 */
int interface_get_id_by_portid(uint8_t port_id) {
    unsigned int i;

    if (port_id == INVALID_PORT) {
        return -ENODEV;
    }

    for (i = 0; i < nr_interfaces; i++) {
        if (interfaces[i]->switch_portid == port_id) {
            return i + 1;
        }
    }

    return -EINVAL;
}

/**
 * @brief find a port_id given an intf_id
 */
int interface_get_portid_by_id(uint8_t intf_id) {
    int portid;

    if (!intf_id || intf_id > nr_interfaces) {
        return -EINVAL;
    }

    portid = interfaces[intf_id - 1]->switch_portid;
    if (portid == INVALID_PORT) {
        return -ENODEV;
    }

    return portid;
}

/**
 * @brief find a dev_id given an intf_id
 */
int interface_get_devid_by_id(uint8_t intf_id) {
    if (!intf_id || intf_id > nr_interfaces) {
        return -EINVAL;
    }

    return interfaces[intf_id - 1]->dev_id;
}

/**
 * @brief set a devid for a given an intf_id
 */
int interface_set_devid_by_id_atomic(uint8_t intf_id, uint8_t dev_id) {
    struct interface *iface;

    if (!intf_id || intf_id > nr_interfaces) {
        return -EINVAL;
    }
    iface = interfaces[intf_id - 1];

    pthread_mutex_lock_debug(&iface->mutex);
    iface->dev_id = dev_id;
    pthread_mutex_unlock_debug(&iface->mutex);

    return 0;
}

/**
 * @brief set iface->linkup_retries holding the iface mutex
 *        This ensures synchronization with interface_power_cycle
 */
void interface_set_linkup_retries_atomic(struct interface *iface, uint8_t val)
{
    /* TODO resolve SW-4249 and uncomment mutex locks */
    /* pthread_mutex_lock_debug(&iface->mutex); */
    iface->linkup_retries = val;
    /* pthread_mutex_unlock_debug(&iface->mutex); */
}

/**
 * @brief           Return the spring interface struct from the index.
 * @warning         Index 0 is for the first spring interface.
 * @return          Interface structure, NULL in case of error.
 * @param[in]       index: configured interface structure
 */
struct interface* interface_spring_get(uint8_t index)
{
    if ((!interfaces) || (index >= nr_spring_interfaces))
        return NULL;

    return interfaces[nr_interfaces - nr_spring_interfaces + index];
}


/**
 * @brief           Return the number of available interfaces.
 * @return          Number of available interfaces, 0 in case of error.
 */
uint8_t interface_get_count(void)
{
    return nr_interfaces;
}


/**
 * @brief           Return the number of available spring interfaces.
 * @return          Number of available spring interfaces, 0 in case of error.
 */
uint8_t interface_get_spring_count(void)
{
    return nr_spring_interfaces;
}


/**
 * @brief           Return the ADC instance used for this interface
 *                  current measurement.
 * @return          ADC instance, 0 in case of error
 * @param[in]       iface: configured interface structure
 */
uint8_t interface_pm_get_adc(struct interface *iface)
{
    if ((!iface) || (!iface->pm)) {
        return 0;
    }

    return iface->pm->adc;
}


/**
 * @brief           Return the ADC channel used for this interface
 *                  current measurement.
 * @return          ADC channel, 0 in case of error
 * @param[in]       iface: configured interface structure
 */
uint8_t interface_pm_get_chan(struct interface *iface)
{
    if ((!iface) || (!iface->pm)) {
        return 0;
    }

    return iface->pm->chan;
}

/**
 * @brief Get the hotplug state of an interface from the DETECT_IN signal
 */
static enum hotplug_state interface_get_hotplug_state(struct interface *iface)
{
    bool polarity, active;
    enum hotplug_state hs = HOTPLUG_ST_UNKNOWN;

    if (iface->detect_in.gpio) {
        polarity = iface->detect_in.polarity;
        active = (gpio_get_value(iface->detect_in.gpio) == polarity);
        if (active) {
            hs = HOTPLUG_ST_PLUGGED;
        } else {
            hs = HOTPLUG_ST_UNPLUGGED;
        }
    }

    return hs;
}

enum hotplug_state interface_get_hotplug_state_atomic(struct interface *iface)
{
    enum hotplug_state hs;

    pthread_mutex_lock_debug(&iface->mutex);
    hs = interface_get_hotplug_state(iface);
    pthread_mutex_unlock_debug(&iface->mutex);

    return hs;
}

static void interface_wd_delayed_handler(void *data);

/* Delayed debounce check */
static int interface_wd_delay_check(struct interface *iface, uint32_t delay)
{
    struct wd_data *wd = &iface->detect_in;

    /*
     * If the work is already scheduled, do not schedule another one now.
     * A new one will be scheduled if more debounce is needed.
     */
    if (!work_available(&wd->work)) {
        return 0;
    }

    pm_activity(SVC_INTF_WD_DEBOUNCE_ACTIVITY);

    /* Schedule the work to run after the debounce timeout */
    return work_queue(HPWORK, &wd->work, interface_wd_delayed_handler, iface,
                      MSEC2TICK(delay));
}

/*
 * Handle an active stable signal as on DB3. The fact that there's
 * only one wake/detect pin to debounce there is assumed.
 *
 * WD as DETECT_IN transition to active
 *
 * - Power ON the interface
 *   Note: If coming back to the active stable state from
 *         the same last stable state after an unstable
 *         transition, power cycle (OFF/ON) the interface.
 *         In that case consecutive hotplug events are
 *         sent to the AP.
 * - Signal HOTPLUG state to the higher layer
 * Requires calling context to hold iface->mutex
 */
static void interface_wd_handle_active_stable(struct interface *iface)
{
    struct wd_data *wd = &iface->detect_in;

    wd->db_state = WD_ST_ACTIVE_STABLE;
    dbg_verbose("W&D: got stable %s_WD Act (gpio %d)\n",
                iface->name, wd->gpio);

    /* Power on the interface, includes the WAKEOUT pulse generation */
    if (wd->last_state == WD_ST_ACTIVE_STABLE) {
        interface_power_off(iface);
    }
    interface_power_on(iface);

    /* Save last stable state for power ON/OFF handling */
    wd->last_state = wd->db_state;
}

/*
 * Handle an inactive stable signal as on DB3. The fact that theres
 * only one wake/detect pin to debounce there is assumed.
 *
 * WD as DETECT_IN transition to inactive
 *
 * Power OFF the interface
 * Signal HOTPLUG state to the higher layer
 *
 * Requires the calling context to hold iface->mutex
 */
static void interface_wd_handle_inactive_stable(struct interface *iface)
{
    struct wd_data *wd = &iface->detect_in;

    wd->db_state = WD_ST_INACTIVE_STABLE;
    dbg_verbose("W&D: got stable %s_WD Ina (gpio %d)\n",
                iface->name, wd->gpio);
    interface_power_off(iface);
    if (iface->switch_portid != INVALID_PORT) {
        svc_hot_unplug(iface->switch_portid, false);
    }
    /* Save last stable state for power ON/OFF handling */
    wd->last_state = wd->db_state;
}

/*
 * Debounce the single WD signal, as on DB3.
 * This handler is also handling the low power mode transitions and
 * wake-ups.
 * Requires the calling context to hold iface->mutex ino order to protect
 * interface->detect_in->db_state
 */
static int interface_debounce_wd(struct interface *iface,
                                 bool active)
{
    struct timeval now, diff, timeout_tv = { 0, 0 };
    struct wd_data *wd = &iface->detect_in;
    irqstate_t flags;

    flags = irqsave();

    /* Debounce WD signal to act as detection, which will trigger
     * the power on/off of the interface and hotplug notifications to
     * the AP.
     * Short pulses are filtered out.
     */
    switch (wd->db_state) {
    case WD_ST_INVALID:
    default:
        gettimeofday(&wd->debounce_tv, NULL);
        wd->db_state = active ?
                       WD_ST_ACTIVE_DEBOUNCE : WD_ST_INACTIVE_DEBOUNCE;
        interface_wd_delay_check(iface, (active ?
                                      WD_ACTIVATION_DEBOUNCE_TIME_MS :
                                      WD_INACTIVATION_DEBOUNCE_TIME_MS));
        break;
    case WD_ST_ACTIVE_DEBOUNCE:
        if (active) {
            timeout_tv.tv_usec = WD_ACTIVATION_DEBOUNCE_TIME_MS * 1000;
            /* Signal did not change ... for how long ? */
            gettimeofday(&now, NULL);
            timersub(&now, &wd->debounce_tv, &diff);
            if (timercmp(&diff, &timeout_tv, >=)) {
                /* We have a stable signal */
                interface_wd_handle_active_stable(iface);
            } else {
                /* Check for a stable signal after the debounce timeout */
                interface_wd_delay_check(iface, WD_ACTIVATION_DEBOUNCE_TIME_MS);
            }
        } else {
            /* Signal did change, reset the debounce timer */
            gettimeofday(&wd->debounce_tv, NULL);
            wd->db_state = WD_ST_INACTIVE_DEBOUNCE;
            interface_wd_delay_check(iface, WD_INACTIVATION_DEBOUNCE_TIME_MS);
        }
        break;
    case WD_ST_INACTIVE_DEBOUNCE:
        if (!active) {
            /* Signal did not change ... for how long ? */
            timeout_tv.tv_usec = WD_INACTIVATION_DEBOUNCE_TIME_MS;
            gettimeofday(&now, NULL);
            timersub(&now, &wd->debounce_tv, &diff);
            if (timercmp(&diff, &timeout_tv, >=)) {
                /* We have a stable signal */
                interface_wd_handle_inactive_stable(iface);
            } else {
                /* Check for a stable signal after the debounce timeout */
                interface_wd_delay_check(iface, WD_INACTIVATION_DEBOUNCE_TIME_MS);
            }
        } else {
            /* Signal did change, reset the debounce timer */
            gettimeofday(&wd->debounce_tv, NULL);
            wd->db_state = WD_ST_ACTIVE_DEBOUNCE;
            interface_wd_delay_check(iface, WD_ACTIVATION_DEBOUNCE_TIME_MS);
        }
        break;
    case WD_ST_ACTIVE_STABLE:
        if (!active) {
            /* Signal did change, reset the debounce timer */
            gettimeofday(&wd->debounce_tv, NULL);
            wd->db_state = WD_ST_INACTIVE_DEBOUNCE;
            interface_wd_delay_check(iface, WD_INACTIVATION_DEBOUNCE_TIME_MS);
        }
        break;
    case WD_ST_INACTIVE_STABLE:
        if (active) {
            /* Signal did change, reset the debounce timer */
            gettimeofday(&wd->debounce_tv, NULL);
            wd->db_state = WD_ST_ACTIVE_DEBOUNCE;
            interface_wd_delay_check(iface, WD_ACTIVATION_DEBOUNCE_TIME_MS);
        }
        break;
    }

    irqrestore(flags);

    return 0;
}

static void interface_wd_delayed_handler(void *data)
{
    struct interface *iface = (struct interface *)data;
    bool polarity, active;

    /* Take mutex */
    pthread_mutex_lock_debug(&iface->mutex);

    /* Verify state */
    if (iface->state != ARA_IFACE_STATE_WD_HANDLER_ACTIVE)
        goto done;

    /* Get signal type, polarity, active state etc. */
    polarity = iface->detect_in.polarity;
    active = (gpio_get_value(iface->detect_in.gpio) == polarity);

    dbg_insane("W&D: got %s DETECT_IN %s (gpio %d)\n",
               iface->name,
               active ? "Act" : "Ina",
               iface->detect_in.gpio);

    /* Debounce and handle state changes */
    interface_debounce_wd(iface, active);

done:
    /* Release mutex */
    pthread_mutex_unlock_debug(&iface->mutex);
}

/* Wake-Detect interrupt handler - IRQ context */
static int interface_wd_irq_handler(int irq, void *context, void *priv)
{
    struct interface *iface = priv;
    struct wd_data *wd;
    int rc;

    if (!iface) {
        dbg_error("%s: NULL interface pointer\n", __func__);
        return -ENODEV;
    }
    wd = &iface->detect_in;
    if (!work_available(&wd->work)) {
        return 0;
    }

    rc = work_queue(HPWORK, &wd->work, interface_wd_delayed_handler, iface, 0);
    if (rc) {
        dbg_error("%s: unable to start work queue, rc=%d\n", __func__, rc);
    }

    return rc;
}

/*
 * Uninstall handler for Wake & Detect pin
 * Requires the calling context to hold iface->mutex
 */
static void interface_uninstall_wd_handler(struct interface *iface, struct wd_data *wd)
{
    iface->state = ARA_IFACE_STATE_WD_HANDLER_INACTIVE;
    if (wd->gpio) {
        gpio_irq_mask(wd->gpio);
        gpio_irq_attach(wd->gpio, NULL, NULL);
    }
}

/*
 * Requires the calling context to hold iface->mutex
 */
static void interface_check_unplug_during_wake_out(struct interface *iface)
{
    enum hotplug_state hs = interface_get_hotplug_state(iface);
    switch (hs) {
    case HOTPLUG_ST_PLUGGED:
        return;
    case HOTPLUG_ST_UNKNOWN:
        /* fall through */
    default:
        dbg_warn("%s: %s: invalid or unknown hotplug state %u (gpio %u)\n",
                 __func__, iface->name, hs, iface->detect_in.gpio);
        /* fall through */
    case HOTPLUG_ST_UNPLUGGED:
        /*
         * The interface hotplug state is either invalid (in which
         * case we need to figure out what's going on) or it now reads
         * as unplugged, despite having been plugged before (or we
         * wouldn't have sent wake out).
         *
         * We'd better debounce the interface again. A full debounce
         * is needed to disambiguate the interface being unplugged
         * from something sending a wake out pulse to the SVC when we
         * checked the hotplug state.
         */
        dbg_warn("Possible unplug during wake out!\n");
        iface->detect_in.db_state = WD_ST_INVALID;
        interface_debounce_wd(iface, false);
    }
}

/*
 * Install handler for Wake & Detect pin
 *
 * Other than being called during initialization, it's called again
 * after wake out pulses are performed. However, if the module was
 * forcibly removed during the wake out pulse itself, we'll have
 * missed the interrupt. The check_for_unplug parameter determines
 * whether we need to check for that case here.
 *
 * Requires the calling context to hold iface->mutex
 */
static int interface_install_wd_handler(struct interface *iface,
                                        bool check_for_unplug)
{
    struct wd_data *wd = &iface->detect_in;

    if (wd->gpio) {
        gpio_direction_in(wd->gpio);
        gpio_set_pull(wd->gpio, GPIO_PULL_TYPE_PULL_NONE);
        if (check_for_unplug) {
            interface_check_unplug_during_wake_out(iface);
        }
        iface->state = ARA_IFACE_STATE_WD_HANDLER_ACTIVE;
        if (gpio_irq_settriggering(wd->gpio, IRQ_TYPE_EDGE_BOTH) ||
            gpio_irq_attach(wd->gpio, interface_wd_irq_handler, iface) ||
            gpio_irq_unmask(wd->gpio)) {
            dbg_error("Failed to attach Wake & Detect handler for pin %d\n",
                      wd->gpio);
            interface_uninstall_wd_handler(iface, wd);
            return -EINVAL;
        }
    }

    return 0;
}

/**
 * @brief           Return the measurement sign pin GPIO configuration.
 * @return          Measurement sign pin GPIO configuration, 0 in case of error.
 * @param[in]       iface: configured interface structure
 */
uint32_t interface_pm_get_spin(struct interface *iface)
{
    if ((!iface) || (!iface->pm)) {
        return 0;
    }

    return iface->pm->spin;
}

static int interface_eject_completion(struct interface *iface)
{
    uint8_t gpio = iface->release_gpio;
    int retval = 0;

    /* De-assert the release line */
    gpio_set_value(gpio, 0);

    switch (iface->if_type) {
    case ARA_IFACE_TYPE_MODULE_PORT:
        /* Do nothing here, let hotplug handle the module detection */
        break;
    case ARA_IFACE_TYPE_MODULE_PORT2:
        /* Release the latch resources */
        retval = vreg_put(latch_ilim);
        if (retval) {
            vreg_put(vlatch_vdd);
        }
        retval = vreg_put(vlatch_vdd);
        break;
    case ARA_IFACE_TYPE_BUILTIN:
        break;
    default:
        dbg_error("%s(): unsupported interface port type: %d\n", __func__,
                  iface->if_type);
        retval = -ENOTSUP;
    }

    /* Notify the SVC of the completion */
    svc_interface_eject_completion_notify(iface);

    return retval;
}

static void interface_eject_completion_atomic(void *data)
{
    struct interface *iface = data;

    pthread_mutex_lock_debug(&iface->mutex);
    interface_eject_completion(iface);
    pthread_mutex_unlock_debug(&iface->mutex);
}

/*
 * @brief - forcibly eject an interface
 * Requires two mutexes to be held in the following order
 *
 * lock(ifc->mutex)
 * lock(latch_ilim_lock)
 * interface_forcibly_eject
 * unlock(latch_ilim_lock)
 * unlock(ifc->mutex)
 *
 * As a result this function may not call another other function
 * that can take either of those mutexes.
 */
static int interface_forcibly_eject(struct interface *iface, uint32_t delay)
{
    uint8_t gpio = iface->release_gpio;
    struct wd_data *wd = &iface->detect_in;
    int retval = 0;

    if (!iface->ejectable) {
        return -ENOTTY;
    }

    /* Secondary interface do not contain the ejection circuitry */
    if (iface->if_order == ARA_IFACE_ORDER_SECONDARY) {
        dbg_warn("Trying to eject secondary interface: %s\n", iface->name);
    }

    dbg_info("Module %s ejecting: using gpio 0x%02X, delay=%u\n",
             iface->name, gpio, delay);

    switch (iface->if_type) {
    case ARA_IFACE_TYPE_MODULE_PORT:
        /*
         * HACK: if there is a module in the slot, but it isn't powered on
         * for some reason (e.g. dummy module), enable power.
         */
        if (gpio_is_valid(wd->gpio)) {
            gpio_direction_in(wd->gpio);
            if ((gpio_get_value(wd->gpio) == wd->polarity) &&
                    (interface_get_vsys_state(iface) != ARA_IFACE_PWR_UP)) {
                interface_vsys_enable(iface);
            }
        }
        break;

    case ARA_IFACE_TYPE_MODULE_PORT2:
        DEBUGASSERT(vlatch_vdd && latch_ilim);

        retval = interface_power_off(iface);
        if (retval) {
            dbg_error("couldn't power off interface '%s' before ejecting, aborting...\n",
                      iface->name);
            return retval;
        }

        retval = vreg_get(vlatch_vdd);
        if (retval) {
            dbg_error("couldn't enable VLATCH_VDD_EN, aborting ejection...\n");
            return retval;
        }

        retval = vreg_get(latch_ilim);
        if (retval) {
            dbg_error("couldn't enable LATCH_ILIM_EN, aborting ejection...\n");
            vreg_put(vlatch_vdd);
            return retval;
        }
        break;

    case ARA_IFACE_TYPE_BUILTIN:
        break;

    default:
        dbg_error("%s(): unsupported interface port type: %d\n", __func__,
                  iface->if_type);
        return -ENOTSUP;
    }

    /* Generate a pulse on the relase pin */
    gpio_set_value(gpio, 1);

    /*
     * Keep the line asserted for the given duration. After timeout
     * de-assert the line.
     */
    if (!work_available(&iface->eject_work)) {
        retval = work_cancel(HPWORK, &iface->eject_work);
        /*
         * work_cancel() doesn't fail in the current
         * implementation. And if it did, we'd be dead in the water
         * anyway.
         */
        DEBUGASSERT(!retval);
    }
    retval = work_queue(HPWORK, &iface->eject_work,
                        interface_eject_completion_atomic,
                        iface, MSEC2TICK(delay));
    if (retval) {
        dbg_error("%s: Could not schedule eject completion work for %s\n",
                      __func__, iface->name);
        /* If completion work cannot be scheduled, finish the work now */
        interface_eject_completion(iface);
    }

    return retval;
}

int interface_forcibly_eject_atomic(struct interface *iface, uint32_t delay)
{
    int retval;

    pthread_mutex_lock_debug(&iface->mutex);
    pthread_mutex_lock_debug(&latch_ilim_lock);

    retval = interface_forcibly_eject(iface, delay);

    pthread_mutex_unlock_debug(&latch_ilim_lock);
    pthread_mutex_unlock_debug(&iface->mutex);

    return retval;
}

/**
 * @brief Given a table of interfaces, power off all associated
 *        power supplies
 * @param interfaces table of interfaces to initialize
 * @param nr_ints number of interfaces to initialize
 * @param nr_spring_ints number of spring interfaces
 * @param vlatch VLATCH step down voltage regulator
 * @param latch_curlim Latch current limiter
 * @param mod_sense_gpio GPIO to sense the order of an interface
 * @return 0 on success, <0 on error
 */
int interface_early_init(struct interface **ints, size_t nr_ints,
                         size_t nr_spring_ints, struct vreg *vlatch,
                         struct vreg *latch_curlim, uint8_t mod_sense_gpio) {
    unsigned int i;
    int rc;
    int fail = 0;
    struct interface *ifc;

    dbg_info("Power off all interfaces\n");

    if (!ints) {
        return -ENODEV;
    }

    interfaces = ints;
    nr_interfaces = nr_ints;
    nr_spring_interfaces = nr_spring_ints;
    vlatch_vdd = vlatch;
    latch_ilim = latch_curlim;
    mod_sense = mod_sense_gpio;

    if (vlatch) {
        rc = vreg_config(vlatch);
        if (rc) {
            dbg_error("Failed to initialize VLATCH_VDD: %d\n", rc);
            return rc;
        }
    }

    if (latch_curlim) {
        rc = vreg_config(latch_curlim);
        if (rc) {
            dbg_error("Failed to initialize LATCH_ILIM: %d\n", rc);
            return rc;
        }
    }

    interface_foreach(ifc, i) {
        atomic_init(&ifc->dme_powermodeind, TSB_DME_POWERMODEIND_NONE);
        rc = interface_config(ifc);
        if (rc < 0) {
            dbg_error("Failed to configure interface %s\n", ifc->name);
            fail = 1;
            /* Continue configuring remaining interfaces */
            continue;
        }
    }

    if (fail) {
        return -1;
    }

    /* Let everything settle for a good long while.*/
    up_udelay(POWER_OFF_TIME_IN_US);

    return 0;
}

/**
 * @brief Given a table of interfaces, initialize and enable all associated
 *        power supplies
 * @param interfaces table of interfaces to initialize
 * @param nr_ints number of interfaces to initialize
 * @param nr_spring_ints number of spring interfaces
 * @param vlatch VLATCH step down voltage regulator
 * @param latch_curlim Latch current limiter
 * @param mod_sense_gpio GPIO to sense the order of an interface
 * @return 0 on success, <0 on error
 * @sideeffects: leaves interfaces powered off on error.
 */
int interface_init(struct interface **ints, size_t nr_ints,
                   size_t nr_spring_ints, struct vreg *vlatch,
                   struct vreg *latch_curlim, uint8_t mod_sense_gpio) {
    unsigned int i;
    int rc;
    struct interface *ifc;

    dbg_info("Initializing all interfaces\n");

    if (!ints) {
        return -ENODEV;
    }

    interfaces = ints;
    nr_interfaces = nr_ints;
    nr_spring_interfaces = nr_spring_ints;
    vlatch_vdd = vlatch;
    latch_ilim = latch_curlim;
    mod_sense = mod_sense_gpio;

    interface_foreach(ifc, i) {
        pthread_mutex_init(&ifc->mutex, NULL);
        pthread_mutex_lock_debug(&ifc->mutex);

        /* Install handlers for DETECT_IN signal */
        ifc->detect_in.db_state = WD_ST_INVALID;
        ifc->detect_in.last_state = WD_ST_INVALID;
        rc = interface_install_wd_handler(ifc, false);

        /* Power on/off the interface based on the DETECT_IN signal state */
        switch (interface_get_hotplug_state(ifc)) {
        case HOTPLUG_ST_PLUGGED:
            /* Port is plugged in, power ON the interface */
            if (interface_power_on(ifc) < 0) {
                dbg_error("Failed to power ON interface %s\n", ifc->name);
            }
            break;
        case HOTPLUG_ST_UNPLUGGED:
            /* Port unplugged, power OFF the interface */
            if (interface_power_off(ifc) < 0) {
                dbg_error("Failed to power OFF interface %s\n", ifc->name);
            }
            break;
        case HOTPLUG_ST_UNKNOWN:
        default:
            break;
        }

        pthread_mutex_unlock_debug(&ifc->mutex);

        if (rc) {
            return rc;
        }
    }

    return 0;
}


/**
 * @brief Disable all associated power supplies. Must have been previously
 * configured with interface_init()
 */
void interface_exit(void) {
    unsigned int i;
    struct interface *ifc;

    dbg_info("Disabling all interfaces\n");

    if (!interfaces) {
        return;
    }

    /* Uninstall handlers for DETECT_IN signal */
    interface_foreach(ifc, i) {
        pthread_mutex_lock_debug(&ifc->mutex);
        interface_uninstall_wd_handler(ifc, &ifc->detect_in);
        pthread_mutex_unlock_debug(&ifc->mutex);
    }

    /* Power off */
    interface_foreach(ifc, i) {
        /*
         * Continue turning off the rest even if this one failed - just ignore
         * the return value of interface_power_off().
         */
        pthread_mutex_lock_debug(&ifc->mutex);
        (void)interface_power_off(ifc);
        pthread_mutex_unlock_debug(&ifc->mutex);
    }

    interfaces = NULL;
    nr_interfaces = 0;
}
