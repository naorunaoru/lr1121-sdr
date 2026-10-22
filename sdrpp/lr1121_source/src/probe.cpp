/* SPDX-License-Identifier: GPL-3.0-only */
#include "device.h"
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
int main(int argc,char** argv) {
    using namespace lr1121;
    try {
        if(argc==2&&std::string(argv[1])=="--ports"){std::cout<<json(ports()).dump(2)<<std::endl;return 0;}
        if(argc<5||argc>11){std::cerr<<"Usage: lr1121_probe PORT RATE INPUT FREQUENCY [SECONDS] [CF32_FILE|-] [agc|GAIN_STEP] [BOOST] [narrow|wide] [SHIFT_0_16] (INPUT: 0=Auto, 1=LF, 2=HF)\n";return 2;}
        auto u32=[](const char* text) {size_t n=0;std::string s(text);auto v=std::stoull(s,&n);
            if(n!=s.size()||s.empty()||s[0]=='-'||v>0xffffffffu)throw std::runtime_error("Expected an unsigned 32-bit integer");return uint32_t(v);};
        uint32_t rate=u32(argv[2]),frequency=u32(argv[4]);unsigned input=u32(argv[3]);
        double seconds=argc>5?std::stod(argv[5]):2;
        GainSettings hardware;
        if(argc>7 && std::string(argv[7])!="agc"){hardware.agc=false;hardware.step=u32(argv[7]);}
        if(argc>8){unsigned boost=u32(argv[8]);if(input==2||(input==0&&frequency>=1500000000u))hardware.boostHF=boost;else hardware.boostLF=boost;}
        validateSettings(frequency,rate,input);
        validateGainSettings(hardware);
        std::string filter=argc>9?argv[9]:"narrow";
        if(filter!="narrow"&&filter!="wide")throw std::runtime_error("Filter must be narrow or wide");
        if(!std::isfinite(seconds)||seconds<=0||seconds>3600)throw std::runtime_error("Duration must be 0..3600 seconds");
        bool saveFile=argc>6&&std::string(argv[6])!="-";
        std::ofstream output;if(saveFile)output.open(argv[6],std::ios::binary);
        if(saveFile&&!output)throw std::runtime_error("Cannot open capture output");
        Device d(argv[1]);d.configure(frequency,rate,input,hardware,filter=="wide",argc>10?u32(argv[10]):15);d.begin();
        auto start=std::chrono::steady_clock::now();bool cancelled=false;Frame last;uint64_t samples=0,limits=0;
        while(true) {
            auto f=d.next();
            if(f.end){last=f;break;}
            limits+=f.limitComponents;samples+=f.samples.size();if(saveFile){output.write(reinterpret_cast<const char*>(f.samples.data()),sizeof(f.samples));if(!output)throw std::runtime_error("Capture file write failed");}
            if(!cancelled && samples>=uint64_t(seconds*rate)){d.cancel();cancelled=true;}
        }
        double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();d.standby();
        json r={{"bridge",d.info},{"frequency_hz",frequency},{"rate_hz",rate},{"input",inputs.at(input)},{"setup",d.setup},{"frames",d.continuity.frames},{"samples",samples},{"limit_components",limits},{"seconds",elapsed},{"end_flags",last.flags},{"sequence_gaps",0},{"crc_errors",0}};
        std::vector<uint32_t> stats;for(size_t i=0;i<last.payload.size();i+=4){const auto* p=last.payload.data()+i;stats.push_back(uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24);}r["terminal_stats"]=stats;
        r["success"]=cancelled&&last.flags==4&&stats.size()==9&&stats[7]==0;
        std::cout<<r.dump(2)<<std::endl;return r["success"].get<bool>()?0:1;
    }catch(const std::exception& e){std::cerr<<"LR1121: "<<e.what()<<std::endl;return 1;}
}
