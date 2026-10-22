/* SPDX-License-Identifier: GPL-3.0-only */
#include "device.h"
#include <cassert>
#include <iostream>
using namespace lr1121;
int main() {
    unsigned checks=0;
    auto check=[&](RetuneProfile a,RetuneProfile b,bool expected){
        assert(canReuseConfiguration(a,b)==expected);++checks;
    };
    for(auto rate:{62500u,125000u,250000u,500000u})for(unsigned input=0;input<3;++input)
    for(bool wide:{false,true})for(bool agc:{false,true}) {
        RetuneProfile a{433920000,rate,input,{agc,10,0,0},wide},b=a;
        check(a,b,true);
        for(auto f:{428000000u,433930000u,440000000u}){b.frequency=f;check(a,b,true);}
        for(auto f:{427999999u,440000001u}){b.frequency=f;check(a,b,false);}
        // Each comparison still uses the initial calibration anchor. A series
        // of small steps must not migrate the calibration window.
        for(uint32_t f=434000000;f<=442000000;f+=1000000){b.frequency=f;check(a,b,f<=440000000);}
        b=a;b.rate=rate==62500?125000:62500;check(a,b,false);
        b=a;b.input=(input+1)%3;check(a,b,false);
        b=a;b.wide=!wide;check(a,b,false);
        b=a;b.gain.agc=!agc;check(a,b,false);
        b=a;++b.gain.step;check(a,b,false);
        b=a;++b.gain.boostLF;check(a,b,false);
        b=a;++b.gain.boostHF;check(a,b,false);
        for(auto frequency:{91600000u,2440000000u}) {
            a.frequency=frequency;b=a;
            b.frequency=frequency+10000000;check(a,b,true);
            b.frequency=frequency+10000001;check(a,b,false);
            b.frequency=frequency-10000000;check(a,b,true);
            b.frequency=frequency-10000001;check(a,b,false);
        }
        for(auto boundary:{400000000u,410000000u,510000001u,850000000u,
                           930000001u,1000000001u,1500000000u,2300000000u}) {
            a.frequency=boundary-1;b=a;b.frequency=boundary;
            check(a,b,false);check(b,a,false);
        }
    }
    RetuneProfile a{433920000,125000,0,{},true},b=a;
    b.frequency=0;check(a,b,false);check(b,a,false);
    std::cout<<checks<<" calibration-window, profile and setting reuse checks passed\n";
}
