#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>

namespace {
constexpr auto stateVersionKey = "version";
constexpr auto editorScaleKey = "editorScale";
constexpr int stateVersion = 1;

float clampFinite(float value, float low, float high, float fallback) noexcept {
    return std::isfinite(value) ? juce::jlimit(low, high, value) : fallback;
}

juce::ValueTree findParameter(juce::ValueTree state, const char* identifier) {
    for (int index = 0; index < state.getNumChildren(); ++index) {
        auto child = state.getChild(index);
        if (child.getProperty("id").toString() == identifier) return child;
    }
    return {};
}
} // namespace

juce::AudioProcessorEditor* LoudnessMeterProcessor::createEditor() {
    return new LoudnessMeterEditor(*this);
}

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "The audio publication requires lock-free 64-bit atomics");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "The audio publication requires lock-free 32-bit atomics");

juce::AudioProcessorValueTreeState::ParameterLayout LoudnessMeterProcessor::makeParameters() {
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{loudness::params::captureMode, 1}, "Capture Mode",
        juce::StringArray{"Restart", "Continuous"}, 0));
    return layout;
}

LoudnessMeterProcessor::LoudnessMeterProcessor()
    : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                      .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts_(*this, nullptr, "LoudnessMeterState", makeParameters()) {
    captureModeParameter_ = apvts_.getRawParameterValue(loudness::params::captureMode);
    jassert(captureModeParameter_ != nullptr);
    publishAudioState();
}

LoudnessMeterProcessor::~LoudnessMeterProcessor() { stopWorker(); }

bool LoudnessMeterProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto input = layouts.getMainInputChannelSet();
    const auto output = layouts.getMainOutputChannelSet();
    return input == output
        && (input == juce::AudioChannelSet::mono() || input == juce::AudioChannelSet::stereo());
}

void LoudnessMeterProcessor::prepareToPlay(double sampleRate, int) {
    stopWorker();
    const auto channels = getTotalNumInputChannels();
    analysisSupported_ = sampleRate == 32000.0 || sampleRate == 44100.0
        || sampleRate == 48000.0 || sampleRate == 88200.0 || sampleRate == 96000.0
        || sampleRate == 176400.0 || sampleRate == 192000.0
        || sampleRate == 352800.0 || sampleRate == 384000.0;
    const auto configuredRate = analysisSupported_ ? static_cast<int>(sampleRate) : 0;
    {
        // The worker is joined, and this mutex also excludes editor snapshot/history reads while
        // the Core vectors are resized and its worker-owned state is cleared.
        std::lock_guard<std::mutex> lock(displayMutex_);
        historyCapacity_ = testCapacities_.history;
        preparedSampleRate_ = sampleRate;
        preparedChannels_ = channels;
        prepared_ = core_.prepare(configuredRate, channels, testCapacities_)
            && peaks_.prepare(configuredRate, channels);
        workerAudioState_ = {};
        core_.drain();
        displaySnapshot_ = {};
        displaySnapshot_.loudness = core_.summary();
        displaySnapshot_.sampleRate = preparedSampleRate_;
        displaySnapshot_.activeChannels = preparedChannels_;
    }
    setLatencySamples(0);
    manualPaused_ = false;
    transportInitialised_ = false;
    transportKnown_ = false;
    lastPlaying_ = false;
    expectedPositionValid_ = false;
    capturing_ = false;
    bypassed_ = false;
    restoreBlocked_ = false;
    lastCaptureMode_ = juce::roundToInt(captureModeParameter_->load(std::memory_order_relaxed));
    lastStateEpoch_ = stateEpoch_.load(std::memory_order_acquire);
    pendingCommands_.store(0, std::memory_order_release);
    currentStatus_ = noTransportStatus | positionUnavailableStatus;
    publishAudioState();
    if (prepared_) startWorker();
}

