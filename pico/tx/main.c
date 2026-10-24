/* SPDX-License-Identifier: MIT */
#ifndef LR1121_VERIFIED_UID
#define LR1121_VERIFIED_UID ""
#endif
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "pico/unique_id.h"
#include "pico/bootrom.h"
#include "pico/version.h"
#include "hardware/spi.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"
#include "tusb.h"
#include "protocol.h"
#include "tx_format.h"
#include "stream.h"

#define PIN_SCK 10
#define PIN_MOSI 11
#define PIN_MISO 12
#define PIN_CS 13
#define PIN_BUSY 14
#define PIN_RESET 5
#define PIN_IRQ 15
#define RADIO_SPI spi1

// Large buffers must not live on the small default RP2040 stack.
static request_t req;
static char line[BRIDGE_LINE_SIZE];
static uint8_t command_rx[BRIDGE_MAX_BYTES];
static uint8_t reply[BRIDGE_MAX_BYTES + 1];
static uint32_t speed_hz;
static volatile bool guard_armed, guard_expired;
static alarm_id_t guard_alarm;
static bool guard_boot_recovery;
static uint32_t boot_phase,boot_block,boot_index;
#define GUARD_MAGIC 0x4c524744u

// Independent of USB, SPI, BUSY and the LR1121's event loop. Keep reset low
// until explicit recovery; the Pico watchdog is a second recovery layer.
static int64_t guard_trip(alarm_id_t id, void *user) {
    (void)id; (void)user;
    gpio_put(PIN_RESET, 0);
    gpio_put(PIN_CS, 1);
    guard_expired = true;
    return 0;
}

static void guarded_reset_begin(void) {
    uint32_t irq = save_and_disable_interrupts();
    gpio_put(PIN_RESET, 0);
    if (guard_alarm > 0) cancel_alarm(guard_alarm);
    guard_alarm = 0;
    watchdog_disable();
    watchdog_hw->scratch[0] = 0;
    guard_armed = guard_expired = false;
    restore_interrupts(irq);
}
void packed_stream(uint32_t id,uint32_t blocks,uint32_t pause_ms,uint32_t spi_hz,uint32_t sample_rate,uint32_t bits);
static uint8_t bench_baseline[256];

static void hex_print(const uint8_t *bytes, size_t n) {
    static char encoded[8192];
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        encoded[2*i] = digits[bytes[i] >> 4];
        encoded[2*i+1] = digits[bytes[i] & 15];
    }
    stdio_put_string(encoded, (int)(2*n), false, false);
}

static void failure(uint32_t id, const char *error, const char *phase, bool sent) {
    printf("{\"id\":%lu,\"ok\":false,\"error\":\"%s\",\"phase\":\"%s\","
           "\"command_sent\":%s,\"busy\":%d}\n",
           (unsigned long)id, error, phase, sent ? "true" : "false", gpio_get(PIN_BUSY));
}

static void (*tx_pump_hook)(void);
static uint32_t tx_spi_guard_us=100,tx_poll_us=500;
static uint32_t tx_center_word,tx_offset_shift;

static bool ready(uint32_t timeout_ms) {
    if (guard_expired) return false;
    if (!timeout_ms) return true; // Only the raw xfer command permits bypass.
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (gpio_get(PIN_BUSY)) {
        if (guard_expired) return false;
        tud_task();
        if (tx_pump_hook) tx_pump_hook();
        if (time_reached(deadline)) return false;
        sleep_us(tx_pump_hook && tx_spi_guard_us<100 ? 2 : 20);
    }
    return true;
}

static void transfer(const uint8_t *tx, uint8_t *rx, size_t n) {
    gpio_put(PIN_CS, 0);
    sleep_us(1);
    uint32_t previous_phase=watchdog_hw->scratch[1];
    if(tx_pump_hook) watchdog_hw->scratch[1]=20;
    spi_write_read_blocking(RADIO_SPI, tx, rx, n);
    if(tx_pump_hook) watchdog_hw->scratch[1]=previous_phase;
    gpio_put(PIN_CS, 1);
    // Give the radio time to assert BUSY before the following ready() check.
    sleep_us(tx_pump_hook ? tx_spi_guard_us : 100);
    if (tx_pump_hook) tx_pump_hook();
}

