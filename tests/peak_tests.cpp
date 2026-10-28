#include "PeakAnalyzer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using loudness::PeakAnalyzer;

namespace {
constexpr double pi = 3.141592653589793238462643383279502884;

void check(bool pass, const char* what) {
    if (!pass) throw std::runtime_error(what);
}

void near(double actual, double expected, double tolerance, const char* what) {
    if (!(std::abs(actual - expected) <= tolerance)) {
        std::cerr << what << ": got " << actual << ", expected " << expected << '\n';
        throw std::runtime_error(what);
    }
}

double sinc(double value) {
    if (value == 0) return 1;
    return std::sin(pi * value) / (pi * value);
}

double directReconstruction(const std::vector<double>& samples, double time) {
    long double value = 0;
    for (std::size_t index = 0; index < samples.size(); ++index)
        value += static_cast<long double>(samples[index]) * sinc(time - static_cast<double>(index));
    return static_cast<double>(value);
}

double directReferencePeak(const std::vector<double>& samples) {
    // Independent ideal-sinc reconstruction. Dense scan plus golden-section refinement is much
    // finer than the production phase grid and shares no coefficients or implementation path.
    constexpr int phases = 4096;
    const double begin = -8;
    const double end = static_cast<double>(samples.size()) + 8;
    double bestTime = begin;
    double best = 0;
    for (int step = 0; step <= static_cast<int>((end - begin) * phases); ++step) {
        const double time = begin + static_cast<double>(step) / phases;
        const double value = std::abs(directReconstruction(samples, time));
        if (value > best) { best = value; bestTime = time; }
    }
    double left = bestTime - 1.0 / phases;
    double right = bestTime + 1.0 / phases;
    for (int iteration = 0; iteration < 60; ++iteration) {
        const double first = left + (right - left) * 0.3819660112501051;
        const double second = left + (right - left) * 0.6180339887498949;
        if (std::abs(directReconstruction(samples, first))
            < std::abs(directReconstruction(samples, second))) left = first;
        else right = second;
    }
    return std::max(best, std::abs(directReconstruction(samples, (left + right) * 0.5)));
}

double blackmanHarris(double value) {
    constexpr double radius = PeakAnalyzer::interpolationDelay;
    if (std::abs(value) >= radius) return 0;
    const double angle = pi * value / radius;
    return 0.35875 + 0.48829 * std::cos(angle)
        + 0.14128 * std::cos(2 * angle) + 0.01168 * std::cos(3 * angle);
}

double legacyModuloTruePeak(const std::vector<double>& signal, int rate) {
    const int factor = rate <= 48000 ? 8 : (rate <= 96000 ? 4 : (rate <= 192000 ? 2 : 1));
    std::array<std::array<double, PeakAnalyzer::tapsPerPhase>, PeakAnalyzer::maximumFactor> coefficients{};
    for (int phase = 0; phase < factor; ++phase) {
        double sum = 0;
        const double fraction = static_cast<double>(phase) / factor;
        for (int tap = 0; tap < PeakAnalyzer::tapsPerPhase; ++tap) {
            const double offset = tap - PeakAnalyzer::interpolationDelay + fraction;
            const double value = sinc(offset) * blackmanHarris(offset);
            coefficients[static_cast<std::size_t>(phase)][static_cast<std::size_t>(tap)] = value;
            sum += value;
        }
        for (double& value : coefficients[static_cast<std::size_t>(phase)]) value /= sum;
    }
    std::array<double, PeakAnalyzer::tapsPerPhase> history{};
    std::size_t write = 0;
    double maximum = 0;
    for (std::size_t frame = 0;
         frame < signal.size() + PeakAnalyzer::boundedTail; ++frame) {
        const bool programme = frame < signal.size();
        const double sample = programme ? signal[frame] : 0;
        history[write] = sample;
        if (programme) maximum = std::max(maximum, std::abs(sample));
        for (int phase = 0; phase < factor; ++phase) {
            double reconstructed = 0;
            for (int tap = 0; tap < PeakAnalyzer::tapsPerPhase; ++tap) {
                const auto index = (write + PeakAnalyzer::tapsPerPhase
                                    - static_cast<std::size_t>(tap))
                    % PeakAnalyzer::tapsPerPhase;
                reconstructed += coefficients[static_cast<std::size_t>(phase)]
                                             [static_cast<std::size_t>(tap)] * history[index];
            }
            maximum = std::max(maximum, std::abs(reconstructed));
        }
        write = (write + 1) % PeakAnalyzer::tapsPerPhase;
    }
    return maximum;
}

template <typename Sample>
void processInBlocks(PeakAnalyzer& analyzer, const std::vector<Sample>& left,
                     const std::vector<Sample>* right, std::uint32_t seed) {
    std::size_t offset = 0;
    while (offset < left.size()) {
        seed = seed * 1664525u + 1013904223u;
        const auto count = std::min(left.size() - offset,
                                    static_cast<std::size_t>(1 + seed % 379));
        const Sample* channels[] = {left.data() + offset,
                                    right ? right->data() + offset : nullptr};
        analyzer.process(channels, count);
        offset += count;
    }
}

void samplePeaksAndInvalidity() {
    PeakAnalyzer analyzer;
    check(analyzer.prepare(48000, 2), "stereo prepare");
    const std::vector<double> left{-1.25, 0.5, 0.25, -0.75};
    const std::vector<double> right{0.1, -1.5, 0.2, 0.3};
    const double* input[] = {left.data(), right.data()};
    analyzer.process(input, left.size());
    const auto beforeTail = analyzer.result();
    check(beforeTail.capturedFrames == left.size(), "programme duration counts only input");
    near(beforeTail.channels[0].samplePeak, 1.25, 0, "double left sample peak exact");
    near(beforeTail.channels[1].samplePeak, 1.5, 0, "double right sample peak exact");
    analyzer.finishSegment();
    check(analyzer.result().capturedFrames == left.size(), "tail does not extend duration");

    analyzer.reset();
    std::vector<float> badLeft{0.1f, std::numeric_limits<float>::quiet_NaN(), 2.0f};
    std::vector<float> goodRight{0.25f, -0.75f, 0.5f};
    const float* badInput[] = {badLeft.data(), goodRight.data()};
    analyzer.process(badInput, badLeft.size());
    const auto& invalid = analyzer.result();
    check(invalid.channels[0].invalidInput && !invalid.channels[0].samplePeakValid
              && !invalid.channels[0].truePeakValid,
          "nonfinite invalidates only its channel");
    check(!invalid.channels[1].invalidInput && invalid.channels[1].samplePeakValid
              && invalid.channels[1].truePeakValid,
          "other channel remains valid");
    near(invalid.channels[1].samplePeak, 0.75, 0, "float sample peak exact in double");
    analyzer.reset();
    check(!analyzer.result().channels[0].invalidInput
              && !analyzer.result().channels[0].samplePeakValid,
          "reset clears channel invalidity and holds");
    const std::vector<float> prior{0.5f, -0.25f};
    const float* priorInput[] = {prior.data(), prior.data()}; // analyser is still stereo here
    analyzer.process(priorInput, prior.size());
    check(analyzer.prepare(96000, 1), "rate/layout reprepare");
    check(analyzer.oversamplingFactor() == 4 && analyzer.result().capturedFrames == 0
              && !analyzer.result().channels[0].samplePeakValid
              && !analyzer.result().channels[1].samplePeakValid,
          "rate/layout reprepare starts a fresh peak capture");
    near(PeakAnalyzer::toDecibels(1.5), 20 * std::log10(1.5), 1e-15,
         "sample-peak dB conversion");
}

void ebuTableNineteen() {
    PeakAnalyzer analyzer;
    check(analyzer.prepare(48000, 1), "EBU test 19 prepare");
    constexpr std::size_t frames = 4096;
    std::vector<double> signal(frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const double fade = frame < 256
            ? std::pow(std::sin(0.5 * pi * static_cast<double>(frame) / 256), 2) : 1;
        signal[frame] = 1.41 * fade * std::sin(0.5 * pi * frame + 0.25 * pi);
    }
    const double* input[] = {signal.data()};
    analyzer.process(input, signal.size());
    const auto& result = analyzer.result().channels[0];
    check(result.samplePeak < 1 && result.truePeak > result.samplePeak,
          "fs/4 phase-45 signal has an intersample over");
    const double measured = PeakAnalyzer::toDecibels(result.truePeak);
    constexpr double exact = 2.9843822531075976;
    check(measured >= exact - 0.4 && measured <= exact + 0.2,
          "EBU table 19 +0.2/-0.4 dBTP tolerance");
}

void ratePolicyAndPhaseSweeps(double& worstLow, double& worstHigh) {
    constexpr std::array<int, 9> rates{32000, 44100, 48000, 88200, 96000, 176400, 192000,
                                       352800, 384000};
    constexpr std::array<double, 5> phases{0, 0.17, 0.39, 0.63, 0.91};
    for (int rate : rates) {
        PeakAnalyzer analyzer;
        check(analyzer.prepare(rate, 1), "supported-rate prepare");
        const int expectedFactor = rate <= 48000 ? 8 : (rate <= 96000 ? 4 : (rate <= 192000 ? 2 : 1));
        check(analyzer.result().supportedRate && analyzer.oversamplingFactor() == expectedFactor,
              "declared >=352.8 kHz reconstruction-rate policy");
        const double upper = std::min(20000.0, rate * 0.45);
        const std::array<double, 4> frequencies{997, std::min(20000.0, rate / 16.0),
                                                        std::min(20000.0, rate / 10.0), upper};
        for (double frequency : frequencies) {
            for (double phase : phases) {
                analyzer.reset();
                const std::size_t frames = static_cast<std::size_t>(rate / 5);
                std::vector<float> signal(frames);
                for (std::size_t frame = 0; frame < frames; ++frame) {
                    const double fade = frame < 512
                        ? std::pow(std::sin(0.5 * pi * static_cast<double>(frame) / 512), 2) : 1;
                    signal[frame] = static_cast<float>(1.2 * fade
                        * std::sin(2 * pi * frequency * frame / rate + phase));
                }
                const float* input[] = {signal.data()};
                analyzer.process(input, signal.size());
                const double error = PeakAnalyzer::toDecibels(
                    analyzer.result().channels[0].truePeak / 1.2);
                worstLow = std::min(worstLow, error);
                worstHigh = std::max(worstHigh, error);
                check(error >= -0.2 && error <= 0.1,
                      "phase-swept declared-band sine reconstruction error");
            }
        }
    }

    PeakAnalyzer unsupported;
    check(unsupported.prepare(22050, 1), "unsupported rate still prepares safely");
    const std::vector<float> signal(64, 1);
    const float* input[] = {signal.data()};
    unsupported.process(input, signal.size());
    check(!unsupported.result().supportedRate && unsupported.oversamplingFactor() == 0
              && unsupported.result().capturedFrames == 0
              && !unsupported.result().channels[0].samplePeakValid,
          "unsupported rate produces no peak analysis");
}

void directTransientAndTail() {
    const std::vector<double> transient{1, -1};
    const double reference = directReferencePeak(transient);
    PeakAnalyzer analyzer;
    check(analyzer.prepare(48000, 1), "transient prepare");
    const double* input[] = {transient.data()};
    analyzer.process(input, transient.size());
    near(analyzer.result().channels[0].truePeak, 1, 1e-14,
         "delayed reconstruction is pending before EOF tail");
    analyzer.finishSegment();
    const double actual = analyzer.result().channels[0].truePeak;
    check(actual > 1.03, "EOF tail finds post-end intersample over");
    const double error = PeakAnalyzer::toDecibels(actual / reference);
    check(error >= -0.2 && error <= 0.1, "finite transient versus direct ideal-sinc reference");
    const auto duration = analyzer.result().capturedFrames;
    analyzer.finishSegment();
    near(analyzer.result().channels[0].truePeak, actual, 0, "tail drain is idempotent");
    check(analyzer.result().capturedFrames == duration, "repeated tail does not count samples");

    const std::vector<double> quiet{0.1, -0.1, 0.1};
    const double* quietInput[] = {quiet.data()};
    analyzer.process(quietInput, quiet.size());
    analyzer.finishSegment();
    near(analyzer.result().channels[0].truePeak, actual, 0,
         "new segment retains whole-capture maximum hold");
}

void partitionInvariance() {
    constexpr int rate = 48000;
    constexpr std::size_t frames = 12000;
    std::vector<double> left(frames), right(frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        left[frame] = 0.8 * std::sin(2 * pi * 19123 * frame / rate + 0.31);
        right[frame] = 1.1 * std::sin(2 * pi * 17231 * frame / rate + 0.73);
    }
    left[3999] += 0.7;
    right[4000] -= 0.6;
    PeakAnalyzer whole, partitioned;
    check(whole.prepare(rate, 2) && partitioned.prepare(rate, 2), "partition prepare");
    const double* input[] = {left.data(), right.data()};
    whole.process(input, frames);
    processInBlocks(partitioned, left, &right, 0x493ad25bu);
    whole.finishSegment();
    partitioned.finishSegment();
    for (int channel = 0; channel < 2; ++channel) {
        const auto index = static_cast<std::size_t>(channel);
        near(whole.result().channels[index].samplePeak,
             partitioned.result().channels[index].samplePeak, 0,
             "sample peak block partition invariant");
        near(whole.result().channels[index].truePeak,
             partitioned.result().channels[index].truePeak, 0,
             "true peak block partition invariant");
    }
    check(whole.result().capturedFrames == frames
              && partitioned.result().capturedFrames == frames,
          "partitioned duration invariant");
}

void legacyModuloEquivalence() {
    constexpr std::array<int, 9> rates{32000,44100,48000,88200,96000,176400,192000,352800,384000};
    for (int rate : rates) {
        std::vector<double> signal(5000);
        std::uint32_t rng=0x31a4f27du;
        for (std::size_t frame=0;frame<signal.size();++frame) {
            rng=rng*1664525u+1013904223u;
            const double noise=(static_cast<double>(rng)/4294967295.0-.5)*.08;
            signal[frame]=.78*std::sin(2*pi*std::min(19000.0,rate*.19)*frame/rate+.31)
                +.17*std::sin(2*pi*997*frame/rate+.73)+noise;
        }
        PeakAnalyzer optimized;
        check(optimized.prepare(rate,1),"optimized legacy-equivalence prepare");
        const double* input[]={signal.data()};
        optimized.process(input,signal.size()); optimized.finishSegment();
        const double legacy=legacyModuloTruePeak(signal,rate);
        near(optimized.result().channels[0].truePeak,legacy,2e-14,
             "duplicated-ring TP matches legacy modulo implementation");
    }
}

// Deterministic programme for the quiet-window skip equivalence test and the timing loop.
// Uses raw engine output only, so it is identical on every platform.
struct Programme {
    std::vector<std::vector<double>> channels; // [channel][frame]
    std::vector<std::size_t> sectionEnds;
};

Programme makeProgramme(int rate, int channelCount) {
    Programme programme;
    programme.channels.assign(static_cast<std::size_t>(channelCount), {});
    std::uint32_t rng = 0x2545f491u ^ static_cast<std::uint32_t>(rate);
    auto noise = [&rng]() {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<double>(rng >> 8) / 8388608.0 - 1.0; // [-1, 1)
    };
    const std::size_t second = static_cast<std::size_t>(rate);
    auto section = [&](std::size_t frames, auto&& generate) {
        for (std::size_t frame = 0; frame < frames; ++frame)
            for (int channel = 0; channel < channelCount; ++channel) {
                const double value = generate(frame, channel);
                programme.channels[static_cast<std::size_t>(channel)].push_back(value);
            }
        programme.sectionEnds.push_back(programme.channels[0].size());
    };
    const double w = 2 * pi / rate;
    section(second / 20, [&](std::size_t, int) { return 1e-4 * noise(); });      // quiet before any hold
    section(second / 10, [&](std::size_t, int) { return 0.9 * noise(); });      // loud burst
    section(second / 2, [&](std::size_t, int) { return 1e-3 * noise(); });       // quiet after hold
    section(second / 4, [&](std::size_t f, int c) {                              // near Nyquist
        return 0.5 * std::sin(w * rate * 0.45 * f + c); });
    section(second / 4, [&](std::size_t, int) { return 0.0; });                  // digital silence
    section(second / 4, [&](std::size_t f, int c) {                              // isolated spikes in quiet
        return (f % 997 == 13 ? 0.7 * (c ? -1 : 1) : 0.0) + 2e-3 * noise(); });
    section(second / 4, [&](std::size_t f, int c) {                              // quiet low sine
        return 0.05 * std::sin(w * 997 * f + 0.3 * c); });
    section(second / 20, [&](std::size_t, int) { return 1.4 * noise(); });       // louder burst, new hold
    section(second / 2, [&](std::size_t f, int) {                                // decaying tail
        return 0.6 * std::exp(-static_cast<double>(f) / (0.1 * rate)) * noise(); });
    return programme;
}

void checkSame(const PeakAnalyzer& a, const PeakAnalyzer& b, int channels, const char* what) {
    for (int channel = 0; channel < channels; ++channel) {
        const auto& x = a.result().channels[static_cast<std::size_t>(channel)];
        const auto& y = b.result().channels[static_cast<std::size_t>(channel)];
        check(x.samplePeakValid == y.samplePeakValid && x.truePeakValid == y.truePeakValid
                  && x.invalidInput == y.invalidInput,
              what);
        const double tpScale = std::max(1e-300, std::max(std::abs(x.truePeak), std::abs(y.truePeak)));
        const double spScale = std::max(1e-300, std::max(std::abs(x.samplePeak), std::abs(y.samplePeak)));
        check(std::abs(x.truePeak - y.truePeak) <= 1e-12 * tpScale, what);
        check(std::abs(x.samplePeak - y.samplePeak) <= 1e-12 * spScale, what);
    }
}

void quietSkipEquivalence() {
    constexpr std::array<int, 9> rates{32000, 44100, 48000, 88200, 96000, 176400, 192000,
                                       352800, 384000};
    for (int rate : rates) {
        for (int channels : {1, 2}) {
            const auto programme = makeProgramme(rate, channels);
            PeakAnalyzer skipping, full;
            check(skipping.prepare(rate, channels) && full.prepare(rate, channels),
                  "quiet-skip prepare");
            full.setQuietSkipForTests(false);
            std::size_t begin = 0;
            std::size_t sectionIndex = 0;
            for (std::size_t end : programme.sectionEnds) {
                // Uneven blocks inside each section also exercise partitioning.
                std::size_t offset = begin;
                while (offset < end) {
                    const std::size_t count = std::min<std::size_t>(end - offset, 1 + (offset * 7) % 211);
                    const double* input[2] = {programme.channels[0].data() + offset,
                                              channels > 1 ? programme.channels[1].data() + offset
                                                           : nullptr};
                    skipping.process(input, count);
                    full.process(input, count);
                    offset += count;
                }
                begin = end;
                checkSame(skipping, full, channels, "quiet skip equals full FIR at section end");
                if (++sectionIndex == 5) { // a segment boundary in the middle keeps the holds
                    skipping.finishSegment();
                    full.finishSegment();
                    checkSame(skipping, full, channels, "quiet skip equals full FIR after tail");
                }
            }
            skipping.finishSegment();
            full.finishSegment();
            checkSame(skipping, full, channels, "quiet skip equals full FIR after final tail");
        }
    }
}

// Minimal PCM/float WAV reader for the optional timing loop.
bool readWav(const char* path, std::vector<std::vector<double>>& channels, int& rate) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0) return false;
    auto u16 = [&](std::size_t o) { return static_cast<int>(static_cast<unsigned char>(bytes[o]))
                                           | (static_cast<int>(static_cast<unsigned char>(bytes[o + 1])) << 8); };
    auto u32 = [&](std::size_t o) { return static_cast<std::uint32_t>(u16(o))
                                           | (static_cast<std::uint32_t>(u16(o + 2)) << 16); };
    int channelCount = 0, bits = 0;
    std::size_t position = 12;
    while (position + 8 <= bytes.size()) {
        const std::size_t size = u32(position + 4);
        const std::size_t body = position + 8;
        if (std::memcmp(bytes.data() + position, "fmt ", 4) == 0) {
            channelCount = u16(body + 2);
            rate = static_cast<int>(u32(body + 4));
            bits = u16(body + 14);
        } else if (std::memcmp(bytes.data() + position, "data", 4) == 0) {
            if (bits != 24 || channelCount < 1 || channelCount > 2) return false;
            const std::size_t frames = std::min(size, bytes.size() - body) / (3 * channelCount);
            channels.assign(static_cast<std::size_t>(channelCount), std::vector<double>(frames));
            for (std::size_t frame = 0; frame < frames; ++frame)
                for (int channel = 0; channel < channelCount; ++channel) {
                    const std::size_t o = body + (frame * channelCount + channel) * 3;
                    std::int32_t v = static_cast<std::int32_t>(
                        (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[o])) << 8)
                        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[o + 1])) << 16)
                        | (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[o + 2])) << 24));
                    channels[static_cast<std::size_t>(channel)][frame] = v / 2147483648.0;
                }
            return true;
        }
        position = body + size + (size & 1);
    }
    return false;
}