void LoudnessMeterProcessor::releaseResources() {
    stopWorker();
    if (prepared_ && capturing_) finishCaptureSegment();
    capturing_ = false;
    currentStatus_ &= ~capturingStatus;
    publishAudioState();
    if (prepared_) drainAndPublish();
    prepared_ = false;
}

void LoudnessMeterProcessor::requestReset() noexcept {
    pendingCommands_.fetch_or(resetCommand, std::memory_order_release);
}
void LoudnessMeterProcessor::requestPause() noexcept {
    pendingCommands_.fetch_or(pauseCommand, std::memory_order_release);
}
void LoudnessMeterProcessor::requestStart() noexcept {
    pendingCommands_.fetch_or(startCommand, std::memory_order_release);
}

void LoudnessMeterProcessor::setEditorScale(float scale) noexcept {
    editorScale_.store(clampFinite(scale, 0.75f, 2.0f, 1.0f), std::memory_order_release);
}

std::uint64_t LoudnessMeterProcessor::doubleBits(double value) noexcept {
    std::uint64_t result = 0;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
double LoudnessMeterProcessor::bitsDouble(std::uint64_t value) noexcept {
    double result = 0;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
LoudnessMeterProcessor::HostState LoudnessMeterProcessor::readHostState() const {
    HostState result;
    if (auto* playHead = getPlayHead()) {
        if (const auto position = playHead->getPosition()) {
            result.known = true;
            result.playing = position->getIsPlaying();
            if (const auto samples = position->getTimeInSamples()) {
                result.positionKnown = true;
                result.position = *samples;
            }
        }
    }
    return result;
}

void LoudnessMeterProcessor::resetCapture() noexcept {
    core_.reset();
    peaks_.reset();
    capturing_ = false;
}

void LoudnessMeterProcessor::finishCaptureSegment() noexcept {
    peaks_.finishSegment();
    core_.discardPartial();
    capturing_ = false;
}

void LoudnessMeterProcessor::updateHostTracking(const HostState& host, int blockSamples) noexcept {
    transportInitialised_ = true;
    transportKnown_ = host.known;
    lastPlaying_ = host.playing;
    expectedPositionValid_ = host.positionKnown;
    if (host.positionKnown) expectedPosition_ = host.position + blockSamples;
}

template <typename Sample>
void LoudnessMeterProcessor::process(juce::AudioBuffer<Sample>& buffer, bool bypassed) noexcept {
    if (!prepared_) return;
    const auto host = readHostState();
    const int blockSamples = buffer.getNumSamples();

    if (stateRestoreInProgress_.load(std::memory_order_acquire)) {
        if (!restoreBlocked_) {
            resetCapture();
            manualPaused_ = false;
            restoreBlocked_ = true;
        }
        currentStatus_ = waitingStatus | (host.known ? 0u : noTransportStatus)
            | (host.positionKnown ? 0u : positionUnavailableStatus)
            | (bypassed ? bypassedStatus : 0u);
        updateHostTracking(host, blockSamples);
        publishAudioState();
        return;
    }

    bool fullReset = false;
    const auto epoch = stateEpoch_.load(std::memory_order_acquire);
    if (restoreBlocked_ || epoch != lastStateEpoch_) {
        restoreBlocked_ = false;
        lastStateEpoch_ = epoch;
        manualPaused_ = false;
        fullReset = true;
    }

    const int captureMode = juce::jlimit(0, 1,
        juce::roundToInt(captureModeParameter_->load(std::memory_order_relaxed)));
    if (captureMode != lastCaptureMode_) {
        lastCaptureMode_ = captureMode;
        fullReset = true;
    }

    const auto commands = pendingCommands_.exchange(0, std::memory_order_acq_rel);
    if ((commands & resetCommand) != 0) fullReset = true;
    if ((commands & pauseCommand) != 0) manualPaused_ = true;
    if ((commands & startCommand) != 0) {
        manualPaused_ = false;
        fullReset = true;
    }

    const bool firstTransportState = !transportInitialised_;
    const bool availabilityChanged = !firstTransportState && host.known != transportKnown_;
    if ((firstTransportState && host.known) || availabilityChanged) fullReset = true;
    const bool stoppedToPlaying = !firstTransportState && host.known && transportKnown_
        && !lastPlaying_ && host.playing;
    if (captureMode == 0 && stoppedToPlaying) fullReset = true;
    if (bypassed_ && !bypassed) fullReset = true;

    const bool seek = captureMode == 0 && host.known && host.playing && host.positionKnown
        && expectedPositionValid_ && host.position != expectedPosition_;

    if (fullReset) resetCapture();
    else if (seek) {
        peaks_.finishSegment();
        core_.discontinuity();
        capturing_ = false;
    }

    const bool transportEligible = captureMode == 1 || !host.known || host.playing;
    const bool eligible = transportEligible && !manualPaused_ && !bypassed;
    if (!eligible && capturing_) finishCaptureSegment();

    if (eligible && analysisSupported_ && !core_.captureLimitReached()) {
        capturing_ = true;
        const auto frames = core_.availableFrames(static_cast<std::size_t>(blockSamples));
        if (frames != 0) {
            const auto* const* input = buffer.getArrayOfReadPointers();
            core_.process(input, frames);
            peaks_.process(input, frames);
        }
        if (core_.captureLimitReached()) finishCaptureSegment();
    } else if (core_.captureLimitReached()) {
        capturing_ = false;
    }

    bypassed_ = bypassed;
    currentStatus_ = (host.known ? 0u : noTransportStatus)
        | (host.positionKnown ? 0u : positionUnavailableStatus)
        | (manualPaused_ ? pausedStatus : 0u)
        | ((!eligible && !manualPaused_ && !bypassed) ? waitingStatus : 0u)
        | (bypassed ? bypassedStatus : 0u)
        | (capturing_ ? capturingStatus : 0u);
    updateHostTracking(host, blockSamples);
    publishAudioState();
}

void LoudnessMeterProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    process(buffer, false);
}
void LoudnessMeterProcessor::processBlock(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) {
    process(buffer, false);
}
void LoudnessMeterProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer,
                                                   juce::MidiBuffer&) {
    process(buffer, true);
}
void LoudnessMeterProcessor::processBlockBypassed(juce::AudioBuffer<double>& buffer,
                                                   juce::MidiBuffer&) {
    process(buffer, true);
}

