#include "PluginEditor.h"

#include <BinaryData.h>

#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace {
namespace Theme {
const juce::Colour background{0xfffefafb}, panel{0xfff8eff1}, field{0xffecdfe3};
const juce::Colour text{0xff1d1519}, label{0xff6f6266}, pale{0xffb03656};
const juce::Colour accent{0xffd95f7c}, clipping{0xffc2410c};

juce::Font font(const juce::Typeface::Ptr& face, float height, float tracking = 0.0f) {
    return juce::Font(juce::FontOptions(face).withHeight(height)).withExtraKerningFactor(tracking);
}
juce::Font labelFont(float h = 11.0f) { return font(juce::SharedResourcePointer<LoudnessEditorFonts>()->inter, h, 0.09f); }
juce::Font bodyFont(float h = 12.0f) { return font(juce::SharedResourcePointer<LoudnessEditorFonts>()->inter, h, 0.01f); }
juce::Font titleFont(float h = 18.0f) { return font(juce::SharedResourcePointer<LoudnessEditorFonts>()->inter, h, 0.18f); }
juce::Font valueFont(float h = 12.0f) { return font(juce::SharedResourcePointer<LoudnessEditorFonts>()->mono, h); }
int widthOf(const juce::Font& f, const juce::String& s) {
    return juce::roundToInt(std::ceil(juce::GlyphArrangement::getStringWidth(f, s)));
}
}

// Design-coordinate geometry (editor is 860 x 526; everything scales together).
namespace Layout {
constexpr int margin = 24, contentRight = 836, contentWidth = 812;
constexpr int laneY = 58, laneH = 18;
constexpr int primaryY = 96, primaryH = 46, primaryW = 203;
constexpr int secondaryY = 156, secondaryH = 36;
constexpr int graphY = 210, graphW = 762, graphH = 236;
constexpr int plotLeft = 36, plotRight = 14, plotTop = 12, plotBottom = 20;
constexpr int barM = 802, barS = 824, barW = 12, barCompW = 22;
constexpr int helpY = 462, helpH = 54;
constexpr int optionGap = 10;
}

constexpr float minLufs = -60.0f;

void setHelp(juce::Component& component, const juce::String& title, const juce::String& body) {
    component.getProperties().set(juce::Identifier("helpTitle"), title.toUpperCase());
    component.getProperties().set(juce::Identifier("help"), body);
}

const char* const defaultHelpTitle = "LOUDNESS METER";
const char* const defaultHelpBody =
    "Transparent mono/stereo loudness measurement following ITU-R BS.1770-5 and EBU Tech 3341/3342. "
    "Hover over a readout, meter or control to see what it measures.";

juce::String dash() { return juce::String::fromUTF8("\xe2\x80\x94"); }

// Loudness-style number: invalid is a dash, silence is minus infinity.
juce::String numberText(double value, bool valid) {
    if (!valid) return dash();
    if (!std::isfinite(value)) return juce::String("-") + juce::String::fromUTF8("\xe2\x88\x9e");
    return juce::String(value, 1);
}

double peakDb(double amplitude) { return loudness::PeakAnalyzer::toDecibels(amplitude); }

float yForLufs(double lufs, float top, float height) {
    const float clamped = std::isnan(lufs) ? minLufs : (float) juce::jlimit((double) minLufs, 0.0, lufs);
    return top + (-clamped / -minLufs) * height;
}

// Draws text with an explicit baseline so mixed font sizes line up.
float drawAtBaseline(juce::Graphics& g, const juce::Font& font, const juce::String& text, float x, float baseline) {
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText(font, text, x, baseline);
    glyphs.draw(g);
    return juce::GlyphArrangement::getStringWidth(font, text);
}

float baselineIn(const juce::Font& font, int rowTop, int rowHeight) {
    return (float) rowTop + ((float) rowHeight - font.getHeight()) * 0.5f + font.getAscent();
}

struct Run {
    juce::String text;
    juce::Colour colour;
};

juce::String keyOf(const std::vector<Run>& runs) {
    juce::String key;
    for (const auto& r : runs) key << r.text << (int) r.colour.getARGB() << '|';
    return key;
}

class FlatLookAndFeel final : public juce::LookAndFeel_V4 {
public:
    void drawCornerResizer(juce::Graphics& g, int w, int h, bool over, bool dragging) override {
        g.setColour(Theme::label.withAlpha(over || dragging ? 0.8f : 0.3f));
        for (int d : {5, 9, 13}) g.drawLine((float) w - d, (float) h - 2, (float) w - 2, (float) h - d, 1.0f);
    }
};

