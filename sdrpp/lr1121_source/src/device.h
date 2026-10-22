#pragma once
#include <array>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <json.hpp>

namespace lr1121 {
using json = nlohmann::json;
// Auto follows the firmware split at 1.5 GHz; LF/HF force the receiver path.
extern const std::array<const char*,3> inputs;
void validateSettings(uint32_t frequency, uint32_t rate, unsigned input);
struct GainSettings {
    bool agc=true;
    unsigned step=10, boostLF=0, boostHF=0;
};
void validateGainSettings(const GainSettings& gain);
struct RetuneProfile {
    uint32_t frequency=0, rate=0;
    unsigned input=0;
    GainSettings gain;
    bool wide=false;
};
// Anchor is the last full configuration, never the last small frequency step.
bool canReuseConfiguration(const RetuneProfile& anchor,const RetuneProfile& next);
std::vector<std::string> ports();
uint32_t crc32(const uint8_t* data, size_t size);
struct Frame {
    uint32_t id=0, sequence=0, sampleIndex=0, rate=0, timestamp=0;
    uint16_t flags=0, limitComponents=0;
    bool end=false;
    std::vector<uint8_t> payload;
    std::array<std::complex<float>,125> samples{};
};
Frame decode(const std::vector<uint8_t>& wire, uint32_t rate);
std::vector<Frame> decodePacked(const std::vector<uint8_t>& wire);
class Continuity {
public:
    uint32_t id=0, rate=0, expected=0;
    uint64_t frames=0;
    void accept(const Frame& frame);
};
// One thread owns each device, including setup, cancellation, and teardown.
class Device {
public:
    explicit Device(const std::string& port);
    ~Device();
    Device(const Device&)=delete;
    Device& operator=(const Device&)=delete;
    void configure(uint32_t frequency, uint32_t rate, unsigned input, const GainSettings& gain={}, bool wideFilter=false, unsigned sampleShift=15);
    void begin(uint32_t blocks=0);
    Frame next();
    void cancel();
    void standby();
    json info;
    Continuity continuity;
    json setup;
private:
    int fd=-1;
    uint32_t requestId=0, sampleRate=125000;
    unsigned sampleShift=15;
    bool streaming=false, cancelled=false, initialized=false;
    bool configured=false;
    RetuneProfile anchor;
    std::vector<uint8_t> buffer;
    std::deque<Frame> packedFrames;
    size_t cursor=0;
    void send(const std::string& data);
    std::vector<uint8_t> take(size_t count, int timeoutMs=3000);
    json request(const std::string& command);
    std::vector<uint8_t> readCommand(const std::string& hex, size_t size);
    void writeCommand(const std::string& hex, int mode=-1);
    std::vector<uint32_t> memory(uint32_t address, size_t words);
    void masked(uint32_t address,uint32_t mask,uint32_t value);
    void installRing();
    void installPackedRing();
};
}
