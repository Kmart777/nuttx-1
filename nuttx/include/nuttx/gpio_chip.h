/*
 * Copyright (c) 2016 Google Inc.
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

#ifndef _GPIO_CHIP_H_
#define _GPIO_CHIP_H_

#include <stdint.h>
#include <nuttx/irq.h>
#include <nuttx/gpio.h>

struct gpio_ops_s
{
    int (*get_direction)(void *driver_data, uint8_t which);
    void (*direction_in)(void *driver_data, uint8_t which);
    void (*direction_out)(void *driver_data, uint8_t which, uint8_t value);
    int (*activate)(void *driver_data, uint8_t which);
    uint8_t (*get_value)(void *driver_data, uint8_t which);
    void (*set_value)(void *driver_data, uint8_t which, uint8_t value);
    int (*deactivate)(void *driver_data, uint8_t which);
    uint8_t (*line_count)(void *driver_data);
    int (*irqattach)(void *driver_data, uint8_t which, xcpt_t isr,
                     uint8_t base, void *priv);
    int (*set_triggering)(void *driver_data, uint8_t which, int trigger);
    int (*mask_irq)(void *driver_data, uint8_t which);
    int (*unmask_irq)(void *driver_data, uint8_t which);
    int (*clear_interrupt)(void *driver_data, uint8_t which);
    int (*set_pull)(void *driver_data, uint8_t which,
                    enum gpio_pull_type pull_type);
    enum gpio_pull_type (*get_pull)(void *driver_data, uint8_t which);
    int (*set_debounce)(void *driver_data, uint8_t which, uint16_t delay);
};

int register_gpio_chip(struct gpio_ops_s *ops, int base, void *driver_data);
int unregister_gpio_chip(void *driver_data);

#endif