double timeAnalyser(const std::vector<std::vector<double>>& channels, int rate, bool skip,
                    double& truePeak) {
    const int channelCount = static_cast<int>(channels.size());
    double best = 1e300;
    for (int run = 0; run < 5; ++run) {
        PeakAnalyzer analyzer;
        analyzer.prepare(rate, channelCount);
        analyzer.setQuietSkipForTests(skip);
        const auto begin = std::chrono::steady_clock::now();
        for (std::size_t offset = 0; offset < channels[0].size(); offset += 512) {
            const std::size_t count = std::min<std::size_t>(512, channels[0].size() - offset);
            const double* input[2] = {channels[0].data() + offset,
                                      channelCount > 1 ? channels[1].data() + offset : nullptr};
            analyzer.process(input, count);
        }
        const auto end = std::chrono::steady_clock::now();
        truePeak = analyzer.result().channels[0].truePeak;
        best = std::min(best, std::chrono::duration<double, std::milli>(end - begin).count());
    }
    return best / (static_cast<double>(channels[0].size()) / rate);
}

void timingReport(const char* wavPath) {
    auto report = [](const char* name, const std::vector<std::vector<double>>& channels, int rate) {
        double tpOn = 0, tpOff = 0;
        const double on = timeAnalyser(channels, rate, true, tpOn);
        const double off = timeAnalyser(channels, rate, false, tpOff);
        std::cout << name << " @" << rate << " Hz: skip off " << off << " ms/s, skip on " << on
                  << " ms/s, speed-up " << off / on << "x, TP delta " << std::abs(tpOn - tpOff) << '\n';
    };
    for (int rate : {48000, 192000}) {
        Programme programme = makeProgramme(rate, 2);
        report("synthetic programme", programme.channels, rate);
    }
    std::vector<std::vector<double>> mix;
    int rate = 0;
    if (wavPath && readWav(wavPath, mix, rate)) report("mix", mix, rate);
    else std::cout << "mix not read (24-bit mono/stereo PCM expected)\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc >= 2 && std::strcmp(argv[1], "--timing") == 0) {
            timingReport(argc >= 3 ? argv[2] : nullptr);
            return 0;
        }
        double worstLow = 0;
        double worstHigh = 0;
        samplePeaksAndInvalidity();
        ebuTableNineteen();
        ratePolicyAndPhaseSweeps(worstLow, worstHigh);
        directTransientAndTail();
        partitionInvariance();
        legacyModuloEquivalence();
        quietSkipEquivalence();
        std::cout << "Production peak tests passed; phase-swept declared-band error ["
                  << worstLow << ", " << worstHigh << "] dB.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
