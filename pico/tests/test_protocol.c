/* SPDX-License-Identifier: MIT */
#include "../firmware/protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static request_t req;
static char input[BRIDGE_LINE_SIZE];
static const char *error;

static bool parse(const char *s) {
    assert(strlen(s) < sizeof(input));
    strcpy(input, s);
    return parse_request(input, &req, &error);
}

int main(void) {
    assert(parse("1 guard 100") && req.command == CMD_GUARD && req.timeout_ms == 100);
    assert(parse("1 guard 2000") && req.timeout_ms == 2000);
    assert(parse("1 guardstatus") && req.command == CMD_GUARD_STATUS);
    assert(!parse("1 guard 0"));
    assert(!parse("1 guard 99"));
    assert(!parse("1 guard 2001"));
    assert(!parse("1 guard -1"));
    assert(!parse("1 guard 4294967296"));
    assert(!parse("1 guardstatus extra"));
    assert(parse("1 read 1000 4 0101"));
    assert(req.id == 1 && req.command == CMD_READ && req.read_len == 4);
    assert(req.tx_len == 2 && req.tx[0] == 1 && req.tx[1] == 1);
    assert(parse("4294967295 xfer 0 aAFF00"));
    assert(req.id == UINT32_MAX && req.timeout_ms == 0 && req.tx_len == 3);
    assert(req.tx[0] == 0xaa && req.tx[1] == 0xff && req.tx[2] == 0);
    assert(parse("  3\treset 10000  "));
    assert(parse("4 speed 100000"));
    assert(parse("5 speed 10000000"));
    assert(parse("5 speed 16000000") && req.speed_hz == 16000000);
    assert(parse("5 speed 24000000") && req.speed_hz == 24000000);
    assert(parse("5 packed 250000 0 12") && req.command == CMD_PACKED && req.iterations == 0);
    assert(parse("5 packed 500000 60000 8") && req.command == CMD_PACKED && req.guard_us == 8);
    assert(!parse("5 packed 250000 1 9"));
    assert(!parse("5 packed 250000 60001 12"));
    assert(!parse("5 packed 250001 1 12"));
    assert(parse("11 bench 1000 5") && req.command == CMD_BENCH && req.iterations == 1000 && req.guard_us == 5);
    assert(parse("12 bench 1 100") && req.guard_us == 100);
    assert(parse("13 stream 8192 0") && req.command == CMD_STREAM && req.iterations == 8192);
    assert(parse("13 stream 100 50") && req.pause_ms == 50);
    assert(parse("14 stream2 62500 0") && req.command==CMD_STREAM2 && req.sample_rate==62500 && req.iterations==0);
    assert(parse("15 stream2 125000 8192") && req.sample_rate==125000 && req.iterations==8192);
    assert(parse("6 write 1 01020304"));
    assert(parse("7 info") && req.command == CMD_INFO);
    assert(parse("8 pins") && req.command == CMD_PINS);
    assert(parse("9 wait 1") && req.command == CMD_WAIT);
    assert(parse("10 bootsel") && req.command == CMD_BOOTSEL);
    const char *bad[] = {
        "", "1", "0 info", "-1 info", "4294967296 info", "999999999999999999999 info",
        "1 unknown", "1 info extra", "1 reset", "1 reset 0", "1 reset 10001",
        "1 speed 99999", "1 speed 62500001", "1 speed +100000", "1 speed 1e6",
        "1 bench 0 5", "1 bench 1001 5", "1 bench 500 0", "1 bench 500 6",
        "1 bench 500 101", "1 bench 500 5 extra", "1 bench 500", "1 bench 4294967296 5",
        "1 stream 0 0", "1 stream 8193 0", "1 stream 100 1", "1 stream 100 51",
        "1 stream2 0 0", "1 stream2 125001 0", "1 stream2 100000 0",
        "1 stream2 62500 -1", "1 stream2 125000 8193", "1 stream2 62500",
        "1 stream2 62500 0 extra",
        "1 stream 100", "1 stream 100 0 extra", "1 stream 100 -1",
        "1 read 1000 4 101", "1 read 1000 4 01gg", "1 read 1000 4 01",
        "1 read 0 4 0101", "1 read 1000 0 0101", "1 read 1000 4097 0101",
        "1 read 1000 -1 0101", "1 read 1000 4294967296 0101", "1 xfer 1000",
        "1 write 1 aa", "1 xfer 1 aabb trailing", "1 read 1 1 aa bb cc dd",
        "1 info\x01", "1 read 1 4 0x0101"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) assert(!parse(bad[i]));
    strcpy(input, "99 read 1000 4096 ");
    size_t prefix = strlen(input);
    memset(input + prefix, 'a', 2 * BRIDGE_MAX_BYTES);
    input[prefix + 2 * BRIDGE_MAX_BYTES] = 0;
    assert(parse_request(input, &req, &error));
    assert(req.tx_len == 4096 && req.read_len == 4096 && req.tx[4095] == 0xaa);
    strcpy(input, "99 xfer 1 ");
    prefix = strlen(input);
    memset(input + prefix, 'a', 2 * BRIDGE_MAX_BYTES + 2);
    input[prefix + 2 * BRIDGE_MAX_BYTES + 2] = 0;
    assert(!parse_request(input, &req, &error));
    puts("Protocol parser: valid commands, malformed requests, ranges, overflow and 4096-byte limits passed");
    return 0;
}
