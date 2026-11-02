#include "ui/EnvelopeEditor.h"

#include "ui/Theme.h"

#include <cmath>

EnvelopeGraph::EnvelopeGraph(InstrumentDocument& d, int t)
    : document(d),
      type(t)
{
    refresh();
}

void EnvelopeGraph::refresh()
{
    envelope = document.snapshot().envelopes[(size_t) type];

    // Make sure typed or loaded values are visible.
    if (isBipolar())
        for (int i = 0; i < envelope.length; ++i)
            while (zoomRange < 64 && std::abs((int) envelope.values[(size_t) i]) > zoomRange)
                zoomRange *= 2;

    repaint();

    if (onDisplayChanged)
        onDisplayChanged();
}

void EnvelopeGraph::setPlayhead(int step)
{
    if (playhead != step)
    {
        playhead = step;
        repaint();
    }
}

void EnvelopeGraph::setMaxDuty(int newMaxDuty)
{
    maxDuty = newMaxDuty;
    repaint();
}

void EnvelopeGraph::setActive(bool shouldBeActive)
{
    if (active != shouldBeActive)
    {
        active = shouldBeActive;
        repaint();
    }
}

int EnvelopeGraph::visibleSteps() const
{
    if (frozenSteps > 0)
        return frozenSteps;

    // Grows in steps of 8, so the graph doesn't rescale with every small length change,
    // and always leaves a little room to drag the length marker further.
    return juce::jlimit(32, Envelope::maxLength, (envelope.length + 11) / 8 * 8);
}

float EnvelopeGraph::stepWidth() const
{
    return (float) getWidth() / (float) visibleSteps();
}

juce::Rectangle<float> EnvelopeGraph::graphArea() const
{
    return getLocalBounds().toFloat().withTrimmedTop((float) rulerHeight);
}

int EnvelopeGraph::stepAtX(float x) const
{
    return juce::jlimit(0, Envelope::maxLength - 1, (int) std::floor(x / stepWidth()));
}

int EnvelopeGraph::visibleRowsAbove() const
{
    return zoomRange;
}

int EnvelopeGraph::valueAtY(float y) const
{
    const auto area = graphArea();
    const auto range = this->range();

    if (isBipolar())
    {
        const auto rowHeight = area.getHeight() / (float) (zoomRange * 2 + 1);
        return juce::jlimit(range.min, range.max, (int) std::lround((area.getCentreY() - y) / rowHeight));
    }

    const auto rows = range.max - range.min + 1;
    const auto rowHeight = area.getHeight() / (float) rows;
    return juce::jlimit(range.min, range.max, range.min + (int) std::floor((area.getBottom() - y) / rowHeight));
}

float EnvelopeGraph::yForValue(int value) const
{
    const auto area = graphArea();

    if (isBipolar())
    {
        const auto rowHeight = area.getHeight() / (float) (zoomRange * 2 + 1);
        return area.getCentreY() - (float) juce::jlimit(-zoomRange, zoomRange, value) * rowHeight;
    }

    const auto range = this->range();
    const auto rows = range.max - range.min + 1;
    return area.getBottom() - area.getHeight() * (float) (value - range.min + 1) / (float) rows;
}

