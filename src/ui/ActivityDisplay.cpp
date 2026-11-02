#include "ui/ActivityDisplay.h"

#include "ui/Theme.h"

#include <cmath>

namespace
{
constexpr int channelDpcm = 3;
constexpr int volumeSteps = 15;
constexpr int meterWidth = 8;
constexpr float flatLevel = 0.0005f;
constexpr float smallestRange = 0.05f;

// About two cycles of a mid-range note.
int traceLengthFor(double sampleRate)
{
    return juce::jlimit(256, ScopeBuffer::capacity / 2, (int) std::lround((sampleRate > 0.0 ? sampleRate : 48000.0) / 50.0));
}
} // namespace

ActivityDisplay::ActivityDisplay(ChipmunkProcessor& p) : processor(p)
{
    setOpaque(true);
    setInterceptsMouseClicks(false, false);
}

void ActivityDisplay::refresh()
{
    const auto length = traceLengthFor(processor.getSampleRate());
    const auto available = length * 2;

    incoming.resize((size_t) available);
    trace.resize((size_t) length);
    processor.getScope().read(incoming.data(), available);

    // The chip's output carries an offset, so centre it before looking for a rising edge.
    auto mean = 0.0f;

    for (const auto sample : incoming)
        mean += sample;

    mean /= (float) available;

    // Starting each picture on a rising edge keeps a steady note still on screen.
    auto start = available - length;

    for (int i = 1; i < available - length; ++i)
    {
        if (incoming[(size_t) i - 1] <= mean && incoming[(size_t) i] > mean)
        {
            start = i;
            break;
        }
    }

    auto peak = 0.0f;

    for (int i = 0; i < length; ++i)
    {
        const auto value = incoming[(size_t) (start + i)] - mean;

        trace[(size_t) i] = value;
        peak = juce::jmax(peak, std::abs(value));
    }

    const auto isFlat = peak < flatLevel;
    const auto newNote = processor.getSoundingNote();
    const auto newVolume = processor.getSoundingVolume();

    // A silent device has nothing to redraw, so it costs nothing while a Set plays.
    if (isFlat && traceIsFlat && newNote == note && newVolume == volume)
        return;

    traceIsFlat = isFlat;
    note = newNote;
    volume = newVolume;
    repaint();
}

void ActivityDisplay::paint(juce::Graphics& g)
{
    auto area = getLocalBounds();

    g.fillAll(theme::panel);

    const auto hasVolume = processor.getChannelIndex() != channelDpcm;

    if (hasVolume)
    {
        auto meter = area.removeFromRight(meterWidth + 8).reduced(4, 5).toFloat();
        const auto stepHeight = meter.getHeight() / (float) volumeSteps;

        for (int step = 0; step < volumeSteps; ++step)
        {
            g.setColour(step < volume ? theme::accent : theme::text.withAlpha(0.12f));
            g.fillRect(meter.getX(), meter.getBottom() - stepHeight * (float) (step + 1) + 0.5f, meter.getWidth(), stepHeight - 1.0f);
        }
    }

    auto scope = area.reduced(4, 4).toFloat();

    g.setColour(theme::text.withAlpha(0.12f));
    g.drawHorizontalLine((int) scope.getCentreY(), scope.getX(), scope.getRight());

    if (! trace.empty())
    {
        auto peak = smallestRange;

        for (const auto sample : trace)
            peak = juce::jmax(peak, std::abs(sample));

        juce::Path path;
        const auto xScale = scope.getWidth() / (float) (trace.size() - 1);
        const auto yScale = scope.getHeight() * 0.5f / peak;

        path.startNewSubPath(scope.getX(), scope.getCentreY() - trace[0] * yScale);

        for (size_t i = 1; i < trace.size(); ++i)
            path.lineTo(scope.getX() + (float) i * xScale, scope.getCentreY() - trace[i] * yScale);

        g.setColour(theme::accent);
        g.strokePath(path, juce::PathStrokeType(1.2f));
    }

    if (note >= 0)
    {
        g.setColour(theme::text);
        g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        g.drawText(midiNoteName(note), area.reduced(6, 3), juce::Justification::topLeft);
    }
}
