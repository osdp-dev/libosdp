/*
 * Copyright (c) 2024-2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <Arduino.h>
#include <utils/utils.h>

extern "C" tick_t osdp_millis_now()
{
  return (tick_t)millis();
}
