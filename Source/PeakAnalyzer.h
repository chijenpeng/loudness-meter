#pragma once

#include <array>
#include <cstddef>

namespace loudness {

// Audio-thread-owned peak analysis. prepare() is called while processing is stopped.
// process() and finishSegment() are allocation-free and bounded; the processor
// publishes Result through its own race-free UI bridge.
class PeakAnalyzer final {
public:
    static constexpr int maximumChannels = 2;
    static constexpr int maximumFactor = 8;
    static constexpr int tapsPerPhase = 65;
    static constexpr int interpolationDelay = tapsPerPhase / 2;
    static constexpr int boundedTail = tapsPerPhase - 1;

    struct ChannelResult {
        double samplePeak = 0;
        double truePeak = 0;
        bool samplePeakValid = false;
        bool truePeakValid = false;
        bool invalidInput = false;
    };
    struct Result {
        std::array<ChannelResult, maximumChannels> channels{};
        std::size_t capturedFrames = 0; // excludes TP-only tail padding
        bool supportedRate = false;
    };

    bool prepare(int sampleRate, int channels) noexcept;
    void reset() noexcept;          // discard local FIR state and all holds
    void finishSegment() noexcept;  // drain one bounded TP-only zero tail, retain holds

    template <typename Sample>
    void process(const Sample* const* input, std::size_t frames) noexcept;

    const Result& result() const noexcept { return result_; }
    int oversamplingFactor() const noexcept { return factor_; }
    static double toDecibels(double amplitude) noexcept;
    // Test-only: disables the lossless quiet-window skip so tests can prove equivalence.
    void setQuietSkipForTests(bool enabled) noexcept { quietSkip_ = enabled; }

private:
    static bool isSupportedRate(int sampleRate) noexcept;
    void makeCoefficients() noexcept;
    void clearLocal() noexcept;
    void processFrame(const double* samples, bool programmeFrame) noexcept;
    void invalidateChannel(int channel) noexcept;

    int rate_ = 0;
    int channels_ = 0;
    int factor_ = 0;
    std::array<std::array<double, tapsPerPhase>, maximumFactor> coefficients_{};
    // Every ring value is written twice. The second copy makes the newest 65-sample window
    // contiguous in reverse order, avoiding a modulo operation for every FIR tap.
    std::array<std::array<double, tapsPerPhase * 2>, maximumChannels> history_{};
    // Lossless skip: l1Max_ is the largest sum of |h| over phases 1..factor-1. A reconstructed
    // value is at most l1Max_ times the largest |x| in the 65-sample window, so while every
    // window sample is <= hold / l1Max_ no phase can raise the true-peak hold.
    double l1Max_ = 0;
    std::array<int, maximumChannels> quietRun_{}; // consecutive newest samples under threshold
    bool quietSkip_ = true;
    std::size_t writeIndex_ = 0;
    bool segmentActive_ = false;
    Result result_{};
};

extern template void PeakAnalyzer::process<float>(const float* const*, std::size_t) noexcept;
extern template void PeakAnalyzer::process<double>(const double* const*, std::size_t) noexcept;

} // namespace loudness
