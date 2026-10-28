#pragma once

#include "LoudnessCore.h"
#include "PeakAnalyzer.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace loudness::params {
inline constexpr auto captureMode = "captureMode";
} // namespace loudness::params

class LoudnessMeterProcessor final : public juce::AudioProcessor {
public:
    struct DisplaySnapshot {
        loudness::Core::Summary loudness;
        loudness::PeakAnalyzer::Result peaks;
        int captureMode = 0;
        bool noTransport = true;
        bool positionUnavailable = true;
        bool paused = false;
        bool waiting = false;
        bool bypassed = false;
        bool capturing = false;
        double sampleRate = 0;
        int activeChannels = 0;
        std::uint64_t loudnessAgeSamples = 0;
        std::uint64_t publicationCounter = 0;
    };

    LoudnessMeterProcessor();
    ~LoudnessMeterProcessor() override;

    void prepareToPlay(double sampleRate, int maximumBlockSize) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlock(juce::AudioBuffer<double>&, juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<double>&, juce::MidiBuffer&) override;

    bool supportsDoublePrecisionProcessing() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    void requestReset() noexcept;
    void requestPause() noexcept;
    void requestStart() noexcept;
    DisplaySnapshot getDisplaySnapshot() const;
    void copyHistory(std::vector<loudness::Core::Point>& destination) const;
    bool flushAnalysisForTests(); // off audio thread; deterministic worker checkpoint

    juce::AudioProcessorValueTreeState& parameters() noexcept { return apvts_; }
    const juce::AudioProcessorValueTreeState& parameters() const noexcept { return apvts_; }
    float editorScale() const noexcept { return editorScale_.load(std::memory_order_acquire); }
    void setEditorScale(float) noexcept;

#if LOUDNESS_METER_TESTING
    void setTestCapacities(loudness::Core::Capacities capacities) noexcept { testCapacities_ = capacities; }
    void stopAnalysisWorkerForTests();
#endif

private:
    enum Command : std::uint32_t { resetCommand = 1, pauseCommand = 2, startCommand = 4 };
    enum Status : std::uint32_t {
        noTransportStatus = 1,
        positionUnavailableStatus = 2,
        pausedStatus = 4,
        waitingStatus = 8,
        bypassedStatus = 16,
        capturingStatus = 32
    };
    struct HostState {
        bool known = false;
        bool playing = false;
        bool positionKnown = false;
        std::int64_t position = 0;
    };
    struct AudioState {
        std::uint64_t generation = 0;
        loudness::PeakAnalyzer::Result peaks;
        std::uint32_t status = 0;
        int captureMode = 0;
    };
    struct AtomicAudioState {
        std::atomic<std::uint64_t> sequence{0};
        std::atomic<std::uint64_t> generation{0};
        std::array<std::atomic<std::uint64_t>, 2> samplePeak{};
        std::array<std::atomic<std::uint64_t>, 2> truePeak{};
        std::atomic<std::uint64_t> capturedFrames{0};
        std::atomic<std::uint32_t> peakFlags{0};
        std::atomic<std::uint32_t> status{0};
        std::atomic<std::uint32_t> captureMode{0};
    };

    static juce::AudioProcessorValueTreeState::ParameterLayout makeParameters();
    static std::uint64_t doubleBits(double) noexcept;
    static double bitsDouble(std::uint64_t) noexcept;
    HostState readHostState() const;
    void resetCapture() noexcept;
    void finishCaptureSegment() noexcept;
    void updateHostTracking(const HostState&, int blockSamples) noexcept;
    void publishAudioState() noexcept;
    bool readAudioState(AudioState&) const noexcept;

    template <typename Sample>
    void process(juce::AudioBuffer<Sample>&, bool bypassed) noexcept;

    void startWorker();
    void stopWorker();
    void workerLoop();
    bool drainAndPublish();

    juce::AudioProcessorValueTreeState apvts_;
    std::atomic<float>* captureModeParameter_ = nullptr;
    std::atomic<float> editorScale_{1};
    std::atomic<std::uint32_t> pendingCommands_{0};
    std::atomic<bool> stateRestoreInProgress_{false};
    std::atomic<std::uint64_t> stateEpoch_{0};

    loudness::Core core_;
    loudness::PeakAnalyzer peaks_;
    loudness::Core::Capacities testCapacities_{};
    std::size_t historyCapacity_ = loudness::Core::maxHistory;
    double preparedSampleRate_ = 0; // guarded by displayMutex_
    int preparedChannels_ = 0;      // guarded by displayMutex_
    bool prepared_ = false;
    bool analysisSupported_ = false;

    // Audio-thread state.
    bool manualPaused_ = false;
    bool transportInitialised_ = false;
    bool transportKnown_ = false;
    bool lastPlaying_ = false;
    bool expectedPositionValid_ = false;
    std::int64_t expectedPosition_ = 0;
    bool capturing_ = false;
    bool bypassed_ = false;
    bool restoreBlocked_ = false;
    int lastCaptureMode_ = 0;
    std::uint64_t lastStateEpoch_ = 0;
    std::uint32_t currentStatus_ = noTransportStatus | positionUnavailableStatus;

    AtomicAudioState audioPublication_;
    AudioState workerAudioState_{};
    mutable std::mutex displayMutex_;
    DisplaySnapshot displaySnapshot_{};

    std::thread worker_;
    std::atomic<bool> workerExit_{false};
    std::atomic<std::uint64_t> flushRequested_{0};
    std::atomic<std::uint64_t> flushCompleted_{0};
    mutable std::mutex workerWaitMutex_;
    std::condition_variable workerWake_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LoudnessMeterProcessor)
};
