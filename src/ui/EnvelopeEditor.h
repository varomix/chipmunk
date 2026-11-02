#pragma once

#include "engine/EnvelopeEdit.h"
#include "engine/InstrumentDocument.h"
#include "ui/FineAdjust.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

// The drawable graph of one envelope. Drag in the graph to draw, and drag the markers
// in the strip above it to change the length, loop and release.
class EnvelopeGraph final : public juce::Component
{
public:
    EnvelopeGraph(InstrumentDocument&, int envelopeType);

    void refresh();
    void setPlayhead(int step);
    void setActive(bool shouldBeActive);
    void setMaxDuty(int newMaxDuty);

    float stepWidth() const;
    const Envelope& getEnvelope() const { return envelope; }
    std::function<void()> onDisplayChanged;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    static constexpr int rulerHeight = 18;

private:
    enum class Drag { none, draw, length, loop, release };

    Envelope& envelopeOf(Instrument& instrument) const { return instrument.envelopes[(size_t) type]; }
    int visibleSteps() const;
    juce::Rectangle<float> graphArea() const;
    int stepAtX(float x) const;
    int valueAtY(float y) const;
    float yForValue(int value) const;
    int visibleRowsAbove() const;
    EnvelopeRange range() const { return envelopeRange(type, maxDuty); }
    bool isBipolar() const { return range().min < 0; }
    Drag markerAt(juce::Point<float>) const;
    void drawTo(juce::Point<float>);
    void fineAdjustTo(float y);

    InstrumentDocument& document;
    int type;
    Envelope envelope;
    int playhead = -1;
    bool active = true;
    int zoomRange = 16;
    int maxDuty = 3;

    Drag drag = Drag::none;
    int frozenSteps = 0;
    int lastDrawStep = -1;
    int lastDrawValue = 0;
    FineAdjust fine;
    int hoverStep = -1;
    int hoverValue = 0;
};

// Each step's number, directly under its column in the graph above. Click to type.
class EnvelopeValueStrip final : public juce::Component,
                                 public juce::SettableTooltipClient
{
public:
    explicit EnvelopeValueStrip(const EnvelopeGraph&, int envelopeType);

    // How many steps apart the numbers are drawn. 1 unless the columns are too narrow to fit them.
    int labelInterval() const;

    // Where a step's number is drawn, which is exactly its column's span.
    juce::Rectangle<float> labelBounds(int step) const;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;

    std::function<void()> onClicked;

private:
    const EnvelopeGraph& graph;
    int type;
};

// A graph with its name, marker buttons and step numbers.
class EnvelopePanel final : public juce::Component
{
public:
    EnvelopePanel(InstrumentDocument&, int envelopeType, const juce::String& name, const juce::String& hint);

    void refresh();
    void setPlayhead(int step) { graph.setPlayhead(step); }
    void setUnusedReason(const juce::String& reason);
    void setMaxDuty(int newMaxDuty) { maxDuty = newMaxDuty; graph.setMaxDuty(newMaxDuty); }

    void paint(juce::Graphics&) override;
    void resized() override;

    // Below this, the buttons need the room that the note under the name takes.
    static constexpr int fullNoteHeight = 110;

    EnvelopeGraph& getGraph() { return graph; }
    juce::TextEditor& getTextField() { return textField; }
    EnvelopeValueStrip& getValueStrip() { return valueStrip; }
    void startTyping();
    const juce::String& getErrorText() const { return errorText; }

private:
    void commitText();
    void showDocumentText();
    void setError(const juce::String&);
    void stopTyping();

    InstrumentDocument& document;
    int type;
    juce::String name, hint, unusedReason, errorText;
    int maxDuty = 3;
    EnvelopeGraph graph;
    EnvelopeValueStrip valueStrip;
    juce::TextEditor textField;
    juce::TextButton loopButton { "Loop" }, releaseButton { "Release" }, relativeButton { "Relative" }, clearButton { "Clear" };
};