void EnvelopeGraph::paint(juce::Graphics& g)
{
    const auto colour = theme::envelopeColour(type).withMultipliedAlpha(active ? 1.0f : 0.3f);
    const auto area = graphArea();
    const auto width = stepWidth();
    const auto steps = visibleSteps();

    g.setColour(theme::panel.darker(0.3f));
    g.fillRect(area);
    g.setColour(theme::panelRaised);
    g.fillRect(getLocalBounds().toFloat().withHeight((float) rulerHeight));

    // Grid.
    g.setColour(theme::outline.withAlpha(0.45f));

    for (int i = 0; i <= steps; i += width >= 6.0f ? 4 : 16)
        g.fillRect((float) i * width, area.getY(), 1.0f, area.getHeight());

    if (isBipolar())
    {
        g.setColour(theme::textDim.withAlpha(0.6f));
        g.fillRect(area.getX(), area.getCentreY() - 0.5f, area.getWidth(), 1.0f);
    }
    else
    {
        const auto range = this->range();

        for (int value = range.min; value <= range.max; ++value)
            if (range.max < 8 || value % 5 == 0)
                g.fillRect(area.getX(), yForValue(value), area.getWidth(), 1.0f);
    }

    // Loop region.
    if (envelope.loop >= 0)
    {
        const auto end = envelope.release >= 0 ? envelope.release : envelope.length;
        g.setColour(theme::loopMarker.withAlpha(0.08f));
        g.fillRect((float) envelope.loop * width, area.getY(), (float) (end - envelope.loop) * width, area.getHeight());
    }

    // Steps.
    const auto gap = width >= 5.0f ? 1.0f : 0.0f;

    for (int i = 0; i < envelope.length; ++i)
    {
        const auto x = (float) i * width;
        const auto value = (int) envelope.values[(size_t) i];
        const auto isPlaying = i == playhead;

        g.setColour(isPlaying ? colour.brighter(0.7f) : colour);

        if (isBipolar())
        {
            const auto rowHeight = area.getHeight() / (float) (zoomRange * 2 + 1);
            const auto y = yForValue(value);
            const auto top = juce::jmin(y - rowHeight * 0.5f, area.getCentreY() - rowHeight * 0.5f);
            const auto bottom = juce::jmax(y + rowHeight * 0.5f, area.getCentreY() + rowHeight * 0.5f);
            g.fillRect(x, top, width - gap, bottom - top);
        }
        else
        {
            const auto y = yForValue(value);
            g.fillRect(x, y, width - gap, juce::jmax(2.0f, area.getBottom() - y));
        }
    }

    if (playhead >= 0 && playhead >= envelope.length)
        g.setColour(colour);

    if (envelope.length == 0)
    {
        g.setColour(theme::textDim);
        g.setFont(juce::FontOptions(12.0f));
        g.drawText("Empty, so it has no effect. Draw here or type below.", area.reduced(8.0f), juce::Justification::centred);
    }

    // Markers.
    const auto drawFlag = [&](int step, juce::Colour flagColour, const char* label) {
        const auto x = (float) step * width;
        g.setColour(flagColour);
        g.fillRect(x, 0.0f, 1.5f, (float) getHeight());
        g.fillRect(x, 0.0f, 14.0f, (float) rulerHeight);
        g.setColour(theme::background);
        g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
        g.drawText(label, juce::Rectangle<float>(x, 0.0f, 14.0f, (float) rulerHeight), juce::Justification::centred);
    };

    if (envelope.length > 0 || drag == Drag::length)
    {
        const auto x = (float) envelope.length * width;
        g.setColour(theme::text);
        g.fillRect(x, 0.0f, 1.5f, (float) getHeight());
        juce::Path handle;
        handle.addTriangle(x - 7.0f, 0.0f, x + 7.0f, 0.0f, x, (float) rulerHeight - 4.0f);
        g.fillPath(handle);
    }

    if (envelope.loop >= 0)
        drawFlag(envelope.loop, theme::loopMarker, "L");

    if (envelope.release >= 0)
        drawFlag(envelope.release, theme::releaseMarker, "R");

    // Readout.
    g.setFont(juce::FontOptions(11.0f));
    g.setColour(theme::textDim);
    juce::String info;

    if (hoverStep >= 0)
        info << "step " << hoverStep << "   value " << hoverValue << "   ";

    info << envelope.length << " steps";

    if (isBipolar())
        info << "   range +/-" << zoomRange;

    g.drawText(info, getLocalBounds().withHeight(rulerHeight).reduced(6, 0), juce::Justification::centredRight);

    g.setColour(theme::outline);
    g.drawRect(getLocalBounds().toFloat(), 1.0f);
}

