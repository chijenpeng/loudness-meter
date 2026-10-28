#pragma once

#include "PluginProcessor.h"

struct LoudnessEditorFonts {
    LoudnessEditorFonts();
    juce::Typeface::Ptr inter, mono;
};

class LoudnessMeterEditor final : public juce::AudioProcessorEditor,
                                  private juce::Timer {
public:
    static constexpr int designWidth = 860;
    static constexpr int designHeight = 526;

    explicit LoudnessMeterEditor(LoudnessMeterProcessor&);
    ~LoudnessMeterEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void refreshForSnapshot();
    void showHelpForSnapshot(const juce::String& helpTitle);

private:
    class View;
    void timerCallback() override;

    LoudnessMeterProcessor& owner_;
    juce::SharedResourcePointer<LoudnessEditorFonts> fonts_;
    std::unique_ptr<View> view_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LoudnessMeterEditor)
};
