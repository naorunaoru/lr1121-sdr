/* SPDX-License-Identifier: MIT */
#ifndef LR1121_TX_FORMAT_H
#define LR1121_TX_FORMAT_H
#include <stdbool.h>
#include <stdint.h>
static inline bool tx_format_valid(uint32_t center, uint32_t shift, uint32_t period) {
    if (shift < 1 || shift > 15 || period < 128 || period > 1600) return false;
    return (center & 1u) && center > (32768u << shift) &&
           center <= UINT32_MAX - (32767u << shift);
}
static inline uint32_t tx_expand_offset(uint32_t center, uint32_t shift, int16_t offset) {
    /* Caller validates the complete signed16 range before receiving frames. */
    int32_t delta = (int32_t)offset * (int32_t)(1u << shift);
    return center + (uint32_t)delta;
}
#endif
