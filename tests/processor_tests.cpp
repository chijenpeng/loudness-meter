#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace allocation_probe {
thread_local bool active = false;
thread_local std::size_t count = 0;
}

void* operator new(std::size_t size) {
    if (allocation_probe::active) ++allocation_probe::count;
    if (void* result = std::malloc(size)) return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try { return ::operator new(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    if (allocation_probe::active) ++allocation_probe::count;
#if defined(_MSC_VER)
    if (void* result = _aligned_malloc(size, static_cast<std::size_t>(alignment))) return result;
#else
    void* result = nullptr;
    if (posix_memalign(&result, static_cast<std::size_t>(alignment), size) == 0) return result;
#endif
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}
void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
    try { return ::operator new(size, alignment); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t& tag) noexcept {
    return ::operator new(size, alignment, tag);
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::align_val_t) noexcept {
#if defined(_MSC_VER)
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}
void operator delete[](void* pointer, std::align_val_t alignment) noexcept {
    ::operator delete(pointer, alignment);
}
void operator delete(void* pointer, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(pointer, alignment);
}
void operator delete[](void* pointer, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(pointer, alignment);
}

namespace {
void check(bool pass, const char* what) {
    if (!pass) throw std::runtime_error(what);
}
void near(double actual, double expected, double tolerance, const char* what) {
    if (!(std::abs(actual - expected) <= tolerance)) {
        std::cerr << what << ": got " << actual << ", expected " << expected << '\n';
        throw std::runtime_error(what);
    }
}

struct FakePlayHead final : juce::AudioPlayHead {
    bool available = true;
    bool playing = true;
    bool positionAvailable = true;
    std::int64_t position = 0;

    juce::Optional<PositionInfo> getPosition() const override {
        if (!available) return juce::nullopt;
        PositionInfo result;
        result.setIsPlaying(playing);
        if (positionAvailable) result.setTimeInSamples(position);
        return result;
    }
};

void setChannels(LoudnessMeterProcessor& processor, int channels) {
    juce::AudioProcessor::BusesLayout layout;
    const auto set = channels == 1 ? juce::AudioChannelSet::mono()
                                   : juce::AudioChannelSet::stereo();
    layout.inputBuses.add(set);
    layout.outputBuses.add(set);
    check(processor.setBusesLayout(layout), "set matching mono/stereo layout");
}

template <typename Sample>
void processConstant(LoudnessMeterProcessor& processor, int channels, int frames, Sample value,
                     bool bypassed = false) {
    juce::AudioBuffer<Sample> buffer(channels, frames);
    for (int channel = 0; channel < channels; ++channel)
        for (int frame = 0; frame < frames; ++frame)
            buffer.setSample(channel, frame, static_cast<Sample>(value + channel * Sample(0.01)));
    std::vector<std::vector<Sample>> before(static_cast<std::size_t>(channels));
    for (int channel = 0; channel < channels; ++channel) {
        const auto* data = buffer.getReadPointer(channel);
        before[static_cast<std::size_t>(channel)].assign(data, data + frames);
    }
    juce::MidiBuffer midi;
    if (bypassed) processor.processBlockBypassed(buffer, midi);
    else processor.processBlock(buffer, midi);
    for (int channel = 0; channel < channels; ++channel)
        check(std::memcmp(before[static_cast<std::size_t>(channel)].data(),
                          buffer.getReadPointer(channel), static_cast<std::size_t>(frames) * sizeof(Sample)) == 0,
              "processor is bit-transparent");
}

void flush(LoudnessMeterProcessor& processor) {
    check(processor.flushAnalysisForTests(), "analysis worker flush");
}

void setActual(LoudnessMeterProcessor& processor, const char* identifier, float value) {
    auto* parameter = processor.parameters().getParameter(identifier);
    check(parameter != nullptr, "parameter exists");
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

float actual(const LoudnessMeterProcessor& processor, const char* identifier) {
    const auto* value = processor.parameters().getRawParameterValue(identifier);
    check(value != nullptr, "raw parameter exists");
    return value->load();
}

juce::ValueTree findParameter(juce::ValueTree state, const char* identifier) {
    for (int index = 0; index < state.getNumChildren(); ++index) {
        auto child = state.getChild(index);
        if (child.getProperty("id").toString() == identifier) return child;
    }
    return {};
}

void identityLayoutAndPayloads() {
    LoudnessMeterProcessor processor;
    check(processor.getName() == "Loudness Meter", "product name");
    check(processor.getLatencySamples() == 0 && processor.supportsDoublePrecisionProcessing(),
          "zero audio latency and double support");
    check(processor.hasEditor(), "custom editor is available");
    const auto& parameters = processor.getParameters();
    check(parameters.size() == 1, "capture mode is the only automated parameter");
    const auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*>(parameters[0]);
    check(withId != nullptr && withId->getParameterID() == loudness::params::captureMode,
          "capture mode remains parameter 0");
    const auto* mode = dynamic_cast<juce::RangedAudioParameter*>(parameters[0]);
    check(mode != nullptr, "capture mode is a ranged parameter");
    const auto modeRange = mode->getNormalisableRange();
    check(modeRange.start == 0 && modeRange.end == 1 && modeRange.interval == 1,
          "captureMode range is choice 0..1");
    near(actual(processor, loudness::params::captureMode), 0, 0, "captureMode default Restart");
    juce::AudioProcessor::BusesLayout mismatch;
    mismatch.inputBuses.add(juce::AudioChannelSet::mono());
    mismatch.outputBuses.add(juce::AudioChannelSet::stereo());
    check(!processor.isBusesLayoutSupported(mismatch), "reject mismatched layout");
    setChannels(processor, 2);
    FakePlayHead head;
    processor.setPlayHead(&head);
    processor.prepareToPlay(48000, 64);

    juce::AudioBuffer<float> floats(2, 4);
    const std::array<std::uint32_t, 8> floatBits{
        0x3f000000u, 0x80000000u, 0x7fc12345u, 0x7f800000u,
        0xbf800000u, 0x00000001u, 0xffc54321u, 0xff800000u};
    for (int channel = 0; channel < 2; ++channel)
        for (int frame = 0; frame < 4; ++frame)
            std::memcpy(floats.getWritePointer(channel) + frame,
                        &floatBits[static_cast<std::size_t>(channel * 4 + frame)], sizeof(float));
    std::array<std::uint32_t, 8> floatBefore{};
    std::memcpy(floatBefore.data(), floats.getReadPointer(0), 4 * sizeof(float));
    std::memcpy(floatBefore.data() + 4, floats.getReadPointer(1), 4 * sizeof(float));
    juce::MidiBuffer midi;
    processor.processBlock(floats, midi);
    check(std::memcmp(floatBefore.data(), floats.getReadPointer(0), 4 * sizeof(float)) == 0
              && std::memcmp(floatBefore.data() + 4, floats.getReadPointer(1), 4 * sizeof(float)) == 0,
          "float finite/nonfinite payloads are bit-identical");

    juce::AudioBuffer<double> doubles(2, 4);
    const std::array<std::uint64_t, 8> doubleBits{
        0x3fe0000000000000ull, 0x8000000000000000ull, 0x7ff8123456789abcull,
        0x7ff0000000000000ull, 0xbff0000000000000ull, 1ull,
        0xfff8543212345678ull, 0xfff0000000000000ull};
    for (int channel = 0; channel < 2; ++channel)
        for (int frame = 0; frame < 4; ++frame)
            std::memcpy(doubles.getWritePointer(channel) + frame,
                        &doubleBits[static_cast<std::size_t>(channel * 4 + frame)], sizeof(double));
    std::array<std::uint64_t, 8> doubleBefore{};
    std::memcpy(doubleBefore.data(), doubles.getReadPointer(0), 4 * sizeof(double));
    std::memcpy(doubleBefore.data() + 4, doubles.getReadPointer(1), 4 * sizeof(double));
    processor.processBlock(doubles, midi);
    check(std::memcmp(doubleBefore.data(), doubles.getReadPointer(0), 4 * sizeof(double)) == 0
              && std::memcmp(doubleBefore.data() + 4, doubles.getReadPointer(1), 4 * sizeof(double)) == 0,
          "double finite/nonfinite payloads are bit-identical");
    flush(processor);
    const auto snapshot = processor.getDisplaySnapshot();
    check(snapshot.loudness.invalidInput && snapshot.peaks.channels[0].invalidInput
              && snapshot.peaks.channels[1].invalidInput,
          "nonfinite input remains transparent and explicitly invalidates analysis");
}

void callbackAllocationAudit() {
    LoudnessMeterProcessor processor;
    setChannels(processor, 2);
    FakePlayHead head;
    processor.setPlayHead(&head);
    processor.prepareToPlay(48000, 512);
    juce::AudioBuffer<float> floats(2, 512);
    juce::AudioBuffer<double> doubles(2, 512);
    floats.clear(); doubles.clear();
    juce::MidiBuffer midi;
    // Warm host/JUCE lazy paths before arming the current-thread global allocation guard.
    processor.processBlock(floats, midi);
    head.position += 512;
    allocation_probe::count = 0;
    allocation_probe::active = true;
    processor.processBlock(floats, midi);
    head.position += 512;
    processor.processBlock(doubles, midi);
    head.position += 512;
    processor.processBlockBypassed(floats, midi);
    allocation_probe::active = false;
    check(allocation_probe::count == 0,
          "float/double/bypass callbacks allocate no memory on the processing thread");
}

void multipleEditorsRestoreScale() {
    LoudnessMeterProcessor processor;
    processor.setEditorScale(1.25f);
    std::unique_ptr<juce::AudioProcessorEditor> first(processor.createEditor());
    std::unique_ptr<juce::AudioProcessorEditor> second(processor.createEditor());
    const int expectedWidth = juce::roundToInt(LoudnessMeterEditor::designWidth * 1.25f);
    const int expectedHeight = juce::roundToInt(LoudnessMeterEditor::designHeight * 1.25f);
    check(first != nullptr && second != nullptr && first->getWidth() == expectedWidth
              && first->getHeight() == expectedHeight && second->getWidth() == expectedWidth
              && second->getHeight() == expectedHeight,
          "multiple editors independently restore the persisted scale");
    check(processor.editorScale() == 1.25f,
          "constructing hidden editors does not rewrite persisted scale");
}

void restartTransportLifecycle() {
    LoudnessMeterProcessor processor;
    setChannels(processor, 1);
    FakePlayHead head;
    processor.setPlayHead(&head);
    processor.prepareToPlay(48000, 19200);
    head.position = 0;
    processConstant(processor, 1, 19200, 0.8f);
    flush(processor);
    auto snapshot = processor.getDisplaySnapshot();
    check(snapshot.loudness.capturedSamples == 19200 && snapshot.loudness.momentaryValid,
          "playing capture reaches first M endpoint");
    near(snapshot.peaks.channels[0].samplePeak, 0.8, 1e-7, "initial peak hold");
    const auto firstGeneration = snapshot.loudness.generation;

    head.playing = false;
    head.position = 19200;
    processConstant(processor, 1, 4800, 2.0f);
    flush(processor);
    snapshot = processor.getDisplaySnapshot();
    check(snapshot.waiting && snapshot.loudness.capturedSamples == 19200,
          "stop freezes completed display and duration");
    near(snapshot.peaks.channels[0].samplePeak, 0.8, 1e-7,
         "stopped input is not measured");

    head.playing = true;
    head.position = 19200;
    processConstant(processor, 1, 19200, 0.25f);
    flush(processor);
    snapshot = processor.getDisplaySnapshot();
    check(snapshot.loudness.generation > firstGeneration
              && snapshot.loudness.capturedSamples == 19200,
          "stopped-to-playing resets before first new sample");
    near(snapshot.peaks.channels[0].samplePeak, 0.25, 1e-7,
         "restart cleared the prior peak hold");
    const auto restartGeneration = snapshot.loudness.generation;
    head.position = 38400;
    processConstant(processor, 1, 4800, 0.2f);
    flush(processor);
    snapshot = processor.getDisplaySnapshot();
    check(snapshot.loudness.generation == restartGeneration
              && snapshot.loudness.capturedSamples == 24000,
          "repeated playing flags do not reset capture");
}

void manualTransportAndZeroBlocks() {
    LoudnessMeterProcessor processor;
    setChannels(processor, 1);
    FakePlayHead head;
    processor.setPlayHead(&head);
    processor.prepareToPlay(48000, 4800);
    processConstant(processor, 1, 4800, 0.6f);
    head.position = 4800;
    processor.requestPause();
    processConstant(processor, 1, 4800, 1.5f);
    flush(processor);
    auto snapshot = processor.getDisplaySnapshot();
    check(snapshot.paused && snapshot.loudness.capturedSamples == 4800,
          "manual pause freezes capture");
    near(snapshot.peaks.channels[0].samplePeak, 0.6, 1e-7,
         "manual-pause input is not measured");

    head.playing = false;
    head.position = 9600;
    processConstant(processor, 1, 64, 1.0f);
    head.playing = true;
    head.position = 9600;
    processConstant(processor, 1, 64, 1.0f);
    flush(processor);
    snapshot = processor.getDisplaySnapshot();
    check(snapshot.paused && snapshot.loudness.capturedSamples == 0
              && !snapshot.peaks.channels[0].samplePeakValid,
          "host restart resets while preserving manual pause");

    processor.requestStart();
    head.position = 9664;
    processConstant(processor, 1, 4800, 0.25f);
    flush(processor);
    snapshot = processor.getDisplaySnapshot();
    check(!snapshot.paused && snapshot.loudness.capturedSamples == 4800,
          "manual start clears and resumes when host plays");

    processor.requestPause();
    processor.requestReset();
    juce::AudioBuffer<float> zero(1, 0);
    juce::MidiBuffer midi;
    processor.processBlock(zero, midi);
    flush(processor);
    snapshot = processor.getDisplaySnapshot();
    check(snapshot.paused && snapshot.loudness.capturedSamples == 0,
          "zero callback applies reset and preserves pause without samples");
}

void availabilityContinuousSeekAndBypass() {
    LoudnessMeterProcessor processor;
    setChannels(processor, 1);
    FakePlayHead head;
    head.available = false;
    processor.setPlayHead(&head);
    processor.prepareToPlay(48000, 19200);
    processConstant(processor, 1, 4800, 0.5f);
    flush(processor);
    auto snapshot = processor.getDisplaySnapshot();
    check(snapshot.noTransport && snapshot.loudness.capturedSamples == 4800,
          "unknown playhead uses explicit continuous fallback");

    head.available = true;
    head.playing = true;
    head.position = 100;
    processConstant(processor, 1, 4800, 0.2f);
    flush(processor);
    snapshot = processor.getDisplaySnapshot();
    check(!snapshot.noTransport && snapshot.loudness.capturedSamples == 4800,
          "unknown-to-known transport starts a fresh generation");
    near(snapshot.peaks.channels[0].samplePeak, 0.2, 1e-7,
         "availability reset precedes new samples");
    head.positionAvailable = false;
    processConstant(processor, 1, 4800, 0.2f);
    flush(processor);
    snapshot = processor.getDisplaySnapshot();
    check(snapshot.positionUnavailable && snapshot.loudness.capturedSamples == 9600,
          "missing sample position does not fabricate seek coverage");
    head.available = false;
    processConstant(processor, 1, 4800, 0.3f);
    flush(processor);
    snapshot = processor.getDisplaySnapshot();
    check(snapshot.noTransport && snapshot.loudness.capturedSamples == 4800,
          "known-to-unknown transport starts fresh fallback capture");

    setActual(processor, loudness::params::captureMode, 1);
    head.available = true;
    head.playing = false;
    head.positionAvailable = true;
    head.position = 20000;
    processConstant(processor, 1, 4800, 0.4f);
    flush(processor);
    snapshot = processor.getDisplaySnapshot();
    check(snapshot.captureMode == 1 && snapshot.capturing
              && snapshot.loudness.capturedSamples == 4800,
          "Continuous mode captures while transport is stopped");
    head.playing = true;
    head.position = 123456;
    processConstant(processor, 1, 4800, 0.35f);
    flush(processor);
    snapshot = processor.getDisplaySnapshot();
    check(snapshot.loudness.capturedSamples == 9600 && !snapshot.loudness.discontinuity,
          "Continuous mode ignores host position jumps as well as play/stop");

    setActual(processor, loudness::params::captureMode, 0);
    head.playing = true;
    head.position = 0;
    processConstant(processor, 1, 19200, 0.5f);
    flush(processor);
    snapshot = processor.getDisplaySnapshot();
    const auto generation = snapshot.loudness.generation;
    check(snapshot.loudness.momentaryValid, "pre-seek complete window");
    head.position = 50000;
    processConstant(processor, 1, 4800, 0.25f);
    flush(processor);
    snapshot = processor.getDisplaySnapshot();
    check(snapshot.loudness.generation == generation && snapshot.loudness.discontinuity
              && snapshot.loudness.capturedSamples == 24000
              && !snapshot.loudness.momentaryValid && snapshot.loudness.momentaryMaxValid,
          "seek keeps global capture but restarts local windows");
    std::vector<loudness::Core::Point> history;
    processor.copyHistory(history);
    check(std::any_of(history.begin(), history.end(), [](const auto& point) {
        return point.gapBefore;
    }), "history exposes seek gap marker");

    head.position = 54800;
    processConstant(processor, 1, 4800, 0.4f);
    head.position = 59600;
    processConstant(processor, 1, 4800, 1.8f, true);
    flush(processor);
    snapshot = processor.getDisplaySnapshot();
    check(snapshot.bypassed && !snapshot.capturing,
          "host bypass freezes and marks analysis");
    check(snapshot.peaks.channels[0].samplePeak < 1.0,
          "bypassed high input is not measured");
    head.position = 64400;
    processConstant(processor, 1, 4800, 0.15f);
    flush(processor);
    snapshot = processor.getDisplaySnapshot();
    check(!snapshot.bypassed && snapshot.loudness.capturedSamples == 4800,
          "return from bypass resets before measurement");
    near(snapshot.peaks.channels[0].samplePeak, 0.15, 1e-7,
         "return-from-bypass peak starts fresh");
}

void stateRoundTripAndRestoreReset() {
    LoudnessMeterProcessor source;
    setActual(source, loudness::params::captureMode, 1);
    source.setEditorScale(1.6f);
    juce::MemoryBlock state;
    source.getStateInformation(state);
    const auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(),
                                                             static_cast<int>(state.getSize()));
    check(xml != nullptr && xml->getIntAttribute("version", 0) == 1,
          "state schema version 1");
    const auto savedTree = juce::ValueTree::fromXml(*xml);
    check(savedTree.getNumChildren() == 1
              && findParameter(savedTree, loudness::params::captureMode).isValid(),
          "state contains only capture mode");
    near(xml->getDoubleAttribute("editorScale", 0), 1.6, 1e-6,
         "editor scale serialized");

    LoudnessMeterProcessor restored;
    setChannels(restored, 1);
    FakePlayHead head;
    restored.setPlayHead(&head);
    restored.prepareToPlay(48000, 4800);
    processConstant(restored, 1, 4800, 0.8f);
    restored.requestPause();
    processConstant(restored, 1, 0, 0.0f);
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    processConstant(restored, 1, 0, 0.0f);
    flush(restored);
    auto snapshot = restored.getDisplaySnapshot();
    check(snapshot.loudness.capturedSamples == 0 && !snapshot.paused,
          "state restore clears capture and manual pause at boundary");
    near(actual(restored, loudness::params::captureMode), 1, 1e-6,
         "capture mode round trip");
    near(restored.editorScale(), 1.6, 1e-6, "editor scale round trip");

    auto invalidXml = juce::AudioProcessor::getXmlFromBinary(state.getData(),
                                                              static_cast<int>(state.getSize()));
    check(invalidXml != nullptr, "read state for invalid-setting test");
    auto invalidTree = juce::ValueTree::fromXml(*invalidXml);
    findParameter(invalidTree, loudness::params::captureMode).setProperty("value", 9, nullptr);
    invalidTree.setProperty("editorScale", 9, nullptr);
    invalidTree.setProperty("unknownRoot", "ignored", nullptr);
    juce::ValueTree unknown("PARAM");
    unknown.setProperty("id", "futureParameter", nullptr);
    unknown.setProperty("value", 0.75, nullptr);
    invalidTree.appendChild(unknown, nullptr);
    juce::MemoryBlock invalidState;
    if (const auto rewritten = invalidTree.createXml())
        juce::AudioProcessor::copyXmlToBinary(*rewritten, invalidState);
    restored.setStateInformation(invalidState.getData(), static_cast<int>(invalidState.getSize()));
    processConstant(restored, 1, 0, 0.0f);
    flush(restored);
    near(actual(restored, loudness::params::captureMode), 1, 1e-6,
         "invalid capture mode clamps high");
    near(restored.editorScale(), 2, 1e-6, "invalid editor scale clamps");
    juce::MemoryBlock cleaned;
    restored.getStateInformation(cleaned);
    const auto cleanedXml = juce::AudioProcessor::getXmlFromBinary(
        cleaned.getData(), static_cast<int>(cleaned.getSize()));
    check(cleanedXml != nullptr && !cleanedXml->hasAttribute("unknownRoot"),
          "unknown state properties are not retained");
    const auto cleanedTree = juce::ValueTree::fromXml(*cleanedXml);
    check(cleanedTree.getNumChildren() == 1
              && findParameter(cleanedTree, loudness::params::captureMode).isValid(),
          "cleaned state retains only capture mode");
}

void limitsQueueOfflineAndNoEditor() {
    LoudnessMeterProcessor coherent;
    setChannels(coherent, 1);
    coherent.setTestCapacities({100, 10, 8, 48000});
    coherent.prepareToPlay(48000, 480);
    for (int block = 1; block <= 20; ++block) {
        processConstant(coherent, 1, 480, 0.1f);
        flush(coherent);
        check(coherent.getDisplaySnapshot().loudness.capturedSamples
                  == static_cast<std::uint64_t>(block * 480),
              "each acknowledged flush includes preceding audio");
    }
    const auto heldFrames = coherent.getDisplaySnapshot().loudness.capturedSamples;
    coherent.releaseResources();
    check(coherent.getDisplaySnapshot().loudness.capturedSamples == heldFrames,
          "release finishes TP tail and preserves held programme duration");

    LoudnessMeterProcessor limited;
    setChannels(limited, 1);
    limited.setTestCapacities({20, 10, 8, 4800});
    FakePlayHead head;
    limited.setPlayHead(&head);
    limited.prepareToPlay(48000, 9600);
    limited.setNonRealtime(true);
    processConstant(limited, 1, 9600, 0.3f);
    flush(limited);
    auto snapshot = limited.getDisplaySnapshot();
    check(snapshot.loudness.captureLimit && snapshot.loudness.capturedSamples == 4800
              && snapshot.peaks.capturedFrames == 4800 && !snapshot.capturing,
          "processor and peaks freeze at exact capture limit");
    check(snapshot.publicationCounter != 0 && snapshot.loudnessAgeSamples <= 4800,
          "editor-facing snapshot exposes publication and data age");

    LoudnessMeterProcessor overrun;
    setChannels(overrun, 1);
    overrun.setTestCapacities({100, 10, 2, 48000});
    overrun.setPlayHead(&head);
    head.position = 0;
    overrun.prepareToPlay(48000, 14400);
    overrun.stopAnalysisWorkerForTests();
    processConstant(overrun, 1, 14400, 0.1f);
    flush(overrun);
    snapshot = overrun.getDisplaySnapshot();
    check(snapshot.loudness.analysisOverrun && !snapshot.loudness.momentaryValid,
          "processor publishes explicit queue overrun invalidity");

    LoudnessMeterProcessor unsupported;
    setChannels(unsupported, 1);
    unsupported.setTestCapacities({20, 10, 8, 2205});
    unsupported.prepareToPlay(22050, 2205);
    processConstant(unsupported, 1, 2205, 0.2f);
    flush(unsupported);
    snapshot = unsupported.getDisplaySnapshot();
    check(snapshot.loudness.unsupportedRate && snapshot.loudness.capturedSamples == 0
              && !snapshot.capturing && !snapshot.peaks.channels[0].samplePeakValid,
          "unsupported rates pass audio without claiming analysis");

    for (const double newRate : {32000.0, 352800.0, 384000.0}) {
        LoudnessMeterProcessor wide;
        setChannels(wide, 1);
        const int frames = static_cast<int>(newRate / 10);
        wide.setTestCapacities({20, 10, 8, static_cast<std::uint64_t>(frames)});
        wide.prepareToPlay(newRate, frames);
        processConstant(wide, 1, frames, 0.2f);
        flush(wide);
        snapshot = wide.getDisplaySnapshot();
        check(!snapshot.loudness.unsupportedRate
                  && snapshot.loudness.capturedSamples == static_cast<std::uint64_t>(frames)
                  && snapshot.peaks.supportedRate && snapshot.peaks.channels[0].samplePeakValid,
              "32, 352.8 and 384 kHz are analysed");
    }

    std::array<std::unique_ptr<LoudnessMeterProcessor>, 3> instances;
    for (auto& instance : instances) {
        instance = std::make_unique<LoudnessMeterProcessor>();
        setChannels(*instance, 1);
        instance->setTestCapacities({20, 10, 8, 48000});
        instance->prepareToPlay(48000, 4800);
        processConstant(*instance, 1, 4800, 0.1f);
        flush(*instance);
        check(instance->getDisplaySnapshot().loudness.capturedSamples == 4800,
              "multiple editor-free instances capture independently");
    }

    LoudnessMeterProcessor reconfigured;
    setChannels(reconfigured, 1);
    reconfigured.setTestCapacities({20, 10, 8, 48000});
    std::atomic<bool> runUi{true};
    std::atomic<bool> coherentUi{true};
    std::thread ui([&] {
        std::vector<loudness::Core::Point> history;
        while (runUi.load(std::memory_order_acquire)) {
            const auto display = reconfigured.getDisplaySnapshot();
            reconfigured.copyHistory(history);
            if (display.activeChannels != 0 && display.activeChannels != 1)
                coherentUi.store(false, std::memory_order_release);
            if (display.sampleRate != 0 && display.sampleRate != 44100
                && display.sampleRate != 48000)
                coherentUi.store(false, std::memory_order_release);
        }
    });
    std::uint64_t priorGeneration = 0;
    for (int iteration = 0; iteration < 12; ++iteration) {
        reconfigured.prepareToPlay(iteration % 2 == 0 ? 44100 : 48000, 480);
        flush(reconfigured);
        const auto generation = reconfigured.getDisplaySnapshot().loudness.generation;
        check(generation > priorGeneration, "processor generation stays monotone across reprepare");
        priorGeneration = generation;
    }
    runUi.store(false, std::memory_order_release);
    ui.join();
    const auto configured = reconfigured.getDisplaySnapshot();
    check(coherentUi.load(std::memory_order_acquire) && configured.activeChannels == 1
              && configured.sampleRate == 48000,
          "editor snapshot/history reads stay coherent across off-audio reprepare");

    LoudnessMeterProcessor fractionalRate;
    setChannels(fractionalRate, 1);
    fractionalRate.prepareToPlay(47999.6, 480);
    processConstant(fractionalRate, 1, 4800, 0.2f);
    flush(fractionalRate);
    const auto fractional = fractionalRate.getDisplaySnapshot();
    check(fractional.loudness.unsupportedRate && fractional.loudness.capturedSamples == 0
              && !fractional.capturing && !fractional.peaks.channels[0].samplePeakValid,
          "nearby fractional sample rate is rejected rather than rounded to a supported rate");
}
} // namespace

int main() {
    try {
        juce::ScopedJuceInitialiser_GUI gui;
        identityLayoutAndPayloads();
        callbackAllocationAudit();
        multipleEditorsRestoreScale();
        restartTransportLifecycle();
        manualTransportAndZeroBlocks();
        availabilityContinuousSeekAndBypass();
        stateRoundTripAndRestoreReset();
        limitsQueueOfflineAndNoEditor();
        std::cout << "Processor tests passed: transparent audio, state, transport, worker, limits.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