static void finish(uint64_t start) {
    printf(",\"busy\":%d,\"dio9\":%d,\"elapsed_us\":%llu}\n",
           gpio_get(PIN_BUSY), gpio_get(PIN_IRQ),
           (unsigned long long)(time_us_64() - start));
}

// Bounded, read-only measurement of the documented 64-word memory-read path.
// The caller must freeze 0x806000 first if mismatch counts should mean errors.
// No USB formatting or printing occurs inside the timed acquisition loop.
static bool bench_ready(uint64_t deadline) {
    do {
        tud_task();
        if (!stdio_usb_connected() || time_us_64() >= deadline) return false;
        if (!gpio_get(PIN_BUSY)) return true;
    } while (true);
}

static void benchmark(void) {
    static const uint8_t command[] = {0x01, 0x06, 0x00, 0x80, 0x60, 0x00, 64};
    uint32_t completed = 0, mismatches = 0, minimum = UINT32_MAX, maximum = 0;
    uint8_t status = 0;
    const char *reason = "complete";
    uint64_t start = time_us_64(), deadline = start + 1000000;
    uint64_t prepare_wait_us = 0;
    while (completed < req.iterations) {
        uint64_t iteration_start = time_us_64();
        if (!bench_ready(deadline)) { reason = "deadline_or_disconnect"; break; }
        gpio_put(PIN_CS, 0);
        sleep_us(1);
        spi_write_read_blocking(RADIO_SPI, command, command_rx, sizeof(command));
        gpio_put(PIN_CS, 1);
        sleep_us(req.guard_us);
        uint64_t wait_start = time_us_64();
        if (!bench_ready(deadline)) { reason = "prepare_timeout_or_disconnect"; break; }
        prepare_wait_us += time_us_64() - wait_start;
        gpio_put(PIN_CS, 0);
        sleep_us(1);
        spi_read_blocking(RADIO_SPI, 0, reply, 257);
        gpio_put(PIN_CS, 1);
        sleep_us(req.guard_us);
        status = reply[0];
        if (status >> 1 != 3) { reason = "radio_status"; break; }
        if (!completed) memcpy(bench_baseline, reply + 1, sizeof(bench_baseline));
        else if (memcmp(bench_baseline, reply + 1, sizeof(bench_baseline))) ++mismatches;
        ++completed;
        uint32_t duration = (uint32_t)(time_us_64() - iteration_start);
        if (duration < minimum) minimum = duration;
        if (duration > maximum) maximum = duration;
    }
    uint64_t elapsed = time_us_64() - start;
    gpio_put(PIN_CS, 1);
    printf("{\"id\":%lu,\"ok\":true,\"benchmark\":\"read_806000_64words\","
           "\"reason\":\"%s\",\"requested\":%lu,\"completed\":%lu,"
           "\"mismatches\":%lu,\"guard_us\":%lu,\"spi_hz\":%lu,"
           "\"acquisition_us\":%llu,\"prepare_wait_us\":%llu,"
           "\"minimum_us\":%lu,\"maximum_us\":%lu,\"status\":%u,\"first_data\":\"",
           (unsigned long)req.id, reason, (unsigned long)req.iterations,
           (unsigned long)completed, (unsigned long)mismatches,
           (unsigned long)req.guard_us, (unsigned long)speed_hz,
           (unsigned long long)elapsed, (unsigned long long)prepare_wait_us,
           (unsigned long)(completed ? minimum : 0), (unsigned long)maximum, status);
    hex_print(bench_baseline, completed ? sizeof(bench_baseline) : 0);
    printf("\",\"busy\":%d}\n", gpio_get(PIN_BUSY));
}

