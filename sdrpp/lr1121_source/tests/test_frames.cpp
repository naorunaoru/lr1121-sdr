/* SPDX-License-Identifier: GPL-3.0-only */
#include "device.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <functional>
using namespace lr1121;
static void put(std::vector<uint8_t>& w,size_t p,uint32_t x){for(int k=0;k<4;k++)w[p+k]=uint8_t(x>>(k*8));}
static void checksum(std::vector<uint8_t>& w){put(w,36,crc32(w.data()+44,w.size()-44));put(w,40,crc32(w.data(),40));}
static std::vector<uint8_t> frame(uint32_t seq,uint32_t rate,bool end=false){
 std::vector<uint8_t> w(end?80:794);std::memcpy(w.data(),"LIQ1",4);w[4]=1;w[5]=end?2:1;
 put(w,8,7);put(w,12,seq);put(w,16,seq*125u);put(w,20,end?0:125);put(w,24,end?36:750);put(w,28,rate);
 if(end){put(w,44,seq);put(w,48,seq);}checksum(w);return w;
}
static void reject(std::function<void()> fn){bool caught=false;try{fn();}catch(const std::exception&){caught=true;}assert(caught);}
int main(){
 assert(crc32(reinterpret_cast<const uint8_t*>("123456789"),9)==0xcbf43926);
 for(uint32_t rate:{62500u,125000u}) {
  auto wire=frame(0,rate);wire[44]=0xff;wire[45]=0xff;wire[46]=0x3f;wire[49]=0x40;checksum(wire);
  auto f=decode(wire,rate);assert(f.samples[0].real()==4194303.0f/4194304.0f);assert(f.samples[0].imag()==-1.0f);
  Continuity c;c.id=7;c.rate=rate;c.accept(f);assert(c.frames==1);c.accept(decode(frame(1,rate,true),rate));
  reject([&]{c.accept(f);});
  auto bad=wire;bad[7]^=1;reject([&]{decode(bad,rate);});
  bad=wire;bad[60]^=1;reject([&]{decode(bad,rate);});
  bad=wire;bad[46]|=0x80;checksum(bad);reject([&]{decode(bad,rate);});
  bad=wire;put(bad,24,0xffffffff);checksum(bad);reject([&]{decode(bad,rate);});
  bad=wire;put(bad,16,1);checksum(bad);reject([&]{decode(bad,rate);});
  reject([&]{decode(wire,rate==62500?125000:62500);});
  bad=frame(1,rate,true);put(bad,48,2);checksum(bad);reject([&]{c.accept(decode(bad,rate));});
  c.expected=0xffffffffu;c.accept(decode(frame(0xffffffffu,rate),rate));c.accept(decode(frame(0,rate),rate));assert(c.expected==1);
  auto id=decode(frame(1,rate),rate);id.id=99;reject([&]{c.accept(id);});
 }
 // Off-datasheet values and both forced inputs are intentional. Only the
 // synthesizer encoding limit, invalid input IDs and rates are rejected.
 for(unsigned input=0;input<inputs.size();input++)for(uint32_t rate:{62500u,125000u,250000u,500000u}){
  for(uint32_t f:{1u,100000000u,404000000u,1100000000u,1500000000u,1700000000u,2740000000u,4095999999u})validateSettings(f,rate,input);
  reject([&]{validateSettings(0,rate,input);});
  reject([&]{validateSettings(4096000000u,rate,input);});
 }
 reject([]{validateSettings(2440000000u,1000000,0);});reject([]{validateSettings(2440000000u,125000,3);});
 for(unsigned banks:{1u,3u,16u}) {
  std::vector<uint8_t> w(44+banks*376);std::memcpy(w.data(),"LIP2",4);w[4]=1;w[5]=1;
  put(w,8,7);put(w,20,banks*125);put(w,24,banks*376);put(w,28,250000);
  for(unsigned b=0;b<banks;b++) {
   auto off=44+b*376;w[off]=uint8_t(b%13);
   // Signed extrema +2047/-2048, plus -1/+1; all remaining pairs zero.
   w[off+1]=255;w[off+2]=7;w[off+3]=128;
   w[off+4]=255;w[off+5]=31;w[off+6]=0;
  }
  checksum(w);auto fs=decodePacked(w);assert(fs.size()==banks);
  Continuity c;c.id=7;c.rate=250000;
  for(unsigned b=0;b<banks;b++) {
   float scale=float(1u<<(b%13))/4194304.0f;
   assert(fs[b].samples[0]==std::complex<float>(2047*scale,-2048*scale));
   assert(fs[b].samples[1]==std::complex<float>(-scale,scale));c.accept(fs[b]);
  }
  auto terminal=frame(banks,250000,true);std::memcpy(terminal.data(),"LIP2",4);checksum(terminal);
  c.accept(decodePacked(terminal).front());
  auto bad=w;bad[44]=13;checksum(bad);reject([&]{decodePacked(bad);});
  bad=w;bad.back()^=1;reject([&]{decodePacked(bad);});
  bad=w;put(bad,20,126);checksum(bad);reject([&]{decodePacked(bad);});
  bad=w;put(bad,24,0xffffffffu);checksum(bad);reject([&]{decodePacked(bad);});
  bad=w;put(bad,28,500000);checksum(bad);reject([&]{decodePacked(bad);});
 }
 // LIP3 uses per-frame xorshift whitening, including partial final words.
 auto whiten=[](std::vector<uint8_t>& w) {
  uint32_t seq=uint32_t(w[12])|uint32_t(w[13])<<8|uint32_t(w[14])<<16|uint32_t(w[15])<<24;
  uint32_t x=(seq^0x9e3779b9u)|1u;
  for(size_t i=44;i<w.size();i+=4) {
   x^=x<<13;x^=x>>17;x^=x<<5;
   for(size_t j=0;j<4 && i+j<w.size();j++)w[i+j]^=uint8_t(x>>(8*j));
  }
 };
 for(unsigned banks:{1u,3u,16u})for(uint32_t seq:{0u,0xffffffffu}) {
  std::vector<uint8_t> w(44+banks*251);std::memcpy(w.data(),"LIP3",4);w[4]=1;w[5]=1;
  put(w,8,7);put(w,12,seq);put(w,16,seq*125u);put(w,20,banks*125);put(w,24,banks*251);put(w,28,500000);
  for(unsigned b=0;b<banks;b++) {
   size_t off=44+b*251;w[off]=b==15?16:b;
   w[off+1]=127;w[off+2]=128;w[off+3]=255;w[off+4]=1;
  }
  auto plain=w;whiten(w);checksum(w);
  // Known first xorshift output at sequence zero: 0x510c4619.
  if(seq==0)assert(w[44]==0x19 && w[45]==(127^0x46) && w[46]==(128^0x0c) && w[47]==(255^0x51));
  auto fs=decodePacked(w);assert(fs.size()==banks);Continuity c;c.id=7;c.rate=500000;c.expected=seq;
  for(unsigned b=0;b<banks;b++) {
   float scale=float(1u<<(b==15?16:b))/4194304.0f;
   assert(fs[b].samples[0]==std::complex<float>(127*scale,-128*scale));
   assert(fs[b].samples[1]==std::complex<float>(-scale,scale));assert(fs[b].limitComponents==2);c.accept(fs[b]);
  }
  auto terminal=frame(seq+banks,500000,true);std::memcpy(terminal.data(),"LIP3",4);checksum(terminal);
  c.accept(decodePacked(terminal).front());
  auto bad=w;bad.back()^=1;reject([&]{decodePacked(bad);});
  bad=w;put(bad,28,250000);checksum(bad);reject([&]{decodePacked(bad);});
  bad=plain;bad[44]=17;whiten(bad);checksum(bad);reject([&]{decodePacked(bad);});
  bad=w;put(bad,20,126);checksum(bad);reject([&]{decodePacked(bad);});
  bad=w;bad[3]='2';checksum(bad);reject([&]{decodePacked(bad);});
 }
 validateGainSettings({});
 for(bool agc:{false,true}) {
  validateGainSettings({agc,4,0,0});validateGainSettings({agc,13,7,3});
  reject([&]{validateGainSettings({agc,3,0,0});});reject([&]{validateGainSettings({agc,14,0,0});});
  reject([&]{validateGainSettings({agc,10,8,0});});reject([&]{validateGainSettings({agc,10,0,4});});
 }
 std::cout<<"LIQ1/LIP2/LIP3 signed conversion, whitening, scaling, CRC rejection, dimensions, sequence/index wrap, terminal counts, and RF/rate guards passed\n";
}
