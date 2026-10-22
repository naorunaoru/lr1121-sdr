#include "stream.h"
#include "stream_queue.h"
#include "stream_format.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "tusb.h"

_Static_assert(CFG_TUD_CDC_RX_BUFSIZE>=CFG_TUD_CDC_EP_BUFSIZE,
               "CDC receive FIFO must hold a complete endpoint transfer");

#define CS 13u
#define BUSY 14u
#define FIFO_FRAMES 64u

typedef struct { uint8_t bytes[IQ_FRAME_BYTES]; uint32_t sequence, timestamp; } frame_t;
static queue_t frames;
static frame_t producer_frame, consumer_frame;
static uint8_t rx[257], command_rx[16];
static atomic_bool stop_requested, finished;
static uint32_t job_id, job_blocks, job_pause, job_rate, job_flags, produced, max_queue, read_ops, max_block_us;
static uint64_t job_start, job_deadline;
static int spi_rx_dma, spi_tx_dma;
static const uint8_t spi_dummy=0;

// Cancellation is observed between SPI transactions. Completing each bounded
// transaction prevents a normal ETX from masquerading as a radio I/O failure.
static bool read_payload(size_t bytes) {
    if (bytes<=17) {
        spi_read_blocking(spi1,0,rx,bytes);
        return true;
    }
    dma_channel_config r=dma_channel_get_default_config((uint)spi_rx_dma);
    channel_config_set_transfer_data_size(&r,DMA_SIZE_8);
    channel_config_set_read_increment(&r,false);
    channel_config_set_write_increment(&r,true);
    channel_config_set_dreq(&r,spi_get_dreq(spi1,false));
    dma_channel_config t=dma_channel_get_default_config((uint)spi_tx_dma);
    channel_config_set_transfer_data_size(&t,DMA_SIZE_8);
    channel_config_set_read_increment(&t,false);
    channel_config_set_write_increment(&t,false);
    channel_config_set_dreq(&t,spi_get_dreq(spi1,true));
    dma_channel_configure((uint)spi_rx_dma,&r,rx,&spi_get_hw(spi1)->dr,bytes,false);
    dma_channel_configure((uint)spi_tx_dma,&t,&spi_get_hw(spi1)->dr,&spi_dummy,bytes,false);
    dma_start_channel_mask((1u<<spi_rx_dma)|(1u<<spi_tx_dma));
    uint64_t deadline=time_us_64()+2000;
    while(dma_channel_is_busy((uint)spi_rx_dma) || dma_channel_is_busy((uint)spi_tx_dma)) {
        if(time_us_64()>=deadline) {
            dma_channel_abort((uint)spi_tx_dma);dma_channel_abort((uint)spi_rx_dma);
            // TX may already have queued bytes in the SPI FIFO. Drain the
            // corresponding replies before cleanup sends another command.
            uint64_t drain_deadline=time_us_64()+20;
            while(spi_is_busy(spi1) && time_us_64()<drain_deadline)tight_loop_contents();
            while(spi_is_readable(spi1))(void)spi_get_hw(spi1)->dr;
            spi_get_hw(spi1)->icr=SPI_SSPICR_RORIC_BITS;
            return false;
        }
        tight_loop_contents();
    }
    return true;
}