EnvelopeGraph::Drag EnvelopeGraph::markerAt(juce::Point<float> p) const
{
    if (p.y > (float) rulerHeight)
        return Drag::none;

    const auto width = stepWidth();
    const auto near = [&](int step, float tolerance) { return std::abs(p.x - (float) step * width - tolerance * 0.5f) <= tolerance; };

    if (envelope.release >= 0 && near(envelope.release, 14.0f))
        return Drag::release;

    if (envelope.loop >= 0 && near(envelope.loop, 14.0f))
        return Drag::loop;

    if (std::abs(p.x - (float) envelope.length * width) <= 9.0f)
        return Drag::length;

    return Drag::none;
}

void EnvelopeGraph::mouseDown(const juce::MouseEvent& e)
{
    frozenSteps = visibleSteps();
    drag = markerAt(e.position);

    if (drag == Drag::none && e.position.y <= (float) rulerHeight)
        return;

    document.beginGesture();

    if (drag == Drag::none)
    {
        drag = Drag::draw;
        lastDrawStep = -1;

        // A step that doesn't exist yet has no value to adjust, so the click draws it first.
        const auto step = stepAtX(e.position.x);

        if (! e.mods.isShiftDown() || step >= envelope.length)
            drawTo(e.position);

        if (e.mods.isShiftDown())
            fine.start(step, (int) envelope.values[(size_t) step], e.position.y);
    }
}

void EnvelopeGraph::fineAdjustTo(float y)
{
    const auto area = graphArea();
    const auto range = this->range();
    const auto rows = isBipolar() ? zoomRange * 2 + 1 : range.max - range.min + 1;
    const auto value = fine.moveTo(y, area.getHeight() / (float) rows, range.min, range.max);

    if (value != (int) envelope.values[(size_t) fine.step])
    {
        document.edit([&](Instrument& instrument) { setEnvelopeValue(envelopeOf(instrument), type, fine.step, value, maxDuty); });
        refresh();
    }

    hoverStep = fine.step;
    hoverValue = value;
    repaint();
}

void EnvelopeGraph::drawTo(juce::Point<float> p)
{
    const auto step = stepAtX(p.x);
    const auto value = valueAtY(p.y);
    const auto from = lastDrawStep >= 0 ? lastDrawStep : step;
    const auto fromValue = lastDrawStep >= 0 ? lastDrawValue : value;

    // Fill the steps between mouse positions, so fast strokes leave no gaps.
    document.edit([&](Instrument& instrument) {
        auto& env = envelopeOf(instrument);
        const auto count = std::abs(step - from);

        for (int i = 0; i <= count; ++i)
        {
            const auto t = count > 0 ? (float) i / (float) count : 1.0f;
            const auto s = from + (step > from ? i : -i);
            setEnvelopeValue(env, type, s, (int) std::lround((float) fromValue + (float) (value - fromValue) * t), maxDuty);
        }
    });

    lastDrawStep = step;
    lastDrawValue = value;
    refresh();
}

void EnvelopeGraph::mouseDrag(const juce::MouseEvent& e)
{
    const auto stepBoundary = (int) std::lround(e.position.x / stepWidth());

    switch (drag)
    {
        case Drag::draw:
            if (! fine.active && e.mods.isShiftDown() && lastDrawStep >= 0)
                fine.start(lastDrawStep, (int) envelope.values[(size_t) lastDrawStep], e.position.y);

            if (fine.active)
                fineAdjustTo(e.position.y);
            else
                drawTo(e.position);

            break;

        case Drag::length:
            document.edit([&](Instrument& instrument) { setEnvelopeLength(envelopeOf(instrument), type, stepBoundary); });
            refresh();
            break;

        case Drag::loop:
            document.edit([&](Instrument& instrument) { setEnvelopeLoop(envelopeOf(instrument), juce::jmax(0, stepAtX(e.position.x))); });
            refresh();
            break;

        case Drag::release:
            document.edit([&](Instrument& instrument) { setEnvelopeRelease(envelopeOf(instrument), type, juce::jmax(1, stepAtX(e.position.x))); });
            refresh();
            break;

        case Drag::none:
            break;
    }
}