/* Fixed-address frequency-buffer upload. Does not start or extend TX.
 * Host waits for ready before sending exactly 4096 big-endian data bytes.
 * Data and checksum are validated before touching RAM; publish sequence last.
 */
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static void putbe32(uint8_t *p,uint32_t v) {
    p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;
}
static bool tx_read_word(uint32_t address,uint32_t *value) {
    uint8_t cmd[7]={1,6,0,0,0,0,1},rx[7],data[5];
    putbe32(cmd+2,address);
    if (!ready(10)) return false;
    transfer(cmd,rx,7);
    if (!ready(10)) return false;
    gpio_put(PIN_CS,0);sleep_us(1);spi_read_blocking(RADIO_SPI,0,data,5);
    gpio_put(PIN_CS,1);sleep_us(tx_pump_hook ? tx_spi_guard_us : 100);
    *value=be32(data+1);return (data[0]>>1)==3;
}
static void tx_fill(void) {
    static uint8_t data[4096];
    uint8_t cmd[262],rx[262];
    uint32_t marker,index,active,period,sum=0,block=req.iterations;
    uint64_t start=time_us_64();
    if (!tx_read_word(0x80f45c,&marker)||marker!=0x46535434||
        !tx_read_word(0x80f46c,&tx_center_word)||!tx_read_word(0x80f4a0,&tx_offset_shift)||
        !tx_read_word(0x80f414,&index)||!tx_read_word(0x80f450,&active)||
        !tx_read_word(0x80f40c,&period)||!tx_format_valid(tx_center_word,tx_offset_shift,period)||
        (block<2 ? (index||active) : (!active||index/2048>block-1||index/2048<block-2))) {
        failure(req.id,"buffer_not_free","txfill",false);return;
    }
    uint64_t setup_done=time_us_64();
    printf("{\"id\":%lu,\"ok\":true,\"ready\":4096}\n",(unsigned long)req.id);
    stdio_flush();
    size_t used=0;
    uint64_t deadline=time_us_64()+100000;
    while (used<sizeof(data)) {
        tud_task();
        if (guard_expired || !stdio_usb_connected() || time_us_64()>=deadline) {
            tud_cdc_read_flush();failure(req.id,"binary_timeout","txfill",false);return;
        }
        used+=tud_cdc_read(data+used,sizeof(data)-used);
    }
    uint64_t usb_done=time_us_64();
    for (size_t i=0;i<sizeof(data);i+=4) {
        uint32_t pair=be32(data+i);
        for(unsigned half=0;half<2;half++) {
            int32_t offset=(int16_t)(pair>>(half*16));
            uint32_t word=tx_expand_offset(tx_center_word,tx_offset_shift,(int16_t)offset);sum+=word;
            if (word<tx_center_word-(32768u<<tx_offset_shift) || word>tx_center_word+(32767u<<tx_offset_shift) || !(word&1)) {
                failure(req.id,"frequency_range","txfill",false);return;
            }
        }
    }
    if (sum!=req.guard_us) {failure(req.id,"checksum","txfill",false);return;}
    /* Prefetch USB while the preceding slot is still playing. Wait locally
     * for release; no host round trip is needed at the 25.6 ms boundary. */
    if (block>=2) {
        deadline=time_us_64()+100000;
        for (;;) {
            tud_task();
            if (guard_expired || !stdio_usb_connected() || time_us_64()>=deadline ||
                !tx_read_word(0x80f414,&index)||!tx_read_word(0x80f450,&active)||
                !active||index/2048>block-1||index/2048<block-2) {
                failure(req.id,"buffer_deadline","txfill",false);return;
            }
            if (index/2048==block-1) break;
            uint32_t remaining=((block-1)*2048-index)*period/32;
            if (remaining>500) {
                uint64_t wake=time_us_64()+remaining-500;
                while(time_us_64()<wake && !guard_expired && stdio_usb_connected()) {
                    tud_task();sleep_us(100);
                }
            }
        }
    }
    uint64_t write_start=time_us_64();
    cmd[0]=1;cmd[1]=5;
    uint32_t base=0x80c000+(block%2)*4096;
    for (size_t i=0;i<sizeof(data);i+=256) {
        putbe32(cmd+2,base+i);memcpy(cmd+6,data+i,256);
        if (!ready(10)) {failure(req.id,"busy_timeout","txfill",true);return;}
        transfer(cmd,rx,sizeof(cmd));
        if (!ready(10)) {failure(req.id,"busy_timeout","txfill",true);return;}
        tud_task();
    }
    uint64_t write_done=time_us_64();
    /* Confirm the most recent write succeeded before publishing. */
    memset(cmd,0,6);transfer(cmd,rx,6);
    if ((rx[0]>>1)!=2) {failure(req.id,"radio_rejected","txfill",true);return;}
    cmd[0]=1;cmd[1]=5;putbe32(cmd+2,0x80f480+(block%2)*4);putbe32(cmd+6,block+1);
    transfer(cmd,rx,10);
    if (!ready(10)) {failure(req.id,"busy_timeout","publish",true);return;}
    printf("{\"id\":%lu,\"ok\":true,\"block\":%lu,\"checksum\":%lu,\"elapsed_us\":%llu,\"setup_us\":%llu,\"usb_us\":%llu,\"wait_us\":%llu,\"write_us\":%llu}\n",
           (unsigned long)req.id,(unsigned long)block,(unsigned long)sum,
           (unsigned long long)(time_us_64()-start),
           (unsigned long long)(setup_done-start),(unsigned long long)(usb_done-setup_done),
           (unsigned long long)(write_start-usb_done),(unsigned long long)(write_done-write_start));
}


