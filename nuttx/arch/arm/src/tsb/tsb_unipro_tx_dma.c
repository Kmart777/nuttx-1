/*
 * Copyright (c) 2015 Google Inc.
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
 *
 * Author: Fabien Parent <fparent@baylibre.com>
 */

#include <errno.h>
#include <pthread.h>
#include <string.h>

#include <nuttx/util.h>
#include <nuttx/irq.h>
#include <nuttx/arch.h>
#include <nuttx/list.h>
#include <nuttx/unipro/unipro.h>
#include <nuttx/device_dma.h>
#include "nuttx/device_atabl.h"

#include "debug.h"
#include "up_arch.h"
#include "tsb_scm.h"
#include "tsb_unipro.h"
#include "tsb_unipro_es2.h"

#if CONFIG_ARCH_UNIPROTX_DMA_NUM_CHANNELS <= 0
#   error DMA UniPro TX must have at least one channel
#endif

#define UNIPRO_DMA_CHANNEL_COUNT CONFIG_ARCH_UNIPROTX_DMA_NUM_CHANNELS

/*
 * With ES3 or later chip, Toshiba implemented ATABL as HW flow control for
 * Unipro TX FIFO. The following strucure is used to store the info associated
 * with each Unipro TX DMA channel. Each Unipro TX DMA channel has a DMA
 * channel handler, a ATABL request, and a CPort currently mapped to request.
 * The first two items are allocated when unipro_tx_init_dma() called. The last
 * item, cportid, changes as new Cport is mapped to the request. 0xffff in
 * cporid indicates the request is currently unmapped.
 */
struct dma_channel {
    void *chan;
    void *req;
    unsigned int cportid;
    uint32_t saved_tx_water_mark;
};

struct unipro_xfer_descriptor {
    struct cport *cport;
    const void *data;
    size_t len;

    void *priv;
    unipro_send_completion_t callback;

    size_t data_offset;
    void *channel;

    struct device_dma_op *dma_op;

    struct list_head list;
};

struct unipro_xfer_descriptor_sync {
    sem_t lock;
    int retval;
};

static struct {
    pthread_t thread;
    sem_t tx_fifo_lock;
} worker;

static struct {
    struct device *dev;
    struct device *atabl_dev;
    struct dma_channel dma_channels[UNIPRO_DMA_CHANNEL_COUNT];
    struct list_head free_channel_list;
    sem_t dma_channel_lock;
    int max_channel;
} unipro_dma;

static uint32_t unipro_read(uint32_t offset) {
    return getreg32((volatile unsigned int*)(AIO_UNIPRO_BASE + offset));
}

static void unipro_write(uint32_t offset, uint32_t v) {
    putreg32(v, (volatile unsigned int*)(AIO_UNIPRO_BASE + offset));
}

static struct dma_channel *pick_dma_channel(struct cport *cport)
{
    struct dma_channel *chan;
    uint32_t chan_index;

    /*
     * Reserve GDMAC channel 0 for CPort 0 to avoid control data operations on
     * CPort 0 be blocked by other CPort operations.
     */
    chan_index = (cport->cportid == 0) ?
            0 : (((cport->cportid - 1) % (unipro_dma.max_channel - 1)) + 1);

    DEBUGASSERT(chan_index < unipro_dma.max_channel);

    chan = &unipro_dma.dma_channels[chan_index];

    return chan;
}

static void unipro_flush_cport(struct cport *cport)
{
    struct unipro_xfer_descriptor *desc;
    struct list_head *iterator = NULL;
    irqstate_t flags;

    if (list_is_empty(&cport->tx_fifo)) {
        goto reset;
    }

    flags = irqsave();
    list_reverse_foreach(&cport->tx_fifo, iterator) {
        desc = containerof(iterator, struct unipro_xfer_descriptor, list);

        if (desc->channel == NULL) {
            list_del(&desc->list);
            irqrestore(flags);

            if (desc->callback) {
                desc->callback(-ECONNRESET, desc->data, desc->priv);
            }

           free(desc);
           flags = irqsave();
        } else {
            struct dma_channel *desc_chan = desc->channel;

            if (desc->dma_op != NULL) {
                device_dma_dequeue(unipro_dma.dev,
                                   desc_chan->chan,
                                   desc->dma_op);
            }

        }
    }
    irqrestore(flags);

reset:
    _unipro_reset_cport(cport->cportid);
    cport->pending_reset = false;
    if (cport->reset_completion_cb) {
        cport->reset_completion_cb(cport->cportid,
                                   cport->reset_completion_cb_priv);
    }
    cport->reset_completion_cb = cport->reset_completion_cb_priv = NULL;
}