// ----- Capture mode: label + plain-text options (1176 OptionRow grammar) -----
class OptionRow final : public juce::Component {
public:
    OptionRow(juce::RangedAudioParameter& parameter, juce::String labelText, juce::StringArray optionList, const juce::String& help)
        : label_(labelText.toUpperCase()), options_(std::move(optionList)),
          attachment_(parameter, [this](float value) { selected_ = juce::roundToInt(value); repaint(); }) {
        labelColumn_ = Theme::widthOf(Theme::labelFont(), label_) + 18;
        attachment_.sendInitialUpdate();
        setHelp(*this, label_, help);
    }
    int preferredWidth() const { const auto r = ranges(); return r.empty() ? labelColumn_ : r.back().getEnd(); }
    void paint(juce::Graphics& g) override {
        g.setFont(Theme::labelFont());
        g.setColour(Theme::label);
        g.drawText(label_, 0, 0, labelColumn_, getHeight(), juce::Justification::centredLeft, false);
        const auto r = ranges();
        for (int i = 0; i < options_.size(); ++i) {
            g.setColour(i == selected_ ? Theme::pale : (i == hovered_ ? Theme::text : Theme::label.withAlpha(0.8f)));
            g.drawText(options_[i].toUpperCase(), r[(size_t) i].getStart(), 0, r[(size_t) i].getLength() + 4, getHeight(),
                       juce::Justification::centredLeft, false);
        }
    }
    void mouseMove(const juce::MouseEvent& e) override { hovered_ = optionAt(e.x); repaint(); }
    void mouseExit(const juce::MouseEvent&) override { hovered_ = -1; repaint(); }
    void mouseDown(const juce::MouseEvent& e) override {
        if (const int i = optionAt(e.x); i >= 0) attachment_.setValueAsCompleteGesture((float) i);
    }
private:
    std::vector<juce::Range<int>> ranges() const {
        std::vector<juce::Range<int>> result;
        const auto font = Theme::labelFont();
        int x = labelColumn_;
        for (const auto& option : options_) {
            const int w = Theme::widthOf(font, option.toUpperCase());
            result.push_back({x, x + w});
            x += w + Layout::optionGap;
        }
        return result;
    }
    int optionAt(int x) const {
        const auto r = ranges();
        for (int i = 0; i < (int) r.size(); ++i)
            if (r[(size_t) i].expanded(Layout::optionGap / 2).contains(x)) return i;
        return -1;
    }
    juce::String label_;
    juce::StringArray options_;
    juce::ParameterAttachment attachment_;
    int labelColumn_ = 0, selected_ = 0, hovered_ = -1;
};

// ----- One plain-text command (Reset / Pause / Start), padded by half an option gap -----
class ActionText final : public juce::Component {
public:
    static constexpr int pad = Layout::optionGap / 2;
    ActionText(juce::String name, std::function<void()> action, const juce::String& help)
        : name_(name.toUpperCase()), action_(std::move(action)) {
        setHelp(*this, name_, help);
    }
    int textWidth() const { return Theme::widthOf(Theme::labelFont(), name_); }
    int preferredWidth() const { return textWidth() + 2 * pad; }
    void setHighlighted(bool on) { if (on != highlighted_) { highlighted_ = on; repaint(); } }
    void paint(juce::Graphics& g) override {
        g.setFont(Theme::labelFont());
        g.setColour(highlighted_ ? Theme::pale : (hovered_ ? Theme::text : Theme::label.withAlpha(0.8f)));
        g.drawText(name_, pad, 0, textWidth() + 4, getHeight(), juce::Justification::centredLeft, false);
    }
    void mouseEnter(const juce::MouseEvent&) override { hovered_ = true; repaint(); }
    void mouseExit(const juce::MouseEvent&) override { hovered_ = false; repaint(); }
    void mouseDown(const juce::MouseEvent&) override { action_(); }
private:
    juce::String name_;
    std::function<void()> action_;
    bool hovered_ = false, highlighted_ = false;
};