void LoudnessMeterProcessor::publishAudioState() noexcept {
    audioPublication_.sequence.fetch_add(1, std::memory_order_acq_rel);
    audioPublication_.generation.store(core_.generation(), std::memory_order_release);
    const auto& peakResult = peaks_.result();
    std::uint32_t peakFlags = peakResult.supportedRate ? 1u : 0u;
    for (std::size_t channel = 0; channel < 2; ++channel) {
        const auto& source = peakResult.channels[channel];
        audioPublication_.samplePeak[channel].store(doubleBits(source.samplePeak),
                                                    std::memory_order_release);
        audioPublication_.truePeak[channel].store(doubleBits(source.truePeak),
                                                  std::memory_order_release);
        if (source.samplePeakValid) peakFlags |= 1u << (1 + channel * 3);
        if (source.truePeakValid) peakFlags |= 1u << (2 + channel * 3);
        if (source.invalidInput) peakFlags |= 1u << (3 + channel * 3);
    }
    audioPublication_.capturedFrames.store(peakResult.capturedFrames, std::memory_order_release);
    audioPublication_.peakFlags.store(peakFlags, std::memory_order_release);
    audioPublication_.status.store(currentStatus_, std::memory_order_release);
    audioPublication_.captureMode.store(static_cast<std::uint32_t>(lastCaptureMode_),
                                        std::memory_order_release);
    audioPublication_.sequence.fetch_add(1, std::memory_order_release);
}