static struct unipro_xfer_descriptor *pick_tx_descriptor(unsigned int cportid)
{
    struct unipro_xfer_descriptor *desc;
    unsigned int cport_count = unipro_cport_count();
    int i;

    for (i = 0; i < cport_count; i++, cportid++) {
        struct cport *cport;

        cportid = cportid % cport_count;
        cport = cport_handle(cportid);
        if (!cport)
            continue;

        if (list_is_empty(&cport->tx_fifo)) {
            if (cport->pending_reset) {
                unipro_flush_cport(cport);
            }

            continue;
        }

        if (cport->pending_reset) {
            unipro_flush_cport(cport);
        }

        desc = containerof(cport->tx_fifo.next, struct unipro_xfer_descriptor,
                list);
        if (desc->channel)
            continue;

        return desc;
    }

    return NULL;
}

static inline void unipro_dma_tx_set_eom_flag(struct cport *cport)
{
    putreg8(1, CPORT_EOM_BIT(cport));
}

static void unipro_xfer_dequeue_descriptor(struct unipro_xfer_descriptor *desc)
{
    irqstate_t flags;

    flags = irqsave();
    list_del(&desc->list);
    irqrestore(flags);

    free(desc);
}

static int unipro_dma_tx_callback(struct device *dev, void *chan,
        struct device_dma_op *op, unsigned int event, void *arg)
{
    struct unipro_xfer_descriptor *desc = arg;
    int retval = OK;

    if (event & DEVICE_DMA_CALLBACK_EVENT_START) {
        int req_activated = 0;
        struct dma_channel *desc_chan = desc->channel;

        if (desc_chan->cportid != 0xFFFF) {
            req_activated = device_atabl_req_is_activated(unipro_dma.atabl_dev,
                                                          desc_chan->req);
        }
        if (req_activated != 0) {
            device_atabl_deactivate_req(unipro_dma.atabl_dev,
                                        desc_chan->req);
        }

        if (desc_chan->cportid != desc->cport->cportid) {
            if (desc_chan->cportid != 0xFFFF) {
                device_atabl_disconnect_cport_from_req(unipro_dma.atabl_dev,
                        desc_chan->req);
                desc_chan->cportid = 0xffff;
            }

            retval = device_atabl_connect_cport_to_req(unipro_dma.atabl_dev,
                             desc->cport->cportid, desc_chan->req);
            if (retval != OK) {
                lldbg("unipro: Failed to connect cport to REQn\n");
                return retval;
            }
        }
        retval = device_atabl_activate_req(unipro_dma.atabl_dev,
                                           desc_chan->req);

        if (retval) {
            lldbg("unipro: Failed to activate cport %d on REQn\n",
                  desc->cport->cportid);
            return retval;
        } else {
            desc_chan->cportid = desc->cport->cportid;
        }
    }

    if (event & DEVICE_DMA_CALLBACK_EVENT_COMPLETE) {
        if (desc->data_offset >= desc->len) {
            struct dma_channel *desc_chan = desc->channel;

            unipro_dma_tx_set_eom_flag(desc->cport);

            retval = device_dma_op_free(unipro_dma.dev, op);

            if (desc->callback != NULL) {
                desc->callback(0, desc->data, desc->priv);
            }

            device_atabl_transfer_completed(unipro_dma.atabl_dev,
                                            desc_chan->req);

            unipro_xfer_dequeue_descriptor(desc);

            if (retval != OK) {
                lldbg("Failed to free DMA op: %d\n", retval);
                goto event_complete_finally;
            }
        } else {
            desc->channel = NULL;
            retval = device_dma_op_free(unipro_dma.dev, op);
            if (retval != OK) {
                lldbg("Failed to free DMA op: %d\n", retval);
                goto event_complete_finally;
            }
        }

        event_complete_finally:
            sem_post(&worker.tx_fifo_lock);
            if (retval) {
                return retval;
            }
    }

