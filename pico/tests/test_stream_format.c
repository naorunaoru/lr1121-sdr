#include "../firmware/stream_format.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    const uint8_t wire[]={0,0,0,0, 0,0x3f,0xff,0xff, 0,0x40,0,0, 0,0x7f,0xff,0xff};
    const uint8_t expected[]={0,0,0, 0xff,0xff,0x3f, 0,0,0x40, 0xff,0xff,0x7f};
    uint8_t packed[12];assert(iq_pack23(packed,wire,4));assert(!memcmp(packed,expected,12));
    const uint8_t bad[]={0,0x80,0,0};assert(!iq_pack23(packed,bad,1));
    assert(iq_crc32((const uint8_t *)"123456789",9)==0xcbf43926);
    uint8_t h[44];iq_header(h,1,0,7,42,125,750,12345,0xabcdef01);
    assert(!memcmp(h,"LIQ1",4));assert(h[4]==1 && h[5]==1 && h[12]==42);
    assert(h[16]==0x82 && h[17]==0x14); // sample 42*125 = 5250
    uint32_t crc=(uint32_t)h[40]|(uint32_t)h[41]<<8|(uint32_t)h[42]<<16|(uint32_t)h[43]<<24;
    assert(crc==iq_crc32(h,40));assert(iq_descriptor(0)==0x800a00);
    assert(iq_descriptor(1)==0x800a10);assert(iq_descriptor(31)==0x80e1f0);
    iq_header_rate(h,1,0,1,0xffffffffu,125,750,0xffffffffu,0,62500);
    assert(h[28]==0x24 && h[29]==0xf4 && h[30]==0 && h[31]==0);
    assert(h[16]==0x83 && h[17]==0xff && h[18]==0xff && h[19]==0xff);
    puts("Binary format: signed-container extremes, CRC-32, header layout and descriptor mapping passed");
}