void EnvelopeGraph::mouseUp(const juce::MouseEvent&)
{
    if (drag != Drag::none)
        document.endGesture();

    drag = Drag::none;
    fine.active = false;
    frozenSteps = 0;
    repaint();

    if (onDisplayChanged)
        onDisplayChanged();
}

void EnvelopeGraph::mouseMove(const juce::MouseEvent& e)
{
    const auto inGraph = e.position.y > (float) rulerHeight;

    hoverStep = inGraph ? stepAtX(e.position.x) : -1;
    hoverValue = valueAtY(e.position.y);
    repaint();

    const auto marker = markerAt(e.position);
    setMouseCursor(marker != Drag::none ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::CrosshairCursor);
}

void EnvelopeGraph::mouseExit(const juce::MouseEvent&)
{
    hoverStep = -1;
    repaint();
}

void EnvelopeGraph::mouseDoubleClick(const juce::MouseEvent& e)
{
    // Double-clicking the L or R flag removes it.
    const auto marker = markerAt(e.position);

    if (marker == Drag::loop)
        document.edit([&](Instrument& instrument) { setEnvelopeLoop(envelopeOf(instrument), -1); });
    else if (marker == Drag::release)
        document.edit([&](Instrument& instrument) { setEnvelopeRelease(envelopeOf(instrument), type, -1); });

    refresh();
}

void EnvelopeGraph::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    if (! isBipolar() || wheel.deltaY == 0.0f)
        return;

    zoomRange = juce::jlimit(8, 64, wheel.deltaY > 0.0f ? zoomRange / 2 : zoomRange * 2);
    repaint();
}

EnvelopeValueStrip::EnvelopeValueStrip(const EnvelopeGraph& g, int t)
    : graph(g),
      type(t)
{
    setMouseCursor(juce::MouseCursor::IBeamCursor);
    setTooltip("Click to type values");
}

int EnvelopeValueStrip::labelInterval() const
{
    // A column needs room for the widest number in this envelope, at the smallest font.
    const auto& envelope = graph.getEnvelope();
    auto longest = 1;

    for (int i = 0; i < envelope.length; ++i)
        longest = juce::jmax(longest, juce::String((int) envelope.values[(size_t) i]).length());

    const auto needed = (float) longest * 5.6f + 1.0f;
    const auto width = graph.stepWidth();

    for (const auto interval : { 1, 2, 4, 8, 16 })
        if (width * (float) interval >= needed)
            return interval;

    return 32;
}

juce::Rectangle<float> EnvelopeValueStrip::labelBounds(int step) const
{
    const auto width = graph.stepWidth();
    return { (float) step * width, 0.0f, width, (float) getHeight() };
}

void EnvelopeValueStrip::paint(juce::Graphics& g)
{
    g.setColour(theme::panelRaised);
    g.fillRect(getLocalBounds());

    const auto& envelope = graph.getEnvelope();

    if (envelope.length == 0)
    {
        g.setColour(theme::textDim);
        g.setFont(juce::FontOptions(12.0f));
        g.drawText(envelopeCanRelease(type) ? "Click to type: 15 12 | 9 8 / 4 0     ( | loop start, / release )"
                                            : "Click to type: 0 4 | 7 12     ( | loop start )",
                   getLocalBounds().reduced(6, 0), juce::Justification::centredLeft);
    }

    const auto interval = labelInterval();
    const auto columnWidth = graph.stepWidth();

    g.setFont(juce::FontOptions(juce::jmin(13.0f, juce::jmax(10.0f, columnWidth * 0.62f))));

    for (int step = 0; step < envelope.length; step += interval)
    {
        const auto isMarker = step == envelope.loop || step == envelope.release;

        g.setColour(step == envelope.release ? theme::releaseMarker : isMarker ? theme::loopMarker : theme::text);

        // Centred on its own column. It may overflow into columns whose numbers are skipped.
        g.drawText(juce::String((int) envelope.values[(size_t) step]),
                   labelBounds(step).withSizeKeepingCentre(columnWidth * (float) interval + 8.0f, (float) getHeight()),
                   juce::Justification::centred, false);
    }

    // Column edges carried down from the graph.
    if (columnWidth >= 6.0f)
    {
        g.setColour(theme::outline.withAlpha(0.6f));

        for (int step = 0; step <= envelope.length; ++step)
            g.fillRect((float) step * columnWidth, 0.0f, 1.0f, (float) getHeight());
    }

    g.setColour(theme::outline);
    g.drawRect(getLocalBounds().toFloat(), 1.0f);
}