// ----- Status words, right-aligned; normal in label colour, faults in clipping -----
class StatusText final : public juce::Component {
public:
    StatusText() {
        setHelp(*this, "Capture status",
                "CAPTURING measuring. READY no capture running. WAITING host stopped. PAUSED held by Pause. BYPASSED not captured; "
                "a new capture starts when bypass ends. NO TRANSPORT, POSITION UNAVAILABLE: host gives no play state or position. "
                "DISCONTINUITY: seek or gap. Red words are faults: UNSUPPORTED RATE, INVALID INPUT, CAPTURE LIMIT (24 h), "
                "ANALYSIS LIMIT (queue or statistics full), ANALYSIS LAG (over 0.5 s behind).");
    }
    void setRuns(std::vector<Run> runs) {
        const auto key = keyOf(runs);
        if (key == key_) return;
        key_ = key; runs_ = std::move(runs); repaint();
    }
    void paint(juce::Graphics& g) override {
        const auto font = Theme::valueFont(11.0f);
        const auto sep = juce::String::fromUTF8("  \xc2\xb7  ");
        const auto ellipsis = juce::String::fromUTF8("\xe2\x80\xa6");
        const float sepW = juce::GlyphArrangement::getStringWidth(font, sep);
        const float ellW = juce::GlyphArrangement::getStringWidth(font, ellipsis);
        std::vector<float> widths;
        for (const auto& r : runs_) widths.push_back(juce::GlyphArrangement::getStringWidth(font, r.text));
        const auto widthFrom = [&](size_t first) {
            float w = first > 0 ? ellW + sepW : 0.0f;
            for (size_t i = first; i < widths.size(); ++i) w += widths[i] + (i > first ? sepW : 0.0f);
            return w;
        };
        size_t first = 0;
        while (first + 1 < runs_.size() && widthFrom(first) > (float) getWidth()) ++first;
        float x = (float) getWidth() - widthFrom(first);
        const float baseline = baselineIn(font, 0, getHeight());
        g.setColour(Theme::label);
        if (first > 0) x += drawAtBaseline(g, font, ellipsis, x, baseline) + drawAtBaseline(g, font, sep, x + ellW, baseline);
        for (size_t i = first; i < runs_.size(); ++i) {
            if (i > first) { g.setColour(Theme::label.withAlpha(0.7f)); x += drawAtBaseline(g, font, sep, x, baseline); }
            g.setColour(runs_[i].colour);
            x += drawAtBaseline(g, font, runs_[i].text, x, baseline);
        }
    }
private:
    std::vector<Run> runs_;
    juce::String key_;
};

// ----- Duration, right-aligned in the option lane -----
class DurationText final : public juce::Component {
public:
    DurationText() {
        setHelp(*this, "Duration", "Captured audio time as hh:mm:ss. It stops while paused, bypassed or waiting for the host, and restarts at zero with every new capture.");
    }
    static int preferredWidth() { return Theme::widthOf(Theme::valueFont(12.0f), "00:00:00") + 2; }
    void set(const juce::String& text) { if (text != text_) { text_ = text; repaint(); } }
    void paint(juce::Graphics& g) override {
        const auto font = Theme::valueFont(12.0f);
        g.setColour(Theme::text);
        drawAtBaseline(g, font, text_, (float) getWidth() - juce::GlyphArrangement::getStringWidth(font, text_), baselineIn(font, 0, getHeight()));
    }
private:
    juce::String text_ = "00:00:00";
};

// ----- Primary readout: label, big number, unit (+ UNSTABLE for LRA) -----
class PrimaryReadout final : public juce::Component {
public:
    PrimaryReadout(juce::String name, juce::String unit, juce::Colour numberColour, const juce::String& help)
        : name_(name.toUpperCase()), unit_(std::move(unit)), colour_(numberColour) { setHelp(*this, name_, help); }
    void set(const juce::String& number, bool valid, bool unstable) {
        if (number == number_ && valid == valid_ && unstable == unstable_) return;
        number_ = number; valid_ = valid; unstable_ = unstable; repaint();
    }
    void paint(juce::Graphics& g) override {
        g.setColour(Theme::label); g.setFont(Theme::labelFont());
        g.drawText(name_, 0, 0, getWidth(), 16, juce::Justification::centredLeft, false);
        const auto big = Theme::valueFont(20.0f), small = Theme::valueFont(11.0f);
        const float baseline = baselineIn(big, 20, 26);
        g.setColour(valid_ ? colour_ : Theme::label);
        float x = drawAtBaseline(g, big, number_, 0.0f, baseline);
        if (!valid_) return;
        g.setColour(Theme::label);
        x += drawAtBaseline(g, small, unit_, x + 6.0f, baseline) + 6.0f;
        if (unstable_) {
            g.setFont(Theme::labelFont());
            g.drawText("UNSTABLE", juce::roundToInt(x + 8.0f), 20, getWidth() - juce::roundToInt(x + 8.0f), 26,
                       juce::Justification::centredLeft, false);
        }
    }
private:
    juce::String name_, unit_, number_ = dash();
    juce::Colour colour_;
    bool valid_ = false, unstable_ = false;
};

// ----- Secondary readout: label + one line of coloured runs -----
class SecondaryReadout final : public juce::Component {
public:
    SecondaryReadout(juce::String name, const juce::String& help) : name_(name.toUpperCase()) { setHelp(*this, name_, help); }
    void set(std::vector<Run> runs) {
        const auto key = keyOf(runs);
        if (key == key_) return;
        key_ = key; runs_ = std::move(runs); repaint();
    }
    void paint(juce::Graphics& g) override {
        g.setColour(Theme::label); g.setFont(Theme::labelFont());
        g.drawText(name_, 0, 0, getWidth(), 16, juce::Justification::centredLeft, false);
        const auto font = Theme::valueFont(12.0f);
        const float baseline = baselineIn(font, 18, 18);
        float x = 0;
        for (const auto& r : runs_) { g.setColour(r.colour); x += drawAtBaseline(g, font, r.text, x, baseline); }
    }
private:
    juce::String name_, key_;
    std::vector<Run> runs_;
};

