#include "LoudnessCore.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace loudness {
namespace {
constexpr double offset = -0.691;
constexpr double pi = 3.141592653589793238462643383279502884;
constexpr double absoluteGate = -70.0;
constexpr double histogramLow = -120.0;
constexpr double binWidth = 0.01;
constexpr int lastBin = 14399;
}

bool Core::supportedRate(int rate) noexcept {
    return rate == 32000 || rate == 44100 || rate == 48000 || rate == 88200 || rate == 96000
        || rate == 176400 || rate == 192000 || rate == 352800 || rate == 384000;
}
double Core::toLufs(double power) noexcept {
    return power > 0 ? offset + 10 * std::log10(power)
                     : -std::numeric_limits<double>::infinity();
}
double Core::toPower(double lufs) noexcept { return std::pow(10.0, (lufs - offset) / 10); }

// De Man bilinear mapping of the BS.1770 prefilter analog response. The published 48 kHz
// table is reproduced, including its deliberately unnormalised RLB numerator (1,-2,1).
std::array<double, 5> Core::shelfCoefficients(int rate) noexcept {
    const double k = std::tan(pi * 1681.974450955533 / rate);
    const double q = 0.7071752369554196;
    const double vh = std::pow(10.0, 3.99984385397 / 20);
    const double vb = std::pow(vh, 0.4996667741545416);
    const double a0 = 1 + k / q + k * k;
    return {(vh + vb * k / q + k * k) / a0, 2 * (k * k - vh) / a0,
            (vh - vb * k / q + k * k) / a0, 2 * (k * k - 1) / a0,
            (1 - k / q + k * k) / a0};
}
std::array<double, 5> Core::highPassCoefficients(int rate) noexcept {
    const double k = std::tan(pi * 38.13547087602444 / rate);
    const double q = 0.5003270373238773;
    const double a0 = 1 + k / q + k * k;
    return {1, -2, 1, 2 * (k * k - 1) / a0, (1 - k / q + k * k) / a0};
}
double Core::Biquad::apply(double x) noexcept {
    double y = c[0] * x + c[1] * x1 + c[2] * x2 - c[3] * y1 - c[4] * y2;
    // After the input stops, the double-precision DF1 states can fall into a subnormal limit
    // cycle that never decays and is very slow on x86/MSVC. Values below 1e-150 contribute
    // nothing measurable and their squares stay normal, so flush them to exact zero.
    if (std::abs(y) < 1e-150) y = 0;
    x2 = x1; x1 = x; y2 = y1; y1 = y;
    return y;
}

