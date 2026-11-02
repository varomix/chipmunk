#include "ui/WaveEditor.h"

#include "ui/Theme.h"

#include <cmath>

namespace
{
constexpr int fdsWaveSize = 64;
constexpr int maxWaves = 64;
constexpr int modulationWidth = 180;
constexpr int infoWidth = 150;
} // namespace

StepGraph::StepGraph(InstrumentDocument& d, Source s) : document(d), source(std::move(s))
{
    setOpaque(true);
}

void StepGraph::setRange(int minValue, int maxValue)
{
    source.minValue = minValue;
    source.maxValue = maxValue;
    repaint();
}

void StepGraph::paint(juce::Graphics& g)
{
    g.fillAll(theme::panel);
    theme::drawScreen(g, getLocalBounds().toFloat());

    const auto count = source.numSteps();

    if (count == 0)
        return;

    const auto area = getLocalBounds().toFloat();
    const auto columnWidth = area.getWidth() / (float) count;
    const auto range = (float) (source.maxValue - source.minValue);
    const auto zeroY = area.getBottom() - area.getHeight() * (float) (std::max(0, source.minValue) - source.minValue) / range;

    g.setColour(theme::outline);
    g.drawHorizontalLine((int) zeroY, area.getX(), area.getRight());

    const auto colour = theme::envelopeColour(envelopeDutyCycle);

    for (int step = 0; step < count; ++step)
    {
        const auto y = area.getBottom() - area.getHeight() * (float) (source.value(step) - source.minValue) / range;
        const auto x = area.getX() + (float) step * columnWidth;
        const auto columnW = std::max(1.0f, columnWidth - 1.0f);

        g.setColour(colour.withAlpha(0.45f));
        g.fillRect(x, std::min(y, zeroY), columnW, std::max(1.0f, std::abs(y - zeroY)));
        g.setColour(colour);
        g.fillRect(x, y <= zeroY ? y : y - 2.0f, columnW, 2.0f);
    }
}

int StepGraph::stepAtX(float x, int count) const
{
    return juce::jlimit(0, count - 1, (int) std::floor(x * (float) count / (float) getWidth()));
}

void StepGraph::drawAt(juce::Point<float> position)
{
    const auto count = source.numSteps();

    if (count == 0 || getWidth() == 0 || getHeight() == 0)
        return;

    const auto step = stepAtX(position.x, count);
    const auto proportion = 1.0f - juce::jlimit(0.0f, 1.0f, position.y / (float) getHeight());
    const auto value = source.minValue + (int) std::lround(proportion * (float) (source.maxValue - source.minValue));

    // Fast strokes skip columns, so fill in between.
    const auto from = lastStep >= 0 ? lastStep : step;

    for (int s = std::min(from, step); s <= std::max(from, step); ++s)
        source.setValue(s, value);

    lastStep = step;
    repaint();
}

void StepGraph::mouseDown(const juce::MouseEvent& e)
{
    document.beginGesture();
    lastStep = -1;

    const auto count = source.numSteps();

    if (e.mods.isShiftDown() && count > 0 && getWidth() > 0)
    {
        const auto step = stepAtX(e.position.x, count);
        fine.start(step, source.value(step), e.position.y);
    }
    else
    {
        drawAt(e.position);
    }
}

void StepGraph::fineAdjustTo(float y)
{
    const auto rowHeight = (float) getHeight() / (float) (source.maxValue - source.minValue);
    const auto value = fine.moveTo(y, rowHeight, source.minValue, source.maxValue);

    if (value != source.value(fine.step))
    {
        source.setValue(fine.step, value);
        repaint();
    }
}

void StepGraph::mouseDrag(const juce::MouseEvent& e)
{
    if (! fine.active && e.mods.isShiftDown() && lastStep >= 0)
        fine.start(lastStep, source.value(lastStep), e.position.y);

    if (fine.active)
        fineAdjustTo(e.position.y);
    else
        drawAt(e.position);
}

void StepGraph::mouseUp(const juce::MouseEvent&)
{
    fine.active = false;
    document.endGesture();
}