// ----- History panel -----
class HistoryGraph final : public juce::Component {
public:
    HistoryGraph() {
        setHelp(*this, "Measured history",
                "Real 100 ms Momentary (grey) and Short-term (rose) readings against capture time, 0 to -60 LUFS. The window grows "
                "from 30 s to 10 min and breaks at resets and discontinuities. The dashed I line is the current Integrated value "
                "drawn across the window, not its history.");
    }
    void setData(const std::vector<loudness::Core::Point>& points, std::uint64_t generation, double rate) {
        points_.clear();
        for (const auto& point : points) if (point.generation == generation) points_.push_back(point);
        rate_ = rate;
        repaint();
    }
    void setIntegrated(bool valid, double lufs) {
        if (valid == integratedValid_ && (!valid || juce::exactlyEqual(lufs, integrated_) || (std::isnan(lufs) && std::isnan(integrated_)))) return;
        integratedValid_ = valid; integrated_ = lufs; repaint();
    }
    void paint(juce::Graphics& g) override {
        g.setColour(Theme::panel); g.fillRect(getLocalBounds());
        const auto plot = plotArea();
        const double lastSample = points_.empty() ? 0.0 : (double) points_.back().endSample;
        const double elapsed = rate_ > 0 ? lastSample / rate_ : 0.0;
        static constexpr double windows[] = {30, 60, 120, 300, 600}, ticks[] = {5, 10, 20, 60, 120};
        int wi = 4;
        for (int i = 0; i < 5; ++i) if (windows[i] >= elapsed) { wi = i; break; }
        const double window = windows[wi], tick = ticks[wi];

        g.setFont(Theme::valueFont(9.0f));
        for (int value = 0; value >= -60; value -= 10) {
            const float y = std::round(yForLufs(value, plot.getY(), plot.getHeight()));
            g.setColour(Theme::label.withAlpha(0.18f)); g.fillRect(plot.getX(), y - 0.5f, plot.getWidth(), 1.0f);
            g.setColour(Theme::label);
            g.drawText(juce::String(value), 0, juce::roundToInt(y) - 6, Layout::plotLeft - 6, 12, juce::Justification::centredRight, false);
        }
        const int tickCount = juce::roundToInt(window / tick);
        for (int k = tickCount; k >= 1; --k) {
            const float x = std::round(plot.getRight() - (float) (k * tick / window) * plot.getWidth());
            const int seconds = juce::roundToInt(k * tick);
            const juce::String text = "-" + juce::String(seconds / 60) + ":" + juce::String(seconds % 60).paddedLeft('0', 2);
            if (k < tickCount) { g.setColour(Theme::label.withAlpha(0.10f)); g.fillRect(x - 0.5f, plot.getY(), 1.0f, plot.getHeight()); }
            g.setColour(Theme::label);
            const int w = 40;
            if (k == tickCount) g.drawText(text, juce::roundToInt(plot.getX()), juce::roundToInt(plot.getBottom()) + 5, w, 11, juce::Justification::centredLeft, false);
            else g.drawText(text, juce::roundToInt(x) - w / 2, juce::roundToInt(plot.getBottom()) + 5, w, 11, juce::Justification::centred, false);
        }
        g.setColour(Theme::label);
        g.drawText("NOW", juce::roundToInt(plot.getRight()) - 40, juce::roundToInt(plot.getBottom()) + 5, 40, 11, juce::Justification::centredRight, false);

        const float integratedY = std::round(yForLufs(integrated_, plot.getY(), plot.getHeight())) + 0.5f;
        {
            juce::Graphics::ScopedSaveState state(g);
            g.reduceClipRegion(plot.expanded(1.0f).toNearestInt());
            if (!points_.empty() && rate_ > 0) {
                const double span = window * rate_, first = lastSample - span;
                drawTrace(g, plot, first, span, true, Theme::label, 1.0f);
                drawTrace(g, plot, first, span, false, Theme::accent, 1.6f);
            }
            if (integratedValid_) {
                const float dashes[] = {4.0f, 3.0f};
                g.setColour(Theme::text.withAlpha(0.45f));
                g.drawDashedLine(juce::Line<float>(plot.getX(), integratedY, plot.getRight(), integratedY), dashes, 2, 1.0f);
            }
        }
        if (integratedValid_) {
            g.setColour(Theme::label); g.setFont(Theme::valueFont(9.0f));
            g.drawText("I", juce::roundToInt(plot.getRight()) + 2, juce::roundToInt(integratedY) - 6, 10, 12, juce::Justification::centred, false);
        }
        drawLegend(g, plot);
    }
private:
    static juce::Rectangle<float> plotArea() {
        return juce::Rectangle<float>((float) Layout::plotLeft, (float) Layout::plotTop,
                                      (float) (Layout::graphW - Layout::plotLeft - Layout::plotRight),
                                      (float) (Layout::graphH - Layout::plotTop - Layout::plotBottom));
    }
    void drawLegend(juce::Graphics& g, juce::Rectangle<float> plot) {
        const auto font = Theme::valueFont(9.0f);
        g.setFont(font);
        float x = plot.getX() + 8.0f;
        const float y = plot.getY() + 5.0f;
        const auto entry = [&](const char* name, juce::Colour colour, bool dashed) {
            if (dashed) {
                const float dashes[] = {3.0f, 2.0f};
                g.setColour(colour);
                g.drawDashedLine(juce::Line<float>(x, y + 5.5f, x + 10.0f, y + 5.5f), dashes, 2, 1.0f);
            } else { g.setColour(colour); g.fillRect(x, y + 4.5f, 10.0f, 2.0f); }
            g.setColour(Theme::label);
            const int w = Theme::widthOf(font, name);
            g.drawText(name, juce::roundToInt(x) + 14, juce::roundToInt(y), w + 2, 11, juce::Justification::centredLeft, false);
            x += 14.0f + (float) w + 10.0f;
        };
        entry("S", Theme::accent, false);
        entry("M", Theme::label, false);
        entry("I", Theme::text.withAlpha(0.45f), true);
    }
    // Per-column min/max decimation, breaking at invalid points and gaps.
    void drawTrace(juce::Graphics& g, juce::Rectangle<float> plot, double first, double span, bool momentary,
                   juce::Colour colour, float thickness) {
        path_.clear();
        bool open = false, column = false;
        int col = 0, minSeq = 0, maxSeq = 0, seq = 0;
        float minX = 0, minY = 0, maxX = 0, maxY = 0;
        const auto emit = [&](float x, float y) { if (!open) { path_.startNewSubPath(x, y); open = true; } else path_.lineTo(x, y); };
        const auto flush = [&] {
            if (!column) return;
            column = false;
            if (minSeq == maxSeq) { emit(minX, minY); return; }
            if (minSeq < maxSeq) { emit(minX, minY); emit(maxX, maxY); }
            else { emit(maxX, maxY); emit(minX, minY); }
        };
        for (const auto& p : points_) {
            const bool valid = momentary ? p.momentaryValid : p.shortValid;
            if (!valid || p.gapBefore) { flush(); open = false; if (!valid) continue; }
            if ((double) p.endSample < first) continue;
            const double power = momentary ? p.momentaryPower : p.shortPower;
            const float x = plot.getX() + (float) (((double) p.endSample - first) / span) * plot.getWidth();
            const float y = yForLufs(loudness::Core::toLufs(power), plot.getY(), plot.getHeight());
            const int c = (int) std::floor(x);
            if (!column || c != col) {
                flush();
                column = true; col = c; seq = 0;
                minX = maxX = x; minY = maxY = y; minSeq = maxSeq = 0;
            } else {
                ++seq;
                if (y < minY) { minY = y; minX = x; minSeq = seq; }
                if (y > maxY) { maxY = y; maxX = x; maxSeq = seq; }
            }
        }
        flush();
        g.setColour(colour);
        g.strokePath(path_, juce::PathStrokeType(thickness, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
    }

    std::vector<loudness::Core::Point> points_;
    juce::Path path_;
    double rate_ = 0, integrated_ = 0;
    bool integratedValid_ = false;
};

// ----- Momentary / Short-term bar sharing the graph's 0..-60 LUFS scale -----
class LevelBar final : public juce::Component {
public:
    LevelBar(juce::String shortName, juce::Colour fill, const juce::String& title, const juce::String& help)
        : name_(std::move(shortName)), fill_(fill) { setHelp(*this, title, help); }
    void set(bool valid, double lufs, bool maxValid, double maxLufs) {
        if (valid == valid_ && juce::exactlyEqual(lufs, lufs_) && maxValid == maxValid_ && juce::exactlyEqual(maxLufs, max_)) return;
        valid_ = valid; lufs_ = lufs; maxValid_ = maxValid; max_ = maxLufs; repaint();
    }
    void paint(juce::Graphics& g) override {
        const float plotH = (float) (Layout::graphH - Layout::plotTop - Layout::plotBottom);
        const float x = (float) (getWidth() - Layout::barW) * 0.5f;
        g.setColour(Theme::label.withAlpha(0.18f)); g.fillRect(x, 0.0f, (float) Layout::barW, plotH);
        if (valid_) {
            const float y = yForLufs(lufs_, 0.0f, plotH);
            g.setColour(fill_); g.fillRect(x, y, (float) Layout::barW, plotH - y);
        }
        if (maxValid_) {
            const float y = juce::jlimit(0.0f, plotH - 2.0f, yForLufs(max_, 0.0f, plotH) - 1.0f);
            g.setColour(Theme::text); g.fillRect(x, y, (float) Layout::barW, 2.0f);
        }
        g.setColour(Theme::label); g.setFont(Theme::labelFont());
        g.drawText(name_, 0, (int) plotH, getWidth(), Layout::plotBottom, juce::Justification::centred, false);
    }
private:
    juce::String name_;
    juce::Colour fill_;
    bool valid_ = false, maxValid_ = false;
    double lufs_ = 0, max_ = 0;
};
}

LoudnessEditorFonts::LoudnessEditorFonts()
    : inter(juce::Typeface::createSystemTypefaceFor(BinaryData::InterSemiBold_ttf, BinaryData::InterSemiBold_ttfSize)),
      mono(juce::Typeface::createSystemTypefaceFor(BinaryData::JetBrainsMonoMedium_ttf, BinaryData::JetBrainsMonoMedium_ttfSize)) {}

class LoudnessMeterEditor::View final : public juce::Component {
public:
    explicit View(LoudnessMeterProcessor& processor)
        : processor_(processor),
          capture_(*dynamic_cast<juce::RangedAudioParameter*>(processor.parameters().getParameter(loudness::params::captureMode)),
                   "Capture", {"Restart", "Continuous"},
                   "Restart holds results while the host is stopped, then clears everything before the first sample of the next Play; "
                   "a seek during play breaks the capture. Continuous ignores host transport and position jumps. Changing mode starts a new capture."),
          reset_("Reset", [this] { processor_.requestReset(); },
                 "Clears readings, maxima, peaks, duration and history at the next audio block and keeps capturing. "
                 "If Pause is on, the meter stays paused until Start."),
          pause_("Pause", [this] { processor_.requestPause(); },
                 "Stops measuring and holds every reading and the history; audio passes through unchanged. "
                 "Only Start resumes, with a new capture."),
          start_("Start", [this] { processor_.requestStart(); },
                 "Clears everything, leaves Pause and begins a new capture at the next eligible sample. "
                 "In Restart mode it waits, empty, while the host is stopped."),
          momentary_("Momentary", "LUFS", Theme::text,
                     "Ungated 400 ms BS.1770 loudness, updated every 100 ms after the first complete window. A dash means no value yet; minus infinity means silence."),
          shortTerm_("Short-term", "LUFS", Theme::text,
                     "Ungated 3 s BS.1770 loudness, updated every 100 ms after the first complete window. A dash means no value yet; minus infinity means silence."),
          integrated_("Integrated", "LUFS", Theme::text,
                      "Gated programme loudness of every eligible complete 400 ms block in this capture, using the BS.1770 absolute and relative gates."),
          range_("Loudness range", "LU", Theme::text,
                 "EBU Tech 3342 loudness range from gated Short-term windows. UNSTABLE is shown while the capture is under 60 seconds; "
                 "that is this product's rule for an unsettled range."),
          momentaryMax_("M max", "The highest valid 400 ms Momentary loudness measured in this capture."),
          shortMax_("S max", "The highest valid 3 s Short-term loudness measured in this capture."),
          truePeak_("True peak max", "Highest reconstructed (oversampled) peak in dBTP, held per channel for this capture. Stereo shows L and R."),
          samplePeak_("Sample peak max", "Highest absolute sample value in dBFS, held per channel for this capture. Stereo shows L and R."),
          momentaryBar_("M", Theme::label.withAlpha(0.6f), "Momentary bar",
                        "Current Momentary loudness on the same 0 to -60 LUFS scale as the history. The dark line marks M MAX."),
          shortBar_("S", Theme::accent, "Short-term bar",
                    "Current Short-term loudness on the same 0 to -60 LUFS scale as the history. The dark line marks S MAX.") {
        setLookAndFeel(&look_);
        for (auto* c : std::initializer_list<juce::Component*>{&capture_, &reset_, &pause_, &start_, &status_, &momentary_, &shortTerm_,
                                                                  &integrated_, &range_, &momentaryMax_, &shortMax_, &truePeak_,
                                                                  &samplePeak_, &duration_, &history_, &momentaryBar_, &shortBar_})
            addAndMakeVisible(c);
        setHelp(*this, defaultHelpTitle, defaultHelpBody);
        helpTitle_ = defaultHelpTitle; helpBody_ = defaultHelpBody;
        refresh(true);
    }
    ~View() override { setLookAndFeel(nullptr); }

