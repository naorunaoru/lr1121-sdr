/* SPDX-License-Identifier: GPL-3.0-only */
#include "device.h"
#include <module.h>
#include <core.h>
#include <signal_path/signal_path.h>
#include <gui/smgui.h>
#include <gui/tuner.h>
#include <utils/flog.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cmath>
#include <cstdio>

SDRPP_MOD_INFO{"lr1121_source","Pico USB / LR1121 I/Q source","LR1121 experimental integration",1,4,0,1};
static ConfigManager config;
class LR1121Source : public ModuleManager::Instance {
public:
    explicit LR1121Source(std::string n):name(std::move(n)) {
        config.acquire();
        rateId=std::clamp(config.conf.value("rateId",0),0,3);
        inputId=std::clamp(config.conf.value("inputId",0),0,2);
        filterId=std::clamp(config.conf.value("filterId",0),0,1);
        if(rateId>=2)filterId=1;
        manualShift=std::clamp(config.conf.value("sampleShift",15),0,16);selectedShift=manualShift;
        gainDB=std::clamp(config.conf.value("gainDB",36),0,60);
        hardwareAgc=config.conf.value("hardwareAgc",true);
        gainStep=std::clamp(config.conf.value("gainStep",10),4,13);
        boostLF=std::clamp(config.conf.value("boostLF",0),0,7);
        boostHF=std::clamp(config.conf.value("boostHF",0),0,3);
        auto p=config.conf.value("port",std::string());std::snprintf(port,sizeof(port),"%s",p.c_str());
        config.release();gain=std::pow(10.0f,gainDB/20.0f);desired=2440000000u;publishHardware();
        if(!*port)refresh();
        handler.ctx=this;handler.stream=&stream;handler.selectHandler=selected;handler.deselectHandler=deselected;
        handler.menuHandler=menu;handler.startHandler=start;handler.stopHandler=stop;handler.tuneHandler=tune;
        sigpath::sourceManager.registerSource("LR1121",&handler);
    }
    ~LR1121Source() {stop(this);sigpath::sourceManager.unregisterSource("LR1121");}
    void postInit() override {}
    void enable() override {enabled=true;}
    void disable() override {stop(this);enabled=false;}
    bool isEnabled() override {return enabled;}
private:
    std::string name;
    char port[512]{};
    int rateId=0,inputId=0,filterId=0,gainDB=36;
    int gainStep=10,boostLF=0,boostHF=0,manualShift=15;
    bool hardwareAgc=true;
    bool enabled=true,running=false;
    SourceManager::SourceHandler handler{};
    dsp::stream<dsp::complex_t> stream;
    std::atomic<bool> stopping{false},failed{false};
    std::atomic<uint32_t> desired{2440000000u};
    std::atomic<float> gain{1},limitPercent{0};
    std::atomic<unsigned> selectedShift{15};
    std::atomic<uint32_t> hardware{0};
    std::atomic<uint64_t> received{0},retunes{0},highWater{0};
    std::thread reader,feeder;
    std::mutex mutex;
    std::condition_variable cv;
    struct Block {uint64_t epoch;std::array<std::complex<float>,125> samples;};
    std::deque<Block> queue;
    uint64_t epoch=0;
    std::string status="Ready. Press Play to receive.",error;
    uint32_t rate() const {return rateId==3?500000:rateId==2?250000:rateId==0?125000:62500;}
    void publishHardware() {hardware=uint32_t(hardwareAgc)|(uint32_t(gainStep)<<4)|(uint32_t(boostLF)<<8)|(uint32_t(boostHF)<<12);}
    static lr1121::GainSettings unpackHardware(uint32_t value) {
        return {bool(value&1),(value>>4)&15,(value>>8)&7,(value>>12)&3};
    }
    void message(const std::string& s) {std::lock_guard<std::mutex> lock(mutex);status=s;}
    void save() {
        config.acquire();config.conf["rateId"]=rateId;config.conf["inputId"]=inputId;config.conf.erase("bandId");
        config.conf["gainDB"]=gainDB;config.conf["port"]=std::string(port);
        config.conf["filterId"]=filterId;config.conf["sampleShift"]=manualShift;
        config.conf["hardwareAgc"]=hardwareAgc;config.conf["gainStep"]=gainStep;
        config.conf["boostLF"]=boostLF;config.conf["boostHF"]=boostHF;config.release(true);
    }
    void refresh() {
        auto p=lr1121::ports();
        if(p.size()==1){std::snprintf(port,sizeof(port),"%s",p[0].c_str());message("Bridge found. Press Play to receive.");}
        else message(p.empty()?"No bridge found. Connect the Pico, then Refresh.":"Multiple bridges: enter the desired port.");
    }
    static void selected(void* ctx) {auto s=(LR1121Source*)ctx;core::setInputSampleRate(s->rate());}
    static void deselected(void* ctx) {stop(ctx);}
    static void start(void* ctx) {
        auto s=(LR1121Source*)ctx;if(s->running)return;
        {std::lock_guard<std::mutex> lock(s->mutex);s->queue.clear();s->error.clear();s->status="Connecting...";s->epoch=0;}
        s->stopping=false;s->failed=false;s->received=0;s->retunes=0;s->highWater=0;s->running=true;
        s->stream.clearWriteStop();
        try {s->feeder=std::thread(&LR1121Source::feed,s);s->reader=std::thread(&LR1121Source::read,s);}
        catch(const std::exception& e){s->fail(e.what());stop(s);}
    }
    static void stop(void* ctx) {
        auto s=(LR1121Source*)ctx;if(!s->running)return;
        s->stopping=true;s->stream.stopWriter();s->cv.notify_all();
        if(s->reader.joinable())s->reader.join();if(s->feeder.joinable())s->feeder.join();
        s->stream.clearWriteStop();s->running=false;
        s->message(s->failed?"Stopped. Adjust settings, then press Play.":"Stopped. Receiver reset to standby.");
    }
    static void tune(double f,void* ctx) {
        auto s=(LR1121Source*)ctx;
        // Invalid frequencies terminate capture with a visible error, rather than
        // letting SDR++ label samples with a frequency the receiver did not use.
        s->desired=(std::isfinite(f)&&f>=0&&f<=4294967295.0)?uint32_t(std::llround(f)):0;
    }
    void fail(const std::string& text) {
        {std::lock_guard<std::mutex> lock(mutex);error=text;status="Capture stopped; press Stop before retrying.";}
        failed=true;stopping=true;stream.stopWriter();cv.notify_all();flog::error("LR1121: {}",text);
    }
    void read() {
        try {
            lr1121::Device device(port);
            while(!stopping) {
                uint32_t frequency=desired;
                uint32_t selectedHardware=hardware;
                unsigned shift=selectedShift;limitPercent=0;
                lr1121::validateSettings(frequency,rate(),unsigned(inputId));
                message("Tuning receiver...");device.configure(frequency,rate(),unsigned(inputId),unpackHardware(selectedHardware),filterId==1,shift);
                flog::info("LR1121: tuned {} Hz using {} setup in {} ms",frequency,
                    device.setup.at("fast_retune").get<bool>()?"cached":"full",
                    int(std::lround(device.setup.at("configure_ms").get<double>())));
                if(stopping)break;
                // Tuning may have continued while setup was running. Configure
                // the newest request before publishing samples for this epoch.
                if(desired!=frequency || hardware!=selectedHardware || selectedShift!=shift)continue;
                uint64_t currentEpoch;
                {std::lock_guard<std::mutex> lock(mutex);queue.clear();currentEpoch=++epoch;}
                device.begin();message("Receiving "+std::to_string(frequency)+" Hz");
                bool cancelled=false;uint64_t limits=0,components=0;
                while(true) {
                    if(!cancelled&&(stopping||desired!=frequency||hardware!=selectedHardware || selectedShift!=shift)){
                        device.cancel();cancelled=true;
                        std::lock_guard<std::mutex> lock(mutex);queue.clear();++epoch;
                    }
                    auto f=device.next();
                    if(f.end) {
                        if(f.flags!=(cancelled?4:0))throw std::runtime_error("Bridge stopped: flags="+std::to_string(f.flags)+" (capture discontinuity)");
                        if(!cancelled)throw std::runtime_error("Continuous stream ended unexpectedly");
                        break;
                    }
                    received+=125;
                    limits+=f.limitComponents;components+=250;
                    if(components>=rate()/2){limitPercent=float(100.0*limits/components);limits=0;components=0;}
                    if(cancelled)continue;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        if(queue.size()>=256)throw std::runtime_error("DSP queue overflow; capture stopped");
                        queue.push_back({currentEpoch,f.samples});
                        highWater=std::max<uint64_t>(highWater.load(),queue.size());
                    }
                    cv.notify_one();
                }
                if(!stopping){++retunes;message("Applying receiver settings...");}
            }
            device.standby();
        } catch(const std::exception& e) {fail(e.what());}
    }
    void feed() {
        size_t count=0;uint64_t currentEpoch=0;
        while(!stopping) {
            Block b;
            {std::unique_lock<std::mutex> lock(mutex);cv.wait(lock,[&]{return stopping||!queue.empty();});
                if(stopping)break;b=std::move(queue.front());queue.pop_front();
                if(b.epoch!=epoch)continue;
            }
            if(b.epoch!=currentEpoch){currentEpoch=b.epoch;count=0;}
            float scale=gain;
            for(const auto& z:b.samples)stream.writeBuf[count++]={z.real()*scale,z.imag()*scale};
            if(count==1000){if(!stream.swap(int(count)))break;count=0;}
        }
    }
    static void menu(void* ctx) {
        auto s=(LR1121Source*)ctx;
        if(s->running)SmGui::BeginDisabled();
        SmGui::LeftLabel("USB port");SmGui::FillWidth();
        if(SmGui::InputText("##lr1121_port",s->port,sizeof(s->port)))s->save();
        if(SmGui::Button("Refresh bridge")){s->refresh();s->save();}
        SmGui::LeftLabel("Sample rate");SmGui::FillWidth();
        if(SmGui::Combo("##lr1121_rate",&s->rateId,"125 kS/s\0" "62.5 kS/s\0" "250 kS/s (12-bit)\0" "500 kS/s (8-bit)\0")){
            if(s->rateId>=2)s->filterId=1;core::setInputSampleRate(s->rate());s->save();
        }
        SmGui::LeftLabel("RF input");SmGui::FillWidth();
        if(SmGui::Combo("##lr1121_input",&s->inputId,"Auto\0LF\0HF\0"))s->save();
        SmGui::LeftLabel("RX filter");SmGui::FillWidth();
        if(s->rateId>=2)SmGui::BeginDisabled();
        if(SmGui::Combo("##lr1121_filter",&s->filterId,"Narrow\0Wide (experimental)\0"))s->save();
        if(s->rateId>=2)SmGui::EndDisabled();
        if(s->running)SmGui::EndDisabled();
        bool changed=SmGui::Checkbox("Hardware AGC",&s->hardwareAgc);
        if(s->hardwareAgc)SmGui::BeginDisabled();
        SmGui::LeftLabel("Hardware gain step");SmGui::FillWidth();
        changed=SmGui::SliderInt("##lr1121_hw_gain",&s->gainStep,4,13)||changed;
        if(s->hardwareAgc)SmGui::EndDisabled();
        bool hf=s->inputId==2 || (s->inputId==0 && s->desired>=1500000000u);
        SmGui::LeftLabel(hf?"LNA boost (HF)":"LNA boost (LF)");SmGui::FillWidth();
        changed=SmGui::SliderInt("##lr1121_boost",hf?&s->boostHF:&s->boostLF,0,hf?3:7)||changed;
        if(changed){s->publishHardware();s->save();}
        if(s->rateId==3) {
            SmGui::LeftLabel("8-bit shift");SmGui::FillWidth();
            if(SmGui::SliderInt("##lr1121_shift",&s->manualShift,0,16)){
                s->selectedShift=s->manualShift;s->save();
            }
            SmGui::Text("Higher shift: more headroom, less precision.");
            SmGui::Text("Shift changes briefly restart RX.");
            char meter[96];std::snprintf(meter,sizeof(meter),"8-bit values at limits: %.3f%%",double(s->limitPercent.load()));
            SmGui::Text(meter);
            SmGui::Text("If clipping: raise shift or lower HW gain.");
        }
        SmGui::LeftLabel("Digital gain (dB)");SmGui::FillWidth();
        if(SmGui::SliderInt("##lr1121_gain",&s->gainDB,0,60)){s->gain=std::pow(10.0f,s->gainDB/20.0f);s->save();}
        SmGui::Text("Stop to change rate, input or filter.");
        SmGui::Text("Hardware gain changes briefly restart RX.");
        SmGui::Text(s->inputId==0?"Auto: firmware selects LF/HF.":"Manual input selection is experimental.");
        SmGui::Text("Extended tuning; sensitivity unverified.");
        SmGui::Text("Usable passband is narrower than Fs.");
        if(s->rateId==2)SmGui::Text("250 kS/s: about 86 kHz at -3 dB.");
        if(s->rateId==3)SmGui::Text("500 kS/s: about 174 kHz at -3 dB.");
        SmGui::Text("Experimental I/Q; receive only.");
        std::string status,error;
        {std::lock_guard<std::mutex> lock(s->mutex);status=s->status;error=s->error;}
        SmGui::Text(status.c_str());
        if(!error.empty())SmGui::TextColored(ImVec4(1,.4f,.3f,1),error.c_str());
        char stats[160];std::snprintf(stats,sizeof(stats),"%.3f M samples | %llu RX restarts",double(s->received)/1e6,(unsigned long long)s->retunes.load());SmGui::Text(stats);
        std::snprintf(stats,sizeof(stats),"Queue peak: %llu/256 frames",(unsigned long long)s->highWater.load());SmGui::Text(stats);
    }
};
MOD_EXPORT void _INIT_(){config.setPath(core::args["root"].s()+"/lr1121_config.json");config.load(json::object());config.enableAutoSave();}
MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name){return new LR1121Source(std::move(name));}
MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance){delete instance;}
MOD_EXPORT void _END_(){config.disableAutoSave();config.save();}