    /*
     * The following only valid on es3 or later chips.
     */
    if (event & DEVICE_DMA_CALLBACK_EVENT_ERROR) {
        struct dma_channel *desc_chan = desc->channel;

        if (device_atabl_req_is_activated(unipro_dma.atabl_dev,
                                          desc_chan->req)) {
            unsigned int cportid = desc_chan->cportid;

            /*
             * save the current water mark setting and write 0 to it based
             * on Toshiba's document.
             */
            desc_chan->saved_tx_water_mark =
                    unipro_read(REG_TX_BUFFER_SPACE_OFFSET_REG(cportid));
            unipro_write(REG_TX_BUFFER_SPACE_OFFSET_REG(cportid), 0);

            return DEVICE_DMA_ERROR_DMA_FAILED;
        } else {
            return OK;
        }
    }

    if (event & DEVICE_DMA_CALLBACK_EVENT_RECOVERED) {
        struct dma_channel *desc_chan = desc->channel;
        uint32_t count;

        for (count = 0; count < 100; count++) {
            if (device_atabl_req_is_activated(unipro_dma.atabl_dev,
                                              desc_chan->req) == 0) {
                break;
            }
        }

        /*
         * restore the saved water mark setting based on Toshiba's document.
         */
        unipro_write(REG_TX_BUFFER_SPACE_OFFSET_REG(desc_chan->cportid),
                     desc_chan->saved_tx_water_mark);

        device_atabl_transfer_completed(unipro_dma.atabl_dev,
                                        desc_chan->req);

        return OK;
    }

    if (event & DEVICE_DMA_CALLBACK_EVENT_DEQUEUED) {
        device_dma_op_free(unipro_dma.dev, op);

        if (desc->callback != NULL) {
            desc->callback(0, desc->data, desc->priv);
        }

        unipro_xfer_dequeue_descriptor(desc);

        sem_post(&worker.tx_fifo_lock);
    }

    return retval;
}

static int unipro_dma_xfer(struct unipro_xfer_descriptor *desc,
                           struct dma_channel *channel)
{
    int retval;
    size_t xfer_len;
    void *cport_buf;
    void *xfer_buf;
    struct device_dma_op *dma_op = NULL;

    DEBUGASSERT(desc->data_offset == 0);

    xfer_len = desc->len;

    retval = device_dma_op_alloc(unipro_dma.dev, 1, 0, &dma_op);
    if (retval != OK) {
        lowsyslog("unipro: failed allocate a DMA op, retval = %d.\n", retval);
        return retval;
    }
    desc->channel = channel;

    dma_op->callback = (void *) unipro_dma_tx_callback;
    dma_op->callback_arg = desc;
    dma_op->callback_events = DEVICE_DMA_CALLBACK_EVENT_COMPLETE;
    dma_op->callback_events |=  DEVICE_DMA_CALLBACK_EVENT_START;
    dma_op->callback_events |=  DEVICE_DMA_CALLBACK_EVENT_ERROR;
    dma_op->callback_events |=  DEVICE_DMA_CALLBACK_EVENT_RECOVERED;
    dma_op->callback_events |=  DEVICE_DMA_CALLBACK_EVENT_DEQUEUED;
    dma_op->sg_count = 1;
    dma_op->sg[0].len = xfer_len;

    desc->dma_op = dma_op;

    DBG_UNIPRO("xfer: chan=%u, len=%u\n", channel->cportid, xfer_len);

    cport_buf = desc->cport->tx_buf;
    xfer_buf = (void*) desc->data;

    /* resuming a paused xfer */
    if (desc->data_offset != 0) {
        cport_buf = (char*) cport_buf + sizeof(uint64_t); /* skip the first DWORD */

        /* move buffer offset to the beginning of the remaning bytes to xfer */
        xfer_buf = (char*) xfer_buf + desc->data_offset;
    }

