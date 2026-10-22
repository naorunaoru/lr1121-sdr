/* SPDX-License-Identifier: GPL-3.0-only */
// Hardware validation driver. One JSON request per line; one result per line.
// Owns the Pico exclusively until EOF, then resets it to standby.
#include "device.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
using Clock=std::chrono::steady_clock;
static double ms(Clock::time_point t){return std::chrono::duration<double,std::milli>(Clock::now()-t).count();}
int main(int argc,char** argv) {
    try {
        if(argc!=2)throw std::runtime_error("Usage: retune_probe SERIAL_PORT");
        lr1121::Device d(argv[1]);
        std::cout<<lr1121::json{{"ready",true},{"bridge",d.info}}<<std::endl;
        std::string line;
        while(std::getline(std::cin,line)) {
            auto c=lr1121::json::parse(line);
            if(c.value("standby",false)){d.standby();std::cout<<"{\"standby\":true}"<<std::endl;continue;}
            lr1121::GainSettings g{c.value("agc",true),c.value("gain",10u),c.value("boostLF",0u),c.value("boostHF",0u)};
            auto started=Clock::now();d.configure(c.at("frequency"),c.value("rate",125000u),c.value("input",0u),g,c.value("wide",true),c.value("shift",15u));
            lr1121::json result{{"request",c},{"setup",d.setup}};
            unsigned blocks=c.value("blocks",32u);
            if(blocks>8192)throw std::runtime_error("Too many blocks");
            if(blocks) {
                std::ofstream out;
                if(c.contains("file")){out.open(c.at("file").get<std::string>(),std::ios::binary);if(!out)throw std::runtime_error("Cannot open capture");}
                uint64_t limits=0;d.begin();
                for(unsigned i=0;i<blocks;++i) {
                    auto f=d.next();limits+=f.limitComponents;if(f.end)throw std::runtime_error("Early terminal frame");
                    if(!i)result["configure_to_first_frame_ms"]=ms(started);
                    if(out.is_open())out.write(reinterpret_cast<const char*>(f.samples.data()),sizeof(f.samples));
                }
                if(out.is_open()&&!out)throw std::runtime_error("Capture write failed");
                auto cancelled=Clock::now();d.cancel();lr1121::Frame end;
                do{end=d.next();}while(!end.end);
                if(end.flags!=4)throw std::runtime_error("Unexpected cancel flags");
                result["cancel_ms"]=ms(cancelled);result["limit_components"]=limits;
                result["frames"]=d.continuity.frames;
                result["terminal_flags"]=end.flags;
            }
            std::cout<<result<<std::endl;
        }
        d.standby();
    }catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}
}