/* A bounded binary session. The host sends {BE sequence, BE expanded checksum,
 * 4096 data bytes}; eight validated frames can wait ahead of the LR double
 * buffer. USB is serviced between every SPI chunk and while waiting for release.
 * No additional core owns SPI or USB. All deadlines/disconnect resets remain. */
#define TXQ_DEPTH 8u
#define TXQ_FRAME 4104u
static uint8_t txq[TXQ_DEPTH][TXQ_FRAME];
static uint32_t qhead, qtail, qpartial, qtotal, qpeak;
static const char *qerror;
static void queue_pump(void) {
    uint32_t previous_phase=watchdog_hw->scratch[1];
    watchdog_hw->scratch[1]=10;
    tud_task();
    watchdog_hw->scratch[1]=previous_phase;
    if (guard_armed && !guard_expired) watchdog_update();
    if (guard_expired || !stdio_usb_connected()) {qerror="guard_or_disconnect";return;}
    if (qerror || qhead==qtotal-2 || qhead-qtail==TXQ_DEPTH) return;
    uint8_t *frame=txq[qhead%TXQ_DEPTH];
    watchdog_hw->scratch[1]=11;
    qpartial+=tud_cdc_read(frame+qpartial,TXQ_FRAME-qpartial);
    watchdog_hw->scratch[1]=previous_phase;
    if (qpartial!=TXQ_FRAME) return;
    uint32_t sum=0;
    if (be32(frame)!=qhead+2) {qerror="sequence";return;}
    for(unsigned i=8;i<TXQ_FRAME;i+=4) {
        uint32_t pair=be32(frame+i);
        for(unsigned h=0;h<2;h++) {
            uint32_t word=tx_expand_offset(tx_center_word,tx_offset_shift,(int16_t)(pair>>(16*h)));
            if(word<tx_center_word-(32768u<<tx_offset_shift) || word>tx_center_word+(32767u<<tx_offset_shift)) {qerror="frequency_range";return;}
            sum+=word;
        }
    }
    if(sum!=be32(frame+4)) {qerror="checksum";return;}
    qhead++;qpartial=0;
    if(qhead-qtail>qpeak) qpeak=qhead-qtail;
}
static bool tx_write_word(uint32_t address,uint32_t value) {
    uint8_t cmd[10]={1,5},rx[10];putbe32(cmd+2,address);putbe32(cmd+6,value);
    if(!ready(10)) return false;
    transfer(cmd,rx,10);return ready(10);
}
static bool tx_mask_word(uint32_t address,uint32_t value) {
    uint8_t cmd[14]={1,12},rx[14];putbe32(cmd+2,address);putbe32(cmd+6,0xffffffff);putbe32(cmd+10,value);
    if(!ready(10)) return false;
    transfer(cmd,rx,14);return ready(10);
}
static void tx_queue(void) {
    uint32_t marker,index,active,period,count;
    if(!guard_armed || !tx_read_word(0x80f45c,&marker)||marker!=0x46535434 ||
       !tx_read_word(0x80f46c,&tx_center_word)||!tx_read_word(0x80f4a0,&tx_offset_shift)||
       !tx_read_word(0x80f414,&index)||index || !tx_read_word(0x80f450,&active)||active ||
       !tx_read_word(0x80f408,&count)||(count+2047)/2048!=req.iterations ||
       !tx_read_word(0x80f40c,&period)||!tx_format_valid(tx_center_word,tx_offset_shift,period)) {
        failure(req.id,"queue_state","txqueue",false);return;
    }
    watchdog_hw->scratch[1]=1;
    qhead=qtail=qpartial=qpeak=0;qerror=NULL;qtotal=req.iterations;
    uint32_t prefill=req.guard_us;
    if(prefill>qtotal-2) prefill=qtotal-2;
    uint64_t begin=time_us_64(),deadline=begin+2000000;
    printf("{\"id\":%lu,\"ok\":true,\"ready\":4104,\"depth\":8,\"prefill\":%lu}\n",
           (unsigned long)req.id,(unsigned long)prefill);stdio_flush();
    while(qhead<prefill && !qerror) {
        queue_pump();
        if(time_us_64()>deadline) qerror="prefill_timeout";
    }
    tx_spi_guard_us=req.pause_ms;tx_poll_us=req.sample_rate;
    tx_pump_hook=queue_pump;
    uint64_t started=time_us_64();
    uint32_t minq=8,starved=0,maxwrite=0,minlead=UINT32_MAX,minpublished=UINT32_MAX;
    watchdog_hw->scratch[1]=2;
    if(!qerror && (!tx_mask_word(0x8004ac,0x94d10)||!tx_mask_word(0x8004bc,2))) qerror="start";
    uint8_t cmd[262],rx[262];
    while(qtail<qtotal-2 && !qerror) {
        uint32_t block=qtail+2;
        watchdog_hw->scratch[1]=3;watchdog_hw->scratch[2]=block;
        deadline=time_us_64()+200000;
        for(;;) {
            queue_pump();
            if(qerror) break;
            if(time_us_64()>deadline || !tx_read_word(0x80f414,&index) ||
               !tx_read_word(0x80f450,&active) || (!active && index) || index/2048>block-1) {
                qerror="buffer_deadline";break;
            }
            watchdog_hw->scratch[3]=index;
            if(qhead>qtail && index/2048==block-1) break;
            if(qhead==qtail) starved++;
            /* Poll locally and receive USB; no host round trip at the boundary. */
            uint64_t wake=time_us_64()+tx_poll_us;
            while(time_us_64()<wake && !qerror) queue_pump();
        }
        if(qerror) break;
        uint32_t queued=qhead-qtail;
        if(qhead<qtotal-2 && queued<minq) minq=queued;
        uint8_t *frame=txq[qtail%TXQ_DEPTH];uint32_t base=0x80c000+(block%2)*4096;
        watchdog_hw->scratch[1]=4;
        uint64_t wstart=time_us_64();cmd[0]=1;cmd[1]=5;
        for(unsigned i=0;i<4096 && !qerror;i+=256) {
            putbe32(cmd+2,base+i);memcpy(cmd+6,frame+8+i,256);
            if(!ready(10)) {qerror="write_busy";break;}
            transfer(cmd,rx,sizeof(cmd));
            if(!ready(10)) qerror="write_busy";
        }
        uint32_t us=(uint32_t)(time_us_64()-wstart);if(us>maxwrite)maxwrite=us;
        if(qerror) break;
        memset(cmd,0,6);transfer(cmd,rx,6);
        if((rx[0]>>1)!=2) {qerror="radio_rejected";break;}
        if(!tx_read_word(0x80f414,&index)||index>=block*2048) {qerror="publish_late";break;}
        uint32_t lead=block*2048-index;if(lead<minlead)minlead=lead;
        if(!tx_write_word(0x80f480+(block%2)*4,block+1)) {qerror="publish_busy";break;}
        if(!tx_read_word(0x80f414,&index)) {qerror="publish_readback";break;}
        if(index>=block*2048) {qerror="published_late";break;}
        lead=block*2048-index;if(lead<minpublished)minpublished=lead;
        qtail++;
    }
    watchdog_hw->scratch[1]=5;
    tx_pump_hook=NULL;
    uint32_t failure_index=index,failure_active=active;
    uint32_t failure_state[41]={0},failure_log[64]={0};
    if(qerror) {
        for(unsigned i=0;i<41 && !guard_expired;i++) {
            if(!tx_read_word(0x80f400+i*4,&failure_state[i])) break;
        }
        uint32_t n=failure_state[37];
        for(unsigned i=0;i<64 && !guard_expired;i++) {
            uint32_t record=(n>=8?n-8:0)+i/8;
            if(record>=n || !tx_read_word(0x80e000+(record%128)*32+(i%8)*4,&failure_log[i])) break;
        }
        /* Fail closed and discard remaining session bytes, never parse as commands. */
        guard_trip(0,NULL);
        uint64_t quiet=time_us_64(),end=quiet+2000000;
        while(time_us_64()<end && time_us_64()-quiet<100000) {
            tud_task();watchdog_update();
            if(tud_cdc_available()) {tud_cdc_read_flush();quiet=time_us_64();}
        }
    }
    watchdog_hw->scratch[1]=6;
    printf("{\"id\":%lu,\"ok\":%s,\"reason\":\"%s\",\"received\":%lu,\"published\":%lu,"
           "\"failure_index\":%lu,\"failure_active\":%lu,\"peak_queue\":%lu,\"min_queue_before_write\":%lu,\"starved_polls\":%lu,\"max_write_us\":%lu,"
           "\"spi_guard_us\":%lu,\"poll_us\":%lu,\"min_published_lead_samples\":%lu,\"min_lead_samples\":%lu,\"prefill_us\":%llu,\"stream_us\":%llu,\"failure_state\":[",
           (unsigned long)req.id,qerror?"false":"true",qerror?qerror:"complete",
           (unsigned long)qhead,(unsigned long)qtail,(unsigned long)failure_index,(unsigned long)failure_active,(unsigned long)qpeak,(unsigned long)minq,
           (unsigned long)starved,(unsigned long)maxwrite,(unsigned long)tx_spi_guard_us,(unsigned long)tx_poll_us,(unsigned long)minpublished,(unsigned long)minlead,
           (unsigned long long)(started-begin),(unsigned long long)(time_us_64()-started));
    for(unsigned i=0;i<41;i++) printf("%s%lu",i?",":"",(unsigned long)failure_state[i]);
    printf("],\"failure_log\":[");
    for(unsigned i=0;i<64;i++) printf("%s%lu",i?",":"",(unsigned long)failure_log[i]);
    printf("]}\n");
}

