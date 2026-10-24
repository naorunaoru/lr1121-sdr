/* SPDX-License-Identifier: MIT */
#include "../tx/protocol.h"
#include "../tx/tx_format.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static request_t r;
static bool parse(const char *s) { char b[128]; const char *e; strcpy(b,s); return parse_request(b,&r,&e); }
int main(void) {
 assert(parse("1 txqueue 300 8") && r.command==CMD_TXQUEUE && r.pause_ms==100 && r.sample_rate==500);
 assert(parse("1 txqueue 300 8 2 100") && r.pause_ms==2 && r.sample_rate==100 && r.guard_us==8);
 assert(parse("1 txqueue 3 1 100 20"));
 const char *bad[]={"1 txqueue 300 8 0 100","1 txqueue 300 8 1 100","1 txqueue 300 8 101 100","1 txqueue 300 8 2 19","1 txqueue 300 8 2 501","1 txqueue 300 8 2","1 txqueue 300 8 2 100 extra","1 txqueue 300 9 2 100","1 txqueue 1564 8 2 100","1 txqueue 300 8 -1 100","1 txqueue 300 8 4294967296 100"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) assert(!parse(bad[i]));
 for(unsigned c=16;c<=64;c*=2) {
  char command[80];snprintf(command,sizeof(command),"1 txqueue 300 8 2 100 %u 1",c);
  assert(parse(command) && r.chunk_words==c && r.probe_mode==1);
 }
 assert(!parse("1 txqueue 300 8 2 100 17 0"));
 assert(parse("1 txqueue 300 8 2 100 64 3") && r.probe_mode==3);
 assert(parse("1 txqueue 300 8 2 100 64 7") && r.probe_mode==7);
 assert(!parse("1 txqueue 300 8 2 100 64 8"));
 assert(!parse("1 txqueue 300 8 2 100 64"));
 assert(!parse("1 txqueue 300 8 2 100 64 0 extra"));
 assert(tx_format_valid(910530969,1,80));
 assert(parse("1 guard 15800") && r.timeout_ms==15800);
 assert(!parse("1 guard 62001"));
 const uint32_t center=910530969;
 assert(tx_format_valid(center,1,128) && tx_format_valid(center,2,145));
 assert(tx_format_valid(center,14,1600));
 assert(!tx_format_valid(center,15,128)); /* Full negative range crosses zero. */
 assert(!tx_format_valid(center,0,128) && !tx_format_valid(center,16,128));
 assert(!tx_format_valid(center,1,79) && !tx_format_valid(center,1,1601));
 assert(!tx_format_valid(center-1,1,128));
 for(uint32_t shift=1;shift<=14;shift++) {
  assert(tx_expand_offset(center,shift,-32768)==center-(32768u<<shift));
  assert(tx_expand_offset(center,shift,32767)==center+(32767u<<shift));
  assert(tx_expand_offset(center,shift,-1)==center-(1u<<shift));
 }
 assert(tx_format_valid(1073741825u,15,128));
 assert(!tx_format_valid(1073741823u,15,128));
 assert(tx_format_valid(UINT32_MAX-(32767u<<15),15,128));
 assert(!tx_format_valid(UINT32_MAX-(32767u<<15)+2,15,128));
 puts("TX queue pacing bounds and legacy defaults passed");
}
