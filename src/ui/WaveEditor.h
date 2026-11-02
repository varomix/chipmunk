#pragma once

#include "engine/InstrumentDocument.h"
#include "ui/FineAdjust.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

// A row of values drawn with the mouse, for a waveform or the FDS modulation table.
class StepGraph final : public juce::Component
{
public:
    struct Source
    {
        std::function<int()> numSteps;
        std::function<int(int step)> value;
        std::function<void(int step, int value)> setValue;
        int minValue = 0, maxValue = 15;
    };

    StepGraph(InstrumentDocument&, Source);

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;

    void setRange(int minValue, int maxValue);

private:
    void drawAt(juce::Point<float>);
    void fineAdjustTo(float y);
    int stepAtX(float x, int count) const;

    InstrumentDocument& document;
    Source source;
    int lastStep = -1;
    FineAdjust fine;
};

// Draws an FDS or N163 sound's waveforms. A sound can step through several, each lasting a set
// number of frames, which is how these chips change their tone over a note.
class WavePanel final : public juce::Component
{
public:
    explicit WavePanel(InstrumentDocument&);

    // Sets the size and range of the chip's waves, from 0 up to "maxValue".
    void setChip(bool isFds);
    void refresh();

    void paint(juce::Graphics&) override;
    void resized() override;

    int getCurrentWave() const { return currentWave; }
    int numWaves() const;
    StepGraph& getGraph() { return waveGraph; }
    juce::TextEditor& getTextField() { return textField; }
    juce::TextEditor& getRepeatField() { return repeatField; }

    void selectWave(int index);
    void addWave();
    void removeWave();
    void setShape(const juce::String& shape);
    void commitText();
    void commitRepeat();

private:
    int size() const;

    // For use inside an edit, which holds the document's lock, so size() would wait on it forever.
    int sizeOf(const Instrument&) const;
    int maxValue() const { return fds ? 63 : 15; }

    // Fills in the default wave if the sound doesn't have one, so there's always something to edit.
    void ensureWave(Instrument&) const;

    InstrumentDocument& document;
    bool fds = true;
    int currentWave = 0;

    StepGraph waveGraph, modulationGraph;
    juce::TextEditor textField, repeatField;
    juce::Label repeatLabel;
    juce::TextButton previousButton { "<" }, nextButton { ">" }, addButton { "+" }, removeButton { "-" };
    juce::TextButton sineButton { "Sine" }, sawButton { "Saw" }, squareButton { "Sqr" }, triangleButton { "Tri" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WavePanel)
};