static void be32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
}
static bool ready(void) {
    uint64_t deadline=time_us_64()+20000;
    while (gpio_get(BUSY)) {
        if (time_us_64()>=deadline) return false;
        tight_loop_contents();
    }
    return true;
}
static void tx(const uint8_t *p, size_t n) {
    gpio_put(CS,0); busy_wait_us_32(1);
    spi_write_read_blocking(spi1,p,command_rx,n);
    gpio_put(CS,1); busy_wait_us_32(5);
}
static bool read_mem(uint32_t address, uint32_t words) {
    uint8_t c[7]={1,6,0,0,0,0,(uint8_t)words}; be32(c+2,address);
    if (!words || words>64 || !ready()) return false;
    tx(c,7);
    if (!ready()) return false;
    gpio_put(CS,0); busy_wait_us_32(1);
    bool received=read_payload(words*4+1);
    gpio_put(CS,1); busy_wait_us_32(5); ++read_ops;
    return received && rx[0]>>1==3;
}
static bool read_word(uint32_t address,uint32_t *v) {
    if (!read_mem(address,1)) return false;
    *v=iq_be32(rx+1); return true;
}
static bool masked(uint32_t address,uint32_t mask,uint32_t value) {
    uint8_t c[14]={1,12}; be32(c+2,address); be32(c+6,mask); be32(c+10,value);
    if (!ready()) return false;
    tx(c,14);
    if (!ready()) return false;
    uint8_t zero[6]={0}; tx(zero,6);
    return command_rx[0]>>1==2;
}
static bool gate(bool enabled) {
    return masked(0xf20014,0x30000010,enabled?0x30000010:0x30000000);
}
static bool stopped(void) {
    if (atomic_load(&stop_requested)) {job_flags|=IQ_ABORT; return true;}
    if (time_us_64()>=job_deadline) {job_flags|=IQ_DEADLINE; return true;}
    return false;
}
static bool pointer_valid(uint32_t pointer) {
    if (pointer==0x800a00 || pointer==0x800a10) return true;
    return pointer>=IQ_DESC_BASE+32 && pointer<IQ_DESC_BASE+IQ_BANKS*16 && !(pointer&15);
}
static bool not_starved(uint32_t descriptor) {
    uint32_t pointer;
    if (!read_word(IQ_POINTER,&pointer) || !pointer_valid(pointer)) {job_flags|=IQ_RADIO_ERROR;return false;}
    if (pointer==descriptor) {
        // The bank is complete but DMA has reached it again before it was rearmed.
        // Re-read after a whole SPI transaction to exclude a pointer-update edge.
        if (!read_word(IQ_POINTER,&pointer)) {job_flags|=IQ_RADIO_ERROR;return false;}
        if (pointer==descriptor) {job_flags|=IQ_DMA_STARVED;return false;}
    }
    return true;
}
static void acquire(void) {
    job_start=time_us_64();
    job_deadline=job_start+1000000;
    if (!gate(true)) job_flags|=IQ_RADIO_ERROR;
    for (uint32_t seq=0;(!job_blocks || seq<job_blocks) && !job_flags;++seq) {
        uint32_t bank=seq%IQ_BANKS, descriptor=iq_descriptor(bank), control=0;
        while (true) {
            if (stopped()) break;
            if (!read_word(descriptor,&control)) {job_flags|=IQ_RADIO_ERROR;break;}
            if (!(control&3)) break;
            busy_wait_us_32(5);
        }
        if (job_flags) break;
        uint64_t block_start=time_us_64();
        if ((control & ~0x001fff03u)!=(IQ_CONTROL & ~0x001fff03u)) {job_flags|=IQ_RADIO_ERROR;break;}
        uint8_t *payload=producer_frame.bytes+IQ_HEADER_BYTES;
        for (uint32_t word=0;word<IQ_BANK_BYTES/4;word+=64) {
            uint32_t n=IQ_BANK_BYTES/4-word; if (n>64)n=64;
            if (stopped()) break;
            if (!read_mem(IQ_DATA_BASE+bank*IQ_BANK_BYTES+word*4,n)) {
                job_flags|=IQ_RADIO_ERROR;break;
            }
            if (!iq_pack23(payload+word*3,rx+1,n)) {job_flags|=IQ_FORMAT_ERROR;break;}
        }
        // The invalid descriptor protects this bank throughout the copy. Check
        // the producer pointer once, immediately before giving ownership back.
        if (job_flags || !not_starved(descriptor)) break;
        // Invalidate -> copy -> rearm. No completed bank is reused before the copy.
        if (!masked(descriptor,0xffffffffu,IQ_CONTROL)) {job_flags|=IQ_RADIO_ERROR;break;}
        producer_frame.sequence=seq;
        producer_frame.timestamp=(uint32_t)(time_us_64()-job_start);
        if (!queue_try_add(&frames,&producer_frame)) {job_flags|=IQ_QUEUE_FULL;break;}
        ++produced;
        // Progress deadline works for both finite and indefinite streams.
        job_deadline=time_us_64()+1000000;
        uint32_t depth=queue_get_level(&frames); if (depth>max_queue)max_queue=depth;
        uint32_t duration=(uint32_t)(time_us_64()-block_start); if(duration>max_block_us)max_block_us=duration;
        if (job_pause && produced==16) {
            uint64_t until=time_us_64()+job_pause*1000;
            while(time_us_64()<until && !stopped()) tight_loop_contents();
        }
    }
    if (!gate(false)) job_flags|=IQ_RADIO_ERROR;
    atomic_store(&finished,true);
    while(true)tight_loop_contents();
}
static bool usb_send(const uint8_t *data,size_t size,uint64_t deadline) {
    size_t offset=0;
    while(offset<size) {
        tud_task();
        if (!tud_cdc_connected() || time_us_64()>=deadline) {atomic_store(&stop_requested,true);return false;}
        uint8_t in[64];
        uint32_t n=tud_cdc_read(in,sizeof(in));
        for(uint32_t i=0;i<n;++i)if(in[i]==3)atomic_store(&stop_requested,true);
        uint32_t available=tud_cdc_write_available();
        if (available) {
            uint32_t nout=(uint32_t)(size-offset);if(nout>available)nout=available;
            offset+=tud_cdc_write(data+offset,nout);
        }
        tud_cdc_write_flush();
    }
    return true;
}
static bool validate(void) {
    uint32_t value;
    if (!read_word(0xf20124,&value) || ((value>>20)&7)!=(job_rate==62500?5u:4u)) return false;
    if (!read_word(0xf20014,&value) || (value&0x30000010)!=0x30000000) return false;
    if (!read_word(IQ_POINTER,&value) || value!=0x800a00) return false;
    for(uint32_t i=0;i<IQ_BANKS;++i) {
        if (!read_mem(iq_descriptor(i),4))return false;
        if (iq_be32(rx+1)!=IQ_CONTROL || iq_be32(rx+5)!=0x901004 ||
            iq_be32(rx+9)!=IQ_DATA_BASE+(i+1)*IQ_BANK_BYTES-4 ||
            iq_be32(rx+13)!=iq_descriptor((i+1)%IQ_BANKS)) return false;
        tud_task();
    }
    return true;
}
void radio_stream(uint32_t id,uint32_t blocks,uint32_t pause_ms,uint32_t spi_hz,uint32_t sample_rate) {
    atomic_store(&stop_requested,false); atomic_store(&finished,false);
    job_flags=produced=max_queue=read_ops=max_block_us=0;
    job_id=id;job_blocks=blocks;job_pause=pause_ms;job_rate=sample_rate;
    if((sample_rate!=62500 && sample_rate!=125000) || spi_hz!=15625000 || !validate()) {
        printf("{\"id\":%lu,\"ok\":false,\"error\":\"stream_configuration\",\"phase\":\"validate\",\"capture_started\":false}\n",(unsigned long)id);
        return;
    }
    if(!stream_queue_init(&frames,sizeof(frame_t),FIFO_FRAMES)) {
        printf("{\"id\":%lu,\"ok\":false,\"error\":\"stream_queue_capacity\"}\n",(unsigned long)id);
        return;
    }
    spi_rx_dma=dma_claim_unused_channel(true);spi_tx_dma=dma_claim_unused_channel(true);
    printf("{\"id\":%lu,\"ok\":true,\"stream\":\"LIQ1\",\"format\":\"iq23le\",\"header_bytes\":44,\"samples_per_frame\":125,\"blocks\":%lu}\n",(unsigned long)id,(unsigned long)blocks);
    stdio_flush();
    uint64_t usb_deadline=time_us_64()+2000000;
    multicore_launch_core1(acquire);
    bool usb_ok=true;
    uint32_t sent=0;
    while(!atomic_load(&finished) || !queue_is_empty(&frames)) {
        if(queue_try_remove(&frames,&consumer_frame)) {
            // CRC and USB framing run on core 0, outside the SPI drain path.
            iq_header_rate(consumer_frame.bytes,1,0,job_id,consumer_frame.sequence,IQ_SAMPLES,IQ_PAYLOAD_BYTES,
                      consumer_frame.timestamp,iq_crc32(consumer_frame.bytes+IQ_HEADER_BYTES,IQ_PAYLOAD_BYTES),job_rate);
            if(usb_ok)usb_ok=usb_send(consumer_frame.bytes,IQ_FRAME_BYTES,usb_deadline);
            if(usb_ok) {++sent;usb_deadline=time_us_64()+2000000;}
        } else {
            tud_task();
            uint8_t c;
            if(tud_cdc_read(&c,1) && c==3)atomic_store(&stop_requested,true);
            if(!tud_cdc_connected() || time_us_64()>=usb_deadline) {
                usb_ok=false;atomic_store(&stop_requested,true);
            }
        }
    }
    multicore_reset_core1();
    if(!usb_ok)job_flags|=IQ_USB_LOST;
    if(job_pause)job_flags|=IQ_INJECTED_PAUSE;
    uint8_t end[IQ_HEADER_BYTES+36]; uint8_t *stats=end+IQ_HEADER_BYTES;
    iq_le32(stats,produced);iq_le32(stats+4,sent);iq_le32(stats+8,max_queue);
    iq_le32(stats+12,read_ops);iq_le32(stats+16,max_block_us);iq_le32(stats+20,spi_hz);
    iq_le32(stats+24,(uint32_t)(time_us_64()-job_start));
    iq_le32(stats+28,job_flags&(IQ_DMA_STARVED|IQ_QUEUE_FULL|IQ_USB_LOST|IQ_RADIO_ERROR|IQ_FORMAT_ERROR)?0xffffffffu:0);
    iq_le32(stats+32,blocks);
    iq_header_rate(end,2,(uint16_t)job_flags,id,produced,0,36,(uint32_t)(time_us_64()-job_start),iq_crc32(stats,36),job_rate);
    if(usb_ok)(void)usb_send(end,sizeof(end),usb_deadline);
    // Drain the last USB packet before returning to the ASCII command loop.
    uint64_t flush_deadline=time_us_64()+100000;
    while(tud_cdc_connected() && tud_cdc_write_available()<CFG_TUD_CDC_TX_BUFSIZE && time_us_64()<flush_deadline) {
        tud_task();tud_cdc_write_flush();
    }
    frames.data=NULL; // Static storage is shared; never pass it to free().
    dma_channel_unclaim((uint)spi_rx_dma);dma_channel_unclaim((uint)spi_tx_dma);
}
