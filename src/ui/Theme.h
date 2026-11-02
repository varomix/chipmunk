#pragma once

#include "engine/Instrument.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace theme
{
// The window, darkest at the edges, and the panels mounted on it.
const juce::Colour background { 0xff121118 };
const juce::Colour header { 0xff1b1924 };
const juce::Colour panel { 0xff272530 };
const juce::Colour panelTop { 0xff312f3c };
const juce::Colour panelRaised { 0xff3a3847 };
const juce::Colour outline { 0xff454354 };
const juce::Colour edge { 0xff09080d };

// The black glass that graphs and traces are drawn on.
const juce::Colour screen { 0xff0c0b11 };
const juce::Colour screenRuler { 0xff17151e };

const juce::Colour text { 0xffeeecf6 };
const juce::Colour textDim { 0xff9c99b0 };

// From the logo: its purple background, the fur and the belly.
const juce::Colour accent { 0xffa985ff };
const juce::Colour tawny { 0xffec9440 };
const juce::Colour cream { 0xfff8dfb0 };
const juce::Colour trace { 0xff2fe3c0 };

const juce::Colour favorite { 0xfff5b83d };
const juce::Colour error { 0xffff6b77 };
const juce::Colour loopMarker { 0xff5de3a1 };
const juce::Colour releaseMarker { 0xffff6b77 };

inline juce::Colour envelopeColour(int type)
{
    switch (type)
    {
        case envelopeVolume: return trace;
        case envelopeArpeggio: return juce::Colour(0xffffb547);
        case envelopePitch: return juce::Colour(0xffff6b7d);
        case envelopeS5bMixer: return juce::Colour(0xff5cb8ff);
        case envelopeS5bNoise: return juce::Colour(0xffff7ad9);
        default: return accent;
    }
}

// Small capitals, spaced out a little, for everything that names a control.
juce::FontOptions labelFont(float height = 9.5f);

// A raised panel, lit from above.
void drawPanel(juce::Graphics&, juce::Rectangle<float> area, float corner = 6.0f);

// A sunken black screen.
void drawScreen(juce::Graphics&, juce::Rectangle<float> area, float corner = 4.0f);

// A panel with its name running up a darker strip on the left. Returns what's left of it.
juce::Rectangle<int> drawModule(juce::Graphics&, juce::Rectangle<int> area, const juce::String& title);
constexpr int moduleTitleWidth = 18;

void drawLed(juce::Graphics&, juce::Point<float> centre, float radius, juce::Colour colour, bool isOn);

// The chip's own lettering, in square dots of a whole number of pixels. It has capitals, digits
// and punctuation only, so check canDrawPixelText first. Text is drawn from the left of area.
bool canDrawPixelText(const juce::String&);
int pixelTextWidth(const juce::String&, int dotSize);
void drawPixelText(juce::Graphics&, const juce::String&, juce::Rectangle<int> area, int dotSize);

class LookAndFeel final : public juce::LookAndFeel_V4
{
public:
    LookAndFeel();

    void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height, float position, float startAngle, float endAngle, juce::Slider&) override;
    juce::Label* createSliderTextBox(juce::Slider&) override;

    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour, bool isOver, bool isDown) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&, bool isOver, bool isDown) override;
    void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool isOver, bool isDown) override;

    void drawComboBox(juce::Graphics&, int width, int height, bool isDown, int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont(juce::ComboBox&) override;
    void positionComboBoxText(juce::ComboBox&, juce::Label&) override;

    void fillTextEditorBackground(juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline(juce::Graphics&, int width, int height, juce::TextEditor&) override;

    juce::Font getPopupMenuFont() override;
    int getDefaultScrollbarWidth() override { return 8; }
};
} // namespace theme
