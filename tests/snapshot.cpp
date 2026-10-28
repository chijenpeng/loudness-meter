#include "PluginEditor.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

namespace {
struct FakePlayHead final : juce::AudioPlayHead {
    std::int64_t position = 0;
    juce::Optional<PositionInfo> getPosition() const override {
        PositionInfo info;
        info.setIsPlaying(true);
        info.setTimeInSamples(position);
        return info;
    }
};

bool render(LoudnessMeterProcessor& processor, const juce::File& file, float scale, const juce::String& helpTitle) {
    processor.setEditorScale(scale);
    LoudnessMeterEditor editor(processor);
    editor.refreshForSnapshot();
    if (helpTitle.isNotEmpty()) editor.showHelpForSnapshot(helpTitle);
    const auto image = editor.createComponentSnapshot(editor.getLocalBounds(), true, 1.0f);
    file.getParentDirectory().createDirectory();
    juce::MemoryOutputStream encoded;
    juce::PNGImageFormat png;
    return png.writeImageToStream(image, encoded)
        && file.replaceWithData(encoded.getData(), encoded.getDataSize());
}


// Minimal RIFF/WAVE reader (PCM 16/24/32-bit and 32-bit float, up to two channels). The snapshot
// target does not link juce_audio_formats, and this tool must not change the build files.
struct WavData {
    int rate = 0, channels = 0;
    std::vector<float> samples[2];
    std::int64_t frames() const { return (std::int64_t) samples[0].size(); }
};

bool loadWav(const juce::File& file, WavData& wav) {
    juce::MemoryBlock bytes;
    if (!file.loadFileAsData(bytes) || bytes.getSize() < 12) return false;
    const auto* d = static_cast<const std::uint8_t*>(bytes.getData());
    const std::size_t size = bytes.getSize();
    const auto u16 = [&](std::size_t o) { return (int) (d[o] | (d[o + 1] << 8)); };
    const auto u32 = [&](std::size_t o) { return (std::uint32_t) d[o] | ((std::uint32_t) d[o + 1] << 8) | ((std::uint32_t) d[o + 2] << 16) | ((std::uint32_t) d[o + 3] << 24); };
    if (std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0) return false;
    int format = 0, bits = 0, channels = 0, rate = 0;
    for (std::size_t pos = 12; pos + 8 <= size;) {
        const std::size_t length = u32(pos + 4), body = pos + 8;
        if (std::memcmp(d + pos, "fmt ", 4) == 0 && body + 16 <= size) {
            format = u16(body); channels = u16(body + 2); rate = (int) u32(body + 4); bits = u16(body + 14);
            if (format == 0xfffe && length >= 26) format = u16(body + 24);
        } else if (std::memcmp(d + pos, "data", 4) == 0) {
            const std::size_t avail = juce::jmin(length, size - body);
            if (channels < 1 || channels > 2 || (format != 1 && format != 3) || bits % 8 != 0) return false;
            const std::size_t bytesPer = (std::size_t) bits / 8, frames = avail / (bytesPer * (std::size_t) channels);
            wav.rate = rate; wav.channels = channels;
            for (int c = 0; c < channels; ++c) wav.samples[c].resize(frames);
            for (std::size_t f = 0; f < frames; ++f)
                for (int c = 0; c < channels; ++c) {
                    const std::uint8_t* q = d + body + (f * (std::size_t) channels + (std::size_t) c) * bytesPer;
                    float v = 0;
                    if (format == 3 && bits == 32) { std::uint32_t u = (std::uint32_t) q[0] | ((std::uint32_t) q[1] << 8) | ((std::uint32_t) q[2] << 16) | ((std::uint32_t) q[3] << 24); std::memcpy(&v, &u, 4); }
                    else if (format == 1 && bits == 16) v = (float) (std::int16_t) (q[0] | (q[1] << 8)) / 32768.0f;
                    else if (format == 1 && bits == 24) v = (float) ((std::int32_t) (((std::uint32_t) q[0] << 8) | ((std::uint32_t) q[1] << 16) | ((std::uint32_t) q[2] << 24)) >> 8) / 8388608.0f;
                    else if (format == 1 && bits == 32) v = (float) ((double) (std::int32_t) ((std::uint32_t) q[0] | ((std::uint32_t) q[1] << 8) | ((std::uint32_t) q[2] << 16) | ((std::uint32_t) q[3] << 24)) / 2147483648.0);
                    else return false;
                    wav.samples[c][f] = v;
                }
            return rate > 0;
        }
        pos = body + length + (length & 1);
    }
    return false;
}

static int longestLength = 0;

// Help text must fit the 812 px, three-line help body at Inter 12.
int checkHelpFits(juce::Component& root, int& checked, juce::String& longest) {
    int failures = 0;
    for (int i = 0; i < root.getNumChildComponents(); ++i) {
        auto& child = *root.getChildComponent(i);
        failures += checkHelpFits(child, checked, longest);
        const auto& props = child.getProperties();
        if (!props.contains("help")) continue;
        ++checked;
        if (props["help"].toString().length() > longestLength) { longestLength = props["help"].toString().length(); longest = props["helpTitle"].toString(); }
        juce::AttributedString text;
        text.setWordWrap(juce::AttributedString::byWord);
        const juce::SharedResourcePointer<LoudnessEditorFonts> fonts;
        text.append(props["help"].toString(),
                    juce::Font(juce::FontOptions(fonts->inter).withHeight(12.0f)).withExtraKerningFactor(0.01f),
                    juce::Colours::black);
        juce::TextLayout layout;
        layout.createLayout(text, 812.0f);
        if (layout.getNumLines() > 3) {
            std::cerr << "Help overflows (" << layout.getNumLines() << " lines): " << props["helpTitle"].toString() << "\n";
            ++failures;
        }
    }
    return failures;
}

// Deterministic synthetic programme: incommensurate sines, small noise, level sections with
// short ramps, and a few Nyquist/4 bursts whose true peak exceeds the sample peak.
double programmeLevelDb(double t) {
    static constexpr double ends[5] = {14, 34, 52, 72, 90}, levels[5] = {-30, -20, -12, -26, -11};
    constexpr double ramp = 1.5;
    for (int i = 0; i < 4; ++i) {
        if (t < ends[i] - ramp) return levels[i];
        if (t < ends[i] + ramp) {
            const double u = (t - (ends[i] - ramp)) / (2 * ramp);
            return levels[i] + (levels[i + 1] - levels[i]) * (u * u * (3 - 2 * u));
        }
    }
    return levels[4];
}
}

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI initialise;
    juce::File output("docs/editor.png");
    juce::File wav;
    bool mono = false;
    juce::String state, helpTitle;
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const juce::String arg(argv[i]);
        if (arg == "--mono") mono = true;
        else if (arg == "--state" && i + 1 < argc) state = argv[++i];
        else if (arg == "--help-for" && i + 1 < argc) helpTitle = argv[++i];
        else if (positional++ == 0) output = juce::File::getCurrentWorkingDirectory().getChildFile(arg);
        else wav = juce::File::getCurrentWorkingDirectory().getChildFile(arg);
    }

    WavData file;
    const bool fromFile = wav != juce::File();
    if (fromFile) {
        if (!loadWav(wav, file)) { std::cerr << "Cannot read " << wav.getFullPathName() << "\n"; return 6; }
        if (file.channels < 2) mono = true;
    }

    LoudnessMeterProcessor processor;
    juce::AudioProcessor::BusesLayout layout;
    const auto channels = mono ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
    layout.inputBuses.add(channels);
    layout.outputBuses.add(channels);
    if (!processor.setBusesLayout(layout)) return 2;
    FakePlayHead playHead;
    processor.setPlayHead(&playHead);
    const double seconds = state == "empty" ? 0.2 : state == "silence" ? 10 : state == "long" ? 660
                         : state == "unsupported" ? 5 : state.isEmpty() ? 90 : 20;
    const int rate = fromFile ? file.rate : (state == "unsupported" ? 22050 : 48000);
    constexpr int block = 1024;
    const std::int64_t total = fromFile ? file.frames() : (std::int64_t) (rate * seconds);
    processor.prepareToPlay(rate, block);
    const int numChannels = mono ? 1 : 2;
    juce::AudioBuffer<float> buffer(numChannels, block);
    juce::MidiBuffer midi;

    std::mt19937 rng(20261028u);
    constexpr double twoPi = juce::MathConstants<double>::twoPi;
    constexpr double freqs[4] = {61.0, 233.0, 997.0, 2711.0}, amps[4] = {0.45, 0.30, 0.18, 0.07};
    double phases[2][4] = {{0, 0, 0, 0}, {0.4, 1.1, 0.7, 2.0}};
    const std::int64_t burstAt[4] = {(std::int64_t) rate * 20, (std::int64_t) rate * 41, (std::int64_t) rate * 60, (std::int64_t) rate * 80};

    std::int64_t lastFlush = 0;
    for (std::int64_t done = 0; done < total;) {
        const int count = (int) juce::jmin<std::int64_t>(block, total - done);
        buffer.setSize(numChannels, count, false, false, true);
        if (state == "silence") buffer.clear();
        else if (fromFile) {
            for (int ch = 0; ch < numChannels; ++ch)
                std::memcpy(buffer.getWritePointer(ch), file.samples[ch].data() + done, sizeof(float) * (std::size_t) count);
        } else {
            for (int sample = 0; sample < count; ++sample) {
                const std::int64_t at = done + sample;
                const double t = std::fmod((double) at / rate, 90.0);
                const double wobble = 1.0 + 0.12 * std::sin(twoPi * 0.13 * t);
                const double gain = std::pow(10.0, programmeLevelDb(t) / 20.0) * wobble;
                for (int ch = 0; ch < numChannels; ++ch) {
                    double value = 0.0;
                    for (int k = 0; k < 4; ++k) {
                        value += amps[k] * std::sin(phases[ch][k]);
                        phases[ch][k] += twoPi * freqs[k] * (ch == 0 ? 1.0 : 1.003) / rate;
                    }
                    const double noise = ((double) (rng() >> 8) / 16777216.0 - 0.5) * 0.02;
                    double out = gain * (value + noise) * (ch == 0 ? 1.0 : 0.85);
                    for (const auto start : burstAt) {
                        const std::int64_t n = at - start;
                        if (n >= 0 && n < 128) {
                            const double window = 0.5 - 0.5 * std::cos(twoPi * (double) n / 127.0);
                            out += 0.7 * (ch == 0 ? 1.0 : 0.8) * window * std::sin(twoPi * 0.25 * (double) n + juce::MathConstants<double>::pi / 4);
                        }
                    }
                    buffer.setSample(ch, sample, (float) out);
                }
            }
        }
        processor.processBlock(buffer, midi);
        done += count;
        playHead.position = done;
        if (done - lastFlush >= (std::int64_t) rate * 2) { processor.flushAnalysisForTests(); lastFlush = done; }
    }
    if (state == "invalid") {
        buffer.setSample(0, 0, std::numeric_limits<float>::quiet_NaN());
        processor.processBlock(buffer, midi);
        playHead.position += buffer.getNumSamples();
    } else if (state == "paused") {
        processor.requestPause();
        for (int i = 0; i < 8; ++i) { processor.processBlock(buffer, midi); playHead.position += buffer.getNumSamples(); }
    }
    if (!processor.flushAnalysisForTests()) return 3;
    std::vector<loudness::Core::Point> history;
    processor.copyHistory(history);
    if (history.empty() && state != "empty" && state != "unsupported") return 5;
    std::cout << "Measured history points: " << history.size();
    if (!history.empty())
        std::cout << ", " << (double) history.front().endSample / rate << " s to " << (double) history.back().endSample / rate << " s";
    std::cout << "\n";
    if (!render(processor, output, 1.0f, helpTitle)
        || !render(processor, output.getSiblingFile(output.getFileNameWithoutExtension() + "-min.png"), 0.75f, helpTitle)
        || !render(processor, output.getSiblingFile(output.getFileNameWithoutExtension() + "-max.png"), 2.0f, helpTitle)) return 4;
    std::cout << "Rendered " << (mono ? "mono" : "stereo") << " " << (double) total / rate << " s"
              << (fromFile ? " file" : " synthetic programme") << " at 0.75x, 1x and 2x\n";
    {
        LoudnessMeterEditor editor(processor);
        int checked = 0;
        juce::String longest;
        const int failures = checkHelpFits(editor, checked, longest);
        std::cout << "Help texts checked: " << checked << ", overflowing: " << failures << ", longest: " << longest << "\n";
        if (failures != 0) return 7;
    }
    return 0;
}