static void run_request(void) {
    uint64_t start = time_us_64();
    unsigned long id = req.id;
    if ((guard_expired && req.command != CMD_RESET && req.command != CMD_INFO &&
         req.command != CMD_PINS && req.command != CMD_GUARD_STATUS) ||
        (guard_armed && (req.command == CMD_STREAM || req.command == CMD_STREAM2 ||
                        req.command == CMD_PACKED || req.command == CMD_BENCH ||
                        req.command == CMD_BOOTSEL))) {
        failure(req.id, "guard_requires_reset", "guard", false);
        return;
    }
    switch (req.command) {
    case CMD_TXQUEUE:
        tx_queue();
        return;
    case CMD_TXFILL:
        tx_fill();
        return;
    case CMD_GUARD:
        if (guard_armed) {
            failure(req.id, "guard_already_armed", "guard", false);
            return;
        }
        guard_armed = true;
        watchdog_hw->scratch[0] = GUARD_MAGIC;
        watchdog_enable(3000, false);
        guard_alarm = add_alarm_in_ms(req.timeout_ms, guard_trip, NULL, true);
        if (guard_alarm <= 0) {
            guard_trip(0, NULL);
            failure(req.id, "guard_alarm_failed", "guard", false);
            return;
        }
        printf("{\"id\":%lu,\"ok\":true,\"armed\":true,\"deadline_ms\":%lu}\n",
               id, (unsigned long)req.timeout_ms);
        return;
    case CMD_GUARD_STATUS:
        printf("{\"id\":%lu,\"ok\":true,\"armed\":%s,\"expired\":%s,\"reset\":%d,\"boot_recovery\":%s,\"boot_phase\":%lu,\"boot_block\":%lu,\"boot_index\":%lu}\n",
               id, guard_armed ? "true" : "false", guard_expired ? "true" : "false",
               gpio_get(PIN_RESET), guard_boot_recovery ? "true" : "false",
               (unsigned long)boot_phase,(unsigned long)boot_block,(unsigned long)boot_index);
        return;
    case CMD_INFO: {
        char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
        pico_get_unique_board_id_string(serial, sizeof(serial));
        printf("{\"id\":%lu,\"ok\":true,\"bridge\":\"lr1121-spi\",\"protocol\":1,"
               "\"firmware\":\"1.0.8-txopt5\",\"reset_guard\":1,\"packed_stream\":\"LIP2\",\"fifo_stream\":\"LIP3\",\"packed_radio_uid\":\"" LR1121_VERIFIED_UID "\",\"sdk\":\"%s\",\"serial\":\"%s\","
               "\"spi_hz\":%lu,\"max_bytes\":%u,\"spi_mode\":0,\"pins\":{"
               "\"sck\":%d,\"mosi\":%d,\"miso\":%d,\"cs\":%d,\"busy\":%d,"
               "\"reset\":%d,\"dio9\":%d}}\n",
               id, PICO_SDK_VERSION_STRING, serial, (unsigned long)speed_hz,
               BRIDGE_MAX_BYTES, PIN_SCK, PIN_MOSI, PIN_MISO, PIN_CS, PIN_BUSY,
               PIN_RESET, PIN_IRQ);
        return;
    }
    case CMD_PINS:
        printf("{\"id\":%lu,\"ok\":true,\"cs\":%d,\"reset\":%d", id,
               gpio_get(PIN_CS), gpio_get(PIN_RESET));
        finish(start);
        return;
    case CMD_BENCH:
        benchmark();
        return;
    case CMD_STREAM:
    case CMD_STREAM2:
    case CMD_PACKED:
        failure(req.id, "tx_study_build", "stream", false);
        return;
    case CMD_SPEED:
        speed_hz = spi_set_baudrate(RADIO_SPI, req.speed_hz);
        printf("{\"id\":%lu,\"ok\":true,\"spi_hz\":%lu}\n", id, (unsigned long)speed_hz);
        return;
    case CMD_BOOTSEL:
        printf("{\"id\":%lu,\"ok\":true,\"rebooting\":\"pico_bootsel\"}\n", id);
        stdio_flush();
        sleep_ms(100);
        reset_usb_boot(0, 0);
        return;
    case CMD_RESET:
        gpio_put(PIN_CS, 1);
        guarded_reset_begin();
        sleep_ms(10);
        gpio_put(PIN_RESET, 1);
        sleep_ms(10);
        if (!ready(req.timeout_ms)) {
            failure(req.id, "busy_timeout", "after_reset", false);
            return;
        }
        printf("{\"id\":%lu,\"ok\":true", id);
        finish(start);
        return;
    case CMD_WAIT:
        if (!ready(req.timeout_ms)) {
            failure(req.id, "busy_timeout", "wait", false);
            return;
        }
        printf("{\"id\":%lu,\"ok\":true", id);
        finish(start);
        return;
    default:
        break;
    }
    if (!ready(req.timeout_ms)) {
        failure(req.id, "busy_timeout", "before_command", false);
        return;
    }
    transfer(req.tx, command_rx, req.tx_len);
    if (req.command != CMD_XFER && !ready(req.timeout_ms)) {
        failure(req.id, "busy_timeout", "after_command", true);
        return;
    }
    if (req.command == CMD_READ) {
        // One status byte followed by payload, all under the second CS assertion.
        gpio_put(PIN_CS, 0);
        sleep_us(1);
        spi_read_blocking(RADIO_SPI, 0, reply, req.read_len + 1);
        gpio_put(PIN_CS, 1);
        sleep_us(100);
        printf("{\"id\":%lu,\"ok\":true,\"status\":%u,\"cmd_status\":%u,\"data\":\"",
               id, reply[0], reply[0] >> 1);
        hex_print(reply + 1, req.read_len);
        printf("\",\"command_rx\":\"");
    } else {
        printf("{\"id\":%lu,\"ok\":true,\"rx\":\"", id);
    }
    hex_print(command_rx, req.tx_len);
    printf("\"");
    finish(start);
}