void EnvelopeValueStrip::mouseDown(const juce::MouseEvent&)
{
    if (onClicked)
        onClicked();
}

EnvelopePanel::EnvelopePanel(InstrumentDocument& d, int t, const juce::String& n, const juce::String& h)
    : document(d),
      type(t),
      name(n),
      hint(h),
      graph(d, t),
      valueStrip(graph, t)
{
    addAndMakeVisible(graph);
    addAndMakeVisible(valueStrip);
    addChildComponent(textField);

    graph.onDisplayChanged = [this] { valueStrip.repaint(); };
    valueStrip.onClicked = [this] { startTyping(); };

    textField.setFont(juce::FontOptions(13.0f));
    textField.setSelectAllWhenFocused(false);
    textField.onReturnKey = [this] { commitText(); };
    textField.onFocusLost = [this] { commitText(); };
    textField.onEscapeKey = [this] { setError({}); showDocumentText(); stopTyping(); };

    for (auto* button : { &loopButton, &releaseButton, &relativeButton, &clearButton })
        addAndMakeVisible(*button);

    releaseButton.setVisible(envelopeCanRelease(type));
    relativeButton.setVisible(type == envelopePitch);

    loopButton.setTooltip("Repeat part of the envelope while the note is held");
    releaseButton.setTooltip("Play the steps after this point when the note is released");
    relativeButton.setTooltip("Add each value to the last one, instead of setting the pitch offset directly");

    loopButton.onClick = [this] {
        document.edit([this](Instrument& instrument) {
            auto& env = instrument.envelopes[(size_t) type];
            setEnvelopeLoop(env, env.loop >= 0 ? -1 : 0);
        });
        refresh();
    };

    releaseButton.onClick = [this] {
        document.edit([this](Instrument& instrument) {
            auto& env = instrument.envelopes[(size_t) type];
            setEnvelopeRelease(env, type, env.release >= 0 ? -1 : env.length - 1);
        });
        refresh();
    };

    relativeButton.onClick = [this] {
        document.edit([this](Instrument& instrument) {
            auto& env = instrument.envelopes[(size_t) type];
            env.relative = ! env.relative;
        });
        refresh();
    };

    clearButton.onClick = [this] {
        document.edit([this](Instrument& instrument) { clearEnvelope(instrument.envelopes[(size_t) type]); });
        refresh();
    };

    refresh();
}

void EnvelopePanel::refresh()
{
    const auto instrument = document.snapshot();
    const auto& env = instrument.envelopes[(size_t) type];

    loopButton.setToggleState(env.loop >= 0, juce::dontSendNotification);
    releaseButton.setToggleState(env.release >= 0, juce::dontSendNotification);
    relativeButton.setToggleState(env.relative, juce::dontSendNotification);

    loopButton.setEnabled(env.length > 0);
    releaseButton.setEnabled(env.length > 1);

    graph.refresh();

    valueStrip.repaint();

    if (! textField.isVisible())
    {
        setError({});
        showDocumentText();
    }
}

void EnvelopePanel::startTyping()
{
    showDocumentText();
    valueStrip.setVisible(false);
    textField.setVisible(true);
    textField.moveCaretToEnd();

    if (isShowing())
        textField.grabKeyboardFocus();
}

void EnvelopePanel::stopTyping()
{
    textField.setVisible(false);
    valueStrip.setVisible(true);
}

