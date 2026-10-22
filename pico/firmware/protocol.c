/* SPDX-License-Identifier: MIT */
#include "protocol.h"
#include <string.h>

static bool number(const char *s, uint32_t minimum, uint32_t maximum, uint32_t *out) {
    if (!s || !*s) return false;
    uint32_t v = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return false;
        uint32_t digit = (uint32_t)(*s - '0');
        if (v > maximum / 10 || (v == maximum / 10 && digit > maximum % 10))
            return false;
        v = v * 10 + digit;
    }
    if (v < minimum) return false;
    *out = v;
    return true;
}

static int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hex_bytes(const char *s, request_t *req, size_t minimum) {
    size_t n = strlen(s);
    if ((n & 1) || n < minimum * 2 || n > BRIDGE_MAX_BYTES * 2) return false;
    for (size_t i = 0; i < n / 2; ++i) {
        int hi = nibble(s[2 * i]), lo = nibble(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        req->tx[i] = (uint8_t)((hi << 4) | lo);
    }
    req->tx_len = n / 2;
    return true;
}

bool parse_request(char *line, request_t *req, const char **error) {
    char *words[6];
    size_t count = 0;
    memset(req, 0, sizeof(*req));
    *error = "syntax";
    // Reject embedded control characters before tokenizing.
    for (char *p = line; *p; ++p)
        if ((*p < 32 && *p != '\t') || (unsigned char)*p > 126) return false;
    char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        if (count == 6) return false;
        words[count++] = p;
        while (*p && *p != ' ' && *p != '\t') ++p;
        if (*p) *p++ = '\0';
    }
    if (count < 2 || !number(words[0], 1, UINT32_MAX, &req->id)) return false;
    const char *name = words[1];
    if (!strcmp(name, "info") || !strcmp(name, "pins") || !strcmp(name, "bootsel")) {
        if (count != 2) return false;
        req->command = !strcmp(name, "info") ? CMD_INFO :
                       !strcmp(name, "pins") ? CMD_PINS : CMD_BOOTSEL;
        return true;
    }
    if (!strcmp(name, "speed")) {
        if (count != 3 || !number(words[2], 100000, 62500000, &req->speed_hz)) {
            *error = "speed_range_100000_to_62500000";
            return false;
        }
        req->command = CMD_SPEED;
        return true;
    }
    if (!strcmp(name, "packed")) {
        if ((count != 5 && count != 6) || !number(words[2],62500,1000000,&req->sample_rate) ||
            (req->sample_rate!=62500 && req->sample_rate!=125000 && req->sample_rate!=250000 &&
             req->sample_rate!=500000 && req->sample_rate!=1000000) ||
            !number(words[3],0,60000,&req->iterations) || !number(words[4],8,12,&req->guard_us) ||
            (req->guard_us!=8 && req->guard_us!=12)) {
            *error="packed_rate_blocks_bits";return false;
        }
        if (count==6 && (!number(words[5],0,18,&req->pause_ms) || (req->pause_ms && req->pause_ms!=11 && req->pause_ms!=17 && req->pause_ms!=18 && (!req->iterations || req->iterations>16)))) return false;
        req->command=CMD_PACKED;return true;
    }
    if (!strcmp(name, "stream2")) {
        if (count != 4 || !number(words[2], 62500, 125000, &req->sample_rate) ||
            (req->sample_rate != 62500 && req->sample_rate != 125000) ||
            !number(words[3], 0, 8192, &req->iterations)) {
            *error = "stream2_rate_62500_or_125000_blocks_0_to_8192";
            return false;
        }
        req->command = CMD_STREAM2;
        return true;
    }
    if (!strcmp(name, "stream")) {
        if (count != 4 || !number(words[2], 1, 8192, &req->iterations) ||
            !number(words[3], 0, 50, &req->pause_ms) ||
            (req->pause_ms != 0 && req->pause_ms != 50)) {
            *error = "stream_blocks_1_to_8192_pause_0_or_50";
            return false;
        }
        req->command = CMD_STREAM;
        return true;
    }
    if (!strcmp(name, "bench")) {
        if (count != 4 || !number(words[2], 1, 1000, &req->iterations) ||
            !number(words[3], 5, 100, &req->guard_us) ||
            (req->guard_us != 5 && req->guard_us != 100)) {
            *error = "bench_iterations_1_to_1000_guard_5_or_100";
            return false;
        }
        req->command = CMD_BENCH;
        return true;
    }
    if (!strcmp(name, "reset") || !strcmp(name, "wait")) {
        if (count != 3 || !number(words[2], 1, BRIDGE_MAX_TIMEOUT_MS, &req->timeout_ms)) {
            *error = "timeout_range_1_to_10000";
            return false;
        }
        req->command = !strcmp(name, "reset") ? CMD_RESET : CMD_WAIT;
        return true;
    }
    if (!strcmp(name, "read") || !strcmp(name, "write") || !strcmp(name, "xfer")) {
        bool read = !strcmp(name, "read"), raw = !strcmp(name, "xfer");
        if (count != (read ? 5u : 4u)) return false;
        if (!number(words[2], raw ? 0 : 1, BRIDGE_MAX_TIMEOUT_MS, &req->timeout_ms)) {
            *error = "invalid_timeout";
            return false;
        }
        if (read) {
            uint32_t n;
            if (!number(words[3], 1, BRIDGE_MAX_BYTES, &n)) {
                *error = "invalid_read_length";
                return false;
            }
            req->read_len = n;
        }
        if (!hex_bytes(words[read ? 4 : 3], req, raw ? 1 : 2)) {
            *error = "invalid_hex_or_length";
            return false;
        }
        req->command = read ? CMD_READ : raw ? CMD_XFER : CMD_WRITE;
        return true;
    }
    *error = "unknown_command";
    return false;
}
