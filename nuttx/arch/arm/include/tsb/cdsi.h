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
 */

#ifndef __ARCH_ARM_INCLUDE_TSB_DSI_H
#define __ARCH_ARM_INCLUDE_TSB_DSI_H

#define TSB_CDSI0 0
#define TSB_CDSI1 1

enum cdsi_direction {
    TSB_CDSI_RX,
    TSB_CDSI_TX,
};

struct cdsi_dev
{
    uint32_t base;
    enum cdsi_direction dir;

    /* CSI clock configuration values */
    uint32_t hsck_mhz;
    uint32_t pll_config_fbd;
    uint32_t pll_config_prd;
    uint32_t pll_config_frs;
    uint32_t pic_com_delay;
};

void cdsi_write(struct cdsi_dev *dev, uint32_t addr, uint32_t v);
uint32_t cdsi_read(struct cdsi_dev *dev, uint32_t addr);

struct cdsi_dev *cdsi_open(int dsi, enum cdsi_direction dir);
void cdsi_close(struct cdsi_dev *dev);

void cdsi_enable(struct cdsi_dev *dev);
void cdsi_disable(struct cdsi_dev *dev);

#endif

