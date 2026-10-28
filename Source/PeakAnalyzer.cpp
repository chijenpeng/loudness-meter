#include "PeakAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace loudness {
namespace {
constexpr double pi = 3.141592653589793238462643383279502884;

double sinc(double value) noexcept {
    if (value == 0) return 1;
    return std::sin(pi * value) / (pi * value);
}

double blackmanHarris(double value) noexcept {
    const double radius = PeakAnalyzer::interpolationDelay;
    if (std::abs(value) >= radius) return 0;
    const double angle = pi * value / radius;
    return 0.35875 + 0.48829 * std::cos(angle)
        + 0.14128 * std::cos(2 * angle) + 0.01168 * std::cos(3 * angle);
}
} // namespace

bool PeakAnalyzer::isSupportedRate(int sampleRate) noexcept {
    return sampleRate == 32000 || sampleRate == 44100 || sampleRate == 48000
        || sampleRate == 88200 || sampleRate == 96000 || sampleRate == 176400
        || sampleRate == 192000 || sampleRate == 352800 || sampleRate == 384000;
}

double PeakAnalyzer::toDecibels(double amplitude) noexcept {
    return amplitude > 0 ? 20 * std::log10(amplitude)
                         : -std::numeric_limits<double>::infinity();
}

bool PeakAnalyzer::prepare(int sampleRate, int channels) noexcept {
    if (channels < 1 || channels > maximumChannels) return false;
    rate_ = sampleRate;
    channels_ = channels;
    factor_ = !isSupportedRate(rate_) ? 0 : (rate_ <= 48000 ? 8 : (rate_ <= 96000 ? 4 : (rate_ <= 192000 ? 2 : 1)));
    makeCoefficients();
    reset();
    result_.supportedRate = factor_ != 0;
    return true;
}

void PeakAnalyzer::makeCoefficients() noexcept {
    for (auto& phase : coefficients_) phase.fill(0);
    l1Max_ = 0;
    if (!factor_) return;
    for (int phase = 0; phase < factor_; ++phase) {
        double sum = 0;
        const double fraction = static_cast<double>(phase) / factor_;
        for (int tap = 0; tap < tapsPerPhase; ++tap) {
            const double offset = tap - interpolationDelay + fraction;
            const double value = sinc(offset) * blackmanHarris(offset);
            coefficients_[static_cast<std::size_t>(phase)][static_cast<std::size_t>(tap)] = value;
            sum += value;
        }
        if (sum != 0) {
            for (double& value : coefficients_[static_cast<std::size_t>(phase)]) value /= sum;
        }
        if (phase > 0) {
            double l1 = 0;
            for (double value : coefficients_[static_cast<std::size_t>(phase)]) l1 += std::abs(value);
            l1Max_ = std::max(l1Max_, l1);
        }
    }
}

void PeakAnalyzer::clearLocal() noexcept {
    for (auto& channel : history_) channel.fill(0);
    writeIndex_ = 0;
    quietRun_.fill(0);
    segmentActive_ = false;
}

void PeakAnalyzer::reset() noexcept {
    clearLocal();
    result_ = {};
    result_.supportedRate = factor_ != 0;
}

void PeakAnalyzer::invalidateChannel(int channel) noexcept {
    auto& channelResult = result_.channels[static_cast<std::size_t>(channel)];
    channelResult.invalidInput = true;
    channelResult.samplePeakValid = false;
    channelResult.truePeakValid = false;
    history_[static_cast<std::size_t>(channel)].fill(0);
    quietRun_[static_cast<std::size_t>(channel)] = 0;
}

void PeakAnalyzer::processFrame(const double* samples, bool programmeFrame) noexcept {
    for (int channel = 0; channel < channels_; ++channel) {
        auto& channelResult = result_.channels[static_cast<std::size_t>(channel)];
        const double sample = samples[static_cast<std::size_t>(channel)];
        auto& history = history_[static_cast<std::size_t>(channel)];
        history[writeIndex_] = sample;
        history[writeIndex_ + tapsPerPhase] = sample;
        if (programmeFrame && !channelResult.invalidInput) {
            const double magnitude = std::abs(sample);
            channelResult.samplePeak = channelResult.samplePeakValid
                ? std::max(channelResult.samplePeak, magnitude) : magnitude;
            channelResult.truePeak = channelResult.truePeakValid
                ? std::max(channelResult.truePeak, magnitude) : magnitude;
            channelResult.samplePeakValid = channelResult.truePeakValid = true;
        }
        if (channelResult.invalidInput) continue;
        // Phase zero is the delayed integer sample grid. Raw sample hold above already includes
        // every programme value on that grid, including values later shifted through the FIR;
        // re-evaluating it cannot add a legitimate peak. Fractional phases carry the TP work.
        if (factor_ > 1 && quietSkip_ && channelResult.truePeakValid) {
            // Holds only grow within a capture, so a sample that was quiet when it arrived
            // is still quiet. The 1e-9 margin covers floating-point rounding of the sums.
            const double threshold = channelResult.truePeak / (l1Max_ * (1 + 1e-9));
            auto& run = quietRun_[static_cast<std::size_t>(channel)];
            run = std::abs(sample) <= threshold ? std::min(run + 1, tapsPerPhase) : 0;
            if (run >= tapsPerPhase) continue;
        } else {
            quietRun_[static_cast<std::size_t>(channel)] = 0;
        }
        const auto* newest = history.data() + writeIndex_ + tapsPerPhase;
        for (int phase = 1; phase < factor_; ++phase) {
            double reconstructed = 0;
            for (int tap = 0; tap < tapsPerPhase; ++tap)
                reconstructed += coefficients_[static_cast<std::size_t>(phase)]
                                             [static_cast<std::size_t>(tap)]
                    * newest[-tap];
            if (!std::isfinite(reconstructed)) {
                invalidateChannel(channel);
                break;
            }
            const double magnitude = std::abs(reconstructed);
            channelResult.truePeak = channelResult.truePeakValid
                ? std::max(channelResult.truePeak, magnitude) : magnitude;
            channelResult.truePeakValid = true;
        }
    }
    writeIndex_ = (writeIndex_ + 1) % tapsPerPhase;
}

template <typename Sample>
void PeakAnalyzer::process(const Sample* const* input, std::size_t frames) noexcept {
    if (!factor_ || !input) return;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        std::array<double, maximumChannels> samples{};
        for (int channel = 0; channel < channels_; ++channel) {
            auto& channelResult = result_.channels[static_cast<std::size_t>(channel)];
            if (!input[channel]) {
                invalidateChannel(channel);
                continue;
            }
            const double sample = static_cast<double>(input[channel][frame]);
            if (!std::isfinite(sample)) {
                invalidateChannel(channel);
                continue;
            }
            if (!channelResult.invalidInput) samples[static_cast<std::size_t>(channel)] = sample;
        }
        processFrame(samples.data(), true);
        ++result_.capturedFrames;
        segmentActive_ = true;
    }
}

void PeakAnalyzer::finishSegment() noexcept {
    if (!factor_ || !segmentActive_) return;
    const std::array<double, maximumChannels> zeros{};
    for (int frame = 0; frame < boundedTail; ++frame) processFrame(zeros.data(), false);
    clearLocal();
}

template void PeakAnalyzer::process<float>(const float* const*, std::size_t) noexcept;
template void PeakAnalyzer::process<double>(const double* const*, std::size_t) noexcept;

} // namespace loudness
