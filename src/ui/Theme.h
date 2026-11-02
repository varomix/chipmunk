#pragma once

#include "engine/Instrument.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace theme
{
const juce::Colour background { 0xff15161c };
const juce::Colour panel { 0xff1f2029 };
const juce::Colour panelRaised { 0xff2a2c37 };
const juce::Colour outline { 0xff383a48 };
const juce::Colour text { 0xffe6e6ec };
const juce::Colour textDim { 0xff8d8fa1 };
const juce::Colour accent { 0xff4ecdc4 };
const juce::Colour favorite { 0xfff4b942 };
const juce::Colour error { 0xffff6b6b };
const juce::Colour loopMarker { 0xff7bd88f };
const juce::Colour releaseMarker { 0xffff6b6b };

inline juce::Colour envelopeColour(int type)
{
    switch (type)
    {
        case envelopeVolume: return juce::Colour(0xff4ecdc4);
        case envelopeArpeggio: return juce::Colour(0xfff4b942);
        case envelopePitch: return juce::Colour(0xffef6f6c);
        default: return juce::Colour(0xff9d8df1);
    }
}

class LookAndFeel final : public juce::LookAndFeel_V4
{
public:
    LookAndFeel()
    {
        using juce::ResizableWindow; using juce::TextEditor; using juce::ComboBox; using juce::ListBox;
        using juce::TextButton; using juce::Label; using juce::Slider; using juce::ToggleButton; using juce::PopupMenu;

        setColour(ResizableWindow::backgroundColourId, background);
        setColour(TextEditor::backgroundColourId, panelRaised);
        setColour(TextEditor::textColourId, text);
        setColour(TextEditor::outlineColourId, outline);
        setColour(TextEditor::focusedOutlineColourId, accent);
        setColour(TextEditor::highlightColourId, accent.withAlpha(0.35f));
        setColour(ComboBox::backgroundColourId, panelRaised);
        setColour(ComboBox::outlineColourId, outline);
        setColour(ComboBox::textColourId, text);
        setColour(ComboBox::arrowColourId, textDim);
        setColour(ListBox::backgroundColourId, panel);
        setColour(ListBox::outlineColourId, outline);
        setColour(TextButton::buttonColourId, panelRaised);
        setColour(TextButton::buttonOnColourId, accent.withAlpha(0.8f));
        setColour(TextButton::textColourOffId, text);
        setColour(TextButton::textColourOnId, background);
        setColour(Label::textColourId, text);
        setColour(Slider::backgroundColourId, panelRaised);
        setColour(Slider::trackColourId, accent.withAlpha(0.7f));
        setColour(Slider::thumbColourId, accent);
        setColour(Slider::textBoxTextColourId, text);
        setColour(Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour(Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour(ToggleButton::textColourId, text);
        setColour(ToggleButton::tickColourId, accent);
        setColour(ToggleButton::tickDisabledColourId, textDim);
        setColour(PopupMenu::backgroundColourId, panelRaised);
        setColour(PopupMenu::textColourId, text);
        setColour(PopupMenu::highlightedBackgroundColourId, accent);
        setColour(PopupMenu::highlightedTextColourId, background);
    }
};
} // namespace theme
