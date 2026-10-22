#include "stream_queue.h"
#include <stdint.h>
#include <string.h>
static uint32_t storage[41580]; // 385 * 432 bytes, including the queue sentinel
bool stream_queue_init(queue_t *q, uint element_size, uint element_count) {
    if(!element_size || element_size>65535 || element_count>65534 ||
       (size_t)(element_count+1)*element_size>sizeof(storage))return false;
    memset(q,0,sizeof(*q));
    lock_init(&q->core,next_striped_spin_lock_num());
    q->element_count=(uint16_t)element_count;
    q->element_size=(uint16_t)element_size;
    q->data=(uint8_t*)storage;
    return true;
}
