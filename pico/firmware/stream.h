/* SPDX-License-Identifier: MIT */
#ifndef RADIO_STREAM_H
#define RADIO_STREAM_H
#include <stdint.h>
void radio_stream(uint32_t id, uint32_t blocks, uint32_t pause_ms, uint32_t spi_hz, uint32_t sample_rate);
#endif
