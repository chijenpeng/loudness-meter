#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace loudness {

// The producer is process()/reset()/discontinuity() on one audio thread. drain()/summary()
// belong to one analysis thread. prepare() is called only while both are stopped.
// No concurrent caller may copy a worker-owned Summary; the processor publishes it
// through its own race-free UI bridge. Producer status fields are atomics.
class Core final {
public:
    static constexpr std::size_t maxBlocks = 864000; // 24 h at 10 Hz
    static constexpr std::size_t maxHistory = 6000;  // last 10 min
    static constexpr std::size_t defaultQueue = 256;
    struct Capacities {
        std::size_t blocks = maxBlocks;
        std::size_t history = maxHistory;
        std::size_t queue = defaultQueue;
        std::uint64_t samples = 0; // zero means rate * 24 h
    };
    struct Point {
        std::uint64_t generation = 0;
        std::uint64_t endSample = 0; // captured samples, half-open window end
        double momentaryPower = 0;
        double shortPower = 0;
        bool momentaryValid = false;
        bool shortValid = false;
        bool gapBefore = false; // first point after a segment discontinuity
        std::uint64_t segment = 0;
    };
    struct Summary {
        std::uint64_t generation = 0;
        std::uint64_t capturedSamples = 0;
        std::uint64_t lastEndSample = 0;
        double momentary = 0, shortTerm = 0, integrated = 0, range = 0;
        double momentaryMax = 0, shortMax = 0;
        bool momentaryValid = false, shortValid = false, integratedValid = false;
        bool rangeValid = false, momentaryMaxValid = false, shortMaxValid = false;
        bool lraUnstable = true;
        bool invalidInput = false, analysisOverrun = false, statisticsOverflow = false;
        bool captureLimit = false, unsupportedRate = false, discontinuity = false;
        std::size_t momentaryBlocks = 0, shortBlocks = 0;
    };

    Core() = default;
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;
    bool prepare(int sampleRate, int channels, Capacities capacities);
    bool prepare(int sampleRate, int channels) { return prepare(sampleRate, channels, {}); }
    void reset() noexcept;              // audio boundary; new generation
    void discontinuity() noexcept;      // audio boundary; retain completed statistics
    void discardPartial() noexcept;     // audio boundary; retain completed statistics/readings
    template <typename Sample> void process(const Sample* const* input, std::size_t frames) noexcept;
    // False means reset publication changed during this bounded drain; worker state is retained,
    // but callers must keep the last coherent external snapshot and drain again.
    bool drain(std::size_t maximumPoints = defaultQueue); // analysis thread, bounded by caller
    const Summary& summary() const noexcept { return summary_; } // analysis thread only
    const Point* historyData() const noexcept { return history_.data(); } // ring, analysis thread
    std::size_t historyCount() const noexcept { return historyCount_; }
    std::size_t historyNextIndex() const noexcept { return historyWrite_; }
    std::size_t allocatedBytes() const noexcept;
    // Test hook: true when every K-weighting biquad state is exactly zero.
    bool filterStateIsZero() const noexcept;
    std::size_t availableFrames(std::size_t requested) const noexcept;
    bool captureLimitReached() const noexcept { return limit_.load(std::memory_order_acquire); }
    std::uint64_t generation() const noexcept { return generation_.load(std::memory_order_acquire); }
    static double toLufs(double power) noexcept;
    static double toPower(double lufs) noexcept;
    static std::array<double, 5> shelfCoefficients(int rate) noexcept;
    static std::array<double, 5> highPassCoefficients(int rate) noexcept;
    // Worker-only, for exact independent energy fixtures and small-capacity boundary tests.
    // These call the same production histogram/statistics path as queued audio points.
    bool addMomentaryPower(double power) noexcept;
    bool addShortPower(double power) noexcept;
    void recomputeStatistics();
private:
    struct Biquad {
        std::array<double, 5> c{};
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        double apply(double x) noexcept;
        void clear() noexcept { x1 = x2 = y1 = y2 = 0; }
    };
    struct Bin { std::uint64_t count = 0; double sum = 0; };
    struct Stats {
        std::vector<double> raw;
        std::vector<std::uint16_t> rawBin; // bin index of raw[i], same binFor as bins
        std::vector<Bin> bins; // allocated to 14400 entries in prepare, never on audio thread
        std::size_t size = 0;
        double absoluteSum = 0;
        std::uint64_t absoluteCount = 0;
        void clear() noexcept;
        bool add(double power, bool inclusive) noexcept;
        struct Aggregate { std::uint64_t count = 0; double sum = 0; };
        Aggregate above(double threshold, bool inclusive) const noexcept;
        double rankPower(std::size_t rank, double threshold, std::vector<double>& workspace) const;
    };
    void resetLocal() noexcept;
    void finishHop() noexcept;
    void invalidateInput() noexcept;
    void ingest(const Point& point);
    void clearWorker(std::uint64_t generation);
    static bool supportedRate(int rate) noexcept;
    static int binFor(double power) noexcept;

    int rate_ = 0, channels_ = 0, hopSamples_ = 0;
    Capacities capacities_{};
    std::uint64_t sampleLimit_ = 0, captured_ = 0, localSamples_ = 0;
    std::atomic<std::uint64_t> capturedPublished_{0};
    std::uint64_t generationLocal_ = 1;
    std::atomic<std::uint64_t> generation_{1};
    std::atomic<std::uint64_t> resetSequence_{0}; // odd while audio-thread reset is publishing
    std::uint64_t segmentLocal_ = 0;
    std::atomic<std::uint64_t> segment_{0};
    std::atomic<bool> invalid_{false}, overrun_{false}, limit_{false}, discontinuity_{false};
    std::array<std::array<Biquad, 2>, 2> filters_{};
    std::array<double, 30> hopPowers_{};
    int hopFill_ = 0, hopIndex_ = 0, hopCount_ = 0;
    double hopEnergy_ = 0;
    std::unique_ptr<Point[]> queue_;
    std::atomic<std::size_t> write_{0}, read_{0}; // monotone SPSC indices
    Stats integrated_, shortStats_;
    std::vector<double> rankWorkspace_;
    std::vector<Point> history_;
    std::size_t historyWrite_ = 0, historyCount_ = 0;
    std::uint64_t workerSegment_ = 0;
    Summary summary_{};
};

extern template void Core::process<float>(const float* const*, std::size_t) noexcept;
extern template void Core::process<double>(const double* const*, std::size_t) noexcept;

} // namespace loudness