static void output_pin(uint pin) {
    gpio_init(pin);
    gpio_put(pin, 1);
    gpio_set_dir(pin, GPIO_OUT);
}

int main(void) {
    output_pin(PIN_CS);
    gpio_init(PIN_RESET);
    gpio_put(PIN_RESET, 0);
    gpio_set_dir(PIN_RESET, GPIO_OUT);
    guard_boot_recovery = watchdog_caused_reboot() && watchdog_hw->scratch[0] == GUARD_MAGIC;
    boot_phase=watchdog_hw->scratch[1];boot_block=watchdog_hw->scratch[2];boot_index=watchdog_hw->scratch[3];
    watchdog_hw->scratch[0] = 0;
    sleep_ms(10);
    gpio_put(PIN_RESET, 1);
    gpio_init(PIN_BUSY);
    gpio_set_dir(PIN_BUSY, GPIO_IN);
    gpio_pull_up(PIN_BUSY);
    gpio_init(PIN_IRQ);
    gpio_set_dir(PIN_IRQ, GPIO_IN);
    gpio_pull_down(PIN_IRQ); // Unconnected optional IRQ reads zero.
    speed_hz = spi_init(RADIO_SPI, 500000);
    spi_set_format(RADIO_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    stdio_init_all();

    uint8_t input[2048];
    size_t input_at = 0, input_size = 0;
    size_t used = 0;
    bool discard = false;
    const char *line_error = "line_too_long";
    uint64_t last_byte = 0;
    while (true) {
        // The absolute alarm deadline is never extended. The watchdog detects
        // a stalled bridge independently during longer image transmissions.
        if (guard_armed && !guard_expired) watchdog_update();
        // Service USB from this loop even when CDC is closed or disconnected.
        // There is no competing stdio USB background task/worker interrupt.
        tud_task();
        if (!stdio_usb_connected()) {
            if (guard_armed && !guard_expired) guard_trip(0, NULL);
            used = 0;
            input_at = input_size = 0;
            discard = false;
            sleep_ms(1);
            continue;
        }
        if (input_at == input_size) {
            input_size = tud_cdc_read(input, sizeof(input));
            input_at = 0;
        }
        int c = input_at < input_size ? input[input_at++] : PICO_ERROR_TIMEOUT;
        if (c == PICO_ERROR_TIMEOUT) {
            if (used && time_us_64() - last_byte > 2000000) {
                used = 0;
                discard = true; // Never execute the tail of a truncated request.
                line_error = "incomplete_line";
            }
            continue;
        }
        last_byte = time_us_64();
        if (c == '\r') continue;
        if (c == '\n') {
            if (discard) {
                failure(0, line_error, "parse", false);
            } else if (used) {
                line[used] = '\0';
                const char *error;
                if (parse_request(line, &req, &error)) run_request();
                else failure(req.id, error, "parse", false);
            }
            used = 0;
            discard = false;
            stdio_flush();
        } else if (!discard) {
            if ((c < 32 && c != '\t') || c > 126) {
                discard = true;
                line_error = "invalid_character";
            } else if (used + 1 >= sizeof(line)) {
                discard = true;
                line_error = "line_too_long";
            } else {
                line[used++] = (char)c;
            }
        }
    }
}