bool Core::prepare(int sampleRate, int channels, Capacities cap) {
    if (channels < 1 || channels > 2 || cap.blocks == 0 || cap.blocks > maxBlocks
        || cap.history == 0 || cap.history > maxHistory || cap.queue < 2
        || cap.queue > defaultQueue) return false;
    rate_ = sampleRate; channels_ = channels;
    capacities_ = cap;
    hopSamples_ = supportedRate(rate_) ? rate_ / 10 : 0;
    sampleLimit_ = cap.samples ? cap.samples : (hopSamples_ ? static_cast<std::uint64_t>(rate_) * 86400 : 0);
    if (sampleLimit_ > static_cast<std::uint64_t>(rate_) * 86400 && hopSamples_) return false;
    queue_.reset(new Point[cap.queue]);
    // prepare() runs with producer and consumer stopped. A replacement queue is a fresh
    // transaction; retaining the old monotone indices would address uninitialised slots in it.
    read_.store(0, std::memory_order_relaxed);
    write_.store(0, std::memory_order_relaxed);
    integrated_.raw.resize(cap.blocks);
    shortStats_.raw.resize(cap.blocks);
    integrated_.bins.resize(14400);
    shortStats_.bins.resize(14400);
    integrated_.rawBin.resize(cap.blocks);
    shortStats_.rawBin.resize(cap.blocks);
    rankWorkspace_.resize(cap.blocks);
    history_.resize(cap.history);
    const auto shelf = supportedRate(rate_) ? shelfCoefficients(rate_) : std::array<double,5>{};
    const auto highpass = supportedRate(rate_) ? highPassCoefficients(rate_) : std::array<double,5>{};
    for (auto& channel : filters_) { channel[0].c = shelf; channel[1].c = highpass; }
    // Generation IDs stay monotone across reprepare so an editor cannot mistake a new capture
    // for an old same-numbered snapshot while sample rate/layout storage is being replaced.
    reset();
    clearWorker(generationLocal_);
    return true;
}
void Core::resetLocal() noexcept {
    for (auto& channel : filters_) for (auto& filter : channel) filter.clear();
    hopPowers_.fill(0);
    hopFill_ = hopIndex_ = hopCount_ = 0;
    hopEnergy_ = 0;
    localSamples_ = 0;
}
void Core::reset() noexcept {
    resetSequence_.fetch_add(1, std::memory_order_acq_rel);
    resetLocal();
    captured_ = 0;
    segmentLocal_ = 0;
    segment_.store(0, std::memory_order_release);
    capturedPublished_.store(0, std::memory_order_release);
    invalid_.store(false, std::memory_order_release);
    overrun_.store(false, std::memory_order_release);
    limit_.store(false, std::memory_order_release);
    discontinuity_.store(false, std::memory_order_release);
    generation_.store(++generationLocal_, std::memory_order_release);
    resetSequence_.fetch_add(1, std::memory_order_release);
}
void Core::discontinuity() noexcept {
    resetLocal();
    segment_.store(++segmentLocal_, std::memory_order_release);
    discontinuity_.store(true, std::memory_order_release);
}
void Core::discardPartial() noexcept { resetLocal(); }
void Core::invalidateInput() noexcept {
    invalid_.store(true, std::memory_order_release);
    resetLocal();
}
void Core::finishHop() noexcept {
    hopPowers_[static_cast<std::size_t>(hopIndex_)] = hopEnergy_ / hopSamples_;
    hopIndex_ = (hopIndex_ + 1) % 30;
    hopCount_ = std::min(hopCount_ + 1, 30);
    hopFill_ = 0; hopEnergy_ = 0;
    Point point{};
    point.generation = generationLocal_;
    point.segment = segmentLocal_;
    point.gapBefore = segmentLocal_ != 0 && localSamples_ == static_cast<std::uint64_t>(hopSamples_);
    point.endSample = captured_;
    if (hopCount_ >= 4) {
        point.momentaryValid = true;
        for (int i = 0; i < 4; ++i) point.momentaryPower += hopPowers_[static_cast<std::size_t>((hopIndex_ + 29 - i) % 30)] / 4;
    }
    if (hopCount_ == 30) {
        point.shortValid = true;
        for (double energy : hopPowers_) point.shortPower += energy / 30;
    }
    const auto write = write_.load(std::memory_order_relaxed);
    if (write - read_.load(std::memory_order_acquire) >= capacities_.queue) {
        overrun_.store(true, std::memory_order_release);
        return;
    }
    queue_[write % capacities_.queue] = point;
    write_.store(write + 1, std::memory_order_release);
}
template <typename Sample> void Core::process(const Sample* const* input, std::size_t frames) noexcept {
    if (!hopSamples_ || limit_.load(std::memory_order_relaxed) || !input) return;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        if (captured_ == sampleLimit_) { limit_.store(true, std::memory_order_release); break; }
        double power = 0;
        bool bad = false;
        for (int channel = 0; channel < channels_; ++channel) {
            if (!input[channel]) { bad = true; continue; }
            const double x = static_cast<double>(input[channel][frame]);
            if (!std::isfinite(x)) { bad = true; continue; }
            if (!invalid_.load(std::memory_order_relaxed)) {
                const double shelf = filters_[static_cast<std::size_t>(channel)][0].apply(x);
                const double weighted = filters_[static_cast<std::size_t>(channel)][1].apply(shelf);
                power += weighted * weighted;
                if (!std::isfinite(power)) bad = true;
            }
        }
        ++captured_;
        if (bad) invalidateInput();
        if (invalid_.load(std::memory_order_relaxed)) continue;
        hopEnergy_ += power;
        if (!std::isfinite(hopEnergy_)) { invalidateInput(); continue; }
        ++hopFill_; ++localSamples_;
        if (hopFill_ == hopSamples_) finishHop();
    }
    capturedPublished_.store(captured_, std::memory_order_release);
    if (captured_ == sampleLimit_) limit_.store(true, std::memory_order_release);
}
template void Core::process<float>(const float* const*, std::size_t) noexcept;
template void Core::process<double>(const double* const*, std::size_t) noexcept;