    dma_op->sg[0].src_addr = (off_t) xfer_buf;
    dma_op->sg[0].dst_addr = (off_t) cport_buf;

    desc->data_offset += xfer_len;

    retval = device_dma_enqueue(unipro_dma.dev, channel->chan, dma_op);
    if (retval) {
        desc->channel = NULL;
        device_dma_op_free(unipro_dma.dev, dma_op);
        lowsyslog("unipro: failed to start DMA transfer: %d\n", retval);
        return retval;
    }

    return 0;
}

static void *unipro_tx_worker(void *data)
{
    struct dma_channel *channel;
    struct unipro_xfer_descriptor *desc;
    unsigned int next_cport;
    int rc = 0;

    while (1) {
        /* Block until a buffer is pending on any CPort */
        sem_wait(&worker.tx_fifo_lock);

        next_cport = 0;
        while ((desc = pick_tx_descriptor(next_cport)) != NULL) {
            next_cport = desc->cport->cportid + 1;
            channel = pick_dma_channel(desc->cport);

            rc = unipro_dma_xfer(desc, channel);
            if (rc) {
                switch (rc) {
                    case -ENOSPC:
                        DBG_UNIPRO("DMA TX failed for lack of TX FIFO space\n");
                        break;
                    default:
                        lowsyslog("unipro: DMA transfer failed: %d\n", rc);
                        break;
                }
            }
        }
    }

    return NULL;
}

static void unipro_reset_notify_dma(unsigned int cportid)
{
    /*
     * if the tx worker is blocked on the semaphore, post something on it
     * in order to unlock it and have the reset happen right away.
     */
    sem_post(&worker.tx_fifo_lock);
}

static int unipro_send_async_dma(unsigned int cportid, const void *buf, size_t len,
        unipro_send_completion_t callback, void *priv)
{
    struct cport *cport;
    struct unipro_xfer_descriptor *desc;
    irqstate_t flags;

    cport = cport_handle(cportid);
    if (!cport) {
        lowsyslog("unipro: invalid cport id: %u, dropping message...\n",
                cportid);
        return -EINVAL;
    }

    if (cport->pending_reset) {
        return -EPIPE;
    }

    desc = zalloc(sizeof(*desc));
    if (!desc)
        return -ENOMEM;

    desc->data = buf;
    desc->len = len;
    desc->data_offset = 0;
    desc->callback = callback;
    desc->priv = priv;
    desc->cport = cport;

    list_init(&desc->list);

    flags = irqsave();
    list_add(&cport->tx_fifo, &desc->list);
    irqrestore(flags);

    sem_post(&worker.tx_fifo_lock);

    return 0;
}

static int unipro_send_cb(int status, const void *buf, void *priv)
{
    struct unipro_xfer_descriptor_sync *desc = priv;

    if (!desc)
        return -EINVAL;

    desc->retval = status;
    sem_post(&desc->lock);

    return 0;
}

static int unipro_send_dma(unsigned int cportid, const void *buf, size_t len)
{
    int retval;
    struct unipro_xfer_descriptor_sync desc;

    sem_init(&desc.lock, 0, 0);

    retval = unipro_send_async_dma(cportid, buf, len, unipro_send_cb, &desc);
    if (retval) {
        goto out;
    }

    sem_wait(&desc.lock);
    retval = desc.retval;

out:
    sem_destroy(&desc.lock);

    return retval;
}

static struct unipro_tx_calltable calltable = {
    unipro_reset_notify_dma,
    unipro_send_dma,
    unipro_send_async_dma
};