bool LoudnessMeterProcessor::readAudioState(AudioState& destination) const noexcept {
    // Every payload field is atomic. Acquire payload loads paired with release stores ensure that
    // observing any new field also observes the writer's preceding odd sequence value; the final
    // sequence acquire therefore cannot accept a mixture under an unchanged old even value.
    for (int attempt = 0; attempt < 8; ++attempt) {
        const auto before = audioPublication_.sequence.load(std::memory_order_acquire);
        if ((before & 1u) != 0) continue;
        AudioState candidate;
        candidate.generation = audioPublication_.generation.load(std::memory_order_acquire);
        const auto peakFlags = audioPublication_.peakFlags.load(std::memory_order_acquire);
        candidate.peaks.supportedRate = (peakFlags & 1u) != 0;
        candidate.peaks.capturedFrames = static_cast<std::size_t>(
            audioPublication_.capturedFrames.load(std::memory_order_acquire));
        for (std::size_t channel = 0; channel < 2; ++channel) {
            auto& result = candidate.peaks.channels[channel];
            result.samplePeak = bitsDouble(
                audioPublication_.samplePeak[channel].load(std::memory_order_acquire));
            result.truePeak = bitsDouble(
                audioPublication_.truePeak[channel].load(std::memory_order_acquire));
            result.samplePeakValid = (peakFlags & (1u << (1 + channel * 3))) != 0;
            result.truePeakValid = (peakFlags & (1u << (2 + channel * 3))) != 0;
            result.invalidInput = (peakFlags & (1u << (3 + channel * 3))) != 0;
        }
        candidate.status = audioPublication_.status.load(std::memory_order_acquire);
        candidate.captureMode = static_cast<int>(
            audioPublication_.captureMode.load(std::memory_order_acquire));
        const auto after = audioPublication_.sequence.load(std::memory_order_acquire);
        if (before == after) {
            destination = candidate;
            return true;
        }
    }
    return false;
}

bool LoudnessMeterProcessor::drainAndPublish() {
    std::lock_guard<std::mutex> lock(displayMutex_);
    if (!core_.drain()) return false;
    AudioState audio = workerAudioState_;
    if (readAudioState(audio)) workerAudioState_ = audio;
    DisplaySnapshot next;
    next.loudness = core_.summary();
    next.sampleRate = preparedSampleRate_;
    next.activeChannels = preparedChannels_;
    next.loudnessAgeSamples = next.loudness.capturedSamples >= next.loudness.lastEndSample
        ? next.loudness.capturedSamples - next.loudness.lastEndSample : 0;
    next.publicationCounter = displaySnapshot_.publicationCounter + 1;
    if (audio.generation == next.loudness.generation) {
        next.peaks = audio.peaks;
        next.captureMode = audio.captureMode;
        next.noTransport = (audio.status & noTransportStatus) != 0;
        next.positionUnavailable =
            (audio.status & positionUnavailableStatus) != 0;
        next.paused = (audio.status & pausedStatus) != 0;
        next.waiting = (audio.status & waitingStatus) != 0;
        next.bypassed = (audio.status & bypassedStatus) != 0;
        next.capturing = (audio.status & capturingStatus) != 0;
    }
    displaySnapshot_ = next;
    return true;
}

void LoudnessMeterProcessor::startWorker() {
    if (worker_.joinable()) return;
    workerExit_.store(false, std::memory_order_release);
    flushRequested_.store(0, std::memory_order_release);
    flushCompleted_.store(0, std::memory_order_release);
    worker_ = std::thread([this] { workerLoop(); });
}

void LoudnessMeterProcessor::stopWorker() {
    if (!worker_.joinable()) return;
    workerExit_.store(true, std::memory_order_release);
    workerWake_.notify_all();
    worker_.join();
}

