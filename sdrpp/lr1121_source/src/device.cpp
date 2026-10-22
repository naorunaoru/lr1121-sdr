#include "device.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>
#endif
namespace lr1121 {
using Clock=std::chrono::steady_clock;
const std::array<const char*,3> inputs{{"Auto", "LF", "HF"}};
void validateSettings(uint32_t f,uint32_t rate,unsigned input) {
    if(rate!=62500 && rate!=125000 && rate!=250000 && rate!=500000)throw std::runtime_error("Select 62.5, 125, 250 or 500 kS/s");
    if(input>=inputs.size())throw std::runtime_error("Select Auto, LF, or HF input");
    // Firmware 1.3 converts Hz to floor(Hz * 16384 / 15625), then stores
    // the result in a 32-bit PLL register. This is an encoding guard, not
    // a claim of RF coverage. Leave undocumented frequencies explorable.
    if(!f || uint64_t(f)*16384/15625>0xffffffffu)
        throw std::runtime_error("Frequency is not representable by the LR1121 synthesizer");
}
static std::string hex(uint32_t v,int width=8) {
    std::ostringstream s;s<<std::hex<<std::setfill('0')<<std::setw(width)<<v;return s.str();
}
void validateGainSettings(const GainSettings& gain) {
    if(gain.step<4 || gain.step>13)throw std::runtime_error("Manual hardware gain step must be 4..13");
    if(gain.boostLF>7 || gain.boostHF>3)throw std::runtime_error("LNA boost must be 0..7 for LF and 0..3 for HF");
}
bool canReuseConfiguration(const RetuneProfile& a,const RetuneProfile& b) {
    if(!a.frequency || !b.frequency || a.rate!=b.rate || a.input!=b.input || a.wide!=b.wide ||
       a.gain.agc!=b.gain.agc || a.gain.step!=b.gain.step ||
       a.gain.boostLF!=b.gain.boostLF || a.gain.boostHF!=b.gain.boostHF)return false;
    if(std::abs(int64_t(b.frequency)-a.frequency)>10000000)return false;
    // Do not cross the firmware's automatic input or modem/RSSI profile
    // boundaries using state configured for the other region.
    auto rssi=[](uint32_t f){return f>=2300000000u?3:(f>=850000000&&f<=930000000?2:
                                                   (f>=410000000&&f<=510000000?1:0));};
    if(rssi(a.frequency)!=rssi(b.frequency) || (a.frequency>=1500000000u)!=(b.frequency>=1500000000u))return false;
    auto calibrated=[](uint32_t f){return f>=400000000 && f<=1000000000;};
    if(calibrated(a.frequency)!=calibrated(b.frequency))return false;
    if(calibrated(a.frequency)) {
        uint32_t low=((a.frequency/1000000-4)/4)*4000000;
        uint32_t high=((a.frequency/1000000+7)/4)*4000000;
        if(b.frequency<low || b.frequency>high)return false;
    }
    return true;
}
static std::vector<uint8_t> unhex(const std::string& s) {
    if(s.size()%2)throw std::runtime_error("Malformed SPI reply");
    std::vector<uint8_t> out;
    for(size_t i=0;i<s.size();i+=2) {
        auto nibble=[](char c)->int {if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;throw std::runtime_error("Invalid SPI hex");};
        out.push_back(uint8_t(nibble(s[i])*16+nibble(s[i+1])));
    }
    return out;
}
static uint32_t le(const uint8_t* p) {return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
static uint32_t be(const uint8_t* p) {return uint32_t(p[3])|uint32_t(p[2])<<8|uint32_t(p[1])<<16|uint32_t(p[0])<<24;}
uint32_t crc32(const uint8_t* p,size_t n) {
    static const auto table=[] {std::array<uint32_t,256> t{};for(uint32_t i=0;i<256;i++){uint32_t c=i;for(int k=0;k<8;k++)c=(c>>1)^((c&1)?0xedb88320u:0);t[i]=c;}return t;}();
    uint32_t c=0xffffffff;while(n--)c=table[(c^*p++)&255]^(c>>8);return c^0xffffffff;
}
Frame decode(const std::vector<uint8_t>& w,uint32_t rate) {
    if(w.size()<44 || std::memcmp(w.data(),"LIQ1",4) || w[4]!=1 || (w[5]!=1&&w[5]!=2))throw std::runtime_error("Invalid LIQ1 frame");
    const uint8_t* p=w.data();
    if(crc32(p,40)!=le(p+40))throw std::runtime_error("Header CRC mismatch");
    Frame f;f.end=p[5]==2;f.flags=uint16_t(p[6])|uint16_t(p[7])<<8;
    f.id=le(p+8);f.sequence=le(p+12);f.sampleIndex=le(p+16);f.rate=le(p+28);f.timestamp=le(p+32);
    uint32_t length=le(p+24);
    if((rate!=62500&&rate!=125000&&rate!=250000)||f.rate!=rate||f.sampleIndex!=f.sequence*125u||
       le(p+20)!=(f.end?0u:125u)||length!=(f.end?36u:750u)||w.size()!=44+length)
        throw std::runtime_error("Invalid frame dimensions/rate/index");
    if(crc32(p+44,length)!=le(p+36))throw std::runtime_error("Payload CRC mismatch");
    f.payload.assign(w.begin()+44,w.end());
    if(!f.end) {
        if(f.flags)throw std::runtime_error("Unexpected data flags");
        auto sample=[](const uint8_t* v) {
            if(v[2]&0x80)throw std::runtime_error("Invalid signed-23 container");
            int32_t x=v[0]|int32_t(v[1])<<8|int32_t(v[2])<<16;
            x=(x&0x3fffff)-(x&0x400000);
            return float(x)/4194304.0f;
        };
        for(size_t i=0;i<125;i++)f.samples[i]={sample(p+44+i*6),sample(p+47+i*6)};
    }
    return f;
}
std::vector<Frame> decodePacked(const std::vector<uint8_t>& w) {
    if(w.size()<44 || (std::memcmp(w.data(),"LIP2",4) && std::memcmp(w.data(),"LIP3",4)) ||
       w[4]!=1 || (w[5]!=1&&w[5]!=2))throw std::runtime_error("Invalid packed frame");
    const auto* p=w.data();
    const bool eight=p[3]=='3';
    const uint32_t rate=eight?500000:250000, stride=eight?251:376;
    uint32_t count=le(p+20),length=le(p+24),seq=le(p+12);
    // CRC covers the transmitted (whitened for LIP3 data) bytes.
    if(crc32(p,40)!=le(p+40) || le(p+28)!=rate || le(p+16)!=seq*125u ||
       length>16*stride || w.size()!=44+length || crc32(p+44,length)!=le(p+36))
        throw std::runtime_error("Packed frame CRC, rate, size or index mismatch");
    if(p[5]==2) {
        if(count || length!=36)throw std::runtime_error("Invalid packed terminal frame");
        Frame f;f.end=true;f.id=le(p+8);f.sequence=seq;f.sampleIndex=le(p+16);f.rate=rate;
        f.timestamp=le(p+32);f.flags=uint16_t(p[6])|uint16_t(p[7])<<8;
        f.payload.assign(w.begin()+44,w.end());return {f};
    }
    if(p[6] || p[7] || !count || count%125 || count>2000 || length!=count/125*stride)
        throw std::runtime_error("Invalid packed data dimensions");
    std::vector<uint8_t> payload(w.begin()+44,w.end());
    if(eight) {
        uint32_t x=(seq^0x9e3779b9u)|1u;
        for(size_t i=0;i<payload.size();++i) {
            if(!(i&3)){x^=x<<13;x^=x>>17;x^=x<<5;}
            payload[i]^=uint8_t(x>>(8*(i&3)));
        }
    }
    std::vector<Frame> result;
    for(uint32_t bank=0;bank<count/125;++bank) {
        const auto* b=payload.data()+bank*stride;
        if(*b>(eight?16:12))throw std::runtime_error("Invalid packed scale");
        float scale=std::ldexp(1.0f,int(*b++)-22);
        Frame f;f.id=le(p+8);f.sequence=seq+bank;f.sampleIndex=f.sequence*125u;
        f.rate=rate;f.timestamp=le(p+32);
        for(unsigned i=0;i<125;++i) {
            int32_t x,y;
            if(eight) {
                x=int(b[0]&127)-int(b[0]&128);y=int(b[1]&127)-int(b[1]&128);b+=2;
                f.limitComponents+=unsigned(x==-128||x==127)+unsigned(y==-128||y==127);
            } else {
                x=b[0]|int32_t(b[1]&15)<<8;y=(b[1]>>4)|int32_t(b[2])<<4;b+=3;
                x=(x&2047)-(x&2048);y=(y&2047)-(y&2048);
            }
            f.samples[i]={x*scale,y*scale};
        }
        result.push_back(std::move(f));
    }
    return result;
}
void Continuity::accept(const Frame& f) {
    if(f.id!=id || f.rate!=rate || f.sequence!=expected)throw std::runtime_error("Stream ID, rate, or sequence gap");
    if(f.end) {
        if(f.payload.size()!=36 || le(f.payload.data())!=expected || le(f.payload.data()+4)!=expected)
            throw std::runtime_error("Terminal frame counts disagree");
    } else {++expected;++frames;}
}
std::vector<std::string> ports() {
    std::vector<std::string> result;
#ifdef __APPLE__
    io_iterator_t it=0;
    if(IOServiceGetMatchingServices(kIOMainPortDefault,IOServiceMatching(kIOSerialBSDServiceValue),&it)!=KERN_SUCCESS)return result;
    io_object_t service;
    while((service=IOIteratorNext(it))) {
        auto path=(CFStringRef)IORegistryEntryCreateCFProperty(service,CFSTR(kIOCalloutDeviceKey),kCFAllocatorDefault,0);
        auto product=(CFStringRef)IORegistryEntrySearchCFProperty(service,kIOServicePlane,CFSTR("USB Product Name"),kCFAllocatorDefault,kIORegistryIterateRecursively|kIORegistryIterateParents);
        if(path && product && CFGetTypeID(path)==CFStringGetTypeID() && CFGetTypeID(product)==CFStringGetTypeID() && CFStringCompare(product,CFSTR("LR1121 SPI Bridge"),0)==kCFCompareEqualTo) {
            char value[1024];if(CFStringGetCString(path,value,sizeof(value),kCFStringEncodingUTF8))result.emplace_back(value);
        }
        if(path)CFRelease(path);if(product)CFRelease(product);IOObjectRelease(service);
    }
    IOObjectRelease(it);
#endif
    std::sort(result.begin(),result.end());return result;
}
Device::Device(const std::string& path) {
    fd=::open(path.c_str(),O_RDWR|O_NOCTTY|O_NONBLOCK);
    if(fd<0)throw std::runtime_error("Cannot open bridge: "+std::string(std::strerror(errno)));
    try {
        if(ioctl(fd,TIOCEXCL)<0)throw std::runtime_error("Cannot claim bridge exclusively");
        termios t{};if(tcgetattr(fd,&t)<0)throw std::runtime_error("Cannot read serial settings");
        cfmakeraw(&t);cfsetspeed(&t,B115200);t.c_cflag|=CLOCAL|CREAD;t.c_cc[VMIN]=0;t.c_cc[VTIME]=0;
        if(tcsetattr(fd,TCSANOW,&t)<0)throw std::runtime_error("Cannot configure serial port");
        int bits=TIOCM_DTR|TIOCM_RTS;if(ioctl(fd,TIOCMBIS,&bits)<0)throw std::runtime_error("Cannot assert USB DTR");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));tcflush(fd,TCIOFLUSH);
        send("\n");std::this_thread::sleep_for(std::chrono::milliseconds(100));tcflush(fd,TCIFLUSH);
        info=request("info");
        if(info.value("bridge","")!="lr1121-spi" || (info.value("firmware","")!="1.0.5" && info.value("firmware","")!="1.0.6" && info.value("firmware","")!="1.0.7"))throw std::runtime_error("Requires LR1121 bridge firmware 1.0.5, 1.0.6 or 1.0.7");
    } catch(...) {::close(fd);fd=-1;throw;}
}
Device::~Device() {
    if(fd>=0) {
        try {if(streaming){cancel();while(!next().end){}}if(initialized)standby();}catch(...){}
        ioctl(fd,TIOCNXCL);::close(fd);
    }
}
void Device::send(const std::string& s) {
    size_t pos=0;auto deadline=Clock::now()+std::chrono::seconds(2);
    while(pos<s.size()) {
        if(Clock::now()>deadline)throw std::runtime_error("USB write timeout; not retried");
        pollfd p{fd,POLLOUT,0};int r=poll(&p,1,100);
        if(r<0&&errno==EINTR)continue;
        if(r<0||(p.revents&(POLLERR|POLLHUP|POLLNVAL)))throw std::runtime_error("USB disconnected while writing");
        if(r==0)continue;
        ssize_t n=::write(fd,s.data()+pos,s.size()-pos);
        if(n>0)pos+=size_t(n);else if(n<0&&errno!=EAGAIN&&errno!=EINTR)throw std::runtime_error("USB write failed");
    }
}
std::vector<uint8_t> Device::take(size_t count,int timeoutMs) {
    auto deadline=Clock::now()+std::chrono::milliseconds(timeoutMs);
    while(buffer.size()-cursor<count) {
        if(Clock::now()>deadline)throw std::runtime_error("USB read timeout");
        pollfd p{fd,POLLIN,0};int r=poll(&p,1,100);
        if(r<0&&errno==EINTR)continue;
        if(r<0||(p.revents&(POLLERR|POLLHUP|POLLNVAL)))throw std::runtime_error("USB disconnected while reading");
        if(r==0)continue;
        uint8_t data[16384];ssize_t n=::read(fd,data,sizeof(data));
        if(n>0)buffer.insert(buffer.end(),data,data+n);
        else if(n<0&&errno!=EAGAIN&&errno!=EINTR)throw std::runtime_error("USB read failed");
    }
    std::vector<uint8_t> out(buffer.begin()+cursor,buffer.begin()+cursor+count);cursor+=count;
    if(cursor==buffer.size()){buffer.clear();cursor=0;}
    else if(cursor>32768){buffer.erase(buffer.begin(),buffer.begin()+cursor);cursor=0;}
    return out;
}
json Device::request(const std::string& command) {
    if(streaming)throw std::runtime_error("ASCII command during binary stream");
    requestId=requestId==0xffffffffu?1:requestId+1;
    send(std::to_string(requestId)+" "+command+"\n");
    std::string line;auto deadline=Clock::now()+std::chrono::seconds(5);
    while(line.size()<20000 && Clock::now()<deadline) {char c=char(take(1)[0]);if(c=='\n')break;line+=c;}
    auto reply=json::parse(line);
    if(reply.at("id").get<uint32_t>()!=requestId || !reply.at("ok").get<bool>())throw std::runtime_error("Bridge command failed: "+reply.dump());
    return reply;
}
std::vector<uint8_t> Device::readCommand(const std::string& cmd,size_t size) {
    auto r=request("read 1500 "+std::to_string(size)+" "+cmd);
    if(r.at("cmd_status").get<int>()!=3)throw std::runtime_error("Radio read rejected");
    auto data=unhex(r.at("data").get<std::string>());
    if(data.size()!=size)throw std::runtime_error("Short radio read");return data;
}
void Device::writeCommand(const std::string& cmd,int mode) {
    request("write 1500 "+cmd);
    auto s=unhex(request("xfer 1500 000000000000").at("rx").get<std::string>());
    if(s.size()!=6 || s[0]>>1!=2 || (mode>=0 && ((s[1]>>1)&7)!=mode))throw std::runtime_error("Radio rejected "+cmd+" / unexpected mode");
}
std::vector<uint32_t> Device::memory(uint32_t a,size_t words) {
    if(!words||words>64)throw std::runtime_error("Memory read length");
    auto d=readCommand("0106"+hex(a)+hex(uint32_t(words),2),words*4);std::vector<uint32_t> v;
    for(size_t i=0;i<words;i++)v.push_back(be(d.data()+4*i));return v;
}
void Device::masked(uint32_t a,uint32_t mask,uint32_t v) {writeCommand("010c"+hex(a)+hex(mask)+hex(v));}
void Device::configure(uint32_t frequency,uint32_t rate,unsigned input,const GainSettings& gain,bool wideFilter,unsigned shift) {
    validateSettings(frequency,rate,input);
    validateGainSettings(gain);
    if(rate==250000 && (!wideFilter || info.value("packed_stream","")!="LIP2"))
        throw std::runtime_error("250 kS/s requires Wide filter and bridge 1.0.6 with the LR1121 packing patch");
    if(shift>16)throw std::runtime_error("8-bit shift must be 0..16");
    if(rate==500000 && (!wideFilter || info.value("fifo_stream","")!="LIP3"))
        throw std::runtime_error("500 kS/s requires Wide filter, bridge 1.0.7 and the FIFO packing extension");
    if(streaming)throw std::runtime_error("Stop stream before reconfiguration");
    auto started=Clock::now();
    RetuneProfile next{frequency,rate,input,gain,wideFilter};
    bool fast=configured&&canReuseConfiguration(anchor,next);
    configured=false; // Any incomplete setup invalidates reuse.
    setup={{"input",inputs.at(input)},{"frequency_hz",frequency},{"filter",wideFilter?"wide_gfsk":"narrow_lora"},
           {"fast_retune",fast}};
    if(fast) {
        // A drained stream has stopped sample requests. Firmware SetRx
        // rebuilds its normal DMA descriptors; installRing checks them below.
        writeCommand("011c01",2);
        writeCommand("0114ffffffff",2);
        writeCommand("020b"+hex(frequency),2);
        uint32_t current=memory(0xf20124,1)[0];
        if(((current>>20)&7)!=(rate==62500?5u:rate==500000?2u:rate==250000?3u:4u) || ((current>>24)&3)!=1)
            throw std::runtime_error("Cached receiver rate changed; restart required");
    } else {
        auto v=readCommand("0101",4);
        if(v!=std::vector<uint8_t>({0x22,3,1,3}))throw std::runtime_error("Undocumented capture requires LR1121 firmware 1.3");
        request("speed 500000");request("reset 1500");initialized=true;
        writeCommand("011c00",1);writeCommand("01170600012c",1);writeCommand("011001",1);
        writeCommand("01120300010202000000",1);writeCommand("011605",1);
        writeCommand("010e",1);writeCommand("010f3f",1);writeCommand("011400800000",1);writeCommand("011c01",2);
        if(frequency>=400000000 && frequency<=1000000000) {
            // Public image-calibration arguments are 8-bit values in 4 MHz steps.
            // 150 MHz calibration was rejected on this module. Retain reset
            // calibration below 400 MHz and above 1 GHz; tuning remains allowed.
            uint32_t low=(frequency/1000000-4)/4, high=(frequency/1000000+7)/4;
            writeCommand("0111"+hex(low,2)+hex(high,2),1);writeCommand("011c01",2);
        }
        writeCommand(wideFilter?"020e01":"020e02",2);writeCommand("020b"+hex(frequency),2);
        if(frequency>=2300000000u)writeCommand("0229764643cececc8c980907ee",2);
        else if(frequency>=850000000 && frequency<=930000000)writeCommand("02292232434564556676060000",2);
        else if(frequency>=410000000 && frequency<=510000000)writeCommand("0229cc0e314463666666060000",2);
        if(wideFilter) {
            // Documented GFSK profile: 10 kb/s, BT 0.5, 5 kHz deviation.
            // Native 58.6/29.3 kHz DSB filter codes already choose rate field 4/5.
            // The I/Q tap's measured passband is narrower than the command's BW.
            writeCommand("020f0000271009"+std::string(rate==500000?"0a":rate==250000?"0b":rate==125000?"0c":"0d")+"00001388",2);
            writeCommand("0210002004200001ff0101",2);
            writeCommand("02061234567800000000",2);
        } else {
            writeCommand(frequency>=2300000000u?"020f070d0100":"020f07050100",2);
            writeCommand("0210000800ff0100",2);writeCommand("022b12",2);
        }
        uint32_t before=memory(0xf20124,1)[0];
        uint32_t field=rate==62500?5:rate==500000?2:rate==250000?3:4;
        if(((before>>20)&7)!=(wideFilter?field:0) || ((before>>24)&3)!=1)throw std::runtime_error("Unexpected native rate profile; no rate patch written");
        if(!wideFilter)masked(0xf20124,0x00700000,field<<20);
        if(memory(0xf20124,1)[0]!=(before|(field<<20)))throw std::runtime_error("Rate readback mismatch");
        // UM LR1121 Rev 1.2, section 7.2.15. Reset above restores chip AGC
        // on every configuration, including a transition back from manual gain.
        if(!gain.agc) {
            masked(0xf20214,0x00080000,0x00080000);
            masked(0xf20230,0x71110000,0x71100000);
        }
    }
    writeCommand("0209ffffff",4);std::this_thread::sleep_for(std::chrono::milliseconds(30));
    if(readCommand("010d",2)!=std::vector<uint8_t>({0,0}))throw std::runtime_error("Radio error after RX setup");
    // SetRx chooses the path by PLL frequency. Stop sample requests before
    // replacing its three path fields; these masks come from the firmware
    // 1.3 RX routine at 0x8BB44..0x8BBB2. Reset removes every override.
    uint32_t pll=memory(0xf30058,1)[0];
    uint32_t expectedPLL=uint32_t(uint64_t(frequency)*16384/15625)|1u;
    if(pll!=expectedPLL)throw std::runtime_error("PLL frequency readback mismatch");
    setup["pll_word"]=pll;
    if(input) {
        masked(0xf20014,0x30000010,0x30000000);
        bool hf=input==2;
        for(auto patch:std::array<std::array<uint32_t,3>,3>{{
            {{0xf3008c,0xf1,hf?1u:0x30u}},
            {{0xf30024,0x10,hf?0u:0x10u}},
            {{0xf30094,9,hf?8u:1u}}
        }}) {
            uint32_t before=memory(patch[0],1)[0];
            masked(patch[0],patch[1],patch[2]);
            uint32_t after=memory(patch[0],1)[0];
            if(after!=((before&~patch[1])|patch[2]))throw std::runtime_error("RF input readback mismatch");
        }
    }
    setup["path_registers"]={{"f3008c",memory(0xf3008c,1)[0]},
                             {"f30024",memory(0xf30024,1)[0]},
                             {"f30094",memory(0xf30094,1)[0]}};
    setup["effective_input"]=(setup["path_registers"]["f3008c"].get<uint32_t>()&1)?"HF":"LF";
    // Apply boost after the RF-path override so the effective physical input
    // determines its limit, even when tuning outside documented bands.
    unsigned boost=setup["effective_input"]=="HF"?gain.boostHF:gain.boostLF;
    if(!gain.agc)masked(0xf20214,0x00f00000,gain.step<<20);
    masked(0xf3008c,0x00070000,boost<<16);
    uint32_t gainReg=memory(0xf20214,1)[0],agcReg=memory(0xf20230,1)[0],boostReg=memory(0xf3008c,1)[0];
    if(((gainReg>>19)&1)!=unsigned(!gain.agc) || ((boostReg>>16)&7)!=boost ||
       (!gain.agc && (((gainReg>>20)&15)!=gain.step || (agcReg&0x71110000)!=0x71100000)))
        throw std::runtime_error("Hardware gain/AGC/boost readback mismatch");
    setup["hardware_gain"]={{"mode",gain.agc?"chip_agc":"manual"},{"step",gain.step},
        {"boost",boost},{"boost_lf",gain.boostLF},{"boost_hf",gain.boostHF},
        {"f20214",gainReg},{"f20230",agcReg},{"f3008c",boostReg}};
    setup["path_registers"]["f3008c"]=boostReg;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    setup["image_calibrated"]=frequency>=400000000 && frequency<=1000000000;
    sampleRate=rate;sampleShift=shift;if(rate>=250000)installPackedRing();else installRing();
    auto speed=request(rate==500000?"speed 24000000":"speed 16000000");if(speed.at("spi_hz").get<int>()!=(rate==500000?20833333:15625000))throw std::runtime_error("Unexpected SPI clock");
    if(!fast)anchor=next;
    configured=true;
    setup["calibration_frequency_hz"]=anchor.frequency;
    setup["configure_ms"]=std::chrono::duration<double,std::milli>(Clock::now()-started).count();
}
static uint32_t descriptor(unsigned i) {return i<2?0x800a00+16*i:0x80e000+16*i;}
void Device::installRing() {
    auto gp=memory(0x800498,6),state=memory(0x800534,3),original=memory(0x800a00,8);
    const std::vector<uint32_t> expected{0xbfe0ffa7,0x901004,0x800324,0x800a10,0xbfe0ffa7,0x901004,0x800324,0x800a00};
    if((gp[1]>>16)!=0x0104 || ((state[1]>>16)&255)!=0 || original!=expected)throw std::runtime_error("RX DMA layout differs; no ring installed");
    masked(0xf20014,0x30000010,0x30000000);
    std::vector<uint32_t> ring;
    for(unsigned i=0;i<32;i++){ring.push_back(0x9fe3e7a7);ring.push_back(0x901004);ring.push_back(0x806000+(i+1)*1000-4);ring.push_back(descriptor((i+1)%32));}
    for(unsigned start=0;start<128;start+=64){std::string cmd="0105"+hex(0x80e000+4*start);for(unsigned k=start;k<start+64;k++)cmd+=hex(ring[k]);writeCommand(cmd);}
    for(unsigned start=0;start<128;start+=64)if(memory(0x80e000+4*start,64)!=std::vector<uint32_t>(ring.begin()+start,ring.begin()+start+64))throw std::runtime_error("Ring scratch readback mismatch");
    for(unsigned i=0;i<8;i++)masked(0x800a00+4*i,0xffffffff,ring[i]);
    if(memory(0x800a00,8)!=std::vector<uint32_t>(ring.begin(),ring.begin()+8)||memory(0x800580,1)[0]!=0x800a00)throw std::runtime_error("Ring head readback mismatch");
}
void Device::begin(uint32_t blocks) {
    if(blocks>8192)throw std::runtime_error("Invalid frame count");
    packedFrames.clear();
    auto r=request(sampleRate==500000?"packed 500000 "+std::to_string(blocks)+" 8 18":sampleRate==250000?"packed 250000 "+std::to_string(blocks)+" 12":
                   "stream2 "+std::to_string(sampleRate)+" "+std::to_string(blocks));
    if(r.value("stream","")!=(sampleRate==500000?"LIP3":sampleRate==250000?"LIP2":"LIQ1") ||
       r.value("format","")!=(sampleRate>=250000?"iq_block_float":"iq23le"))throw std::runtime_error("Invalid stream acknowledgement");
    continuity={};continuity.id=requestId;continuity.rate=sampleRate;cancelled=false;streaming=true;
}
Frame Device::next() {
    if(!streaming)throw std::runtime_error("Stream is not active");
    if(!packedFrames.empty()) {
        auto f=std::move(packedFrames.front());packedFrames.pop_front();continuity.accept(f);return f;
    }
    auto w=take(44);
    if(sampleRate>=250000) {
        if(std::memcmp(w.data(),sampleRate==500000?"LIP3":"LIP2",4)||w[4]!=1||crc32(w.data(),40)!=le(w.data()+40)||
           (w[5]!=1&&w[5]!=2)||le(w.data()+24)>6016)throw std::runtime_error("Invalid packed binary header");
        auto p=take(le(w.data()+24));w.insert(w.end(),p.begin(),p.end());
        auto decoded=decodePacked(w);
        for(auto& f:decoded)packedFrames.push_back(std::move(f));
        auto f=std::move(packedFrames.front());packedFrames.pop_front();
        continuity.accept(f);if(f.end)streaming=false;return f;
    }
    // Validate the fixed header before using any advertised payload size.
    if(std::memcmp(w.data(),"LIQ1",4)||w[4]!=1||crc32(w.data(),40)!=le(w.data()+40)||
       (w[5]!=1&&w[5]!=2)||le(w.data()+24)!=(w[5]==1?750u:36u))throw std::runtime_error("Invalid binary header");
    auto p=take(le(w.data()+24));w.insert(w.end(),p.begin(),p.end());auto f=decode(w,sampleRate);
    continuity.accept(f);if(f.end)streaming=false;return f;
}
void Device::cancel() {if(streaming&&!cancelled){send(std::string(1,3));cancelled=true;}}
void Device::standby() {
    if(streaming)throw std::runtime_error("Drain stream before standby");
    configured=false;
    if(initialized){request("reset 1500");request("speed 500000");initialized=false;}
}
void Device::installPackedRing() {
    // This firmware is deliberately bound to the module whose application
    // extension was installed and verified. Never call unknown flash on another.
    auto uid=info.value("packed_radio_uid",std::string());
    if(uid.size()!=16 || readCommand("0125",8)!=unhex(uid))
        throw std::runtime_error("Packing patch was not verified on this LR1121 module");
    uint32_t callback=memory(0x8004ac,1)[0];
    masked(0x80f004,0xffffffff,callback);masked(0x80f008,0xffffffff,0);
    masked(0x80f078,0xffffffff,0);
    masked(0x80f000,0xffffffff,0);masked(0x8004ac,0xffffffff,0x94510);
    masked(0x8004bc,2,2);
    if(readCommand("0101",4)!=std::vector<uint8_t>({0x22,3,1,3}) ||
       memory(0x80f000,1)[0]!=0x49515033 || memory(0x8004ac,1)[0]!=callback)
        throw std::runtime_error("LR1121 packing extension self-test failed");
    if(sampleRate==500000 && memory(0x80f078,1)[0]!=0x46494631)
        throw std::runtime_error("500 kS/s FIFO extension is missing; install the matching LR1121 application");
    auto gp=memory(0x800498,6),state=memory(0x800534,3),original=memory(0x800a00,8);
    const std::vector<uint32_t> expected{0xbfe0ffa7,0x901004,0x800324,0x800a10,0xbfe0ffa7,0x901004,0x800324,0x800a00};
    if((gp[1]>>16)!=0x0104 || ((state[1]>>16)&255)!=0 || original!=expected)
        throw std::runtime_error("RX DMA layout differs; no packed ring installed");
    masked(0xf20014,0x30000010,0x30000000);
    std::vector<uint32_t> ring;
    for(unsigned i=0;i<16;i++) {
        ring.insert(ring.end(),{0x9fe3e7a7,0x901004,0x806000+i*2048+996,descriptor((i+1)%16)});
        writeCommand("0105"+hex(0x806000+i*2048)+"00000000");
    }
    std::string cmd="01050080e000";for(auto v:ring)cmd+=hex(v);writeCommand(cmd);
    if(memory(0x80e000,64)!=ring)throw std::runtime_error("Packed ring readback mismatch");
    for(unsigned i=0;i<8;i++)masked(0x800a00+4*i,0xffffffff,ring[i]);
    if(memory(0x800a00,8)!=std::vector<uint32_t>(ring.begin(),ring.begin()+8) || memory(0x800580,1)[0]!=0x800a00)
        throw std::runtime_error("Packed ring head mismatch");
    // Reset acknowledgement/timer state as well as producer state on every retune.
    writeCommand("01050080f000"+std::string(32*8,'0'));
    writeCommand("01050080f100"+std::string(16*8,'0'));
    writeCommand("01050080f00049515033"+hex(callback)+"000000010000000000000000"+
                 hex(sampleRate==500000?8:12)+"00000000"+hex(sampleRate==500000?(256|sampleShift):0));
    masked(0x8004ac,0xffffffff,0x94510);masked(0x8004bc,2,2);
    setup["sample_format"]=sampleRate==500000?"8-bit saturated I/Q, manual shift":"12-bit I/Q with per-block scale";
    setup["protocol"]=sampleRate==500000?"LIP3":"LIP2";
    if(sampleRate==500000)setup["sample_shift"]=sampleShift;
    setup["packing_patch_verified"]=true;
}
}
