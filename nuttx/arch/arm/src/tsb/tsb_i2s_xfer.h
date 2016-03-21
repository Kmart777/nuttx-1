/**
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
 *
 * @author Kim Mui
 * @brief Pseudo DMA driver that uses memcpy instead of real DMA
 */

#ifndef __TSB_I2S_XFER_H
#define __TSB_I2S_XFER_H

#include "tsb_i2s.h"

/*
 * Data movement routines provided by tsb_i2s_xfer,c or tsb_i2s_xfer_dma.c
 */

int tsb_i2s_start_receiver(struct tsb_i2s_info *info);
void tsb_i2s_stop_receiver(struct tsb_i2s_info *info, int is_err);
int tsb_i2s_start_transmitter(struct tsb_i2s_info *info);
void tsb_i2s_stop_transmitter(struct tsb_i2s_info *info, int is_err);
int tsb_i2s_tx_data(struct tsb_i2s_info *info);
int tsb_i2s_rx_data(struct tsb_i2s_info *info);

/*
 *  Nothing need to be done in the following routines if we use SW pulling
 */
int tsb_i2s_xfer_open(struct tsb_i2s_info *info);
void tsb_i2s_xfer_close(struct tsb_i2s_info *info);
int tsb_i2s_xfer_prepare_receiver(struct tsb_i2s_info *info);
int tsb_i2s_xfer_prepare_transmitter(struct tsb_i2s_info *info);

#endif /* __TSB_I2S_XFER_H */