int unipro_tx_init_dma(struct unipro_tx_calltable **table)
{
    int i;
    int retval;
    int avail_chan = 0;
    enum device_dma_dev dst_device = DEVICE_DMA_DEV_MEM;

    sem_init(&worker.tx_fifo_lock, 0, 0);
    sem_init(&unipro_dma.dma_channel_lock, 0, 0);

    unipro_dma.dev = device_open(DEVICE_TYPE_DMA_HW, 0);
    if (!unipro_dma.dev) {
        lldbg("unipro: Failed to open DMA driver.\n");
        return -ENODEV;
    }

    /*
     * Setup HW hand shake threshold.
     */
    for (i = 0; i < unipro_cport_count(); i++) {
        uint32_t offset_value =
            unipro_read(REG_TX_BUFFER_SPACE_OFFSET_REG(i));


#ifdef CONFIG_ARCH_UNIPROTX_DMA_WMB
        unipro_write(REG_TX_BUFFER_SPACE_OFFSET_REG(i),
                     offset_value | (0x10 << 8));
#else
        unipro_write(REG_TX_BUFFER_SPACE_OFFSET_REG(i),
                     offset_value | (0x20 << 8));
#endif
    }

    /*
     * Open Atabl driver.
     */
    unipro_dma.atabl_dev = device_open(DEVICE_TYPE_ATABL_HW, 0);
    if (!unipro_dma.atabl_dev) {
        lldbg("unipro: Failed to open ATABL driver.\n");

        device_close(unipro_dma.dev);
        return -ENODEV;
    }

    unipro_dma.max_channel = 0;
    list_init(&unipro_dma.free_channel_list);
    avail_chan = device_dma_chan_free_count(unipro_dma.dev);

    if (avail_chan > ARRAY_SIZE(unipro_dma.dma_channels)) {
        avail_chan = ARRAY_SIZE(unipro_dma.dma_channels);
    }

    DEBUGASSERT(avail_chan > 1);

    dst_device = DEVICE_DMA_DEV_UNIPRO;

    if (device_atabl_req_free_count(unipro_dma.atabl_dev) < avail_chan) {
        device_close(unipro_dma.dev);
        device_close(unipro_dma.atabl_dev);
        return -ENODEV;
    }

    for (i = 0; i < avail_chan; i++) {
        struct device_dma_params chan_params = {
                .src_dev = DEVICE_DMA_DEV_MEM,
                .src_devid = 0,
                .src_inc_options = DEVICE_DMA_INC_AUTO,
                .dst_dev = dst_device,
                .dst_devid = 0,
                .dst_inc_options = DEVICE_DMA_INC_AUTO,
                .transfer_size = DEVICE_DMA_TRANSFER_SIZE_64,
                .burst_len = DEVICE_DMA_BURST_LEN_16,
                .swap = DEVICE_DMA_SWAP_SIZE_NONE,
        };

        if (device_atabl_req_alloc(unipro_dma.atabl_dev,
                                    &unipro_dma.dma_channels[i].req)) {
            break;
        }

        chan_params.dst_devid = device_atabl_req_to_peripheral_id(
                                    unipro_dma.atabl_dev,
                                    unipro_dma.dma_channels[i].req);

        device_dma_chan_alloc(unipro_dma.dev, &chan_params,
                              &unipro_dma.dma_channels[i].chan);

        if (unipro_dma.dma_channels[i].chan == NULL) {
            lowsyslog("unipro: couldn't allocate all %u requested channel(s)\n",
                    ARRAY_SIZE(unipro_dma.dma_channels));
            break;
        }

        unipro_dma.dma_channels[i].cportid = 0xFFFF;
        unipro_dma.max_channel++;
    }

    if (unipro_dma.max_channel <= 0) {
        lowsyslog("unipro: couldn't allocate a single DMA channel\n");
        retval = -ENODEV;
        goto error_no_channel;
    }

    lowsyslog("unipro: %d DMA channel(s) allocated\n", unipro_dma.max_channel);

    retval = pthread_create(&worker.thread, NULL, unipro_tx_worker, NULL);
    if (retval) {
        lldbg("unipro: Failed to create worker thread: %s.\n", strerror(errno));
        goto error_worker_create;
    }

    *table = &calltable;

    return 0;

error_worker_create:

    for (i = 0; i < unipro_dma.max_channel; i++) {
        device_atabl_req_free(unipro_dma.atabl_dev,
                              &unipro_dma.dma_channels[i].req);

        device_dma_chan_free(unipro_dma.dev, &unipro_dma.dma_channels[i]);
    }

    unipro_dma.max_channel = 0;

error_no_channel:
    device_close(unipro_dma.atabl_dev);
    unipro_dma.atabl_dev = NULL;

    device_close(unipro_dma.dev);
    unipro_dma.dev = NULL;

    return retval;
}