void EnvelopePanel::showDocumentText()
{
    textField.setText(formatEnvelope(document.snapshot().envelopes[(size_t) type]), juce::dontSendNotification);
}

void EnvelopePanel::setError(const juce::String& message)
{
    errorText = message;
    textField.setColour(juce::TextEditor::outlineColourId, message.isEmpty() ? theme::outline : theme::error);
    textField.setColour(juce::TextEditor::focusedOutlineColourId, message.isEmpty() ? theme::accent : theme::error);
    textField.repaint();
    repaint();
}

void EnvelopePanel::commitText()
{
    auto env = document.snapshot().envelopes[(size_t) type];
    const auto before = formatEnvelope(env);
    const auto typed = textField.getText().toStdString();
    std::string error;

    if (! parseEnvelope(typed, type, env, error, maxDuty))
    {
        setError(error);
        return;
    }

    setError({});

    if (formatEnvelope(env) != before)
    {
        document.edit([&](Instrument& instrument) { instrument.envelopes[(size_t) type] = env; });
        graph.refresh();
    }

    showDocumentText();
    stopTyping();

    loopButton.setToggleState(env.loop >= 0, juce::dontSendNotification);
    releaseButton.setToggleState(env.release >= 0, juce::dontSendNotification);
    loopButton.setEnabled(env.length > 0);
    releaseButton.setEnabled(env.length > 1);
}

void EnvelopePanel::setUnusedReason(const juce::String& reason)
{
    if (unusedReason != reason)
    {
        unusedReason = reason;
        graph.setActive(reason.isEmpty());
        repaint();
    }
}

void EnvelopePanel::paint(juce::Graphics& g)
{
    auto area = getLocalBounds().withWidth(118);

    g.setColour(theme::panel);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 4.0f);

    g.setColour(theme::envelopeColour(type).withMultipliedAlpha(unusedReason.isEmpty() ? 1.0f : 0.4f));
    g.fillRect(getLocalBounds().withWidth(3).reduced(0, 4));

    g.setColour(theme::text);
    g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    g.drawText(name, area.removeFromTop(26).withTrimmedLeft(12).withTrimmedTop(6), juce::Justification::centredLeft);

    // A short panel keeps its buttons rather than its note. Errors still show on the text field's outline.
    if (getHeight() < fullNoteHeight)
        return;

    // The error takes the hint's place, since it has to stay readable beside the text being fixed.
    const auto& note = errorText.isNotEmpty() ? errorText : unusedReason.isNotEmpty() ? unusedReason : hint;

    g.setColour(errorText.isNotEmpty() ? theme::error : unusedReason.isNotEmpty() ? theme::favorite : theme::textDim);
    g.setFont(juce::FontOptions(11.0f));
    g.drawFittedText(note, area.removeFromTop(30).withTrimmedLeft(12).withTrimmedRight(4), juce::Justification::topLeft, 2);
}

void EnvelopePanel::resized()
{
    auto area = getLocalBounds().reduced(0, 4);
    auto side = area.removeFromLeft(118).withTrimmedLeft(10).withTrimmedRight(6);
    area.removeFromRight(6);

    // Same left edge and width as the graph, so every number sits under its column.
    const auto bottom = area.removeFromBottom(24);
    textField.setBounds(bottom);
    valueStrip.setBounds(bottom);
    area.removeFromBottom(3);
    graph.setBounds(area);

    side.removeFromTop(getHeight() < fullNoteHeight ? 26 : 56);

    auto row = side.removeFromTop(22);
    loopButton.setBounds(row.removeFromLeft(row.getWidth() / 2 - 1));
    row.removeFromLeft(2);

    if (releaseButton.isVisible())
        releaseButton.setBounds(row);
    else if (relativeButton.isVisible())
        relativeButton.setBounds(row);
    else
        clearButton.setBounds(row);

    if (releaseButton.isVisible() || relativeButton.isVisible())
    {
        side.removeFromTop(2);
        clearButton.setBounds(side.removeFromTop(22).withWidth(side.getWidth() / 2 - 1));
    }
}