    void refresh(bool force = false) {
        const auto next = processor_.getDisplaySnapshot();
        if (!force && next.publicationCounter == snapshot_.publicationCounter) return;
        const bool historyChanged = force || next.loudness.generation != snapshot_.loudness.generation
            || next.loudness.lastEndSample != snapshot_.loudness.lastEndSample || !juce::exactlyEqual(next.sampleRate, snapshot_.sampleRate);
        snapshot_ = next;
        if (historyChanged) {
            processor_.copyHistory(historyData_);
            history_.setData(historyData_, snapshot_.loudness.generation, snapshot_.sampleRate);
        }
        update();
    }

    void resized() override {
        using namespace Layout;
        capture_.setBounds(margin, laneY, capture_.preferredWidth(), laneH);
        ruleX_ = capture_.getRight() + 12;
        int x = ruleX_ + 1 + 12 - ActionText::pad;
        for (auto* a : {&reset_, &pause_, &start_}) { a->setBounds(x, laneY, a->preferredWidth(), laneH); x = a->getRight(); }
        const int durationW = DurationText::preferredWidth(), durationX = contentRight - durationW;
        duration_.setBounds(durationX, laneY, durationW, laneH);
        const int statusX = x - ActionText::pad + 12;
        status_.setBounds(statusX, laneY, durationX - 16 - statusX, laneH);
        int i = 0;
        for (auto* r : {&momentary_, &shortTerm_, &integrated_, &range_}) r->setBounds(margin + i++ * primaryW, primaryY, primaryW, primaryH);
        i = 0;
        for (auto* r : {&momentaryMax_, &shortMax_, &truePeak_, &samplePeak_}) r->setBounds(margin + i++ * primaryW, secondaryY, primaryW, secondaryH);
        history_.setBounds(margin, graphY, graphW, graphH);
        const int barTop = graphY + plotTop, barHeight = graphH - plotTop;
        momentaryBar_.setBounds(barM + barW / 2 - barCompW / 2, barTop, barCompW, barHeight);
        shortBar_.setBounds(barS + barW / 2 - barCompW / 2, barTop, barCompW, barHeight);
        helpArea_ = {margin, helpY, contentWidth, helpH};
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(Theme::background);
        g.setColour(Theme::pale);
        drawAtBaseline(g, Theme::titleFont(), "LOUDNESS METER", (float) Layout::margin, 33.0f);
        g.setColour(Theme::label.withAlpha(0.38f));
        g.fillRect(ruleX_, Layout::laneY + 4, 1, 10);
        g.setColour(Theme::pale); g.setFont(Theme::labelFont());
        g.drawText(helpTitle_, helpArea_.withHeight(16), juce::Justification::centredLeft, false);
        g.setColour(Theme::label); g.setFont(Theme::bodyFont());
        g.drawFittedText(helpBody_, helpArea_.withTrimmedTop(18), juce::Justification::topLeft, 3, 1.0f);
    }