WavePanel::WavePanel(InstrumentDocument& d)
    : document(d),
      waveGraph(d, StepGraph::Source {
          [this] { return size(); },
          [this](int step) {
              const auto instrument = document.snapshot();
              const auto position = currentWave * size() + step;
              return position < instrument.waveLength ? (int) instrument.wave[(size_t) position] : (int) Instrument::defaultWaveStep(step, size(), maxValue());
          },
          [this](int step, int value) {
              document.edit([&](Instrument& instrument) {
                  ensureWave(instrument);
                  instrument.wave[(size_t) (currentWave * sizeOf(instrument) + step)] = (int8_t) value;
              });
          },
          0, 63 }),
      modulationGraph(d, StepGraph::Source {
          [] { return 32; },
          [this](int step) { return (int) document.snapshot().fdsModulation[(size_t) step]; },
          [this](int step, int value) { document.edit([&](Instrument& instrument) { instrument.fdsModulation[(size_t) step] = (int8_t) value; }); },
          -64, 63 })
{
    addAndMakeVisible(waveGraph);
    addAndMakeVisible(modulationGraph);

    addAndMakeVisible(textField);
    textField.setFont(juce::FontOptions(11.0f));
    textField.setTextToShowWhenEmpty("Wave values", theme::textDim);
    textField.onReturnKey = [this] { commitText(); };
    textField.onFocusLost = [this] { commitText(); };

    addAndMakeVisible(repeatField);
    repeatField.setFont(juce::FontOptions(12.0f));
    repeatField.setInputRestrictions(3, "0123456789");
    repeatField.setTooltip("How many frames this wave plays before moving to the next");
    repeatField.onReturnKey = [this] { commitRepeat(); };
    repeatField.onFocusLost = [this] { commitRepeat(); };

    addAndMakeVisible(repeatLabel);
    repeatLabel.setText("Frames", juce::dontSendNotification);
    repeatLabel.setFont(juce::FontOptions(11.0f));
    repeatLabel.setColour(juce::Label::textColourId, theme::textDim);

    for (auto* button : { &previousButton, &nextButton, &addButton, &removeButton, &sineButton, &sawButton, &squareButton, &triangleButton })
        addAndMakeVisible(*button);

    previousButton.onClick = [this] { selectWave(currentWave - 1); };
    nextButton.onClick = [this] { selectWave(currentWave + 1); };
    addButton.onClick = [this] { addWave(); };
    removeButton.onClick = [this] { removeWave(); };
    sineButton.onClick = [this] { setShape("sine"); };
    sawButton.onClick = [this] { setShape("saw"); };
    squareButton.onClick = [this] { setShape("square"); };
    triangleButton.onClick = [this] { setShape("triangle"); };
}

void WavePanel::setChip(bool isFds)
{
    fds = isFds;
    waveGraph.setRange(0, maxValue());
    modulationGraph.setVisible(fds);
    currentWave = 0;
    resized();
    refresh();
}

int WavePanel::size() const
{
    return sizeOf(document.snapshot());
}

int WavePanel::sizeOf(const Instrument& instrument) const
{
    return fds ? fdsWaveSize : juce::jlimit(4, 64, instrument.waveSize);
}

int WavePanel::numWaves() const
{
    const auto instrument = document.snapshot();

    return juce::jmax(1, instrument.waveLength / sizeOf(instrument));
}

void WavePanel::ensureWave(Instrument& instrument) const
{
    const auto waveSize = sizeOf(instrument);

    if (instrument.waveLength < waveSize || (fds && instrument.waveSize != fdsWaveSize))
        instrument.setDefaultWave(waveSize, maxValue());

    if (instrument.waveRepeat.length == 0)
    {
        instrument.waveRepeat.length = 1;
        instrument.waveRepeat.values[0] = 1;
    }
}

void WavePanel::selectWave(int index)
{
    currentWave = juce::jlimit(0, numWaves() - 1, index);
    refresh();
}

void WavePanel::addWave()
{
    document.edit([this](Instrument& instrument) {
        ensureWave(instrument);

        const auto waveSize = sizeOf(instrument);
        const auto count = instrument.waveLength / waveSize;

        if (count >= maxWaves || instrument.waveLength + waveSize > Instrument::maxWaveSteps || instrument.waveRepeat.length >= Envelope::maxLength)
            return;

        // A new wave starts as a copy of the last, ready to be changed a little.
        std::copy_n(instrument.wave.begin() + (count - 1) * waveSize, waveSize, instrument.wave.begin() + count * waveSize);
        instrument.waveLength += waveSize;
        instrument.waveRepeat.values[(size_t) instrument.waveRepeat.length++] = 1;
    });

    selectWave(numWaves() - 1);
}

void WavePanel::removeWave()
{
    if (numWaves() <= 1)
        return;

    document.edit([this](Instrument& instrument) {
        const auto waveSize = sizeOf(instrument);
        const auto next = instrument.wave.begin() + (currentWave + 1) * waveSize;

        std::copy(next, instrument.wave.begin() + instrument.waveLength, instrument.wave.begin() + currentWave * waveSize);
        instrument.waveLength -= waveSize;

        auto& repeat = instrument.waveRepeat;

        if (currentWave < repeat.length)
        {
            std::copy(repeat.values.begin() + currentWave + 1, repeat.values.begin() + repeat.length, repeat.values.begin() + currentWave);
            --repeat.length;
        }

        if (repeat.loop >= repeat.length) repeat.loop = -1;
        if (repeat.release >= repeat.length) repeat.release = -1;
    });

    selectWave(currentWave);
}

