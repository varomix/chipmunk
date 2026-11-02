#pragma once

#include "plugin/PluginProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

// An oscilloscope of the device's output, with the note and volume the channel is playing.
class ActivityDisplay final : public juce::Component
{
public:
    explicit ActivityDisplay(ChipmunkProcessor&);

    // Fetches the latest output, and repaints if there's anything new to show.
    void refresh();

    void paint(juce::Graphics&) override;

    // The newest audio being shown, for tests.
    const std::vector<float>& getTrace() const { return trace; }
    int getShownNote() const { return note; }
    int getShownVolume() const { return volume; }

private:
    ChipmunkProcessor& processor;

    std::vector<float> incoming;
    std::vector<float> trace;
    int note = -1;
    int volume = 0;
    bool traceIsFlat = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ActivityDisplay)
};
