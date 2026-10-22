#ifndef STREAM_FORMAT_H
#define STREAM_FORMAT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define IQ_BANKS 32u
#define IQ_BANK_BYTES 1000u
#define IQ_SAMPLES 125u
#define IQ_PAYLOAD_BYTES 750u
#define IQ_HEADER_BYTES 44u
#define IQ_FRAME_BYTES (IQ_HEADER_BYTES + IQ_PAYLOAD_BYTES)
#define IQ_CONTROL 0x9fe3e7a7u
#define IQ_DATA_BASE 0x00806000u
#define IQ_DESC_BASE 0x0080e000u
#define IQ_POINTER 0x00800580u
enum { IQ_DMA_STARVED=1, IQ_QUEUE_FULL=2, IQ_ABORT=4, IQ_USB_LOST=8,
       IQ_RADIO_ERROR=16, IQ_DEADLINE=32, IQ_FORMAT_ERROR=64, IQ_INJECTED_PAUSE=128 };
uint32_t iq_descriptor(uint32_t index);
uint32_t iq_crc32(const uint8_t *p, size_t n);
bool iq_pack23(uint8_t *dst, const uint8_t *wire, size_t words);
void iq_le32(uint8_t *p, uint32_t v);
uint32_t iq_be32(const uint8_t *p);
void iq_header(uint8_t *p, uint8_t type, uint16_t flags, uint32_t id,
               uint32_t sequence, uint32_t samples, uint32_t length,
               uint32_t timestamp_us, uint32_t crc);
void iq_header_rate(uint8_t *p, uint8_t type, uint16_t flags, uint32_t id,
               uint32_t sequence, uint32_t samples, uint32_t length,
               uint32_t timestamp_us, uint32_t crc, uint32_t sample_rate);
#endif
