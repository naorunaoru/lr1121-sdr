/* SPDX-License-Identifier: GPL-3.0-only */
#include <gui/widgets/fft_signal_info.h>
#include <cassert>
#include <iostream>
#include <vector>

int main() {
    float strength, snr;
    auto measure = [&](const std::vector<float>& line, double span, double center, double bw) {
        strength = snr = 123.0f;
        return ImGui::calculateFFTSignalInfo(line.data(), int(line.size()), span, center, bw, strength, snr);
    };
    // Exact crash case: WFM is wider than the complete source FFT span.
    std::vector<float> fft(8192, -100.0f);
    for (double span : {62500.0, 125000.0}) {
        assert(!measure(fft, span, 0, 150000));
        assert(strength == 0 && snr == 0);
        assert(!measure(fft, span, 0, span));
        assert(strength == 0 && snr == 0);
    }
    // Known peak and noise level preserve ordinary SNR measurement.
    fft[4096] = -40;
    assert(measure(fft,125000,0,12500));
    assert(strength == -40 && snr == 60);
    fft[4096] = -100;
    fft.back() = -30;
    assert(measure(fft,125000,60000,12500));
    assert(strength == -30 && snr == 70); // clipped right edge, left noise remains
    fft.back() = -100;
    fft.front() = -20;
    assert(measure(fft,125000,-60000,12500));
    assert(strength == -20 && snr == 80); // clipped left edge
    for (double center : {-1e300, -100000.0, 100000.0, 1e300}) {
        assert(!measure(fft,125000,center,12500));
        assert(strength == 0 && snr == 0);
    }
    for (double bad : {0.0, -1.0, double(INFINITY), double(NAN)}) {
        assert(!measure(fft,bad,0,12500));
        assert(!measure(fft,125000,0,bad));
    }
    assert(!measure(fft,125000,NAN,12500));
    assert(!ImGui::calculateFFTSignalInfo(nullptr,8192,125000,0,12500,strength,snr));
    assert(!measure({},125000,0,12500));
    // Edge, sub-bin, exact-full-span and oversized channels across FFT sizes.
    unsigned cases = 0;
    for (int size : {1,2,3,16,8192,65536}) {
        std::vector<float> line(size,-100);
        for (double span : {62500.0,125000.0})
            for (double bw : {.001,100.0,12500.0,62500.0,125000.0,150000.0,1e300})
                for (int step=-12;step<=12;++step) {
                    bool valid = measure(line,span,step*span/16,bw);
                    assert(std::isfinite(strength) && std::isfinite(snr));
                    if (valid) { assert(strength == -100 && snr == 0); }
                    else { assert(strength == 0 && snr == 0); }
                    ++cases;
                }
    }
    std::fill(fft.begin(),fft.end(),-INFINITY);
    assert(!measure(fft,125000,0,12500));
    assert(std::isfinite(strength) && std::isfinite(snr));
    std::cout << "Crash regression, known SNR, both clipped edges, invalid inputs and " << cases << " span/size cases passed\n";
}