    // Walks up from the component under the mouse to the first ancestor that carries help.
    void showHelp(juce::Component* component) {
        const juce::Identifier helpKey("help"), titleKey("helpTitle");
        juce::String title = defaultHelpTitle, body = defaultHelpBody;
        for (auto* c = component; c != nullptr; c = c->getParentComponent()) {
            if (c->getProperties().contains(helpKey)) {
                title = c->getProperties()[titleKey].toString(); body = c->getProperties()[helpKey].toString(); break;
            }
            if (c == this) break;
        }
        if (title != helpTitle_ || body != helpBody_) { helpTitle_ = title; helpBody_ = body; repaint(helpArea_); }
    }

    void showHelpForTitle(const juce::String& title) {
        const auto wanted = title.toUpperCase();
        const std::function<juce::Component*(juce::Component&)> find = [&](juce::Component& c) -> juce::Component* {
            for (auto* child : c.getChildren()) {
                if (child->getProperties()["helpTitle"].toString() == wanted) return child;
                if (auto* found = find(*child)) return found;
            }
            return nullptr;
        };
        showHelp(find(*this));
    }

private:
    void update() {
        const auto& l = snapshot_.loudness;
        const juce::Colour pale = Theme::text, lab = Theme::label;
        momentary_.set(numberText(l.momentary, l.momentaryValid), l.momentaryValid, false);
        shortTerm_.set(numberText(l.shortTerm, l.shortValid), l.shortValid, false);
        integrated_.set(numberText(l.integrated, l.integratedValid), l.integratedValid, false);
        range_.set(numberText(l.range, l.rangeValid), l.rangeValid, l.rangeValid && l.lraUnstable);
        const auto lufsRuns = [&](double v, bool valid) {
            if (!valid) return std::vector<Run>{{numberText(v, false), lab}};
            return std::vector<Run>{{numberText(v, true), pale}, {" LUFS", lab}};
        };
        momentaryMax_.set(lufsRuns(l.momentaryMax, l.momentaryMaxValid));
        shortMax_.set(lufsRuns(l.shortMax, l.shortMaxValid));

        const auto& a = snapshot_.peaks.channels[0];
        const auto& b = snapshot_.peaks.channels[1];
        const bool stereo = snapshot_.activeChannels > 1;
        const auto peakRuns = [&](bool useTrue, const char* unit) {
            const auto value = [&](const auto& ch) {
                const bool valid = useTrue ? ch.truePeakValid : ch.samplePeakValid;
                return Run{numberText(peakDb(useTrue ? ch.truePeak : ch.samplePeak), valid), valid ? pale : lab};
            };
            std::vector<Run> runs;
            bool anyValid = useTrue ? a.truePeakValid : a.samplePeakValid;
            if (stereo) {
                runs = {{"L ", lab}, value(a), {"  R ", lab}, value(b)};
                anyValid = anyValid || (useTrue ? b.truePeakValid : b.samplePeakValid);
            } else runs = {value(a)};
            if (anyValid) runs.push_back({juce::String(" ") + unit, lab});
            return runs;
        };
        truePeak_.set(peakRuns(true, "dBTP"));
        samplePeak_.set(peakRuns(false, "dBFS"));

        const double seconds = snapshot_.sampleRate > 0 ? (double) l.capturedSamples / snapshot_.sampleRate : 0.0;
        const int whole = (int) seconds;
        duration_.set(juce::String::formatted("%02d:%02d:%02d", whole / 3600, (whole / 60) % 60, whole % 60));

        history_.setIntegrated(l.integratedValid, l.integrated);
        momentaryBar_.set(l.momentaryValid, l.momentary, l.momentaryMaxValid, l.momentaryMax);
        shortBar_.set(l.shortValid, l.shortTerm, l.shortMaxValid, l.shortMax);

        std::vector<Run> words;
        const auto normal = [&](const char* w) { words.push_back({w, Theme::label}); };
        const auto fault = [&](const char* w) { words.push_back({w, Theme::clipping}); };
        if (l.unsupportedRate) fault("UNSUPPORTED RATE");
        else if (snapshot_.bypassed) normal("BYPASSED");
        else if (snapshot_.paused) normal("PAUSED");
        else if (snapshot_.waiting) normal("WAITING");
        else if (snapshot_.capturing) normal("CAPTURING");
        else normal("READY");
        if (snapshot_.noTransport) normal("NO TRANSPORT");
        else if (snapshot_.positionUnavailable) normal("POSITION UNAVAILABLE");
        if (l.invalidInput) fault("INVALID INPUT");
        if (l.discontinuity) normal("DISCONTINUITY");
        if (l.captureLimit) fault("CAPTURE LIMIT");
        if (l.analysisOverrun || l.statisticsOverflow) fault("ANALYSIS LIMIT");
        if (snapshot_.capturing && snapshot_.sampleRate > 0
            && snapshot_.loudnessAgeSamples > (std::uint64_t) std::llround(snapshot_.sampleRate * 0.5))
            fault("ANALYSIS LAG");
        status_.setRuns(std::move(words));
        pause_.setHighlighted(snapshot_.paused);
    }