int Core::binFor(double power) noexcept {
    // The last bin is a catch-all for everything at or above +24 LUFS. Cut-bin corrections and
    // rank selection use raw energies, so a catch-all bin stays exact.
    const double index = std::floor((toLufs(power) - histogramLow) / binWidth);
    return index > lastBin ? lastBin : static_cast<int>(index);
}
void Core::Stats::clear() noexcept {
    size = 0; absoluteSum = 0; absoluteCount = 0;
    std::fill(bins.begin(), bins.end(), Bin{});
}
bool Core::Stats::add(double power, bool inclusive) noexcept {
    if (!std::isfinite(power) || power < 0) return false;
    const double gate = Core::toPower(absoluteGate);
    if (inclusive ? power < gate : power <= gate) return true;
    const auto index = Core::binFor(power);
    if (index < 0 || index >= static_cast<int>(bins.size()) || size >= raw.size()
        || size >= rawBin.size()) return false;
    auto& bin = bins[static_cast<std::size_t>(index)];
    const double sum = bin.sum + power;
    const double all = absoluteSum + power;
    if (bin.count == std::numeric_limits<std::uint64_t>::max() || !std::isfinite(sum)
        || !std::isfinite(all)) return false;
    rawBin[size] = static_cast<std::uint16_t>(index);
    raw[size++] = power;
    ++bin.count; bin.sum = sum; absoluteSum = all; ++absoluteCount;
    return true;
}
Core::Stats::Aggregate Core::Stats::above(double threshold, bool inclusive) const noexcept {
    Aggregate result{};
    const int cut = Core::binFor(threshold);
    // Bins strictly above the cut are unambiguous. Only the cut bin uses raw energies.
    for (int index = std::max(0, cut + 1); index < static_cast<int>(bins.size()); ++index) {
        const auto& bin = bins[static_cast<std::size_t>(index)];
        result.count += bin.count; result.sum += bin.sum;
    }
    if (cut >= 0 && cut < static_cast<int>(bins.size()) && bins[static_cast<std::size_t>(cut)].count) {
        for (std::size_t i = 0; i < size; ++i) {
            const double value = raw[i];
            if (rawBin[i] == cut && (inclusive ? value >= threshold : value > threshold)) {
                ++result.count; result.sum += value;
            }
        }
    }
    return result;
}
double Core::Stats::rankPower(std::size_t rank, double threshold, std::vector<double>& workspace) const {
    const int cut = Core::binFor(threshold);
    for (int binIndex = std::max(0, cut); binIndex < static_cast<int>(bins.size()); ++binIndex) {
        const auto& bin = bins[static_cast<std::size_t>(binIndex)];
        if (!bin.count) continue;
        std::size_t eligible = static_cast<std::size_t>(bin.count);
        if (binIndex == cut) {
            eligible = 0;
            for (std::size_t i = 0; i < size; ++i)
                if (rawBin[i] == cut && raw[i] >= threshold) ++eligible;
        }
        if (rank >= eligible) { rank -= eligible; continue; }
        std::size_t count = 0;
        for (std::size_t i = 0; i < size; ++i)
            if (rawBin[i] == binIndex && raw[i] >= threshold)
                workspace[count++] = raw[i];
        auto begin = workspace.begin();
        std::nth_element(begin, begin + static_cast<std::ptrdiff_t>(rank), begin + static_cast<std::ptrdiff_t>(count));
        return workspace[rank];
    }
    return std::numeric_limits<double>::quiet_NaN();
}
void Core::clearWorker(std::uint64_t generation) {
    integrated_.clear(); shortStats_.clear();
    historyWrite_ = historyCount_ = 0;
    summary_ = {};
    summary_.generation = generation;
    workerSegment_ = 0;
}
bool Core::addMomentaryPower(double power) noexcept {
    const bool ok = integrated_.add(power, false);
    if (!ok) summary_.statisticsOverflow = true;
    return ok;
}
bool Core::addShortPower(double power) noexcept {
    const bool ok = shortStats_.add(power, true);
    if (!ok) summary_.statisticsOverflow = true;
    return ok;
}
void Core::recomputeStatistics() {
    if (summary_.statisticsOverflow) return;
    if (integrated_.absoluteCount) {
        const double threshold = std::max(toPower(absoluteGate),
            integrated_.absoluteSum / integrated_.absoluteCount / 10);
        const auto selected = integrated_.above(threshold, false);
        if (selected.count) {
            summary_.integratedValid = true;
            summary_.integrated = toLufs(selected.sum / selected.count);
        }
    }
    if (shortStats_.absoluteCount) {
        const double threshold = std::max(toPower(absoluteGate),
            shortStats_.absoluteSum / shortStats_.absoluteCount / 100);
        const auto selected = shortStats_.above(threshold, true);
        if (selected.count) {
            const auto lowRank = static_cast<std::size_t>(std::floor((selected.count - 1) * 0.1 + 0.5));
            const auto highRank = static_cast<std::size_t>(std::floor((selected.count - 1) * 0.95 + 0.5));
            const double low = shortStats_.rankPower(lowRank, threshold, rankWorkspace_);
            const double high = shortStats_.rankPower(highRank, threshold, rankWorkspace_);
            summary_.rangeValid = std::isfinite(low) && std::isfinite(high);
            if (summary_.rangeValid) summary_.range = 10 * std::log10(high / low);
        }
    }
}
void Core::ingest(const Point& point) {
    if (point.segment != workerSegment_) {
        workerSegment_ = point.segment;
        summary_.momentaryValid = summary_.shortValid = false;
    }
    summary_.capturedSamples = point.endSample;
    summary_.lastEndSample = point.endSample;
    if (point.momentaryValid) {
        const double value = toLufs(point.momentaryPower);
        summary_.momentary = value; summary_.momentaryValid = true;
        if (!summary_.momentaryMaxValid || value > summary_.momentaryMax) {
            summary_.momentaryMax = value; summary_.momentaryMaxValid = true;
        }
        ++summary_.momentaryBlocks;
        addMomentaryPower(point.momentaryPower);
    }
    if (point.shortValid) {
        const double value = toLufs(point.shortPower);
        summary_.shortTerm = value; summary_.shortValid = true;
        if (!summary_.shortMaxValid || value > summary_.shortMax) {
            summary_.shortMax = value; summary_.shortMaxValid = true;
        }
        ++summary_.shortBlocks;
        addShortPower(point.shortPower);
    }
    history_[historyWrite_] = point;
    historyWrite_ = (historyWrite_ + 1) % history_.size();
    historyCount_ = std::min(historyCount_ + 1, history_.size());
    recomputeStatistics();
}
bool Core::drain(std::size_t maximumPoints) {
    const auto resetSequence = resetSequence_.load(std::memory_order_acquire);
    if ((resetSequence & 1u) != 0) return false;
    auto currentGeneration = generation_.load(std::memory_order_acquire);
    if (summary_.generation != currentGeneration) clearWorker(currentGeneration);
    const auto maximum = std::min(maximumPoints, capacities_.queue);
    for (std::size_t processed = 0; processed < maximum; ++processed) {
        currentGeneration = generation_.load(std::memory_order_acquire);
        if (summary_.generation != currentGeneration) clearWorker(currentGeneration);
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) break;
        const Point point = queue_[read % capacities_.queue];
        read_.store(read + 1, std::memory_order_release);
        // Reset can occur after the load above. Rebase before judging this point;
        // otherwise the first new-generation point can be consumed and lost.
        currentGeneration = generation_.load(std::memory_order_acquire);
        if (summary_.generation != currentGeneration) clearWorker(currentGeneration);
        if (point.generation != currentGeneration) continue;
        ingest(point);
    }
    currentGeneration = generation_.load(std::memory_order_acquire);
    if (summary_.generation != currentGeneration) clearWorker(currentGeneration);
    if (workerSegment_ != segment_.load(std::memory_order_acquire)) {
        summary_.momentaryValid = summary_.shortValid = false;
    }
    summary_.capturedSamples = std::max(
        capturedPublished_.load(std::memory_order_acquire), summary_.lastEndSample);
    summary_.lraUnstable = summary_.capturedSamples < static_cast<std::uint64_t>(rate_) * 60;
    summary_.invalidInput = invalid_.load(std::memory_order_acquire);
    summary_.analysisOverrun = overrun_.load(std::memory_order_acquire);
    summary_.captureLimit = limit_.load(std::memory_order_acquire);
    summary_.unsupportedRate = !supportedRate(rate_);
    summary_.discontinuity = discontinuity_.load(std::memory_order_acquire);
    if (summary_.invalidInput || summary_.analysisOverrun || summary_.statisticsOverflow) {
        summary_.momentaryValid = summary_.shortValid = summary_.integratedValid = summary_.rangeValid = false;
        summary_.momentaryMaxValid = summary_.shortMaxValid = false;
    }
    // A concurrent reset after flag reads makes this result stale. Never return old-generation
    // statistics tagged as the new generation; the caller may drain again for new points.
    currentGeneration = generation_.load(std::memory_order_acquire);
    if (summary_.generation != currentGeneration) clearWorker(currentGeneration);
    const auto finalResetSequence = resetSequence_.load(std::memory_order_acquire);
    return finalResetSequence == resetSequence && (finalResetSequence & 1u) == 0;
}
std::size_t Core::allocatedBytes() const noexcept {
    return capacities_.queue * sizeof(Point) + 3 * capacities_.blocks * sizeof(double)
        + 2 * capacities_.blocks * sizeof(std::uint16_t)
        + 2 * integrated_.bins.size() * sizeof(Bin) + capacities_.history * sizeof(Point);
}
bool Core::filterStateIsZero() const noexcept {
    for (const auto& channel : filters_)
        for (const auto& f : channel)
            if (f.x1 != 0 || f.x2 != 0 || f.y1 != 0 || f.y2 != 0) return false;
    return true;
}
std::size_t Core::availableFrames(std::size_t requested) const noexcept {
    if (!hopSamples_ || captured_ >= sampleLimit_) return 0;
    return static_cast<std::size_t>(std::min<std::uint64_t>(
        requested, sampleLimit_ - captured_));
}
} // namespace loudness
