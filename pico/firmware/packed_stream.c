/* SPDX-License-Identifier: MIT */
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

#undef IQ_BANKS
#undef IQ_BANK_BYTES
#undef IQ_SAMPLES
#undef IQ_PAYLOAD_BYTES
#undef IQ_FRAME_BYTES
#undef IQ_CONTROL
#define IQ_BANKS 16u
#define IQ_BANK_BYTES 2048u
#define IQ_SAMPLES 125u
#define IQ_PAYLOAD_BYTES 376u // maximum 375-byte packed payload, rounded to a word
#define IQ_FRAME_BYTES (IQ_HEADER_BYTES+IQ_PAYLOAD_BYTES)
#define IQ_CONTROL 0x9fe3e7a7u
#define CS 13u
#define BUSY 14u
#define FIFO_FRAMES 384u // 96 ms of 500 kS/s blocks; shared static queue

typedef struct { uint8_t bytes[IQ_FRAME_BYTES]; uint32_t sequence, timestamp, shift; } frame_t;
static queue_t frames;
static frame_t producer_frame, consumer_frame;
static uint8_t usb_group[IQ_HEADER_BYTES+16*(IQ_PAYLOAD_BYTES+1)];
static uint8_t rx[257], command_rx[16];
static atomic_bool stop_requested, finished;
static uint32_t job_id, job_blocks, job_pause, job_rate, job_bits, payload_bytes, job_flags, produced, max_queue, read_ops, max_block_us;
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
    if(job_pause>=14 && job_pause<=17) {
        size_t burst=job_pause==14?4:job_pause==16?16:8;
        for(size_t offset=0;offset<bytes;offset+=burst) {
            size_t n=bytes-offset;if(n>burst)n=burst;
            spi_read_blocking(spi1,0,rx+offset,n);
            if(offset+n<bytes)busy_wait_us_32(1);
        }
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
// Direct little-endian RAM response. Flag publication occurs only after the
// previous reply completed; LR callback disables TX before rebuilding DMA.
static bool direct_bank(uint32_t address) {
    uint8_t c[14]={1,12};be32(c+2,0x80f024);be32(c+6,0xffffffffu);be32(c+10,address);
    if(!ready())return false;
    tx(c,14);
    if(!ready())return false;
    busy_wait_us_32(15); // allow main-loop callback after command completion
    gpio_put(CS,0);busy_wait_us_32(1);
    bool ok=read_payload(257);
    gpio_put(CS,1);busy_wait_us_32(5);++read_ops;
    // Rest of bridge expects big-endian register words.
    for(unsigned i=1;i<257;i+=4) {
        uint8_t a=rx[i],b=rx[i+1];rx[i]=rx[i+3];rx[i+1]=rx[i+2];rx[i+2]=b;rx[i+3]=a;
    }
    return ok && rx[0]>>1==3;
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
    if(job_pause>=4 && job_pause<=6) {
        uint32_t callback;
        if(!read_word(0x80f004,&callback) || !masked(0x8004ac,0xffffffffu,callback))job_flags|=IQ_RADIO_ERROR;
    }
    if (!gate(true)) job_flags|=IQ_RADIO_ERROR;
    if (job_pause==2) busy_wait_us_32(20000);
    if((job_pause>=3 && job_pause<=6) || job_pause==12) {
        for(unsigned k=0;k<200 && !job_flags;k++) {
            if(job_pause==12) {if(!direct_bank(0x80f400))job_flags|=IQ_RADIO_ERROR;}
            else if(job_pause==6) {uint8_t zero[6]={0};if(!ready())job_flags|=IQ_RADIO_ERROR;else tx(zero,6);}
            else if(!read_mem(0x80e000,job_pause==5?1:64))job_flags|=IQ_RADIO_ERROR;
        }
        busy_wait_us_32(20000);
        if(!gate(false))job_flags|=IQ_RADIO_ERROR;
        if(job_pause>=4 && job_pause<=6 && (!masked(0x8004ac,0xffffffffu,0x94510) || !masked(0x8004bc,2,2)))job_flags|=IQ_RADIO_ERROR;
        busy_wait_us_32(20000);
    }
    for (uint32_t seq=0;(!job_blocks || seq<job_blocks) && !job_flags;++seq) {
        uint32_t bank=seq%IQ_BANKS, descriptor=iq_descriptor(bank), control=0;
        uint32_t bank_address=IQ_DATA_BASE+bank*IQ_BANK_BYTES;
        uint64_t block_start=time_us_64();
        while (true) {
            if (stopped()) break;
            if (!((job_pause==10 || job_pause==11 || job_pause==18) ? direct_bank(bank_address) : read_mem(bank_address,64))) {job_flags|=IQ_RADIO_ERROR;break;}
            control=iq_be32(rx+1);
            if (control&0x80000000u) break;
            busy_wait_us_32(5);
        }
        if (job_flags) break;
        if ((control&0xffffffu)!=((seq+1)&0xffffffu) || ((control>>24)&127)>16) {job_flags|=IQ_RADIO_ERROR;break;}
        producer_frame.shift=(control>>24)&127;
        uint8_t *payload=producer_frame.bytes+IQ_HEADER_BYTES;
        for(uint32_t w=0;w<63;++w) {
            payload[4*w]=rx[4*w+8];payload[4*w+1]=rx[4*w+7];
            payload[4*w+2]=rx[4*w+6];payload[4*w+3]=rx[4*w+5];
        }
        for (uint32_t word=63;word<(payload_bytes+3)/4;word+=64) {
            uint32_t n=(payload_bytes+3)/4-word; if (n>64)n=64;
            if (stopped()) break;
            if (!read_mem(bank_address+4+word*4,n)) {
                job_flags|=IQ_RADIO_ERROR;break;
            }
            for(uint32_t w=0;w<n;++w) {
                payload[4*(word+w)]=rx[4*w+4];
                payload[4*(word+w)+1]=rx[4*w+3];
                payload[4*(word+w)+2]=rx[4*w+2];
                payload[4*(word+w)+3]=rx[4*w+1];
            }
        }
        // The invalid descriptor protects this bank throughout the copy. Check
        // the producer pointer once, immediately before giving ownership back.
        if (job_flags || ((!job_pause || (job_pause==11 || job_pause==17)) && !not_starved(descriptor))) break;
        // Invalidate -> copy -> rearm. No completed bank is reused before the copy.
        if ((!job_pause || (job_pause==11 || job_pause==17)) && !masked(descriptor,0xffffffffu,IQ_CONTROL)) {job_flags|=IQ_RADIO_ERROR;break;}
        producer_frame.sequence=seq;
        producer_frame.timestamp=(uint32_t)(time_us_64()-job_start);
        if (!queue_try_add(&frames,&producer_frame)) {job_flags|=IQ_QUEUE_FULL;break;}
        ++produced;
        // Progress deadline works for both finite and indefinite streams.
        job_deadline=time_us_64()+1000000;
        uint32_t depth=queue_get_level(&frames); if (depth>max_queue)max_queue=depth;
        uint32_t duration=(uint32_t)(time_us_64()-block_start); if(duration>max_block_us)max_block_us=duration;
        if (job_pause==7 && produced==16) {
            uint64_t until=time_us_64()+job_pause*1000;
            while(time_us_64()<until && !stopped()) tight_loop_contents();
        }
    }
    if (!gate(false) || !masked(0x80f008,0xffffffffu,0)) job_flags|=IQ_RADIO_ERROR;
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
static void packed_header(uint8_t *p,uint8_t type,uint16_t flags,uint32_t id,
        uint32_t sequence,uint32_t samples,uint32_t length,uint32_t timestamp,uint32_t crc,uint32_t rate) {
    iq_header_rate(p,type,flags,id,sequence,samples,length,timestamp,crc,rate);
    memcpy(p,job_pause==18?"LIP3":"LIP2",4);iq_le32(p+16,sequence*IQ_SAMPLES);
    iq_le32(p+40,iq_crc32(p,40));
}
// Experimental LIP3: reset the reversible scrambler at each USB data frame.
// CRC covers scrambled wire bytes; sequence supplies the deterministic seed.
static void scramble(uint8_t *p,size_t n,uint32_t sequence) {
    uint32_t x=(sequence ^ 0x9e3779b9u)|1u;
    for(size_t i=0;i<n;) {
        x^=x<<13;x^=x>>17;x^=x<<5;
        for(unsigned shift=0;shift<32 && i<n;shift+=8) p[i++]^=(uint8_t)(x>>shift);
    }
}
static bool validate(void) {
    uint32_t value;
    if (!read_word(0x80f000,&value) || value!=0x49515033) return false;
    if (!read_word(0x80f014,&value) || value!=job_bits) return false;
    if (!read_word(0x80f008,&value) || value!=1) return false;
    if (!read_word(0x8004ac,&value) || value!=0x94510) return false;
    if (!read_word(0xf20124,&value) || ((value>>20)&7)!=(job_rate==62500?5u:job_rate==125000?4u:job_rate==250000?3u:job_rate==500000?2u:1u)) return false;
    if (!read_word(0xf20014,&value) || (value&0x30000010)!=0x30000000) return false;
    if (!read_word(IQ_POINTER,&value) || value!=0x800a00) return false;
    for(uint32_t i=0;i<IQ_BANKS;++i) {
        if (!read_mem(iq_descriptor(i),4))return false;
        uint32_t control=iq_be32(rx+1);
        if ((job_pause ? (control&~0x1fe00000u)!=(IQ_CONTROL&~0x1fe00000u) : control!=IQ_CONTROL) || iq_be32(rx+5)!=0x901004 ||
            iq_be32(rx+9)!=IQ_DATA_BASE+i*IQ_BANK_BYTES+1000-4 ||
            iq_be32(rx+13)!=iq_descriptor((i+1)%IQ_BANKS)) return false;
        tud_task();
    }
    return true;
}
void packed_stream(uint32_t id,uint32_t blocks,uint32_t pause_ms,uint32_t spi_hz,uint32_t sample_rate,uint32_t bits) {
    atomic_store(&stop_requested,false); atomic_store(&finished,false);
    job_flags=produced=max_queue=read_ops=max_block_us=0;
    job_id=id;job_blocks=blocks;job_pause=pause_ms;job_rate=sample_rate;
    job_bits=bits;payload_bytes=bits==8?250:375;
    if((bits!=8 && bits!=12) || (spi_hz!=15625000 && spi_hz!=20833333) || !validate()) {
        printf("{\"id\":%lu,\"ok\":false,\"error\":\"stream_configuration\",\"phase\":\"validate\",\"capture_started\":false}\n",(unsigned long)id);
        return;
    }
    if(!stream_queue_init(&frames,sizeof(frame_t),FIFO_FRAMES)) {
        printf("{\"id\":%lu,\"ok\":false,\"error\":\"stream_queue_capacity\"}\n",(unsigned long)id);
        return;
    }
    spi_rx_dma=dma_claim_unused_channel(true);spi_tx_dma=dma_claim_unused_channel(true);
    printf("{\"id\":%lu,\"ok\":true,\"stream\":\"%s\",\"format\":\"iq_block_float\",\"header_bytes\":44,\"samples_per_block\":125,\"max_blocks_per_frame\":16,\"blocks\":%lu}\n",(unsigned long)id,job_pause==18?"LIP3":"LIP2",(unsigned long)blocks);
    stdio_flush();
    uint64_t usb_deadline=time_us_64()+2000000;
    multicore_launch_core1(acquire);
    bool usb_ok=true;
    uint32_t sent=0;
    uint32_t group_count=0,group_sequence=0,group_timestamp=0;
    while(!atomic_load(&finished) || !queue_is_empty(&frames) || group_count) {
        if(queue_try_remove(&frames,&consumer_frame)) {
            if(!group_count) {group_sequence=consumer_frame.sequence;group_timestamp=consumer_frame.timestamp;}
            uint8_t *dst=usb_group+IQ_HEADER_BYTES+group_count*(payload_bytes+1);
            *dst++=(uint8_t)consumer_frame.shift;
            memcpy(dst,consumer_frame.bytes+IQ_HEADER_BYTES,payload_bytes);
            ++group_count;
        } else {
            tud_task();
            uint8_t c;
            if(tud_cdc_read(&c,1) && c==3)atomic_store(&stop_requested,true);
            if(!tud_cdc_connected() || time_us_64()>=usb_deadline) {
                usb_ok=false;atomic_store(&stop_requested,true);
            }
        }
        if(group_count && (group_count==16 || (atomic_load(&finished) && queue_is_empty(&frames)))) {
            uint32_t length=group_count*(payload_bytes+1);
            if(job_pause==18)scramble(usb_group+IQ_HEADER_BYTES,length,group_sequence);
            packed_header(usb_group,1,0,job_id,group_sequence,IQ_SAMPLES*group_count,length,
                          group_timestamp,iq_crc32(usb_group+IQ_HEADER_BYTES,length),job_rate);
            if(usb_ok)usb_ok=usb_send(usb_group,IQ_HEADER_BYTES+length,usb_deadline);
            if(usb_ok) {sent+=group_count;usb_deadline=time_us_64()+2000000;}
            group_count=0;
        }
    }
    multicore_reset_core1();
    if(!usb_ok)job_flags|=IQ_USB_LOST;
    if(job_pause==7)job_flags|=IQ_INJECTED_PAUSE;
    uint8_t end[IQ_HEADER_BYTES+36]; uint8_t *stats=end+IQ_HEADER_BYTES;
    iq_le32(stats,produced);iq_le32(stats+4,sent);iq_le32(stats+8,max_queue);
    iq_le32(stats+12,read_ops);iq_le32(stats+16,max_block_us);iq_le32(stats+20,spi_hz);
    iq_le32(stats+24,(uint32_t)(time_us_64()-job_start));
    iq_le32(stats+28,job_flags&(IQ_DMA_STARVED|IQ_QUEUE_FULL|IQ_USB_LOST|IQ_RADIO_ERROR|IQ_FORMAT_ERROR)?0xffffffffu:0);
    iq_le32(stats+32,blocks);
    packed_header(end,2,(uint16_t)job_flags,id,produced,0,36,(uint32_t)(time_us_64()-job_start),iq_crc32(stats,36),job_rate);
    if(usb_ok)(void)usb_send(end,sizeof(end),usb_deadline);
    // Drain the last USB packet before returning to the ASCII command loop.
    uint64_t flush_deadline=time_us_64()+100000;
    while(tud_cdc_connected() && tud_cdc_write_available()<CFG_TUD_CDC_TX_BUFSIZE && time_us_64()<flush_deadline) {
        tud_task();tud_cdc_write_flush();
    }
    frames.data=NULL; // Static storage is shared; never pass it to free().
    dma_channel_unclaim((uint)spi_rx_dma);dma_channel_unclaim((uint)spi_tx_dma);
}