void LoudnessMeterProcessor::workerLoop() {
    while (!workerExit_.load(std::memory_order_acquire)) {
        const auto requestedBeforeDrain = flushRequested_.load(std::memory_order_acquire);
        if (drainAndPublish()) {
            flushCompleted_.store(requestedBeforeDrain, std::memory_order_release);
            workerWake_.notify_all();
        }
        std::unique_lock<std::mutex> lock(workerWaitMutex_);
        workerWake_.wait_for(lock, std::chrono::milliseconds(10), [this] {
            return workerExit_.load(std::memory_order_acquire)
                || flushRequested_.load(std::memory_order_acquire)
                    != flushCompleted_.load(std::memory_order_acquire);
        });
    }
    const auto finalRequest = flushRequested_.load(std::memory_order_acquire);
    if (drainAndPublish()) flushCompleted_.store(finalRequest, std::memory_order_release);
    workerWake_.notify_all();
}

bool LoudnessMeterProcessor::flushAnalysisForTests() {
    if (!prepared_) return false;
    if (!worker_.joinable()) {
        return drainAndPublish();
    }
    const auto request = flushRequested_.fetch_add(1, std::memory_order_acq_rel) + 1;
    workerWake_.notify_all();
    std::unique_lock<std::mutex> lock(workerWaitMutex_);
    return workerWake_.wait_for(lock, std::chrono::seconds(2), [this, request] {
        return flushCompleted_.load(std::memory_order_acquire) >= request;
    });
}

LoudnessMeterProcessor::DisplaySnapshot LoudnessMeterProcessor::getDisplaySnapshot() const {
    std::lock_guard<std::mutex> lock(displayMutex_);
    return displaySnapshot_;
}

void LoudnessMeterProcessor::copyHistory(
    std::vector<loudness::Core::Point>& destination) const {
    std::lock_guard<std::mutex> lock(displayMutex_);
    destination.clear();
    const auto count = core_.historyCount();
    destination.reserve(count);
    if (count == 0) return;
    const auto next = core_.historyNextIndex();
    const auto start = count == historyCapacity_ ? next : 0;
    const auto* data = core_.historyData();
    for (std::size_t index = 0; index < count; ++index)
        destination.push_back(data[(start + index) % historyCapacity_]);
}

void LoudnessMeterProcessor::getStateInformation(juce::MemoryBlock& destination) {
    auto state = apvts_.copyState();
    state.setProperty(juce::Identifier{stateVersionKey}, stateVersion, nullptr);
    state.setProperty(juce::Identifier{editorScaleKey}, editorScale(), nullptr);
    if (const auto xml = state.createXml()) copyXmlToBinary(*xml, destination);
}

void LoudnessMeterProcessor::setStateInformation(const void* data, int bytes) {
    const auto xml = getXmlFromBinary(data, bytes);
    if (xml == nullptr || !xml->hasTagName(apvts_.state.getType())) return;
    const auto incoming = juce::ValueTree::fromXml(*xml);
    auto clean = apvts_.copyState();
    const auto sourceCapture = findParameter(incoming, loudness::params::captureMode);
    auto targetCapture = findParameter(clean, loudness::params::captureMode);
    if (sourceCapture.isValid() && targetCapture.isValid()) {
        const auto value = juce::roundToInt(clampFinite(
            static_cast<float>(sourceCapture.getProperty("value")), 0.0f, 1.0f, 0.0f));
        targetCapture.setProperty("value", value, nullptr);
    }
    const float scale = clampFinite(static_cast<float>(
                                        incoming.getProperty(juce::Identifier{editorScaleKey}, 1.0f)),
                                    0.75f, 2.0f, 1.0f);
    stateRestoreInProgress_.store(true, std::memory_order_release);
    apvts_.replaceState(clean);
    editorScale_.store(scale, std::memory_order_release);
    stateEpoch_.fetch_add(1, std::memory_order_acq_rel);
    stateRestoreInProgress_.store(false, std::memory_order_release);
}

#if LOUDNESS_METER_TESTING
void LoudnessMeterProcessor::stopAnalysisWorkerForTests() { stopWorker(); }
#endif

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new LoudnessMeterProcessor();
}