void WavePanel::setShape(const juce::String& shape)
{
    document.edit([&](Instrument& instrument) {
        ensureWave(instrument);

        const auto waveSize = sizeOf(instrument);
        const auto top = maxValue();

        for (int i = 0; i < waveSize; ++i)
        {
            const auto phase = (double) i / waveSize;
            const auto value = shape == "saw" ? phase * top
                             : shape == "square" ? (phase < 0.5 ? (double) top : 0.0)
                             : shape == "triangle" ? (phase < 0.5 ? phase * 2.0 : 2.0 - phase * 2.0) * top
                                                   : (double) Instrument::defaultWaveStep(i, waveSize, top);

            instrument.wave[(size_t) (currentWave * waveSize + i)] = (int8_t) std::lround(value);
        }
    });

    refresh();
}

void WavePanel::commitText()
{
    juce::StringArray tokens;
    tokens.addTokens(textField.getText(), " ,", "");
    tokens.removeEmptyStrings();

    if (tokens.size() == size())
    {
        document.edit([&](Instrument& instrument) {
            ensureWave(instrument);

            for (int i = 0; i < tokens.size(); ++i)
                instrument.wave[(size_t) (currentWave * sizeOf(instrument) + i)] = (int8_t) juce::jlimit(0, maxValue(), tokens[i].getIntValue());
        });
    }

    refresh();
}

void WavePanel::commitRepeat()
{
    const auto frames = juce::jlimit(1, 127, repeatField.getText().getIntValue());

    document.edit([&](Instrument& instrument) {
        ensureWave(instrument);

        if (currentWave < instrument.waveRepeat.length)
            instrument.waveRepeat.values[(size_t) currentWave] = (int8_t) frames;
    });

    refresh();
}

void WavePanel::refresh()
{
    currentWave = juce::jlimit(0, numWaves() - 1, currentWave);

    const auto instrument = document.snapshot();
    const auto waveSize = size();

    if (! textField.hasKeyboardFocus(false))
    {
        juce::StringArray values;

        for (int i = 0; i < waveSize; ++i)
        {
            const auto position = currentWave * waveSize + i;
            values.add(juce::String(position < instrument.waveLength ? (int) instrument.wave[(size_t) position] : (int) Instrument::defaultWaveStep(i, waveSize, maxValue())));
        }

        textField.setText(values.joinIntoString(" "), false);
    }

    if (! repeatField.hasKeyboardFocus(false))
        repeatField.setText(juce::String(currentWave < instrument.waveRepeat.length ? (int) instrument.waveRepeat.values[(size_t) currentWave] : 1), false);

    previousButton.setEnabled(currentWave > 0);
    nextButton.setEnabled(currentWave < numWaves() - 1);
    removeButton.setEnabled(numWaves() > 1);

    waveGraph.repaint();
    modulationGraph.repaint();
    repaint();
}

void WavePanel::paint(juce::Graphics& g)
{
    theme::drawPanel(g, getLocalBounds().toFloat());
    theme::drawLed(g, { 14.0f, 14.0f }, 4.0f, theme::envelopeColour(envelopeDutyCycle), true);

    g.setColour(theme::text);
    g.setFont(theme::labelFont(12.0f));
    g.drawText("WAVE", 24, 6, 50, 16, juce::Justification::centredLeft);

    g.setColour(theme::textDim);
    g.setFont(juce::FontOptions(11.0f));
    g.drawText(juce::String(currentWave + 1) + " of " + juce::String(numWaves()), 76, 6, 80, 16, juce::Justification::centredLeft);

    if (fds)
        g.drawText("Modulation table", modulationGraph.getX(), 6, modulationWidth, 16, juce::Justification::centredLeft);
}

void WavePanel::resized()
{
    auto area = getLocalBounds().reduced(4, 4).withTrimmedLeft(4);
    auto info = area.removeFromLeft(infoWidth).withTrimmedTop(20);

    auto navigation = info.removeFromTop(24);
    const auto buttonWidth = navigation.getWidth() / 4;

    for (auto* button : { &previousButton, &nextButton, &addButton, &removeButton })
        button->setBounds(navigation.removeFromLeft(buttonWidth).reduced(1, 0));

    info.removeFromTop(2);

    auto repeatRow = info.removeFromTop(22);
    repeatLabel.setBounds(repeatRow.removeFromLeft(50));
    repeatField.setBounds(repeatRow.removeFromLeft(50));

    info.removeFromTop(2);

    auto shapes = info.removeFromTop(22);
    const auto shapeWidth = shapes.getWidth() / 4;

    for (auto* button : { &sineButton, &sawButton, &squareButton, &triangleButton })
    {
        button->setBounds(shapes.removeFromLeft(shapeWidth).reduced(1, 0));
        button->setConnectedEdges(0);
    }

    area.removeFromLeft(6);

    textField.setBounds(area.removeFromBottom(24));
    area.removeFromBottom(4);

    if (fds)
    {
        auto modulation = area.removeFromRight(modulationWidth).withTrimmedTop(18);
        modulationGraph.setBounds(modulation);
        area.removeFromRight(6);
    }

    waveGraph.setBounds(area);
}