    LoudnessMeterProcessor& processor_;
    FlatLookAndFeel look_;
    OptionRow capture_;
    ActionText reset_, pause_, start_;
    StatusText status_;
    PrimaryReadout momentary_, shortTerm_, integrated_, range_;
    SecondaryReadout momentaryMax_, shortMax_, truePeak_, samplePeak_;
    DurationText duration_;
    HistoryGraph history_;
    LevelBar momentaryBar_, shortBar_;
    LoudnessMeterProcessor::DisplaySnapshot snapshot_;
    std::vector<loudness::Core::Point> historyData_;
    juce::Rectangle<int> helpArea_;
    juce::String helpTitle_, helpBody_;
    int ruleX_ = 0;
};

LoudnessMeterEditor::LoudnessMeterEditor(LoudnessMeterProcessor& owner)
    : AudioProcessorEditor(owner), owner_(owner), view_(std::make_unique<View>(owner)) {
    setOpaque(true);
    addAndMakeVisible(*view_);
    const float scale = owner_.editorScale();
    setResizable(true, true);
    setResizeLimits(juce::roundToInt(designWidth * 0.75f), juce::roundToInt(designHeight * 0.75f),
                    designWidth * 2, designHeight * 2);
    getConstrainer()->setFixedAspectRatio((double) designWidth / designHeight);
    setSize(juce::roundToInt(designWidth * scale), juce::roundToInt(designHeight * scale));
    startTimerHz(30);
}
LoudnessMeterEditor::~LoudnessMeterEditor() = default;
void LoudnessMeterEditor::paint(juce::Graphics& g) { g.fillAll(Theme::background); }
void LoudnessMeterEditor::resized() {
    const float scale = juce::jmin((float) getWidth() / designWidth, (float) getHeight() / designHeight);
    view_->setBounds(0, 0, designWidth, designHeight);
    view_->setTransform(juce::AffineTransform::scale(scale));
    if (isShowing() && getPeer() != nullptr) owner_.setEditorScale(scale);
}
void LoudnessMeterEditor::timerCallback() {
    view_->refresh();
    auto* under = juce::Desktop::getInstance().getMainMouseSource().getComponentUnderMouse();
    view_->showHelp(under == view_.get() || (under != nullptr && view_->isParentOf(under)) ? under : nullptr);
}
void LoudnessMeterEditor::refreshForSnapshot() { view_->refresh(true); }
void LoudnessMeterEditor::showHelpForSnapshot(const juce::String& title) { view_->showHelpForTitle(title); }
