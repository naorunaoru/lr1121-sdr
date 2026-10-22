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
#include "tusb.h"
#include "protocol.h"
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
void packed_stream(uint32_t id,uint32_t blocks,uint32_t pause_ms,uint32_t spi_hz,uint32_t sample_rate,uint32_t bits);
static uint8_t bench_baseline[256];

static void hex_print(const uint8_t *bytes, size_t n) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        putchar_raw(digits[bytes[i] >> 4]);
        putchar_raw(digits[bytes[i] & 15]);
    }
}

static void failure(uint32_t id, const char *error, const char *phase, bool sent) {
    printf("{\"id\":%lu,\"ok\":false,\"error\":\"%s\",\"phase\":\"%s\","
           "\"command_sent\":%s,\"busy\":%d}\n",
           (unsigned long)id, error, phase, sent ? "true" : "false", gpio_get(PIN_BUSY));
}

static bool ready(uint32_t timeout_ms) {
    if (!timeout_ms) return true; // Only the raw xfer command permits bypass.
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (gpio_get(PIN_BUSY)) {
        tud_task();
        if (time_reached(deadline)) return false;
        sleep_us(20);
    }
    return true;
}

static void transfer(const uint8_t *tx, uint8_t *rx, size_t n) {
    gpio_put(PIN_CS, 0);
    sleep_us(1);
    spi_write_read_blocking(RADIO_SPI, tx, rx, n);
    gpio_put(PIN_CS, 1);
    // Give the radio time to assert BUSY before the following ready() check.
    sleep_us(100);
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

static void run_request(void) {
    uint64_t start = time_us_64();
    unsigned long id = req.id;
    switch (req.command) {
    case CMD_INFO: {
        char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
        pico_get_unique_board_id_string(serial, sizeof(serial));
        printf("{\"id\":%lu,\"ok\":true,\"bridge\":\"lr1121-spi\",\"protocol\":1,"
               "\"firmware\":\"1.0.7\",\"packed_stream\":\"LIP2\",\"fifo_stream\":\"LIP3\",\"packed_radio_uid\":\"" LR1121_VERIFIED_UID "\",\"sdk\":\"%s\",\"serial\":\"%s\","
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
        radio_stream(req.id,req.iterations,req.pause_ms,speed_hz,125000);
        return;
    case CMD_STREAM2:
        radio_stream(req.id,req.iterations,0,speed_hz,req.sample_rate);
        return;
    case CMD_PACKED:
        packed_stream(req.id,req.iterations,req.pause_ms,speed_hz,req.sample_rate,req.guard_us);
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
        gpio_put(PIN_RESET, 0);
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
    output_pin(PIN_RESET);
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

    size_t used = 0;
    bool discard = false;
    const char *line_error = "line_too_long";
    uint64_t last_byte = 0;
    while (true) {
        // Service USB from this loop even when CDC is closed or disconnected.
        // There is no competing stdio USB background task/worker interrupt.
        tud_task();
        if (!stdio_usb_connected()) {
            used = 0;
            discard = false;
            sleep_ms(1);
            continue;
        }
        int c = getchar_timeout_us(1000);
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
