/* SPDX-License-Identifier: MIT */
#ifndef LR1121_STREAM_QUEUE_H
#define LR1121_STREAM_QUEUE_H
#include "pico/util/queue.h"
// The two stream engines run exclusively. They share reserved storage, so
// switching between raw and packed frames cannot fragment the small RP2040 heap.
bool stream_queue_init(queue_t *q, uint element_size, uint element_count);
#endif
