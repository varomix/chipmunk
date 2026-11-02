// Runs the real processor and editor without a host. Pass a folder to also write screenshots.
#include "ui/PluginEditor.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>

namespace
{
int failures = 0;

void check(bool ok, const juce::String& what)
{
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8());
    failures += ok ? 0 : 1;
}

struct TempFolder
{
    juce::File folder = juce::File::createTempFile("chipmunk_test").getSiblingFile("chipmunk_test_" + juce::String(juce::Random::getSystemRandom().nextInt64()));
    juce::File presets = folder.getChildFile("Presets");
    TempFolder() { presets.createDirectory(); }
    ~TempFolder() { folder.deleteRecursively(); }
};

std::vector<float> render(ChipmunkProcessor& processor, int numSamples, juce::MidiBuffer midi = {})
{
    juce::AudioBuffer<float> buffer(2, numSamples);
    buffer.clear();
    processor.processBlock(buffer, midi);
    return { buffer.getReadPointer(0), buffer.getReadPointer(0) + numSamples };
}

double rms(const std::vector<float>& audio, size_t from = 0)
{
    double sum = 0.0;
    for (size_t i = from; i < audio.size(); ++i)
        sum += audio[i] * audio[i];
    return std::sqrt(sum / (double) (audio.size() - from));
}

// Finds the period by autocorrelation, since a saw's stair-steps cross zero more than once a cycle.
double frequency(const std::vector<float>& audio, int from)
{
    const auto compareLength = 12000;
    auto best = -1.0e30;
    auto bestLag = 0;

    for (int lag = 30; lag < 600; ++lag)
    {
        auto sum = 0.0;
        for (int i = 0; i < compareLength; ++i)
            sum += audio[(size_t) (from + i)] * audio[(size_t) (from + i + lag)];

        // A small preference for short lags, so whole multiples of the period don't win.
        sum *= 1.0 - lag * 1.0e-4;

        if (sum > best)
        {
            best = sum;
            bestLag = lag;
        }
    }

    // Average across many cycles for precision.
    const auto cycles = 30;
    auto bestLong = -1.0e30;
    auto longLag = bestLag * cycles;

    for (int lag = bestLag * cycles - cycles; lag <= bestLag * cycles + cycles; ++lag)
    {
        auto sum = 0.0;
        for (int i = 0; i < compareLength; ++i)
            sum += audio[(size_t) (from + i)] * audio[(size_t) (from + i + lag)];

        if (sum > bestLong)
        {
            bestLong = sum;
            longLag = lag;
        }
    }

    return 48000.0 * cycles / longLag;
}

juce::MidiBuffer noteAt(int position, bool on, int note = 69)
{
    juce::MidiBuffer midi;
    midi.addEvent(on ? juce::MidiMessage::noteOn(1, note, (juce::uint8) 127) : juce::MidiMessage::noteOff(1, note), position);
    return midi;
}

void setParam(ChipmunkProcessor& p, const char* id, float plainValue)
{
    auto* parameter = p.getParameters().getParameter(id);
    parameter->setValueNotifyingHost(parameter->convertTo0to1(plainValue));
}

juce::MouseEvent mouseEvent(juce::Component& target, juce::Point<float> position, juce::Point<float> down, int clicks = 1, int extraModifiers = 0)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), position, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier | extraModifiers),
                            1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &target, &target, now, down, now, clicks, position != down);
}

void strokeThrough(EnvelopeGraph& graph, std::vector<juce::Point<float>> points)
{
    graph.mouseDown(mouseEvent(graph, points[0], points[0]));
    for (size_t i = 1; i < points.size(); ++i)
        graph.mouseDrag(mouseEvent(graph, points[i], points[0]));
    graph.mouseUp(mouseEvent(graph, points.back(), points[0]));
}

void testStateRestoresIdentically()
{
    std::puts("saved state, as in a Live Set");

    TempFolder temp;
    ChipmunkProcessor original(temp.presets);
    original.prepareToPlay(48000.0, 512);

    Preset preset;
    std::string error;
    parsePreset("Chipmunk preset 1\nname=Saved Sound\nchannel=triangle\nglide=9\nvolume=15 12 | 9 8 / 4 0\narpeggio=| 0 12\npitch=1 -1\npitchRelative=1\n", preset, error);
    original.loadPreset(preset);
    setParam(original, "gain", -3.0f);

    juce::MemoryBlock state;
    original.getStateInformation(state);

    ChipmunkProcessor restored(temp.presets);
    restored.prepareToPlay(48000.0, 512);
    restored.setStateInformation(state.getData(), (int) state.getSize());

    check(formatPreset(restored.currentPreset()) == formatPreset(original.currentPreset()), "sound and settings come back");
    check(std::abs(restored.getParameters().getRawParameterValue("gain")->load() + 3.0f) < 0.01f, "output level comes back");

    render(original, 512);
    render(restored, 512);
    const auto a = render(original, 48000, noteAt(100, true));
    const auto b = render(restored, 48000, noteAt(100, true));
    check(a == b && rms(a) > 0.01, "both play bit-identical audio");

    juce::MemoryBlock again;
    restored.getStateInformation(again);
    check(again == state, "saving the restored state gives identical bytes");

    // State from before instruments existed.
    ChipmunkProcessor oldSet(temp.presets);
    auto oldState = oldSet.getParameters().copyState();
    oldState.setProperty("marker", 1, nullptr);
    juce::MemoryBlock oldBlock;
    oldSet.copyXmlToBinary(*oldState.createXml(), oldBlock);
    oldSet.setStateInformation(oldBlock.getData(), (int) oldBlock.getSize());
    check(oldSet.getDocument().snapshot().envelopes[envelopeVolume].length == 0, "sets saved by earlier builds still load");

    // State from before the rename.
    ChipmunkProcessor renamedFrom(temp.presets);
    auto renamedState = renamedFrom.getParameters().copyState();
    renamedState.setProperty("marker", 2, nullptr);
    auto renamedXml = renamedState.createXml();
    renamedXml->setTagName("VaroNES");
    juce::MemoryBlock renamedBlock;
    renamedFrom.copyXmlToBinary(*renamedXml, renamedBlock);
    renamedFrom.setStateInformation(renamedBlock.getData(), (int) renamedBlock.getSize());
    check((int) renamedFrom.getParameters().state.getProperty("marker") == 2, "sets saved under the old name still load");
    juce::MemoryBlock resaved;
    renamedFrom.getStateInformation(resaved);
    check(renamedFrom.getXmlFromBinary(resaved.getData(), (int) resaved.getSize())->hasTagName("Chipmunk"), "they save under the new name");

    const char junk[] = "not a state";
    oldSet.setStateInformation(junk, sizeof(junk));
    check(oldSet.currentPreset().name == "Init", "junk state is ignored");

    check(! restored.getDocument().canUndo(), "reopening a project starts with empty undo history");
}

void testAudioPath()
{
    std::puts("audio path");

    TempFolder temp;
    ChipmunkProcessor processor(temp.presets);
    processor.prepareToPlay(48000.0, 512);

    render(processor, 512);
    const auto silent = render(processor, 4800);
    check(rms(silent) < 0.0001, "silent before any note");

    const auto a = render(processor, 24000, noteAt(0, true));
    check(rms(a, 2400) > 0.05, "MIDI notes sound");

    // An edit made on the UI side reaches the sounding note.
    const auto loud = rms(render(processor, 12000));
    processor.getDocument().edit([](Instrument& inst) { std::string e; parseEnvelope("2", envelopeVolume, inst.envelopes[envelopeVolume], e); });
    render(processor, 2400);
    const auto quiet = rms(render(processor, 12000));
    std::printf("  rms before edit %.4f, after %.4f\n", loud, quiet);
    check(quiet < loud * 0.4 && quiet > 0.001, "a document edit changes the held note");
    check(processor.getEnvelopePosition(envelopeVolume) == 0, "playhead position is published");

    render(processor, 12000, noteAt(0, false));
    check(processor.getEnvelopePosition(envelopeVolume) == -1, "playhead clears after note-off");
    processor.getDocument().undo();

    for (int channel = 0; channel < 3; ++channel)
    {
        setParam(processor, "channel", (float) channel);
        render(processor, 512);
        const auto audio = render(processor, 24000, noteAt(0, true, 60));
        render(processor, 4800, noteAt(0, false, 60));
        check(rms(audio, 2400) > 0.01, "channel " + juce::String(channel) + " sounds");
    }

    setParam(processor, "channel", 0.0f);
    render(processor, 512);
    processor.audition();
    const auto auditioned = render(processor, 12000);
    check(rms(auditioned, 2400) > 0.05, "audition plays a note");

    // Keyboard state note-offs run from a timer in real use, so stop it directly here.
    processor.getKeyboardState().allNotesOff(1);
    render(processor, 512);
    check(rms(render(processor, 12000), 2400) < 0.001, "audition note ends");
}

void testEnvelopeGestures()
{
    std::puts("envelope editing with real mouse handlers");

    TempFolder temp;
    ChipmunkProcessor processor(temp.presets);
    ChipmunkEditor editor(processor);
    auto& document = processor.getDocument();

    auto& volumePanel = editor.getPanel(envelopeVolume);
    auto& graph = volumePanel.getGraph();
    const auto w = (float) graph.getWidth(), h = (float) graph.getHeight();
    const auto top = (float) EnvelopeGraph::rulerHeight;
    const auto stepWidth = w / 32.0f;
    const auto stepX = [&](int step) { return ((float) step + 0.5f) * stepWidth; };
    const auto yFor = [&](int value) { return h - (h - top) * ((float) value + 0.5f) / 16.0f; };
    const auto volume = [&] { return document.snapshot().envelopes[envelopeVolume]; };

    check(w > 100.0f && h > 40.0f, "graph has a usable size");

    strokeThrough(graph, { { stepX(0), yFor(15) }, { stepX(5), yFor(5) } });
    auto env = volume();
    check(env.length == 6, "dragging across the graph draws those steps");
    check(env.values[0] == 15 && env.values[5] == 5, "values follow the mouse height");
    check(env.values[2] == 11 || env.values[2] == 12, "fast strokes are interpolated");

    strokeThrough(graph, { { stepX(3), yFor(2) } });
    check(volume().values[3] == 2, "a single click sets a single step");

    check(document.undo() && volume().values[3] != 2, "one click is one undo step");
    check(document.undo() && volume().length == 0, "one stroke is one undo step");
    document.redo();
    document.redo();
    editor.refreshFromProcessor();

    // Holding Shift adjusts one step from its current value, at a fraction of the normal speed.
    {
        constexpr auto shift = juce::ModifierKeys::shiftModifier;
        const auto rowHeight = (h - top) / 16.0f;
        const auto pixelsPerValue = juce::jlimit(12.0f, 32.0f, rowHeight * 4.0f);
        const auto shiftEvent = [&](juce::Point<float> p, int modifiers = juce::ModifierKeys::shiftModifier) { return mouseEvent(graph, p, p, 1, modifiers); };

        document.edit([](Instrument& inst) { inst.envelopes[envelopeVolume] = {}; });
        document.edit([](Instrument& inst) {
            for (int i = 0; i < 6; ++i)
                setEnvelopeValue(inst.envelopes[envelopeVolume], envelopeVolume, i, 8, 3);
        });
        editor.refreshFromProcessor();

        const auto x = stepX(2);
        const auto startY = yFor(1); // Far from the bar's height, where a normal click would set it.

        graph.mouseDown(shiftEvent({ x, startY }));
        graph.mouseUp(shiftEvent({ x, startY }));
        check(volume().values[2] == 8, "a Shift-click leaves the value alone");

        const auto undoBefore = volume();
        document.undo();
        check(volume().values[1] != 8, "and adds no undo step");
        document.redo();
        check(volume().values[1] == undoBefore.values[1], "undo state is intact");
        editor.refreshFromProcessor();

        graph.mouseDown(shiftEvent({ x, startY }));
        graph.mouseDrag(shiftEvent({ x, startY - rowHeight }));
        check(pixelsPerValue < rowHeight * 2.0f || volume().values[2] == 8, "one row of movement is less than one value");
        graph.mouseDrag(shiftEvent({ x, startY - pixelsPerValue * 2.0f }));
        check(volume().values[2] == 10, "the value moves from where it was, at the slower speed");

        // A wobble sideways stays on the step.
        graph.mouseDrag(shiftEvent({ stepX(4), startY - pixelsPerValue * 3.0f }));
        check(volume().values[2] == 11 && volume().values[3] == 8 && volume().values[4] == 8, "other steps aren't touched");

        // Shift can come up before the mouse without the value jumping to the mouse's height.
        graph.mouseDrag(shiftEvent({ x, startY - pixelsPerValue * 3.0f }, 0));
        check(volume().values[2] == 11, "letting go of Shift mid-drag keeps the value");

        // However far past the limit it goes, moving back reverses at once.
        graph.mouseDrag(shiftEvent({ x, startY - pixelsPerValue * 40.0f }));
        check(volume().values[2] == 15, "values stop at their limit");
        graph.mouseDrag(shiftEvent({ x, startY - pixelsPerValue * 39.0f }));
        check(volume().values[2] == 14, "and moving back reverses straight away");
        graph.mouseUp(shiftEvent({ x, startY - pixelsPerValue * 39.0f }));

        document.undo();
        check(volume().values[2] == 8, "one fine adjustment is one undo step");
        document.redo();
        editor.refreshFromProcessor();

        // Pressing Shift partway through a normal stroke starts from what was just drawn.
        graph.mouseDown(mouseEvent(graph, { stepX(4), yFor(3) }, { stepX(4), yFor(3) }));
        check(volume().values[4] == 3, "a normal click still sets the value");
        graph.mouseDrag(shiftEvent({ stepX(4), yFor(3) }));
        check(volume().values[4] == 3, "pressing Shift mid-stroke doesn't jump");
        graph.mouseDrag(shiftEvent({ stepX(4), yFor(3) - pixelsPerValue }));
        check(volume().values[4] == 4, "and it fine-tunes from there");
        graph.mouseUp(shiftEvent({ stepX(4), yFor(3) - pixelsPerValue }));

        // After release, dragging draws normally again.
        graph.mouseDown(mouseEvent(graph, { stepX(5), yFor(12) }, { stepX(5), yFor(12) }));
        graph.mouseUp(mouseEvent(graph, { stepX(5), yFor(12) }, { stepX(5), yFor(12) }));
        check(volume().values[5] == 12, "normal drawing comes back afterwards");

        // A step past the end has no value yet, so the click draws it.
        graph.mouseDown(shiftEvent({ stepX(9), yFor(6) }));
        graph.mouseUp(shiftEvent({ stepX(9), yFor(6) }));
        check(volume().length == 10 && volume().values[9] == 6, "a Shift-click past the end draws the new step");

        // Pitch has a row far too thin to hit with the mouse, which is what this is for.
        auto& pitchGraph = editor.getPanel(envelopePitch).getGraph();
        const auto pitch = [&] { return document.snapshot().envelopes[envelopePitch]; };
        document.edit([](Instrument& inst) { setEnvelopeValue(inst.envelopes[envelopePitch], envelopePitch, 0, 0, 3); });
        editor.refreshFromProcessor();

        const auto px = pitchGraph.stepWidth() * 0.5f, py = (float) pitchGraph.getHeight() * 0.5f;
        pitchGraph.mouseDown(mouseEvent(pitchGraph, { px, py }, { px, py }, 1, shift));
        pitchGraph.mouseDrag(mouseEvent(pitchGraph, { px, py + 36.0f }, { px, py }, 1, shift));
        check(pitch().values[0] == -3, "bipolar envelopes move a value every 12 pixels at most zooms");
        pitchGraph.mouseUp(mouseEvent(pitchGraph, { px, py + 36.0f }, { px, py }, 1, shift));

        document.edit([](Instrument& inst) { inst.envelopes[envelopeVolume] = {}; inst.envelopes[envelopePitch] = {}; });
        document.edit([](Instrument& inst) {
            const std::array<int, 6> values { 15, 13, 11, 2, 7, 5 };
            for (int i = 0; i < 6; ++i)
                setEnvelopeValue(inst.envelopes[envelopeVolume], envelopeVolume, i, values[(size_t) i], 3);
        });
        editor.refreshFromProcessor();
    }

    // Length handle sits in the ruler at the envelope's end.
    strokeThrough(graph, { { 6.0f * stepWidth, 8.0f }, { 10.0f * stepWidth, 8.0f } });
    check(volume().length == 10, "dragging the length marker lengthens");
    strokeThrough(graph, { { 10.0f * stepWidth, 8.0f }, { 8.0f * stepWidth, 8.0f } });
    check(volume().length == 8, "and shortens");

    document.edit([](Instrument& inst) { setEnvelopeLoop(inst.envelopes[envelopeVolume], 1); setEnvelopeRelease(inst.envelopes[envelopeVolume], envelopeVolume, 6); });
    editor.refreshFromProcessor();

    strokeThrough(graph, { { 1.0f * stepWidth + 5.0f, 8.0f }, { 3.0f * stepWidth + 2.0f, 8.0f } });
    check(volume().loop == 3, "dragging the L marker moves the loop");
    strokeThrough(graph, { { 6.0f * stepWidth + 5.0f, 8.0f }, { 5.0f * stepWidth + 2.0f, 8.0f } });
    check(volume().release == 5, "dragging the R marker moves the release");
    strokeThrough(graph, { { 5.0f * stepWidth + 5.0f, 8.0f }, { 1.0f * stepWidth + 2.0f, 8.0f } });
    check(volume().release == 4, "the release stops at the loop");

    graph.mouseDoubleClick(mouseEvent(graph, { 4.0f * stepWidth + 5.0f, 8.0f }, { 4.0f * stepWidth + 5.0f, 8.0f }, 2));
    check(volume().release == -1 && volume().loop == 3, "double-clicking R removes the release");
    graph.mouseDoubleClick(mouseEvent(graph, { 3.0f * stepWidth + 5.0f, 8.0f }, { 3.0f * stepWidth + 5.0f, 8.0f }, 2));
    check(volume().loop == -1, "double-clicking L removes the loop");

    // Bipolar graph: the centre is zero.
    auto& arpGraph = editor.getPanel(envelopeArpeggio).getGraph();
    const auto arpH = (float) arpGraph.getHeight();
    const auto centre = top + (arpH - top) * 0.5f;
    strokeThrough(arpGraph, { { stepX(0), centre } });
    check(document.snapshot().envelopes[envelopeArpeggio].values[0] == 0, "centre of a pitch graph is zero");
    strokeThrough(arpGraph, { { stepX(1), top + 1.0f } });
    check(document.snapshot().envelopes[envelopeArpeggio].values[1] > 10, "top is positive");
    strokeThrough(arpGraph, { { stepX(2), arpH - 1.0f } });
    check(document.snapshot().envelopes[envelopeArpeggio].values[2] < -10, "bottom is negative");

    // Text fields.
    auto& field = volumePanel.getTextField();
    check(volumePanel.getValueStrip().isVisible() && ! field.isVisible(), "numbers show until you click them");
    volumePanel.getValueStrip().mouseDown(mouseEvent(volumePanel.getValueStrip(), { 5.0f, 5.0f }, { 5.0f, 5.0f }));
    check(field.isVisible() && ! volumePanel.getValueStrip().isVisible(), "clicking the numbers opens the text field");
    field.setText("15 12 | 9 / 3 0");
    field.onReturnKey();
    env = volume();
    check(env.length == 5 && env.loop == 2 && env.release == 3, "typed text applies on Return");
    check(volumePanel.getErrorText().isEmpty(), "with no error shown");
    check(volumePanel.getValueStrip().isVisible() && ! field.isVisible(), "and the numbers come back");
    volumePanel.startTyping();

    field.setText("15 99");
    field.onReturnKey();
    check(volumePanel.getErrorText().contains("outside"), "bad text shows why: " + volumePanel.getErrorText());
    check(field.isVisible(), "the field stays open until the text is fixed");
    check(volume().length == 5, "and leaves the envelope alone");
    field.onEscapeKey();
    check(volumePanel.getErrorText().isEmpty() && field.getText() == "15 12 | 9 / 3 0", "Escape restores the text");
    check(volumePanel.getValueStrip().isVisible(), "and returns to the numbers");

    document.edit([](Instrument& inst) { setEnvelopeLength(inst.envelopes[envelopeVolume], envelopeVolume, 2); });
    editor.refreshFromProcessor();
    check(field.getText() == "15 12", "the text follows graph changes");

    const auto versionBefore = document.version();
    editor.refreshFromProcessor();
    check(document.version() == versionBefore, "refreshing doesn't change the document");
}

void testChips()
{
    std::puts("VRC6, MMC5 and S5B on the device");

    TempFolder temp;
    ChipmunkProcessor processor(temp.presets);
    processor.prepareToPlay(48000.0, 512);
    ChipmunkEditor editor(processor);
    auto& document = processor.getDocument();

    // Every channel plays through the real audio path, is in tune and goes silent.

    const auto names = ChipmunkProcessor::channelNames();
    double pulseLevel = 0.0;

    for (const auto channel : { 0, 4, 5, 6, 7 })
    {
        Preset preset;
        preset.channel = channel == 0 ? ChannelType::pulse1 : channel == 4 ? ChannelType::vrc6Pulse1 : channel == 5 ? ChannelType::vrc6Saw : channel == 6 ? ChannelType::mmc5Pulse1 : ChannelType::s5bSquare1;
        preset.voice.dutyCycle = channel == 4 ? 7 : 2; // 50% on each, for comparing levels.
        preset.instrument.sawMasterVolume = 0;

        processor.loadPreset(preset);
        render(processor, 512);
        const auto audio = render(processor, 48000, noteAt(0, true));
        const auto level = rms(audio, 4800);
        const auto hz = frequency(audio, 4800);

        // The 5B runs its tones at half the rate its period suggests. FamiStudio writes the same
        // period for the same note, so a note sounds an octave lower here too.
        const auto expectedHz = channel == 7 ? 220.0 : 440.0;
        const auto cents = 1200.0 * std::log2(hz / expectedHz);

        render(processor, 512, noteAt(0, false));
        render(processor, 9600);
        const auto after = rms(render(processor, 4800));

        if (channel == 0)
            pulseLevel = level;

        std::printf("  %-11s %7.2f Hz (%+5.1f cents), rms %.4f (%+5.1f dB vs pulse), after note-off %.6f\n", names[channel].toRawUTF8(), hz, cents, level, 20.0 * std::log10(level / pulseLevel), after);
        check(std::abs(cents) < 12.0, names[channel] + " is in tune");
        check(after < 0.002, names[channel] + " stops on note-off");
        check(std::abs(20.0 * std::log10(level / pulseLevel)) < 12.0, names[channel] + " is at a comparable level");
    }

    // VRC6 duty widths work through the parameter.
    {
        Preset preset;
        preset.channel = ChannelType::vrc6Pulse1;
        preset.voice.dutyCycle = 0;
        processor.loadPreset(preset);
        render(processor, 512);
        const auto thin = rms(render(processor, 24000, noteAt(0, true)), 2400);
        render(processor, 4800, noteAt(0, false));

        setParam(processor, "duty", 7.0f);
        render(processor, 512);
        const auto wide = rms(render(processor, 24000, noteAt(0, true)), 2400);
        render(processor, 4800, noteAt(0, false));

        std::printf("  VRC6 duty 6.25%% rms %.4f, 50%% rms %.4f\n", thin, wide);
        check(std::abs(thin - wide) > 0.01, "the 8 VRC6 pulse widths reach the chip");
        editor.refreshFromProcessor();
        check(editor.getDutyBox().getItemText(7) == "50%" && editor.getDutyBox().getItemText(0) == "6.25%", "the Duty menu shows VRC6 widths");
        check(editor.getDutyBox().getText() == "50%", "and shows the right one for this setting: " + editor.getDutyBox().getText());
    }

    // Saw master volume.
    {
        Preset preset;
        preset.channel = ChannelType::vrc6Saw;
        preset.instrument.sawMasterVolume = 2;
        processor.loadPreset(preset);
        render(processor, 512);
        const auto quarter = rms(render(processor, 24000, noteAt(0, true)), 2400);
        document.edit([](Instrument& i) { i.sawMasterVolume = 0; });
        render(processor, 2400);
        const auto full = rms(render(processor, 24000));
        render(processor, 4800, noteAt(0, false));
        std::printf("  saw at quarter volume rms %.4f, at full %.4f\n", quarter, full);
        check(full > quarter * 2.5, "saw volume changes the level, even on a held note");
    }

    // S5B hardware envelope and mixer.
    {
        Preset plain;
        plain.channel = ChannelType::s5bSquare1;
        processor.loadPreset(plain);
        render(processor, 512);
        const auto square = render(processor, 24000, noteAt(0, true));
        render(processor, 4800, noteAt(0, false));

        Preset buzz = plain;
        buzz.instrument.s5bEnvelopeShape = 1;
        buzz.instrument.s5bAutoPitch = true;
        buzz.instrument.s5bAutoPitchOctave = 3;
        std::string error;
        parseEnvelope("15", envelopeVolume, buzz.instrument.envelopes[envelopeVolume], error);
        processor.loadPreset(buzz);
        render(processor, 512);
        const auto buzzy = render(processor, 24000, noteAt(0, true));
        render(processor, 4800, noteAt(0, false));
        check(buzzy != square && rms(buzzy, 2400) > 0.01, "the hardware envelope changes the sound");

        Preset noisy = plain;
        parseEnvelope("1", envelopeS5bMixer, noisy.instrument.envelopes[envelopeS5bMixer], error);
        parseEnvelope("8", envelopeS5bNoise, noisy.instrument.envelopes[envelopeS5bNoise], error);
        processor.loadPreset(noisy);
        render(processor, 512);
        const auto noise = render(processor, 24000, noteAt(0, true));
        render(processor, 4800, noteAt(0, false));
        check(rms(noise, 2400) > 0.01, "the mixer envelope can select noise");

        long squareCrossings = 0, noiseCrossings = 0;
        for (size_t i = 2401; i < 24000; ++i)
        {
            squareCrossings += square[i - 1] < 0.0f && square[i] >= 0.0f;
            noiseCrossings += noise[i - 1] < 0.0f && noise[i] >= 0.0f;
        }
        std::printf("  rising zero crossings, tone %ld, noise %ld\n", squareCrossings, noiseCrossings);
        check(noiseCrossings != squareCrossings, "noise differs from the tone");

        Preset silent = plain;
        parseEnvelope("3", envelopeS5bMixer, silent.instrument.envelopes[envelopeS5bMixer], error);
        processor.loadPreset(silent);
        render(processor, 512);
        const auto off = render(processor, 24000, noteAt(0, true));
        render(processor, 4800, noteAt(0, false));
        std::printf("  mixer off rms %.5f, tone rms %.5f\n", rms(off, 9600), rms(square, 9600));
        check(rms(off, 9600) < rms(square, 9600) * 0.5, "mixer value 3 turns both off");
    }

    // Editor.
    processor.loadPreset(Preset {});
    setParam(processor, "channel", 7.0f);
    editor.refreshFromProcessor();
    check(editor.getPanel(envelopeS5bMixer).isVisible() && editor.getPanel(envelopeS5bNoise).isVisible(), "S5B shows its mixer and noise envelopes");
    check(! editor.getPanel(envelopeDutyCycle).isVisible(), "and hides duty, which it doesn't have");
    check(editor.getS5bShapeBox().isVisible() && ! editor.getSawVolumeBox().isVisible(), "and its envelope settings");

    editor.getS5bShapeBox().setSelectedId(3, juce::sendNotificationSync);
    check(document.snapshot().s5bEnvelopeShape == 2, "changing the shape edits the sound");
    document.undo();
    editor.refreshFromProcessor();
    check(document.snapshot().s5bEnvelopeShape == 0 && editor.getS5bShapeBox().getSelectedId() == 1, "and can be undone, with the menu following");

    const auto versionBefore = document.version();
    editor.refreshFromProcessor();
    check(document.version() == versionBefore, "showing settings doesn't edit the sound");

    setParam(processor, "channel", 5.0f);
    editor.refreshFromProcessor();
    check(editor.getSawVolumeBox().isVisible() && ! editor.getS5bShapeBox().isVisible() && ! editor.getPanel(envelopeS5bMixer).isVisible(), "the saw shows only its own setting");
    check(editor.getPanel(envelopeDutyCycle).isVisible(), "other channels keep the duty panel");

    setParam(processor, "channel", 4.0f);
    editor.refreshFromProcessor();
    auto& dutyPanel = editor.getPanel(envelopeDutyCycle);
    dutyPanel.startTyping();
    dutyPanel.getTextField().setText("0 3 7");
    dutyPanel.getTextField().onReturnKey();
    check(formatEnvelope(document.snapshot().envelopes[envelopeDutyCycle]) == "0 3 7", "VRC6 duty envelopes accept 0 to 7");

    setParam(processor, "channel", 0.0f);
    editor.refreshFromProcessor();
    dutyPanel.startTyping();
    dutyPanel.getTextField().setText("0 7");
    dutyPanel.getTextField().onReturnKey();
    check(dutyPanel.getErrorText().contains("0 to 3"), "other pulse channels stop at 3: " + dutyPanel.getErrorText());
    dutyPanel.getTextField().onEscapeKey();

    // Saved state, with chip settings.
    Preset full;
    full.name = "Full S5B";
    full.channel = ChannelType::s5bSquare1;
    full.instrument.s5bEnvelopeShape = 4;
    full.instrument.s5bAutoPitch = false;
    full.instrument.s5bAutoPitchOctave = -2;
    full.instrument.s5bEnvelopePeriod = 2345;
    full.instrument.sawMasterVolume = 2;
    std::string err;
    parseEnvelope("0 1 2 3", envelopeS5bMixer, full.instrument.envelopes[envelopeS5bMixer], err);
    parseEnvelope("| 31 0", envelopeS5bNoise, full.instrument.envelopes[envelopeS5bNoise], err);
    processor.loadPreset(full);

    juce::MemoryBlock state;
    processor.getStateInformation(state);
    ChipmunkProcessor a(temp.presets), b(temp.presets);
    a.prepareToPlay(48000.0, 512);
    b.prepareToPlay(48000.0, 512);
    a.setStateInformation(state.getData(), (int) state.getSize());
    b.setStateInformation(state.getData(), (int) state.getSize());
    check(formatPreset(a.currentPreset()) == formatPreset(processor.currentPreset()), "a saved set brings chip settings back");
    check(a.getChannelIndex() == 7, "and the channel");
    render(a, 512);
    render(b, 512);
    const auto first = render(a, 24000, noteAt(0, true));
    const auto second = render(b, 24000, noteAt(0, true));
    std::printf("  restored S5B rms %.5f\n", rms(first));
    check(first == second && rms(first) > 0.001, "restored chips play bit-identical audio");

    // New chips must start from the same state, whatever was in memory before. Fill the heap with junk first.
    {
        std::vector<std::unique_ptr<char[]>> junk;
        for (int i = 0; i < 200; ++i)
        {
            junk.push_back(std::make_unique<char[]>(16384 + (size_t) i * 64));
            std::memset(junk.back().get(), 0x7f, 16384 + (size_t) i * 64);
        }
        junk.clear();

        ChipmunkProcessor c(temp.presets);
        c.prepareToPlay(48000.0, 512);
        c.setStateInformation(state.getData(), (int) state.getSize());
        render(c, 512);
        check(render(c, 24000, noteAt(0, true)) == first, "a chip created in dirty memory sounds the same");
    }

    // Imports.
    const juce::File projects(CHIPMUNK_PROJECTS_DIR);
    FmsImportResult vrc6, s5b, mmc5, multi;
    juce::String error;
    PresetLibrary::readProject(projects.getChildFile("TestVRC6.fms"), vrc6, error);
    PresetLibrary::readProject(projects.getChildFile("TestS5B.fms"), s5b, error);
    PresetLibrary::readProject(projects.getChildFile("TestMMC5.fms"), mmc5, error);
    PresetLibrary::readProject(projects.getChildFile("TestMulti.fms"), multi, error);

    int vrc6Sounds = 0, sawSounds = 0, s5bSounds = 0, shapes = 0, mixers = 0, mmc5Sounds = 0;
    for (const auto& p : vrc6.presets)
    {
        vrc6Sounds += p.channel == ChannelType::vrc6Pulse1;
        sawSounds += p.channel == ChannelType::vrc6Saw;
    }
    for (const auto& p : s5b.presets)
    {
        s5bSounds += p.channel == ChannelType::s5bSquare1;
        shapes += p.instrument.s5bEnvelopeShape > 0;
        mixers += ! p.instrument.envelopes[envelopeS5bMixer].isEmpty(envelopeS5bMixer);
    }
    for (const auto& p : mmc5.presets)
        mmc5Sounds += p.channel == ChannelType::mmc5Pulse1;

    std::printf("  TestVRC6: %d pulse, %d saw. TestS5B: %d S5B, %d with a hardware envelope, %d with a mixer envelope. TestMMC5: %d on MMC5. TestMulti: %d sounds, %d other-chip\n",
                vrc6Sounds, sawSounds, s5bSounds, shapes, mixers, mmc5Sounds, (int) multi.presets.size(), multi.skippedExpansion);
    check(vrc6Sounds + sawSounds == 5, "VRC6 instruments land on VRC6 channels");
    check(s5bSounds == 9, "S5B instruments land on the S5B channel");
    check(mmc5Sounds == 0, "2A03 instruments stay on pulse when they're used equally on both");
    check((int) multi.presets.size() == 41 && multi.skippedExpansion == 0, "a project using every chip brings in all of its sounds");
    check(shapes > 0 && mixers > 0, "with their envelopes and settings");

    processor.getKeyboardState().allNotesOff(1);
}

void testNumbersLineUpWithColumns()
{
    std::puts("step numbers line up with their columns");

    TempFolder temp;
    ChipmunkProcessor processor(temp.presets);
    ChipmunkEditor editor(processor);

    for (const auto [width, height] : std::vector<std::pair<int, int>> {{1000, 640}, {1100, 760}, {1800, 1300}})
    {
        editor.setSize(width, height);

        for (int type = 0; type <= envelopeDutyCycle; ++type)
        {
            auto& panel = editor.getPanel(type);
            auto& graph = panel.getGraph();
            auto& strip = panel.getValueStrip();

            check(strip.getX() == graph.getX() && strip.getWidth() == graph.getWidth(), "the numbers and graph share the same span");

            for (const auto length : { 8, 32, 40, 64, 128, 256 })
            {
                // The widest numbers these envelopes commonly hold.
                processor.getDocument().edit([&](Instrument& inst) {
                    auto& env = inst.envelopes[(size_t) type];
                    setEnvelopeLength(env, type, length);
                    for (int i = 0; i < length; ++i)
                        env.values[(size_t) i] = (int8_t) (type == envelopeDutyCycle ? 3 : type == envelopeVolume ? 15 : 12);
                });
                editor.refreshFromProcessor();

                // Clicking at a label's centre in the graph must select that label's own step.
                bool aligned = true;
                const auto interval = strip.labelInterval();
                const auto steps = graph.getWidth() / juce::jmax(0.001f, graph.stepWidth());

                for (int step = 0; step < length; step += interval)
                {
                    const auto bounds = strip.labelBounds(step);
                    const auto expectedX = (float) step * graph.stepWidth();
                    aligned = aligned && std::abs(bounds.getX() - expectedX) < 0.001f && std::abs(bounds.getWidth() - graph.stepWidth()) < 0.001f;
                }

                check(aligned && steps >= (float) length, juce::String(length) + " steps at " + juce::String(width) + " wide, type " + juce::String(type) + ", showing every " + juce::String(interval));

                if (length <= 40)
                    check(interval == 1, "up to 40 steps, every number is shown");
            }
        }
    }
}

void testBrowser()
{
    std::puts("browser and library");

    TempFolder temp;
    ChipmunkProcessor processor(temp.presets);
    processor.prepareToPlay(48000.0, 512);
    ChipmunkEditor editor(processor);
    auto& browser = editor.getBrowser();

    const auto total = (int) factoryPresets().size();
    check(browser.getNumVisible() == total, "lists the factory sounds");

    browser.getSearchField().setText("major");
    browser.refreshList(); // The field's own change callback arrives through the message loop.
    check(browser.getNumVisible() == 1 && browser.getVisibleName(0) == "Major Arp", "search filters by name");
    browser.getSearchField().setText("NOI");
    browser.refreshList();
    const auto numNoise = std::count_if(factoryPresets().begin(), factoryPresets().end(), [](const Preset& p) { return p.channel == ChannelType::noise; });
    check(browser.getNumVisible() == (int) numNoise, "search filters by channel");
    browser.getSearchField().setText("");
    browser.refreshList();
    check(browser.getNumVisible() == total, "clearing the search shows everything");

    browser.getList().selectRow(1);
    check(processor.currentPreset().name == browser.getVisibleName(1).toStdString(), "selecting a row loads it");
    check(processor.getKeyboardState().isNoteOn(1, 60), "and auditions it");

    browser.step(1);
    check(browser.getList().getSelectedRow() == 2 && processor.currentPreset().name == browser.getVisibleName(2).toStdString(), "stepping loads the next sound");
    browser.step(-1);
    check(browser.getList().getSelectedRow() == 1, "and the previous one");

    // Loading sets the channel too.
    int noiseRow = -1;
    for (int i = 0; i < browser.getNumVisible(); ++i)
        if (browser.getVisibleName(i) == "Snare")
            noiseRow = i;
    browser.getList().selectRow(noiseRow);
    check(processor.getChannelIndex() == 2, "a sound brings its channel");
    editor.refreshFromProcessor();
    check(formatEnvelope(editor.getPanel(envelopeVolume).getGraph().isVisible() ? processor.getDocument().snapshot().envelopes[envelopeVolume] : Envelope {}) == "15 13 11 9 7 6 5 4 3 2 1 0", "its envelopes appear");

    // Favorites persist.
    browser.clickRow(noiseRow, 5.0f);
    check(processor.getLibrary().isFavorite("Snare"), "clicking the star marks a favorite");
    browser.getFavoritesToggle().setToggleState(true, juce::sendNotificationSync);
    check(browser.getNumVisible() == 1 && browser.getVisibleName(0) == "Snare", "favorites filter works");
    browser.getFavoritesToggle().setToggleState(false, juce::sendNotificationSync);
    {
        PresetLibrary reopened(temp.presets);
        check(reopened.isFavorite("Snare"), "favorites survive a restart");
    }

    // Saving.
    browser.getNameField().setText("My Kick");
    browser.getSaveButton().onClick();
    check(temp.presets.getChildFile("My Kick.chmk").existsAsFile(), "Save writes a .chmk file");
    check(browser.getNumVisible() == total + 1, "the new sound appears in the list");
    check(browser.getVisibleName(browser.getList().getSelectedRow()) == "My Kick", "and is selected");

    browser.getNameField().setText("My Kick");
    processor.getDocument().edit([](Instrument& inst) { std::string e; parseEnvelope("7", envelopeVolume, inst.envelopes[envelopeVolume], e); });
    browser.getSaveButton().onClick();
    check(browser.getNumVisible() == total + 1, "saving the same name updates it instead of adding another");

    Preset loaded;
    juce::String error;
    check(PresetLibrary::readFile(temp.presets.getChildFile("My Kick.chmk"), loaded, error) && formatEnvelope(loaded.instrument.envelopes[envelopeVolume]) == "7", "the file has the latest sound");

    const auto countBeforeOldFile = processor.getLibrary().getEntries().size();
    temp.presets.getChildFile("Old Sound.vnes").replaceWithText("VaroNES preset 1\nname=Old Sound\n");
    processor.getLibrary().rescan();
    check(processor.getLibrary().getEntries().size() == countBeforeOldFile + 1, "preset files from before the rename are listed");
    temp.presets.getChildFile("Old Sound.vnes").deleteFile();
    processor.getLibrary().rescan();

    temp.presets.getChildFile("Broken.chmk").replaceWithText("nonsense");
    processor.getLibrary().rescan();
    browser.refreshList();
    check(browser.getNumVisible() == total + 1, "broken files in the folder are skipped");

    browser.getNameField().setText("   ");
    browser.getSaveButton().onClick();
    check(browser.getStatus().contains("name"), "saving without a name explains why it can't");

    // Import.
    const auto ftiFile = temp.folder.getChildFile("Lead.fti");
    juce::MemoryOutputStream fti;
    fti.write("FTI2.4", 6);
    fti.writeByte(1);
    fti.writeInt(4);
    fti.write("Lead", 4);
    fti.writeByte(1);
    fti.writeByte(1);
    fti.writeInt(3);
    fti.writeInt(-1);
    fti.writeInt(-1);
    fti.writeInt(0);
    fti.writeByte(15);
    fti.writeByte(8);
    fti.writeByte(0);
    ftiFile.replaceWithData(fti.getData(), fti.getDataSize());

    browser.importFile(ftiFile);
    check(processor.currentPreset().name == "Lead" && formatEnvelope(processor.getDocument().snapshot().envelopes[envelopeVolume]) == "15 8 0", ".fti files import");
    check(browser.getNameField().getText() == "Lead", "and show their name ready to save");
    check(editor.isInterestedInFileDrag({ ftiFile.getFullPathName() }) && ! editor.isInterestedInFileDrag({ "/tmp/a.mp4" }), "dropping only accepts sound files");
    check(processor.currentPreset().voice.dutyCycle == 0, "imports keep the chip's default duty");

    const auto badFile = temp.folder.getChildFile("Bad.fti");
    badFile.replaceWithText("nope");
    browser.importFile(badFile);
    check(browser.getStatus().contains("Bad.fti") && processor.currentPreset().name == "Lead", "a bad file reports an error and changes nothing");

    processor.getKeyboardState().allNotesOff(1);
}

// The expected data comes from the note scripts decoded separately by a local script, fms_to_script.py.
void testProjectImport()
{
    std::puts("FamiStudio project import");

    const juce::File projects(CHIPMUNK_PROJECTS_DIR);

    struct Expected { const char* file; int sounds, skippedExpansion, skippedDpcm; };

    for (const auto& e : std::vector<Expected> {
             { "TestBase", 6, 0, 0 }, { "TestFamiTrackerTempo", 2, 0, 0 }, { "TestVRC6", 11, 0, 0 }, { "TestVRC7", 12, 0, 0 }, { "TestFDS", 12, 0, 0 },
             { "TestMMC5", 6, 0, 0 }, { "TestN163", 11, 0, 0 }, { "TestS5B", 15, 0, 0 }, { "TestEPSM", 18, 0, 0 }, { "TestMulti", 0, 0, 0 }, { "TestMultiEPSM", 0, 0, 0 } })
    {
        FmsImportResult result;
        juce::String error;
        const auto ok = PresetLibrary::readProject(projects.getChildFile(juce::String(e.file) + ".fms"), result, error);
        std::printf("  %-22s %2d sounds, %2d expansion, %d DPCM  %s\n", e.file, (int) result.presets.size(), result.skippedExpansion, result.skippedDpcm, error.toRawUTF8());
        const auto expectCounts = e.sounds > 0; // The combined projects are counted by hand below.
        check(ok && (! expectCounts || ((int) result.presets.size() == e.sounds && result.skippedExpansion == e.skippedExpansion && result.skippedDpcm == e.skippedDpcm)),
              juce::String(e.file) + " reads, with every instrument accounted for");
    }

    FmsImportResult base;
    juce::String error;
    PresetLibrary::readProject(projects.getChildFile("TestBase.fms"), base, error);

    check(base.presets.size() == 6 && base.presets[0].name == "Base1" && base.presets[4].name == "Base5", "names come through");
    check(base.presets[5].channel == ChannelType::dpcm && base.presets[5].kit != nullptr, "the sample instrument comes in as a DPCM kit");

    int mappedKeys = 0;
    for (const auto& mapping : base.presets[5].kit->mappings)
        if (mapping.sample != nullptr)
        {
            ++mappedKeys;
            check((int) mapping.sample->data.size() >> 4 == 0x96, "its sample has the length FamiStudio's trace shows");
        }
    check(mappedKeys > 0, "with keys mapped");

    // Every envelope must match what the separately written decoder found in the same file.
    juce::StringArray script;
    projects.getParentDirectory().getChildFile("TestBase.script").readLines(script);

    std::vector<Instrument> expected;
    for (const auto& line : script)
    {
        if (line.startsWith("I "))
            expected.emplace_back();

        if (line.startsWith("E "))
        {
            const auto fields = juce::StringArray::fromTokens(line, " ", "");
            auto& env = expected.back().envelopes[(size_t) fields[1].getIntValue()];
            env.length = fields[2].getIntValue();
            env.loop = fields[3].getIntValue();
            env.release = fields[4].getIntValue();
            env.relative = fields[5].getIntValue() != 0;
            for (int i = 0; i < env.length; ++i)
                env.values[(size_t) i] = (int8_t) fields[6 + i].getIntValue();
        }
    }

    bool same = true;
    for (size_t i = 0; i < 5; ++i)
        for (int t = 0; t < numEnvelopeTypes; ++t)
        {
            const auto& a = base.presets[i].instrument.envelopes[(size_t) t];
            const auto& b = expected[i].envelopes[(size_t) t];
            same = same && formatEnvelope(a) == formatEnvelope(b) && a.relative == b.relative;
        }
    check(same, "all 2A03 envelopes match the song decoder's");
    check(formatEnvelope(base.presets[0].instrument.envelopes[envelopeVolume]) == "1 3 6 12 | 15 / 13 11 9 6 4 3 2", "Base1's volume is right");

    bool dutyZero = true;
    for (const auto& preset : base.presets)
        if (preset.channel != ChannelType::dpcm)
            dutyZero = dutyZero && preset.voice.dutyCycle == 0;
    check(dutyZero, "sounds keep FamiStudio's default duty");

    // Channels come from how the songs use each instrument.
    bool allPulse = true;
    for (size_t i = 0; i < 5; ++i)
        allPulse = allPulse && base.presets[i].channel == ChannelType::pulse1;
    check(allPulse, "TestBase plays each instrument mostly on its two pulse channels");

    // Bad files.
    TempFolder temp;
    const auto writeAndRead = [&](const juce::String& name, const void* data, size_t size, juce::String& message) {
        const auto file = temp.folder.getChildFile(name);
        file.replaceWithData(data, size);
        FmsImportResult r;
        return PresetLibrary::readProject(file, r, message);
    };

    juce::MemoryBlock good;
    projects.getChildFile("TestBase.fms").loadFileAsData(good);

    juce::String message;
    check(! writeAndRead("a.fms", "nope", 4, message) && message.contains("isn't a FamiStudio project"), "other files are rejected");

    auto oldVersion = good;
    static_cast<uint8_t*>(oldVersion.getData())[4] = 5;
    check(! writeAndRead("b.fms", oldVersion.getData(), oldVersion.getSize(), message) && message.contains("very old"), "very old projects explain what to do");

    auto newVersion = good;
    static_cast<uint8_t*>(newVersion.getData())[4] = 25;
    newVersion.setSize(good.getSize() - 100);
    check(! writeAndRead("c.fms", newVersion.getData(), newVersion.getSize(), message), "unreadable newer projects are rejected: " + message);

    juce::Random random(42);
    for (int i = 0; i < 300; ++i)
    {
        auto corrupt = good;
        auto* bytes = static_cast<uint8_t*>(corrupt.getData());
        for (int j = 0; j < 5; ++j)
            bytes[random.nextInt((int) corrupt.getSize())] = (uint8_t) random.nextInt(256);
        if (i % 3 == 0)
            corrupt.setSize((size_t) random.nextInt((int) corrupt.getSize()));
        writeAndRead("d.fms", corrupt.getData(), corrupt.getSize(), message);
    }
    check(true, "300 corrupted files don't crash");

    // The browser.
    ChipmunkProcessor processor(temp.presets);
    processor.prepareToPlay(48000.0, 512);
    ChipmunkEditor editor(processor);
    auto& browser = editor.getBrowser();
    const auto total = browser.getNumVisible();

    check(editor.isInterestedInFileDrag({ "/some/song.fms" }), "dropping a project is accepted");

    browser.importFile(projects.getChildFile("TestBase.fms"));
    check(browser.getNumVisible() == total + 6, "imported sounds join the list");
    processor.getKeyboardState().allNotesOff(1);
    check(browser.getVisibleName(0) == "Base1" && browser.getList().getSelectedRow() == 0, "at the top, with the first one selected");
    check(processor.currentPreset().name == "Base1" && processor.getDocument().snapshot().envelopes[envelopeVolume].length == 12, "and loaded");
    check(browser.getStatus().contains("6 sounds"), "the status counts them: " + browser.getStatus());

    render(processor, 512);
    const auto audio = render(processor, 24000);
    check(rms(audio, 2400) > 0.01, "the imported sound auditions");
    processor.getKeyboardState().allNotesOff(1);

    browser.getList().selectRow(2);
    check(processor.currentPreset().name == "Base3", "stepping through imported sounds works");

    browser.getNameField().setText("Base3");
    browser.getSaveButton().onClick();
    check(temp.presets.getChildFile("Base3.chmk").existsAsFile(), "a sound can be saved");
    check(browser.getNumVisible() == total + 7 && browser.getVisibleName(0) == "Base1", "without disturbing the imported list");
    check(! temp.presets.getChildFile("Base1.chmk").exists(), "nothing else is written to disk");
    processor.getKeyboardState().allNotesOff(1);

    const auto before = processor.currentPreset().name;
    browser.importFile(projects.getChildFile("TestVRC6.fms"));
    std::printf("  after a second import: %d rows, status: %s\n", browser.getNumVisible(), browser.getStatus().toRawUTF8());
    processor.getKeyboardState().allNotesOff(1);

    const auto junkFile = temp.folder.getChildFile("junk.fms");
    junkFile.replaceWithText("junk");
    const auto current = processor.currentPreset().name;
    browser.importFile(junkFile);
    check(processor.currentPreset().name == current, "a bad project changes nothing");
    juce::ignoreUnused(before);
}

juce::File writeTestWav(const juce::File& file, double seconds, double sampleRate = 44100.0, int channels = 1, double frequency = 120.0, float level = 0.8f)
{
    file.deleteFile();

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> stream(file.createOutputStream());
    std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(stream.get(), sampleRate, (unsigned int) channels, 16, {}, 0));
    stream.release();

    juce::AudioBuffer<float> buffer(channels, (int) (seconds * sampleRate));

    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            buffer.setSample(c, i, level * std::sin(juce::MathConstants<double>::twoPi * frequency * i / sampleRate) * (float) (1.0 - (double) i / buffer.getNumSamples()));

    writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
    writer.reset();
    return file;
}

// How long the channel keeps moving after a note, in seconds.
double soundingTime(const std::vector<float>& audio, double sampleRate, float threshold = 0.002f)
{
    int last = 0;
    for (int i = 1; i < (int) audio.size(); ++i)
        if (std::abs(audio[(size_t) i] - audio[(size_t) i - 1]) > threshold)
            last = i;
    return last / sampleRate;
}

void testDpcm()
{
    std::puts("DPCM kits");

    TempFolder temp;
    ChipmunkProcessor processor(temp.presets);
    processor.prepareToPlay(48000.0, 512);
    ChipmunkEditor editor(processor);
    auto& dpcm = editor.getDpcmEditor();
    auto& document = processor.getDocument();

    const auto kick = writeTestWav(temp.folder.getChildFile("Kick.wav"), 0.2);
    const auto snare = writeTestWav(temp.folder.getChildFile("Snare.wav"), 0.15, 48000.0, 2, 250.0);
    const auto longOne = writeTestWav(temp.folder.getChildFile("Long.wav"), 30.0, 22050.0);
    const auto silent = writeTestWav(temp.folder.getChildFile("Silent.wav"), 0.1, 44100.0, 1, 100.0, 0.0f);
    const auto notAudio = temp.folder.getChildFile("Fake.wav");
    notAudio.replaceWithText("not audio");

    check(editor.isInterestedInFileDrag({ kick.getFullPathName() }), "audio files can be dropped");
    check(PresetLibrary::isSampleFile("a.aiff") && PresetLibrary::isSampleFile("b.dmc") && ! PresetLibrary::isSampleFile("c.mp4"), "common formats are recognized");

    // Dropping switches to DPCM and maps to consecutive keys.
    editor.filesDropped({ kick.getFullPathName(), snare.getFullPathName() }, 0, 0);
    check(processor.getChannelIndex() == 3, "dropping samples switches to the DPCM channel");
    check(dpcm.isVisible() && ! editor.getPanel(envelopeVolume).isVisible(), "and shows the key map instead of envelopes");
    check(dpcm.getNumMappedKeys() == 2, "both files are mapped");
    check(dpcm.getStatus().contains("2 samples"), "the status says so: " + dpcm.getStatus());

    auto kit = document.kit();
    const auto& kickMapping = kit->mappings[(size_t) midiNoteToNoteValue(60)];
    const auto& snareMapping = kit->mappings[(size_t) midiNoteToNoteValue(61)];
    check(kickMapping.sample != nullptr && kickMapping.sample->name == "Kick", "the first lands on C3");
    check(snareMapping.sample != nullptr && snareMapping.sample->name == "Snare", "the second on the next key");
    check(kickMapping.pitch == 15, "a short sample uses the fastest rate");
    check((int) kickMapping.sample->data.size() % 16 == 0 && kickMapping.sample->data.size() > 500, "sample data is sized for the chip's length register");
    processor.getKeyboardState().allNotesOff(1);

    // Plays through the real audio path, at the right length.
    render(processor, 512);
    render(processor, 512);
    const auto hit = render(processor, 48000, noteAt(0, true, 60));
    const auto sampleSeconds = (double) kickMapping.sample->data.size() * 8.0 / dpcmRateHz(kickMapping.pitch);
    const auto heard = soundingTime(hit, 48000.0);
    std::printf("  rms %.4f, sounded for %.3f s, sample is %.3f s\n", rms(hit), heard, sampleSeconds);
    check(rms(hit) > 0.01, "a mapped key plays");
    check(std::abs(heard - sampleSeconds) < 0.02, "for as long as the sample lasts");

    // A short MIDI note doesn't cut a one-shot.
    juce::MidiBuffer shortNote;
    shortNote.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 127), 0);
    shortNote.addEvent(juce::MidiMessage::noteOff(1, 60), 100);
    const auto shortHit = render(processor, 48000, shortNote);
    check(std::abs(soundingTime(shortHit, 48000.0) - sampleSeconds) < 0.02, "a short note still rings to the end of the sample");

    render(processor, 4800);
    const auto silentKey = render(processor, 12000, noteAt(0, true, 70));
    check(soundingTime(silentKey, 48000.0) < 0.002 || rms(silentKey, 2400) < 0.001, "unmapped keys are silent");
    render(processor, 512, noteAt(0, false, 70));

    // Rate changes speed.
    dpcm.setKeyPitch(60, 7);
    processor.getKeyboardState().allNotesOff(1);
    render(processor, 512);
    const auto slower = render(processor, 96000, noteAt(0, true, 60));
    const auto slowSeconds = (double) kickMapping.sample->data.size() * 8.0 / dpcmRateHz(7);
    std::printf("  at 8.4 kHz it sounds for %.3f s, expected %.3f s\n", soundingTime(slower, 48000.0), slowSeconds);
    check(std::abs(soundingTime(slower, 48000.0) - slowSeconds) < 0.05, "a lower rate plays slower");
    check(document.kit()->mappings[(size_t) midiNoteToNoteValue(60)].pitch == 7, "pitch is stored per key");
    check(document.kit()->mappings[(size_t) midiNoteToNoteValue(61)].pitch == 15, "and doesn't affect other keys");

    // Looping.
    dpcm.setKeyPitch(60, 15);
    dpcm.setKeyLoop(60, true);
    processor.getKeyboardState().allNotesOff(1);
    render(processor, 512);
    const auto looping = render(processor, 96000, noteAt(0, true, 60));
    check(soundingTime(looping, 48000.0) > 1.9, "a looping key keeps playing");
    render(processor, 512, noteAt(0, false, 60));
    render(processor, 4800);
    check(soundingTime(render(processor, 12000), 48000.0) < 0.01, "and stops when the note ends");
    dpcm.setKeyLoop(60, false);

    // Moving, undo and removal.
    dpcm.moveKey(60, 64);
    dpcm.refresh();
    check(document.kit()->mappings[(size_t) midiNoteToNoteValue(64)].sample->name == "Kick" && document.kit()->mappings[(size_t) midiNoteToNoteValue(60)].sample == nullptr, "a sample can move to a free key");
    dpcm.moveKey(64, 61);
    dpcm.refresh();
    check(document.kit()->mappings[(size_t) midiNoteToNoteValue(61)].sample->name == "Kick" && document.kit()->mappings[(size_t) midiNoteToNoteValue(64)].sample->name == "Snare", "moving onto a used key swaps them");
    document.undo();
    document.undo();
    dpcm.refresh();
    check(document.kit()->mappings[(size_t) midiNoteToNoteValue(60)].sample->name == "Kick", "kit edits can be undone");

    dpcm.removeKey(61);
    dpcm.refresh();
    check(dpcm.getNumMappedKeys() == 1, "a key can be removed");
    document.undo();
    dpcm.refresh();
    check(dpcm.getNumMappedKeys() == 2, "and brought back");

    // Bad and long files.
    const auto before = document.version();
    dpcm.addFiles({ notAudio.getFullPathName() });
    check(dpcm.getStatus().contains("Fake.wav") && document.version() == before, "a bad file explains itself and changes nothing");
    dpcm.addFiles({ silent.getFullPathName() });
    check(dpcm.getStatus().contains("silent"), "a silent file says so");

    dpcm.setStartKey(72);
    dpcm.addFiles({ longOne.getFullPathName() });
    const auto& longMapping = document.kit()->mappings[(size_t) midiNoteToNoteValue(72)];
    check(longMapping.sample != nullptr && (int) longMapping.sample->data.size() <= dpcmMaxSampleBytes, "a long file fits the chip's limit");
    check(longMapping.pitch == 0, "using the slowest rate");
    check(dpcm.getStatus().contains("cut off"), "and says it was cut off: " + dpcm.getStatus());
    processor.getKeyboardState().allNotesOff(1);

    // A raw .dmc file plays untouched.
    const auto dmc = temp.folder.getChildFile("Raw.dmc");
    const char rawBytes[] = "\xff\xff\x00\x00\xff\x00\xff\x00\x55\xaa\x55\xaa\x12\x34\x56\x78\x9a";
    dmc.replaceWithData(rawBytes, 17);
    dpcm.setStartKey(80);
    dpcm.addFiles({ dmc.getFullPathName() });
    check(document.kit()->mappings[(size_t) midiNoteToNoteValue(80)].sample->data.size() == 17, ".dmc data is used as it is");
    processor.getKeyboardState().allNotesOff(1);

    // Saved state.
    juce::MemoryBlock state;
    processor.getStateInformation(state);

    ChipmunkProcessor restored(temp.presets);
    restored.prepareToPlay(48000.0, 512);
    restored.setStateInformation(state.getData(), (int) state.getSize());
    check(formatPreset(restored.currentPreset()) == formatPreset(processor.currentPreset()), "a saved set brings the kit back");
    check(restored.getChannelIndex() == 3, "and its channel");

    juce::MemoryBlock again;
    restored.getStateInformation(again);
    check(again == state, "saving again gives identical bytes");

    render(processor, 512);
    render(restored, 512);
    render(processor, 4800);
    render(restored, 4800);
    ChipmunkProcessor freshA(temp.presets), freshB(temp.presets);
    freshA.prepareToPlay(48000.0, 512);
    freshB.prepareToPlay(48000.0, 512);
    freshA.setStateInformation(state.getData(), (int) state.getSize());
    freshB.setStateInformation(state.getData(), (int) state.getSize());
    render(freshA, 512);
    render(freshB, 512);
    const auto a = render(freshA, 24000, noteAt(0, true, 60));
    const auto b = render(freshB, 24000, noteAt(0, true, 60));
    check(a == b && rms(a) > 0.01, "restored kits play bit-identical audio");

    // Presets.
    editor.getBrowser().getNameField().setText("My Kit");
    editor.getBrowser().getSaveButton().onClick();
    Preset saved;
    juce::String error;
    check(PresetLibrary::readFile(temp.presets.getChildFile("My Kit.chmk"), saved, error) && saved.channel == ChannelType::dpcm && saved.kit != nullptr, "kits save as presets");
    check(saved.kit->mappings[(size_t) midiNoteToNoteValue(60)].sample->data == document.kit()->mappings[(size_t) midiNoteToNoteValue(60)].sample->data, "with their sample data");
    processor.getKeyboardState().allNotesOff(1);

    // Other sounds still work after a kit, without its DAC level changing them.
    ChipmunkProcessor clean(temp.presets);
    clean.prepareToPlay(48000.0, 512);
    render(clean, 512);
    const auto reference = rms(render(clean, 24000, noteAt(0, true)), 2400);
    setParam(clean, "channel", 3.0f);
    render(clean, 512);
    setParam(clean, "channel", 0.0f);
    render(clean, 512);
    const auto afterKit = rms(render(clean, 24000, noteAt(0, true)), 2400);
    std::printf("  pulse rms %.4f, after visiting DPCM %.4f\n", reference, afterKit);
    check(std::abs(reference - afterKit) < reference * 0.05, "leaving DPCM doesn't change other channels");

    // Replacing kits under a playing sample, repeatedly, as the UI would.
    ChipmunkProcessor swap(temp.presets);
    swap.prepareToPlay(48000.0, 512);
    setParam(swap, "channel", 3.0f);
    auto first = std::make_shared<DpcmKit>(*document.kit());
    swap.getDocument().load({}, "One", first);
    first.reset();
    render(swap, 512);
    render(swap, 2400, noteAt(0, true, 60));

    bool stable = true;
    for (int i = 0; i < 200; ++i)
    {
        swap.getDocument().editKit([&](DpcmKit& k) {
            auto copy = std::make_shared<DpcmSampleData>(*k.mappings[(size_t) midiNoteToNoteValue(60)].sample);
            k.mappings[(size_t) midiNoteToNoteValue(60)].sample = copy;
        });
        const auto audio = render(swap, 128, i % 5 == 0 ? noteAt(0, true, 60) : juce::MidiBuffer {});
        for (auto v : audio)
            stable = stable && std::isfinite(v) && std::abs(v) <= 2.0f;
    }
    check(stable, "200 kit replacements during playback stay safe");
}

void testFactorySounds()
{
    std::puts("factory sounds");

    TempFolder temp;
    ChipmunkProcessor processor(temp.presets);
    processor.prepareToPlay(48000.0, 512);

    auto loudestPeak = 0.0f;
    juce::String loudestName, silent;

    for (const auto& preset : factoryPresets())
    {
        if (preset.name == "Empty DPCM Kit")
            continue;

        processor.loadPreset(preset);
        render(processor, 512);
        processor.applyMachineChange();
        render(processor, 512);

        const auto note = preset.channel == ChannelType::dpcm || preset.channel == ChannelType::drumKit ? 36 : 69;
        const auto audio = render(processor, 24000, noteAt(0, true, note));
        auto peak = 0.0f;

        for (const auto sample : audio)
            peak = juce::jmax(peak, std::abs(sample));

        if (rms(audio) < 0.002)
            silent += juce::String(preset.name) + " ";

        if (peak > loudestPeak)
        {
            loudestPeak = peak;
            loudestName = preset.name;
        }

        render(processor, 512, noteAt(0, false, note));
        render(processor, 48000);
    }

    std::printf("  %d sounds, loudest is %s at peak %.3f\n", (int) factoryPresets().size(), loudestName.toRawUTF8(), loudestPeak);
    check(silent.isEmpty(), "every sound makes noise: " + silent);
    check(loudestPeak < 1.0f, "no sound clips at the default output level");
}

void testMachine()
{
    std::puts("NTSC and PAL");

    TempFolder temp;
    ChipmunkProcessor processor(temp.presets);
    processor.prepareToPlay(48000.0, 512);
    check(! processor.isEnginePal(), "NTSC is the default");

    const auto machineParameter = processor.getParameters().getParameter("machine");
    check(! machineParameter->isAutomatable(), "the machine can't be automated, since changing it cuts the sound");

    const auto names = ChipmunkProcessor::channelNames();
    std::map<int, double> ntscHz;

    for (const auto machine : { 0, 1 })
    {
        setParam(processor, "machine", (float) machine);
        render(processor, 512); // The block that notices, which keeps using the old chip.
        processor.applyMachineChange();
        check(processor.isEnginePal() == (machine == 1), machine == 1 ? "switches to PAL" : "stays on NTSC");

        for (const auto channel : { 0, 1, 4, 5, 6, 7 })
        {
            Preset preset;
            preset.channel = channel == 0 ? ChannelType::pulse1 : channel == 1 ? ChannelType::triangle : channel == 4 ? ChannelType::vrc6Pulse1 : channel == 5 ? ChannelType::vrc6Saw : channel == 6 ? ChannelType::mmc5Pulse1 : ChannelType::s5bSquare1;
            preset.voice.dutyCycle = channel == 4 ? 7 : 2;
            preset.instrument.sawMasterVolume = 0;

            processor.loadPreset(preset);
            render(processor, 512);
            const auto audio = render(processor, 48000, noteAt(0, true));
            const auto hz = frequency(audio, 4800);

            if (machine == 0)
                ntscHz[channel] = hz;

            const auto cents = 1200.0 * std::log2(hz / ntscHz[channel]);

            if (machine == 1)
            {
                std::printf("  %-11s PAL %7.2f Hz, NTSC %7.2f Hz, %+5.1f cents\n", names[channel].toRawUTF8(), hz, ntscHz[channel], cents);
                check(std::abs(cents) < 12.0, names[channel] + " stays in tune on PAL");
            }

            render(processor, 512, noteAt(0, false));
            render(processor, 9600);
        }
    }

    // Envelopes step at the machine's own rate, here measured from a stepped volume.
    {
        Preset preset;
        std::string error;
        parsePreset("Chipmunk preset 1\nname=Steps\nchannel=pulse\nvolume=15 0\n", preset, error);
        processor.loadPreset(preset);
        render(processor, 512);

        const auto audio = render(processor, 4800, noteAt(0, true));
        // Silence still drifts back to centre, so look for the wave's last sharp edge.
        auto last = 0;
        for (int i = 1; i < 4800; ++i)
            if (std::abs(audio[(size_t) i] - audio[(size_t) i - 1]) > 0.02f)
                last = i;

        std::printf("  one PAL frame of sound lasts %d samples, expected about %d\n", last, 48000 / 50);
        check(std::abs(last - 960) < 60, "PAL envelopes step at 50 Hz");
        render(processor, 512, noteAt(0, false));
    }

    // Rebuilding the chip keeps the sound the device was playing, and samples stay usable.
    for (const auto& preset : factoryPresets())
        if (preset.name == "Synth Drum Kit")
            processor.loadPreset(preset);

    render(processor, 512);
    check(rms(render(processor, 12000, noteAt(0, true, 36))) > 0.002, "a kit plays on PAL");

    setParam(processor, "machine", 0.0f);
    render(processor, 512);
    processor.applyMachineChange();
    check(! processor.isEnginePal() && rms(render(processor, 12000, noteAt(0, true, 36))) > 0.002, "and after switching back");

    // The setting is saved, and restoring it rebuilds the chip with the right tuning.
    setParam(processor, "machine", 1.0f);
    render(processor, 512);
    processor.applyMachineChange();

    juce::MemoryBlock state;
    processor.getStateInformation(state);

    {
        ChipmunkProcessor restored(temp.presets);
        restored.setStateInformation(state.getData(), (int) state.getSize());
        restored.prepareToPlay(48000.0, 512);
        check(restored.isEnginePal(), "a saved Set reopens on PAL");

        ChipmunkProcessor running(temp.presets);
        running.prepareToPlay(48000.0, 512);
        running.setStateInformation(state.getData(), (int) state.getSize());
        render(running, 512);
        running.applyMachineChange();
        check(running.isEnginePal(), "and so does a device that's already running");
    }

    // Sets saved before this setting existed stay on NTSC.
    {
        auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), (int) state.getSize());

        for (auto* child = xml->getFirstChildElement(); child != nullptr; child = child->getNextElement())
        {
            if (child->getStringAttribute("id") == "machine")
            {
                xml->removeChildElement(child, true);
                break;
            }
        }

        juce::MemoryBlock oldState;
        juce::AudioProcessor::copyXmlToBinary(*xml, oldState);

        ChipmunkProcessor old(temp.presets);
        old.setStateInformation(oldState.getData(), (int) oldState.getSize());
        old.prepareToPlay(48000.0, 512);
        check(! old.isEnginePal(), "older Sets open on NTSC");
    }
}

void testActivityDisplay()
{
    std::puts("activity display");

    TempFolder temp;
    ChipmunkProcessor processor(temp.presets);
    processor.prepareToPlay(48000.0, 512);
    ChipmunkEditor editor(processor);
    auto& display = editor.getActivityDisplay();

    render(processor, 4096);
    editor.refreshFromProcessor();
    check(display.getShownNote() == -1 && display.getShownVolume() == 0, "shows no note before playing");

    auto peak = [&] {
        auto result = 0.0f;
        for (const auto sample : display.getTrace())
            result = juce::jmax(result, std::abs(sample));
        return result;
    };

    render(processor, 4800, noteAt(0, true));
    editor.refreshFromProcessor();
    check(display.getShownNote() == 69 && display.getShownVolume() > 0, "shows the note and volume");
    check(peak() > 0.02f, "the scope shows the output");

    // A steady note stays still, because each picture starts at a rising edge.
    const auto first = display.getTrace();
    render(processor, 1237);
    editor.refreshFromProcessor();
    auto largestChange = 0.0f;
    for (size_t i = 0; i < first.size() / 2; ++i)
        largestChange = juce::jmax(largestChange, std::abs(first[i] - display.getTrace()[i]));
    check(largestChange < peak() * 0.5f, "a steady note holds still");

    render(processor, 512, noteAt(0, false));
    render(processor, 24000);
    editor.refreshFromProcessor();
    check(display.getShownNote() == -1 && peak() < 0.01f, "goes quiet after the note");

    // The buffer wraps around without losing the newest samples.
    ScopeBuffer scope;
    std::vector<float> ramp(ScopeBuffer::capacity + 100);
    for (size_t i = 0; i < ramp.size(); ++i)
        ramp[i] = (float) i;
    scope.write(ramp.data(), (int) ramp.size() - 7);
    scope.write(ramp.data() + ramp.size() - 7, 7);
    std::array<float, 10> tail {};
    scope.read(tail.data(), 10);
    check(tail[9] == ramp.back() && tail[0] == ramp.back() - 9.0f, "the scope keeps the newest samples in order");
}

void testLiveTemplates()
{
    std::puts("Live templates");

    TempFolder temp;

    for (const auto* title : { "Chipmunk 2A03", "Chipmunk All Chips" })
    {
        const auto file = juce::File(CHIPMUNK_TEMPLATES_DIR).getChildFile(juce::String(title) + ".als");
        juce::FileInputStream raw(file);
        juce::GZIPDecompressorInputStream unzipped(&raw, false, juce::GZIPDecompressorInputStream::gzipFormat);
        const auto xml = juce::parseXML(unzipped.readEntireStreamAsString());
        check(xml != nullptr, juce::String(title) + " is readable");

        if (xml == nullptr)
            continue;

        // Every target Live can automate needs its own id, or the Set won't open properly.
        std::set<juce::String> ids;
        auto hasDuplicates = false;
        const std::function<void(const juce::XmlElement&)> collect = [&](const juce::XmlElement& element) {
            const auto tag = element.getTagName();

            if (element.hasAttribute("Id") && (tag.endsWith("AutomationTarget") || tag.endsWith("ModulationTarget") || tag == "Pointee" || tag.startsWith("ControllerTargets")))
                hasDuplicates = ! ids.insert(element.getStringAttribute("Id")).second || hasDuplicates;

            for (auto* child : element.getChildIterator())
                collect(*child);
        };
        collect(*xml);
        check(! hasDuplicates, "no two targets share an id");

        auto* liveSet = xml->getChildByName("LiveSet");
        auto numTracks = 0;
        auto allRestored = true;

        for (auto* track : liveSet->getChildByName("Tracks")->getChildWithTagNameIterator("MidiTrack"))
        {
            ++numTracks;

            auto* name = track->getChildByName("Name")->getChildByName("EffectiveName");

            // The device's saved state, unwrapped the way the AU wrapper does.
            const juce::XmlElement* buffer = nullptr;
            const std::function<void(const juce::XmlElement&)> findBuffer = [&](const juce::XmlElement& element) {
                if (element.getTagName() == "Buffer")
                    buffer = &element;
                for (auto* child : element.getChildIterator())
                    findBuffer(*child);
            };
            findBuffer(*track);

            juce::MemoryBlock plistData;
            plistData.loadFromHexString(buffer->getAllSubText());
            const auto plist = plistData.toString();
            const auto dataStart = plist.indexOf(plist.indexOf("jucePluginState"), "<data>") + 6;
            juce::MemoryOutputStream state;
            juce::Base64::convertFromBase64(state, plist.substring(dataStart, plist.indexOf(dataStart, "</data>")).removeCharacters(" \t\r\n"));

            ChipmunkProcessor processor(temp.presets);
            processor.setStateInformation(state.getData(), (int) state.getDataSize());
            processor.prepareToPlay(48000.0, 512);

            const auto preset = processor.currentPreset();
            const auto trackName = name->getStringAttribute("Value");
            const auto note = preset.channel == ChannelType::dpcm || preset.channel == ChannelType::drumKit ? 36 : 69;

            render(processor, 512);
            const auto level = rms(render(processor, 12000, noteAt(0, true, note)));
            const auto isFactorySound = std::any_of(factoryPresets().begin(), factoryPresets().end(), [&](const Preset& p) { return p.name == preset.name && p.channel == preset.channel; });

            std::printf("  %-13s plays %-16s on %-11s rms %.3f\n", trackName.toRawUTF8(), preset.name.c_str(), ChipmunkProcessor::channelNames()[processor.getChannelIndex()].toRawUTF8(), level);
            allRestored = allRestored && isFactorySound && level > 0.002 && ! processor.isEnginePal();
        }

        check(numTracks == (juce::String(title).contains("All") ? 13 : 5), "has the full lineup");
        check(allRestored, "every track restores its sound and plays");
    }
}

void testMidiEffects()
{
    std::puts("note effects from MIDI controllers");

    TempFolder temp;
    ChipmunkProcessor processor(temp.presets);
    processor.prepareToPlay(48000.0, 512);

    const auto withController = [](int controller, int value, int position = 0) {
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::controllerEvent(1, controller, value), position);
        return midi;
    };

    render(processor, 512);

    // A slide reaches the new note, passing through the notes between, over the frames asked for.
    render(processor, 512, withController(MidiEffect::slideFrames, 60));
    render(processor, 4800, noteAt(0, true, 57));
    const auto start = frequency(render(processor, 24000), 0);

    render(processor, 512, noteAt(0, true, 69));
    const auto middle = render(processor, 24000);

    // Counts cycles near the slide's middle, since its pitch keeps moving.
    auto crossings = 0;
    for (size_t i = 12001; i < middle.size(); ++i)
        crossings += middle[i - 1] < 0.0f && middle[i] >= 0.0f ? 1 : 0;

    const auto during = crossings * 48000.0 / 12000.0;
    render(processor, 48000);
    const auto end = frequency(render(processor, 24000), 0);

    std::printf("  slide from A3 to A4: %.1f Hz, then %.1f Hz, ending at %.1f Hz\n", start, during, end);
    check(std::abs(1200.0 * std::log2(start / 220.0)) < 15.0, "a slide starts from the note before");
    check(during > start * 1.1 && during < end * 0.95, "and moves through the pitches between");
    check(std::abs(1200.0 * std::log2(end / 440.0)) < 15.0, "ending on the new note");

    render(processor, 512, noteAt(0, false, 57));
    render(processor, 512, noteAt(0, false, 69));
    render(processor, 512, withController(MidiEffect::slideFrames, 0));
    render(processor, 4800);

    // Volume takes effect on the sounding note.
    render(processor, 4800, noteAt(0, true));
    const auto loud = rms(render(processor, 4800));
    render(processor, 512, withController(MidiEffect::volume, 42));
    render(processor, 2400);
    const auto soft = rms(render(processor, 4800));
    std::printf("  volume 15 gives rms %.4f, volume 5 gives %.4f\n", loud, soft);
    check(soft < loud * 0.5 && soft > 0.001, "volume changes a sounding note");

    // Fine pitch moves the period directly.
    const auto before = frequency(render(processor, 24000), 0);
    render(processor, 512, withController(MidiEffect::finePitch, 64 + 20));
    const auto after = frequency(render(processor, 24000), 0);
    std::printf("  fine pitch +20: %.1f Hz to %.1f Hz\n", before, after);
    check(after < before * 0.97, "fine pitch detunes the note, and higher periods are flatter");
    render(processor, 512, withController(MidiEffect::finePitch, 64));

    // Stopping ends a sound the note-off alone wouldn't.
    render(processor, 512, withController(MidiEffect::stopNote, 127));
    render(processor, 4800);
    check(rms(render(processor, 4800)) < 0.002, "stop silences the channel");
    check(processor.getSoundingNote() == -1, "and it reports silence");
    render(processor, 512, withController(MidiEffect::stopNote, 0));
    render(processor, 512, noteAt(0, false));
    render(processor, 4800);

    // Duty set by a controller also applies to later notes, which otherwise use the device's setting.
    processor.setChannelIndex(0);
    render(processor, 512, withController(MidiEffect::dutyCycle, 1));
    render(processor, 512, noteAt(0, true));
    const auto thin = render(processor, 9600);
    render(processor, 512, noteAt(0, false));
    render(processor, 512, withController(MidiEffect::dutyCycle, 2));
    render(processor, 512, noteAt(0, true));
    const auto square = render(processor, 9600);
    check(rms(thin, 4800) < rms(square, 4800) * 0.97, "duty changes the sound, and sticks for later notes");
    render(processor, 512, noteAt(0, false));

    // A released note's last volume can hold as in FamiStudio, but only while the host plays.
    struct PlayingHead final : juce::AudioPlayHead
    {
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setIsPlaying(true);
            return info;
        }
    };

    processor.getDocument().edit([](Instrument& inst) {
        auto& env = inst.envelopes[envelopeVolume];
        env.length = 3;
        env.loop = env.release = -1;
        env.values[0] = env.values[1] = 12;
        env.values[2] = 6;
        setEnvelopeRelease(env, envelopeVolume, 2);
    });

    const auto tailAfterRelease = [&] {
        render(processor, 512, noteAt(0, true));
        render(processor, 4800);
        render(processor, 512, noteAt(0, false));
        render(processor, 9600);
        return rms(render(processor, 4800));
    };

    PlayingHead playingHead;
    render(processor, 512, withController(MidiEffect::holdRelease, 127));
    check(tailAfterRelease() < 0.002, "a release tail still stops by itself when the host isn't playing");

    processor.setPlayHead(&playingHead);
    check(tailAfterRelease() > 0.002, "holding keeps the tail going while the host plays");
    render(processor, 512, withController(MidiEffect::stopNote, 127));
    render(processor, 4800);
    check(rms(render(processor, 4800)) < 0.002, "and stop ends it");
    render(processor, 512, withController(MidiEffect::stopNote, 0));

    render(processor, 512, withController(MidiEffect::holdRelease, 0));
    check(tailAfterRelease() < 0.002, "without holding, the tail stops by itself");
    processor.setPlayHead(nullptr);
}

void testLoadingIsQuiet()
{
    std::puts("opening a Set makes no sound");

    auto allQuiet = true;

    // Hosts restore the state either side of preparing, so both orders have to be quiet.
    for (int channel = 0; channel < ChipmunkProcessor::channelNames().size(); ++channel)
    {
        TempFolder temp;
        ChipmunkProcessor source(temp.presets);

        for (const auto& preset : factoryPresets())
        {
            if (ChipmunkProcessor::channelNames()[channel] == "DPCM" ? preset.name == "Synth Drum Kit" : false)
                source.loadPreset(preset);
            if (preset.channel == ChannelType::drumKit && ChipmunkProcessor::channelNames()[channel] == "Drum Kit")
                source.loadPreset(preset);
        }

        source.setChannelIndex(channel);

        juce::MemoryBlock state;
        source.getStateInformation(state);

        for (const auto stateFirst : { true, false })
        {
            ChipmunkProcessor processor(temp.presets);

            if (stateFirst)
                processor.setStateInformation(state.getData(), (int) state.getSize());

            processor.prepareToPlay(48000.0, 512);

            if (! stateFirst)
                processor.setStateInformation(state.getData(), (int) state.getSize());

            auto peak = 0.0f;

            for (int block = 0; block < 100; ++block)
            {
                for (const auto sample : render(processor, 512))
                    peak = std::max(peak, std::abs(sample));

                // A channel on a costly chip gets rebuilt from the message thread, once the host asks for it.
                if (block == 1)
                    processor.applyMachineChange();
            }

            allQuiet = allQuiet && peak < 0.001f;

            if (peak >= 0.001f)
                std::printf("  %s, %s: peak %.5f\n", ChipmunkProcessor::channelNames()[channel].toRawUTF8(), stateFirst ? "state first" : "prepare first", peak);
        }
    }

    check(allQuiet, "every channel is silent until a note plays, in either order");

    // Switching on and off the channels that use the DAC is just as quiet.
    TempFolder temp;
    ChipmunkProcessor processor(temp.presets);
    processor.prepareToPlay(48000.0, 512);
    render(processor, 4800);

    auto peak = 0.0f;

    for (const auto index : { 3, 0, 14, 1, 3, 14, 2 })
    {
        processor.setChannelIndex(index);

        for (int block = 0; block < 10; ++block)
            for (const auto sample : render(processor, 512))
                peak = std::max(peak, std::abs(sample));
    }

    std::printf("  switching channels peaks at %.5f\n", peak);
    check(peak < 0.001f, "switching channels makes no sound");
}

void testDrumKit()
{
    std::puts("drum kit");

    TempFolder temp;
    ChipmunkProcessor processor(temp.presets);
    processor.prepareToPlay(48000.0, 512);

    const auto index = ChipmunkProcessor::channelNames().indexOf("Drum Kit");
    check(index == 14, "the kit is the last channel, which keeps saved channel numbers the same");

    const Preset* found = nullptr;
    for (const auto& preset : factoryPresets())
        found = preset.channel == ChannelType::drumKit ? &preset : found;

    check(found != nullptr, "there's a factory kit");
    processor.loadPreset(*found);
    render(processor, 512);
    check(processor.getChannelIndex() == index, "loading it selects the kit");

    // The text form keeps both kinds of key, and keys sharing a sound still share it.
    auto shared = std::make_shared<DpcmKit>(*found->kit);
    shared->mappings[(size_t) midiNoteToNoteValue(44)] = shared->mappings[(size_t) midiNoteToNoteValue(42)];
    shared->mappings[(size_t) midiNoteToNoteValue(44)].noiseNote = 50;

    Preset withShared = *found;
    withShared.kit = shared;

    Preset parsed;
    std::string error;
    check(parsePreset(formatPreset(withShared), parsed, error) && parsed.channel == ChannelType::drumKit, "a kit survives saving");
    check(parsed.kit != nullptr && parsed.kit->mappings[(size_t) midiNoteToNoteValue(36)].sample != nullptr, "with its samples");

    const auto& hat = parsed.kit->mappings[(size_t) midiNoteToNoteValue(42)];
    const auto& hatCopy = parsed.kit->mappings[(size_t) midiNoteToNoteValue(44)];
    check(hat.noise != nullptr && hat.noise->name == "Closed Hat" && hat.noise == hatCopy.noise && hatCopy.noiseNote == 50, "and its noise keys");
    check(hat.noise != nullptr && hat.noise->instrument.envelopes[envelopeVolume].length == 5, "and their envelopes");

    // Each kind of key sounds, and an unmapped key doesn't.
    const auto hit = [&](int key) {
        const auto audio = render(processor, 4800, noteAt(0, true, key));
        render(processor, 512, noteAt(0, false, key));
        render(processor, 48000);
        return rms(audio);
    };

    check(hit(36) > 0.01, "a sample key plays");
    check(hit(42) > 0.005, "a noise key plays");
    check(hit(61) < 0.001, "an empty key is silent");

    // The DAC's level makes noise quieter, so a kit with no samples has to leave it at rest.
    const auto noiseLevel = [&](bool withSamples, const char* channelName) {
        TempFolder folder;
        ChipmunkProcessor other(folder.presets);
        other.prepareToPlay(48000.0, 512);

        auto kit = std::make_shared<DpcmKit>();
        kit->mappings[(size_t) midiNoteToNoteValue(42)] = found->kit->mappings[(size_t) midiNoteToNoteValue(42)];

        if (withSamples)
            kit->mappings[(size_t) midiNoteToNoteValue(36)] = found->kit->mappings[(size_t) midiNoteToNoteValue(36)];

        Instrument held;
        other.getDocument().load(held, "Test", kit);
        other.setChannelIndex(ChipmunkProcessor::channelNames().indexOf(channelName));
        render(other, 512);
        return rms(render(other, 4800, noteAt(0, true, 42)));
    };

    const auto noSamples = noiseLevel(false, "Drum Kit");
    const auto plainNoise = noiseLevel(false, "Noise");
    const auto someSamples = noiseLevel(true, "Drum Kit");
    std::printf("  noise rms: kit with no samples %.4f, kit with samples %.4f\n", noSamples, someSamples);
    check(noSamples > someSamples * 1.1 && plainNoise > 0.0f, "a kit with no samples leaves noise at full level");

    // Noise and a sample play at once, and stop separately, as they're separate channels in a song.
    auto both = noteAt(0, true, 36);
    both.addEvent(juce::MidiMessage::noteOn(1, 46, (juce::uint8) 127), 0);
    render(processor, 512, both);
    check(processor.getSoundingNote() != -1, "noise and a sample start together");

    juce::MidiBuffer stopSample;
    stopSample.addEvent(juce::MidiMessage::controllerEvent(1, MidiEffect::stopSample, 127), 0);
    render(processor, 512, stopSample);
    render(processor, 2400);
    check(processor.getSoundingNote() != -1, "stopping samples leaves noise going");

    juce::MidiBuffer stopNoise;
    stopNoise.addEvent(juce::MidiMessage::controllerEvent(1, MidiEffect::stopNote, 127), 0);
    render(processor, 512, stopNoise);
    render(processor, 2400);
    check(processor.getSoundingNote() == -1, "and stopping noise ends the rest");

    auto resets = noteAt(0, false, 36);
    resets.addEvent(juce::MidiMessage::noteOff(1, 46), 0);
    resets.addEvent(juce::MidiMessage::controllerEvent(1, MidiEffect::stopSample, 0), 0);
    resets.addEvent(juce::MidiMessage::controllerEvent(1, MidiEffect::stopNote, 0), 0);
    render(processor, 512, resets);

    // Choosing a key puts its sound in the document, and edits land in the kit and can be undone.
    auto& document = processor.getDocument();
    const auto hatKey = midiNoteToNoteValue(42);

    document.selectKitKey(hatKey);
    check(document.snapshot().envelopes[envelopeVolume].length == 5, "choosing a key shows its sound");

    document.edit([](Instrument& inst) { inst.envelopes[envelopeVolume].values[0] = 3; });
    check(document.kit()->mappings[(size_t) hatKey].noise->instrument.envelopes[envelopeVolume].values[0] == 3, "edits go into the kit");
    render(processor, 512);

    document.undo();
    check(document.kit()->mappings[(size_t) hatKey].noise->instrument.envelopes[envelopeVolume].values[0] == 12, "and undo takes them back");
    check(document.snapshot().envelopes[envelopeVolume].values[0] == 12, "in what's shown as well");
    render(processor, 512);

    // Editing a sound while it plays keeps it going, and removing it stops it.
    render(processor, 512, noteAt(0, true, 42));
    document.edit([](Instrument& inst) { inst.envelopes[envelopeVolume].values[1] = 9; });
    render(processor, 512);
    check(processor.getSoundingNote() != -1, "editing a playing sound keeps it going");
    document.editKit([&](DpcmKit& kit) { kit.mappings[(size_t) hatKey] = {}; });
    render(processor, 512);
    render(processor, 512);
    check(processor.getSoundingNote() == -1, "removing it stops it");

    // The editor shows the key list and the chosen sound's envelopes together.
    ChipmunkEditor editor(processor);
    editor.setSize(1000, 720);
    editor.refreshFromProcessor();
    check(editor.getDpcmEditor().isVisible() && editor.getDpcmEditor().getNumMappedKeys() == 6, "the list has samples and noise keys");

    const auto before = editor.getDpcmEditor().getNumMappedKeys();
    editor.getDpcmEditor().addNoiseSound();
    editor.getDpcmEditor().refresh();
    check(editor.getDpcmEditor().getNumMappedKeys() == before + 1, "a noise sound can be added");

    // The divider shares the room between the keys and the envelopes.
    editor.setSize(1100, 760);
    processor.setKitListShare(0.4f);

    auto& divider = editor.getKitDivider();
    auto& list = editor.getDpcmEditor();
    auto& volumePanel = editor.getPanel(envelopeVolume);

    const auto dragDividerTo = [&](int y) {
        const auto centre = divider.getLocalBounds().getCentre().toFloat();
        const juce::Point<float> target { centre.x, (float) (y - divider.getY()) + centre.y };
        divider.mouseDown(mouseEvent(divider, centre, centre));
        divider.mouseDrag(mouseEvent(divider, target, centre));
        divider.mouseUp(mouseEvent(divider, target, centre));
    };

    processor.getDocument().selectKitKey(processor.getDocument().selectedKitKey() >= 0 ? processor.getDocument().selectedKitKey() : 36 - 23);
    editor.refreshFromProcessor();
    check(divider.isVisible(), "a Drum Kit has a divider");

    if (! volumePanel.isVisible())
    {
        const auto kit = processor.getDocument().kit();
        for (int value = noteMusicalMin; value <= noteMusicalMax && ! volumePanel.isVisible(); ++value)
        {
            if (kit != nullptr && kit->mappings[(size_t) value].noise != nullptr)
            {
                processor.getDocument().selectKitKey(value);
                editor.refreshFromProcessor();
            }
        }
    }

    check(volumePanel.isVisible(), "choosing a noise key shows its envelopes");

    const auto startHeight = list.getHeight();
    dragDividerTo(divider.getY() + 60);
    check(list.getHeight() == startHeight + 60 && divider.getY() == list.getBottom() && volumePanel.getY() == divider.getBottom(), "dragging down gives the list more room, right under the mouse");

    const auto panelsBottom = editor.getPanel(envelopeDutyCycle).getBottom();
    dragDividerTo(100000);
    check(volumePanel.getHeight() >= 78 && std::abs(editor.getPanel(envelopeDutyCycle).getBottom() - panelsBottom) <= 2, "the envelopes keep a usable height");
    check(volumePanel.getChildren().size() > 0, "and stay on screen");

    auto buttonsFit = true;
    for (auto* child : volumePanel.getChildren())
        if (child->isVisible())
            buttonsFit = buttonsFit && child->getBottom() <= volumePanel.getHeight();
    check(buttonsFit, "short panels keep all their buttons inside");

    dragDividerTo(0);
    check(list.getHeight() >= 130 && list.getY() > 0, "the list keeps a usable height");

    const auto reopenedHeight = [&] {
        dragDividerTo(list.getY() + 300);
        const auto height = list.getHeight();
        ChipmunkEditor second(processor);
        second.setSize(1100, 760);
        second.refreshFromProcessor();
        return std::pair { height, second.getDpcmEditor().getHeight() };
    }();
    check(reopenedHeight.first == reopenedHeight.second, "closing and opening the window keeps the division");

    juce::MemoryBlock dividedState;
    processor.getStateInformation(dividedState);
    const auto savedShare = processor.getKitListShare();
    ChipmunkProcessor reloaded(temp.presets);
    reloaded.setStateInformation(dividedState.getData(), (int) dividedState.getSize());
    check(std::abs(reloaded.getKitListShare() - savedShare) < 0.001f, "the division saves with the Set");

    editor.setSize(1000, 640);
    check(volumePanel.getHeight() >= 78 && list.getHeight() >= 130, "it still fits a smaller window");

    editor.setSize(1100, 760);
    divider.mouseDoubleClick(mouseEvent(divider, {}, {}, 2));
    check(std::abs(processor.getKitListShare() - 0.4f) < 0.001f, "double-clicking the divider resets it");
}

void testNewChips()
{
    std::puts("FDS, N163, VRC7 and EPSM");

    TempFolder temp;
    ChipmunkProcessor processor(temp.presets);
    processor.prepareToPlay(48000.0, 512);

    // Each of these starts with its chip off, to keep other devices cheap.
    for (const auto* name : { "FDS Wave", "N163 Wave", "VRC7 FM", "EPSM Square", "EPSM FM", "EPSM Rhythm" })
    {
        const auto index = ChipmunkProcessor::channelNames().indexOf(name);

        processor.setChannelIndex(index);
        render(processor, 512);
        processor.applyMachineChange();
        render(processor, 512);

        // A new sound on the channel, with none of its own settings, still has to play.
        juce::AudioBuffer<float> buffer(2, 24000);
        buffer.clear();
        auto midi = noteAt(0, true, 60);
        processor.processBlock(buffer, midi);

        const std::vector<float> left(buffer.getReadPointer(0), buffer.getReadPointer(0) + 24000);
        const std::vector<float> right(buffer.getReadPointer(1), buffer.getReadPointer(1) + 24000);

        std::printf("  %-12s rms left %.4f right %.4f\n", name, rms(left), rms(right));
        check(rms(left) > 0.001 && rms(right) > 0.001, juce::String(name) + " plays on both sides");

        render(processor, 512, noteAt(0, false, 60));
        render(processor, 96000);
    }

    // Notes sound where they do in FamiStudio, which is two octaves down on FDS and one on EPSM squares.
    for (const auto& [name, expectedHz] : std::vector<std::pair<const char*, double>> { { "FDS Wave", 110.0 }, { "N163 Wave", 440.0 }, { "EPSM Square", 220.0 } })
    {
        processor.setChannelIndex(ChipmunkProcessor::channelNames().indexOf(name));
        render(processor, 512);
        processor.applyMachineChange();
        render(processor, 512);

        const auto hz = frequency(render(processor, 48000, noteAt(0, true)), 4800);
        const auto cents = 1200.0 * std::log2(hz / expectedHz);

        std::printf("  %-12s A4 measures %.2f Hz, %+.1f cents\n", name, hz, cents);
        check(std::abs(cents) < 15.0, juce::String(name) + " is in tune");

        render(processor, 512, noteAt(0, false));
        render(processor, 96000);
    }

    // Everything these chips keep in a sound survives saving.
    Preset preset;
    preset.channel = ChannelType::fdsWave;
    preset.instrument.setDefaultWave(64, 63);
    preset.instrument.fdsMasterVolume = 2;
    preset.instrument.fdsModSpeed = 300;
    preset.instrument.fdsModDepth = 22;
    preset.instrument.fdsModDelay = 4;
    preset.instrument.fdsAutoMod = true;
    preset.instrument.fdsAutoModNumerator = 3;
    preset.instrument.fdsModulation[5] = -12;
    preset.instrument.waveRepeat.length = 1;
    preset.instrument.waveRepeat.values[0] = 9;

    Preset loaded;
    std::string error;
    check(parsePreset(formatPreset(preset), loaded, error) && loaded.instrument.wave == preset.instrument.wave && loaded.instrument.waveLength == 64
              && loaded.instrument.fdsModSpeed == 300 && loaded.instrument.fdsAutoMod && loaded.instrument.fdsModulation[5] == -12 && loaded.instrument.waveRepeat.values[0] == 9,
          "an FDS sound saves and loads");

    preset = {};
    preset.channel = ChannelType::vrc7Fm1;
    preset.instrument.vrc7Patch = 0;
    preset.instrument.vrc7PatchRegisters[3] = 200;
    preset.instrument.vrc7OverrideStop = true;
    check(parsePreset(formatPreset(preset), loaded, error) && loaded.channel == ChannelType::vrc7Fm1 && loaded.instrument.vrc7Patch == 0
              && loaded.instrument.vrc7PatchRegisters[3] == 200 && loaded.instrument.vrc7OverrideStop, "a VRC7 sound saves and loads");

    preset = {};
    preset.channel = ChannelType::epsmFm1;
    preset.instrument.epsmPatchRegisters[30] = 77;
    check(parsePreset(formatPreset(preset), loaded, error) && loaded.channel == ChannelType::epsmFm1 && loaded.instrument.epsmPatchRegisters[30] == 77, "an EPSM sound saves and loads");

    preset = {};
    preset.channel = ChannelType::n163Wave1;
    preset.instrument.setDefaultWave(32, 15);
    preset.instrument.n163WavePosition = 64;
    check(parsePreset(formatPreset(preset), loaded, error) && loaded.channel == ChannelType::n163Wave1 && loaded.instrument.waveSize == 32 && loaded.instrument.n163WavePosition == 64, "an N163 sound saves and loads");

    // The editor offers each chip's own settings, and they reach the sound.
    {
        ChipmunkEditor editor(processor);
        const auto select = [&](const char* name) {
            processor.setChannelIndex(ChipmunkProcessor::channelNames().indexOf(name));
            editor.refreshFromProcessor();
        };

        select("FDS Wave");
        check(editor.getWavePanel().isVisible() && ! editor.getPanel(envelopeDutyCycle).isVisible(), "FDS shows its wave editor instead of duty");

        auto& graph = editor.getWavePanel().getGraph();
        graph.mouseDown(mouseEvent(graph, { 5.0f, 2.0f }, { 5.0f, 2.0f }));
        graph.mouseUp(mouseEvent(graph, { 5.0f, 2.0f }, { 5.0f, 2.0f }));
        check(processor.getDocument().snapshot().waveLength == 64 && processor.getDocument().snapshot().wave[0] > 50, "drawing fills in a wave to edit");
        check(processor.getDocument().canUndo(), "and can be undone");

        // Shift fine-tunes one step of a wave, which is too fine to hit with the mouse.
        {
            constexpr auto shift = juce::ModifierKeys::shiftModifier;
            const auto wave = [&](int step) { return (int) processor.getDocument().snapshot().wave[(size_t) step]; };
            const auto event = [&](juce::Point<float> p, int modifiers = juce::ModifierKeys::shiftModifier) { return mouseEvent(graph, p, p, 1, modifiers); };
            const auto columnX = [&](int step) { return ((float) step + 0.5f) * (float) graph.getWidth() / 64.0f; };
            const auto y = (float) graph.getHeight() * 0.9f;
            const auto pixelsPerValue = juce::jlimit(12.0f, 32.0f, (float) graph.getHeight() / 63.0f * 4.0f);

            processor.getDocument().edit([](Instrument& i) { i.wave[20] = 30; i.wave[21] = 30; i.wave[22] = 30; });

            graph.mouseDown(event({ columnX(21), y }));
            graph.mouseUp(event({ columnX(21), y }));
            check(wave(21) == 30, "a Shift-click leaves a wave step alone");

            graph.mouseDown(event({ columnX(21), y }));
            graph.mouseDrag(event({ columnX(22), y - pixelsPerValue * 3.0f }));
            check(wave(21) == 33 && wave(20) == 30 && wave(22) == 30, "Shift-dragging moves that one step, slowly");

            graph.mouseDrag(event({ columnX(21), y - pixelsPerValue * 3.0f }, 0));
            check(wave(21) == 33, "letting go of Shift mid-drag keeps the value");

            graph.mouseDrag(event({ columnX(21), y - pixelsPerValue * 100.0f }));
            check(wave(21) == 63, "wave steps stop at their limit");
            graph.mouseDrag(event({ columnX(21), y - pixelsPerValue * 99.0f }));
            check(wave(21) == 62, "and moving back reverses straight away");
            graph.mouseUp(event({ columnX(21), y - pixelsPerValue * 99.0f }));

            processor.getDocument().undo();
            check(wave(21) == 30, "one fine adjustment is one undo step");

            graph.mouseDown(event({ columnX(30), y }, 0));
            const auto drawn = wave(30);
            graph.mouseDrag(event({ columnX(30), y }));
            check(wave(30) == drawn, "pressing Shift mid-stroke doesn't jump");
            graph.mouseDrag(event({ columnX(30), y - pixelsPerValue }));
            check(wave(30) == drawn + 1, "and it fine-tunes from there");
            graph.mouseUp(event({ columnX(30), y - pixelsPerValue }));

            graph.mouseDown(event({ columnX(31), 2.0f }, 0));
            graph.mouseUp(event({ columnX(31), 2.0f }, 0));
            check(wave(31) > 50, "normal drawing comes back afterwards");
        }

        editor.getWavePanel().addWave();
        check(editor.getWavePanel().numWaves() == 2 && editor.getWavePanel().getCurrentWave() == 1, "waves can be added");
        check(processor.getDocument().snapshot().waveRepeat.length == 2, "and each has a time");

        editor.getWavePanel().getRepeatField().setText("9");
        editor.getWavePanel().commitRepeat();
        check(processor.getDocument().snapshot().waveRepeat.values[1] == 9, "frames per wave can be typed");

        editor.getWavePanel().setShape("square");
        check(processor.getDocument().snapshot().wave[64] == 63 && processor.getDocument().snapshot().wave[127] == 0, "shapes fill only the current wave");

        editor.getWavePanel().removeWave();
        check(editor.getWavePanel().numWaves() == 1, "waves can be removed");

        select("N163 Wave");
        check(editor.getN163SizeBox().isVisible() && editor.getWavePanel().isVisible(), "N163 has its wave editor and size");
        editor.getN163SizeBox().setSelectedId(32, juce::sendNotificationSync);
        check(processor.getDocument().snapshot().waveSize == 32 && processor.getDocument().snapshot().waveLength == 32, "changing the size makes a new wave");
        check(processor.getDocument().snapshot().wave[0] <= 15, "with the N163's smaller range");

        // Each of these edits the wave while the document is locked, so they mustn't ask it for anything.
        {
            auto& panel = editor.getWavePanel();

            graph.mouseDown(mouseEvent(graph, { 5.0f, 2.0f }, { 5.0f, 2.0f }));
            graph.mouseUp(mouseEvent(graph, { 5.0f, 2.0f }, { 5.0f, 2.0f }));
            check(processor.getDocument().snapshot().wave[0] > 12, "N163 waves can be drawn");

            panel.addWave();
            check(panel.numWaves() == 2, "N163 waves can be added");
            panel.setShape("square");
            check(processor.getDocument().snapshot().wave[32] == 15, "and shaped");

            juce::StringArray values;
            for (int i = 0; i < 32; ++i)
                values.add("3");
            panel.getTextField().setText(values.joinIntoString(" "));
            panel.commitText();
            check(processor.getDocument().snapshot().wave[63] == 3, "and typed");

            panel.getRepeatField().setText("4");
            panel.commitRepeat();
            panel.removeWave();
            check(panel.numWaves() == 1, "and removed");
        }

        {
            constexpr auto shift = juce::ModifierKeys::shiftModifier;
            const auto y = (float) graph.getHeight() * 0.5f;
            const auto x = 5.5f * (float) graph.getWidth() / 32.0f;
            const auto pixelsPerValue = juce::jlimit(12.0f, 32.0f, (float) graph.getHeight() / 15.0f * 4.0f);

            processor.getDocument().edit([](Instrument& i) { i.wave[5] = 7; });

            graph.mouseDown(mouseEvent(graph, { x, y }, { x, y }, 1, shift));
            graph.mouseDrag(mouseEvent(graph, { x, y + pixelsPerValue * 2.0f }, { x, y }, 1, shift));
            check(processor.getDocument().snapshot().wave[5] == 5, "N163 waves fine-tune too");
            graph.mouseDrag(mouseEvent(graph, { x, y - pixelsPerValue * 100.0f }, { x, y }, 1, shift));
            check(processor.getDocument().snapshot().wave[5] == 15, "up to their smaller limit");
            graph.mouseUp(mouseEvent(graph, { x, y }, { x, y }, 1, shift));
        }

        select("VRC7 FM");
        check(editor.getVrc7PatchBox().isVisible() && ! editor.getPanel(envelopeDutyCycle).isVisible(), "VRC7 shows its patches");
        check(! editor.getPatchField().isVisible(), "preset patches have no registers to type");
        editor.getVrc7PatchBox().setSelectedId(1, juce::sendNotificationSync);
        editor.refreshFromProcessor();
        check(processor.getDocument().snapshot().vrc7Patch == 0 && editor.getPatchField().isVisible(), "a custom patch shows its registers");
        editor.getPatchField().setText("1 2 3 4 5 6 7 8");
        editor.commitPatchField();
        check(processor.getDocument().snapshot().vrc7PatchRegisters[7] == 8, "and they can be typed");
        editor.getPatchField().setText("1 2 3");
        editor.commitPatchField();
        check(processor.getDocument().snapshot().vrc7PatchRegisters[0] == 1 && editor.getPatchField().getText().startsWith("1 2 3 4"), "a wrong count is rejected");

        select("EPSM FM");
        check(editor.getPatchField().isVisible() && editor.getPatchField().getText().startsWith("4 192"), "EPSM FM shows its 31 registers");

        select("EPSM Square");
        check(editor.getPanel(envelopeS5bMixer).isVisible() && editor.getS5bShapeBox().isVisible(), "EPSM squares have the S5B's controls");

        select("EPSM Rhythm");
        check(editor.getPanel(envelopeVolume).isVisible() && ! editor.getPanel(envelopeArpeggio).isVisible(), "drums only have volume");

        select("Pulse");
        check(editor.getPanel(envelopeDutyCycle).isVisible() && ! editor.getWavePanel().isVisible(), "other channels are unchanged");
    }

    // The N163's channel count is saved, and rebuilds the chip.
    processor.setChannelIndex(ChipmunkProcessor::channelNames().indexOf("N163 Wave"));
    processor.getDocument().edit([](Instrument& i) { i.setDefaultWave(16, 15); });
    setParam(processor, "n163Channels", 5.0f);
    render(processor, 512);
    processor.applyMachineChange();
    render(processor, 512);
    const auto hz = frequency(render(processor, 48000, noteAt(0, true)), 4800);
    std::printf("  N163 with 5 channels: A4 measures %.2f Hz\n", hz);
    check(std::abs(1200.0 * std::log2(hz / 440.0)) < 15.0, "more N163 channels stay in tune");
}

void testPolyphony()
{
    std::puts("voices");

    constexpr double rate = 48000.0;
    constexpr int length = 24000;

    using Events = std::function<void(PolyEngine&, int position)>;

    const auto renderPoly = [&](int voices, int type, const Events& events) {
        PolyEngine engine;
        engine.prepare(rate, false, 0, 1, voices);
        engine.setChannelType((ChannelType) type);
        engine.setInstrument(Instrument {});

        std::vector<float> output((size_t) length);

        for (int position = 0; position < length; position += 64)
        {
            events(engine, position);
            engine.render(output.data() + position, nullptr, 64);
        }

        return output;
    };

    const auto differs = [](const std::vector<float>& a, const std::vector<float>& b) {
        auto most = 0.0f;
        for (size_t i = 0; i < a.size(); ++i)
            most = std::max(most, std::abs(a[i] - b[i]));
        return most;
    };

    const auto loudest = [](const std::vector<float>& a, size_t from) {
        auto most = 0.0f;
        for (auto i = from; i < a.size(); ++i)
            most = std::max(most, std::abs(a[i]));
        return most;
    };

    const auto noteAt = [](int position, int when, int key) {
        return [=](PolyEngine& engine, int now) {
            if (now == position)
                engine.noteOn(key, 127);
            if (now == when)
                engine.noteOff(key);
        };
    };

    const auto pulse = (int) ChannelType::pulse1;
    const auto a = noteAt(0, 16000, 60), b = noteAt(0, 16000, 67), c = noteAt(6400, 16000, 72);

    const auto soloA = renderPoly(2, pulse, a), soloB = renderPoly(2, pulse, b);

    // One voice has to stay the plain mono engine, with its last-note priority.
    {
        NesEngine mono;
        mono.prepare(rate, false, 0, 1);
        mono.setInstrument(Instrument {});

        std::vector<float> expected((size_t) length);

        for (int position = 0; position < length; position += 64)
        {
            if (position == 0) { mono.noteOn(60, 127); mono.noteOn(67, 127); }
            if (position == 16000) { mono.noteOff(67); mono.noteOff(60); }
            mono.render(expected.data() + position, nullptr, 64);
        }

        const auto single = renderPoly(1, pulse, [&](PolyEngine& engine, int now) {
            if (now == 0) { engine.noteOn(60, 127); engine.noteOn(67, 127); }
            if (now == 16000) { engine.noteOff(67); engine.noteOff(60); }
        });

        check(differs(single, expected) == 0.0f, "one voice sounds exactly like the mono engine");
    }

    // Each note has its own chip, so a chord is exactly its notes added together.
    {
        const auto chord = renderPoly(2, pulse, [&](PolyEngine& engine, int now) { a(engine, now); b(engine, now); });
        auto sum = soloA;
        for (size_t i = 0; i < sum.size(); ++i)
            sum[i] += soloB[i];

        check(differs(chord, sum) < 1.0e-5f, "a chord is its notes played separately, added together");
    }

    // Letting go of one note leaves the other alone.
    {
        const auto chord = renderPoly(2, pulse, [&](PolyEngine& engine, int now) {
            if (now == 0) { engine.noteOn(60, 127); engine.noteOn(67, 127); }
            if (now == 6400) engine.noteOff(67);
            if (now == 16000) engine.noteOff(60);
        });

        const auto gap = differs({ chord.begin() + 8000, chord.begin() + 16000 }, { soloA.begin() + 8000, soloA.begin() + 16000 });
        check(gap < 0.01f, "releasing one note leaves the other playing");
        check(loudest(chord, 20000) < 0.001f, "all notes end");
    }

    // With no voice free, the oldest note goes, and its late note-off doesn't touch the new one.
    {
        const auto stolen = renderPoly(2, pulse, [&](PolyEngine& engine, int now) {
            a(engine, now);
            b(engine, now);
            if (now == 6400) engine.noteOn(72, 127);
            if (now == 8000) engine.noteOff(60);
            if (now == 16000) engine.noteOff(72);
        });

        // A voice keeps its waveform's phase across notes, so compare pitches rather than samples.
        const auto strength = [&](double hz) {
            double re = 0.0, im = 0.0;
            for (int i = 9000; i < 16000; ++i)
            {
                const auto angle = juce::MathConstants<double>::twoPi * hz * i / rate;
                re += stolen[(size_t) i] * std::cos(angle);
                im += stolen[(size_t) i] * std::sin(angle);
            }
            return std::hypot(re, im) / 7000.0;
        };

        const auto c4 = strength(261.63), g4 = strength(392.0), c5 = strength(523.25);
        check(g4 > 0.02 && c5 > 0.02 && c4 < g4 * 0.1, "a new note takes the oldest voice, and the old note-off changes nothing");
    }

    // A released voice that's still ringing is only taken once nothing is silent.
    {
        Instrument withTail;
        withTail.envelopes[envelopeVolume].length = 3;
        withTail.envelopes[envelopeVolume].values[0] = withTail.envelopes[envelopeVolume].values[1] = 15;
        withTail.envelopes[envelopeVolume].values[2] = 15;
        withTail.envelopes[envelopeVolume].release = 1;

        PolyEngine engine;
        engine.prepare(rate, false, 0, 1, 3);
        engine.setInstrument(withTail);

        std::array<float, 64> block {};
        engine.noteOn(60, 127);
        engine.render(block.data(), nullptr, 64);
        engine.noteOn(64, 127);
        engine.render(block.data(), nullptr, 64);
        engine.noteOff(64);
        engine.render(block.data(), nullptr, 64);
        check(engine.getSoundingNote() >= 0, "the display follows a sounding voice");
        engine.allNotesOff();
        for (int i = 0; i < 40; ++i)
            engine.render(block.data(), nullptr, 64);
        check(engine.getSoundingNote() < 0, "all notes off silences every voice");
    }

    // The same key takes its own voice back, rather than playing against its own tail.
    {
        const auto twice = renderPoly(4, pulse, [&](PolyEngine& engine, int now) {
            if (now == 0 || now == 3200) engine.noteOn(60, 127);
            if (now == 16000) engine.noteOff(60);
        });
        const auto once = renderPoly(1, pulse, [&](PolyEngine& engine, int now) {
            if (now == 0 || now == 3200) { engine.noteOff(60); engine.noteOn(60, 127); }
            if (now == 16000) engine.noteOff(60);
        });

        check(differs(twice, once) < 1.0e-5f, "repeating a key doesn't stack voices");
    }

    // The whole plugin, including the rebuild and saving.
    {
        TempFolder temp;
        ChipmunkProcessor processor(temp.presets);
        processor.prepareToPlay(rate, 512);

        auto& parameter = *processor.getParameters().getParameter("voices");
        check(parameter.convertFrom0to1(parameter.getValue()) == 1.0f, "new devices start mono");

        parameter.setValueNotifyingHost(parameter.convertTo0to1(4.0f));
        processor.applyMachineChange();

        juce::AudioBuffer<float> buffer(2, 512);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 127), 0);
        midi.addEvent(juce::MidiMessage::noteOn(1, 64, (juce::uint8) 127), 0);
        midi.addEvent(juce::MidiMessage::noteOn(1, 67, (juce::uint8) 127), 0);

        auto peak = 0.0f;
        for (int i = 0; i < 10; ++i)
        {
            buffer.clear();
            processor.processBlock(buffer, midi);
            midi.clear();
            peak = std::max(peak, buffer.getMagnitude(0, 512));
        }

        check(peak > 0.05f, "a chord plays through the plugin after changing voices");

        juce::MemoryBlock state;
        processor.getStateInformation(state);

        ChipmunkProcessor restored(temp.presets);
        restored.setStateInformation(state.getData(), (int) state.getSize());
        auto& restoredParameter = *restored.getParameters().getParameter("voices");
        check(restoredParameter.convertFrom0to1(restoredParameter.getValue()) == 4.0f, "voices save with the Set");
        check(restored.currentPreset().voices == 4, "voices are part of the sound");

        Preset parsed;
        std::string error;
        check(parsePreset(formatPreset(restored.currentPreset()), parsed, error) && parsed.voices == 4, "voices save in sound files");
        check(formatPreset(Preset {}).find("voices") == std::string::npos, "mono sounds save as they always did");

        // Sets from before this setting have no value for it.
        auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), (int) state.getSize());
        for (auto* child = xml->getFirstChildElement(); child != nullptr;)
        {
            auto* next = child->getNextElement();
            if (child->getStringAttribute("id") == "voices")
                xml->removeChildElement(child, true);
            child = next;
        }

        juce::MemoryBlock oldState;
        juce::AudioProcessor::copyXmlToBinary(*xml, oldState);
        restored.setStateInformation(oldState.getData(), (int) oldState.getSize());
        check(restoredParameter.convertFrom0to1(restoredParameter.getValue()) == 1.0f, "older Sets open mono");
    }

    // Every channel has to open silently with extra voices too, and take them on its first block.
    {
        auto allQuiet = true;

        for (int channel = 0; channel < ChipmunkProcessor::channelNames().size(); ++channel)
        {
            TempFolder temp;
            ChipmunkProcessor processor(temp.presets);

            for (const auto& preset : factoryPresets())
                if ((ChipmunkProcessor::channelNames()[channel] == "DPCM" && preset.name == "Synth Drum Kit") || (preset.channel == ChannelType::drumKit && ChipmunkProcessor::channelNames()[channel] == "Drum Kit"))
                    processor.loadPreset(preset);

            processor.setChannelIndex(channel);

            auto& parameter = *processor.getParameters().getParameter("voices");
            parameter.setValueNotifyingHost(parameter.convertTo0to1(3.0f));

            processor.prepareToPlay(rate, 512);

            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;

            for (int i = 0; i < 40; ++i)
            {
                buffer.clear();
                processor.processBlock(buffer, midi);
                allQuiet = allQuiet && buffer.getMagnitude(0, 512) < 0.002f;
            }
        }

        check(allQuiet, "three voices open silently on every channel");
    }

    // Two samples can ring together on a kit, which one voice can't do.
    {
        TempFolder temp;
        ChipmunkProcessor processor(temp.presets);

        for (const auto& preset : factoryPresets())
            if (preset.name == "Synth Drum Kit")
                processor.loadPreset(preset);

        auto& parameter = *processor.getParameters().getParameter("voices");
        parameter.setValueNotifyingHost(parameter.convertTo0to1(2.0f));
        processor.prepareToPlay(rate, 512);

        const auto kit = processor.getDocument().kit();
        check(kit != nullptr, "extra voices share the sound's kit");

        std::vector<int> keys;
        for (int value = noteMusicalMin; value <= noteMusicalMax; ++value)
            if (kit != nullptr && kit->mappings[(size_t) value].sample != nullptr)
                keys.push_back(value + 23);

        const auto play = [&](std::vector<int> played) {
            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            for (const auto key : played)
                midi.addEvent(juce::MidiMessage::noteOn(1, key, (juce::uint8) 127), 0);

            auto peak = 0.0f;
            for (int i = 0; i < 6; ++i)
            {
                buffer.clear();
                processor.processBlock(buffer, midi);
                midi.clear();
                peak = std::max(peak, buffer.getMagnitude(0, 512));
            }
            return peak;
        };

        check(keys.size() >= 2 && play({ keys[0], keys[1] }) > 0.02f, "samples play on extra voices");
    }
}

void testResizing()
{
    std::puts("window");

    TempFolder temp;
    ChipmunkProcessor processor(temp.presets);
    ChipmunkEditor editor(processor);

    for (const auto [width, height] : std::vector<std::pair<int, int>> {{1000, 640}, {1100, 760}, {1800, 1300}})
    {
        editor.setSize(width, height);

        bool insideWindow = true, positive = true;
        for (auto* child : editor.getChildren())
        {
            if (! child->isVisible() || dynamic_cast<juce::TooltipWindow*>(child) != nullptr || dynamic_cast<juce::ResizableCornerComponent*>(child) != nullptr)
                continue;
            insideWindow = insideWindow && editor.getLocalBounds().contains(child->getBounds());
            positive = positive && child->getWidth() > 0 && child->getHeight() > 0;
        }

        for (auto* child : editor.getChildren())
            if (auto* toggle = dynamic_cast<juce::ToggleButton*>(child))
                if (toggle->isVisible())
                    positive = positive && toggle->getWidth() >= 90 && toggle->getRight() <= width;

        check(insideWindow && positive, juce::String(width) + "x" + juce::String(height) + " keeps everything inside the window");
    }
}

void testComputerKeyboard()
{
    std::puts("computer keyboard and key highlights");

    TempFolder temp;
    ChipmunkProcessor processor(temp.presets);
    processor.prepareToPlay(48000.0, 512);
    auto& state = processor.getKeyboardState();

    std::set<int> down;
    ComputerKeyboard keys(state, [&](int keyCode) { return down.count(keyCode) > 0; });
    const auto press = [&](int keyCode) { down.insert(keyCode); keys.update(); };
    const auto lift = [&](int keyCode) { down.erase(keyCode); keys.update(); };

    press('a');
    check(state.isNoteOn(1, 60), "A plays C");
    check(rms(render(processor, 12000), 2400) > 0.01, "and it sounds");
    press('w');
    press(';');
    check(state.isNoteOn(1, 61) && state.isNoteOn(1, 76), "the other keys follow the piano, up to the semicolon");
    lift('a');
    lift('w');
    lift(';');
    check(! state.isNoteOn(1, 60) && ! state.isNoteOn(1, 61) && ! state.isNoteOn(1, 76), "lifting a key stops its note");

    press('a');
    press('x');
    lift('x');
    check(keys.getBaseNote() == 72 && state.isNoteOn(1, 60), "X moves up an octave, leaving the held note alone");
    lift('a');
    check(! state.isNoteOn(1, 60), "and that note still stops when its key lifts");
    press('a');
    check(state.isNoteOn(1, 72), "new notes use the new octave");
    lift('a');

    for (int i = 0; i < 12; ++i)
    {
        press('x');
        lift('x');
    }

    check(keys.getBaseNote() == 96, "octaves stop at the top of the keyboard");

    for (int i = 0; i < 12; ++i)
    {
        press('z');
        lift('z');
    }

    check(keys.getBaseNote() == 24, "and at the bottom");
    check(keys.labelForNote(24) == "A" && keys.labelForNote(25) == "W" && keys.labelForNote(23).isEmpty() && keys.labelForNote(41).isEmpty(), "each note knows its key");

    press('s');
    down.clear();
    keys.releaseLifted();
    check(! state.isNoteOn(1, 26), "a key lifted while something else had focus still stops its note");
    down.insert('s');
    keys.releaseLifted();
    check(! state.isNoteOn(1, 26), "but a key can't start a note without the focus");
    keys.update();
    check(state.isNoteOn(1, 26), "until it has it");
    keys.releaseAll();
    check(! state.isNoteOn(1, 26), "everything can be stopped at once");

    check(keys.usesKey(juce::KeyPress('a')) && keys.usesKey(juce::KeyPress('Z')) && keys.usesKey(juce::KeyPress(';')), "the keys are recognised");
    check(! keys.usesKey(juce::KeyPress('q')) && ! keys.usesKey(juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0)), "other keys and shortcuts are left alone");

    ChipmunkEditor editor(processor);
    check(! editor.keyPressed(juce::KeyPress('a')) && ! editor.keyStateChanged(true), "a plugin leaves the letter keys to its host");
    editor.setComputerKeyboardEnabled(true);
    check(editor.keyPressed(juce::KeyPress('a')) && editor.keyStateChanged(true), "the standalone app takes them");
    check(! editor.keyPressed(juce::KeyPress('q')), "but not keys it doesn't use");
    editor.setComputerKeyboardEnabled(false);

    // MIDI from the host lights the keys, including notes too short to catch between frames.
    auto& keyboard = editor.getKeyboard();
    editor.refreshFromProcessor();
    check(keyboard.getExternalNotes().none(), "nothing shows before any MIDI");
    render(processor, 512, noteAt(0, true, 64));
    editor.refreshFromProcessor();
    check(keyboard.getExternalNotes()[64] && keyboard.getExternalNotes().count() == 1, "a MIDI note lights its key");
    editor.refreshFromProcessor();
    check(keyboard.getExternalNotes()[64], "for as long as it's held");
    render(processor, 512, noteAt(0, false, 64));
    editor.refreshFromProcessor();
    check(keyboard.getExternalNotes().none(), "and goes out when it ends");

    {
        auto midi = noteAt(0, true, 72);
        midi.addEvents(noteAt(100, false, 72), 0, -1, 0);
        render(processor, 512, midi);
    }

    editor.refreshFromProcessor();
    check(keyboard.getExternalNotes()[72], "a very short note still flashes");
    editor.refreshFromProcessor();
    check(keyboard.getExternalNotes().none(), "for one frame");

    {
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
        midi.addEvent(juce::MidiMessage::noteOn(1, 127, (juce::uint8) 100), 1);
        midi.addEvent(juce::MidiMessage::allNotesOff(1), 2);
        render(processor, 512, midi);
    }

    editor.refreshFromProcessor();
    editor.refreshFromProcessor();
    check(keyboard.getExternalNotes().none(), "all notes off clears them, top note included");
}

void testFmNotesEnd()
{
    std::puts("FM notes end after their last note-off");

    constexpr int blockSize = 4800; // A tenth of a second.

    // The level after a note lasting half a second is released, a set time later.
    const auto levelAfterRelease = [](ChannelType type, const Instrument& instrument, int blocksAfter) {
        NesEngine engine;
        engine.prepare(48000.0, false, NesEngine::chipsNeededFor(type));
        engine.setChannelType(type);
        engine.setInstrument(instrument);

        std::vector<float> audio(blockSize);
        const auto block = [&] {
            engine.render(audio.data(), blockSize);
            auto mean = 0.0;
            for (const auto sample : audio)
                mean += sample;
            mean /= blockSize;
            auto sum = 0.0;
            for (const auto sample : audio)
                sum += (sample - mean) * (sample - mean);
            return std::sqrt(sum / blockSize);
        };

        engine.noteOn(60, 127);
        for (int i = 0; i < 5; ++i)
            block();
        engine.noteOff(60);

        auto level = 0.0;
        for (int i = 0; i < blocksAfter; ++i)
            level = block();
        return level;
    };

    for (const auto& preset : factoryPresets())
        if (preset.channel == ChannelType::epsmFm1 || preset.channel == ChannelType::vrc7Fm1)
            check(levelAfterRelease(preset.channel, preset.instrument, 20) < 0.001, preset.name + " is silent 2 seconds after its note ends");

    const auto withRelease = [](int rate, int algorithm = 7, int carrierRate = -1) {
        Instrument instrument;
        instrument.epsmPatchRegisters[0] = (uint8_t) algorithm;

        for (size_t y = 0; y < 4; ++y)
            instrument.epsmPatchRegisters[y * 7 + 7] = (uint8_t) (0xf0 | (y == 3 && carrierRate >= 0 ? carrierRate : rate));

        return instrument;
    };

    for (const auto rate : { 0, 1 })
        check(levelAfterRelease(ChannelType::epsmFm1, withRelease(rate), 2) < 0.001, "a patch with release rate " + juce::String(rate) + ", from an older Set or typed in, stops at once");

    check(levelAfterRelease(ChannelType::epsmFm1, withRelease(2), 5) > 0.05, "a slow release that does fade still rings out");
    check(levelAfterRelease(ChannelType::epsmFm1, withRelease(15, 7, 0), 2) < 0.001, "one operator that never fades is enough to need stopping");
    check(levelAfterRelease(ChannelType::epsmFm1, withRelease(0, 0, 8), 20) < 0.001, "with algorithm 0, only the last operator's release matters");
    check(levelAfterRelease(ChannelType::epsmFm1, withRelease(0, 0, 8), 1) > 0.0005, "so that sound still rings out");
}

void testLegalNotices()
{
    std::puts("legal notices");

    TempFolder temp;
    ChipmunkProcessor processor(temp.presets);
    ChipmunkEditor editor(processor);
    auto& about = editor.getAboutPanel();

    const auto notice = AboutPanel::getNotice();
    check(notice.contains("Copyright") && notice.contains("ABSOLUTELY NO WARRANTY") && notice.contains("GNU Affero General Public License"), "the notice has what the AGPL asks for");

    for (const auto* part : { "JUCE", "FamiStudio", "Nes_Snd_Emu", "Nuked-OPN2", "emu2413", "emu2149" })
        check(notice.contains(part), juce::String("it credits ") + part);

    check(! about.isVisible() && editor.getAboutButton().isVisible(), "the panel starts closed, with a button to open it");
    editor.getAboutButton().onClick();
    check(about.isVisible() && about.getBounds() == editor.getLocalBounds(), "the button opens it over the whole window");

    for (const auto [width, height] : std::vector<std::pair<int, int>> {{1000, 640}, {1800, 1300}})
    {
        editor.setSize(width, height);
        auto fits = about.getBounds() == editor.getLocalBounds();
        for (auto* child : about.getChildren())
            fits = fits && about.getLocalBounds().contains(child->getBounds()) && child->getWidth() > 0;
        check(fits, "it fits a " + juce::String(width) + " wide window");
    }

    check(editor.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)) && ! about.isVisible(), "Escape closes it");
}

void writeScreenshots(const juce::File& folder)
{
    TempFolder temp;
    ChipmunkProcessor processor(temp.presets);
    processor.prepareToPlay(48000.0, 512);
    ChipmunkEditor editor(processor);

    const auto shoot = [&](const juce::String& name) {
        const auto image = editor.createComponentSnapshot(editor.getLocalBounds());
        juce::PNGImageFormat png;
        juce::FileOutputStream out(folder.getChildFile(name + ".png"));
        out.setPosition(0);
        out.truncate();
        png.writeImageToStream(image, out);
    };

    editor.refreshFromProcessor();
    shoot("01_init");

    Preset preset;
    std::string error;
    parsePreset("Chipmunk preset 1\nname=Showcase\nchannel=pulse\nvolume=15 14 12 11 | 10 9 10 11 / 8 6 5 3 2 1 0\narpeggio=| 0 0 4 4 7 7 12 12\npitch=0 0 0 0 | 1 2 2 1 -1 -2 -2 -1\ndutyEnvelope=| 0 0 1 1 2 2 3 3\n", preset, error);
    processor.loadPreset(preset);
    processor.getLibrary().setFavorite("Major Arp", true);
    processor.getLibrary().setFavorite("Snare", true);
    render(processor, 512);
    render(processor, 4800, noteAt(0, true));
    editor.getBrowser().refreshList();
    editor.refreshFromProcessor();
    shoot("02_showcase");

    for (const auto& entry : processor.getLibrary().getEntries())
        if (entry.preset.name == "Snare")
            processor.loadPreset(entry.preset);
    render(processor, 512);
    editor.refreshFromProcessor();
    editor.getPanel(envelopeArpeggio).startTyping();
    editor.getPanel(envelopeArpeggio).getTextField().setText("1 2 99");
    editor.getPanel(envelopeArpeggio).getTextField().onReturnKey();
    editor.getBrowser().getSearchField().setText("n");
    editor.getBrowser().refreshList();
    shoot("03_noise_with_error");

    editor.setSize(1000, 640);
    shoot("04_smallest");

    editor.setSize(1100, 760);
    parsePreset("Chipmunk preset 1\nname=Long\nvolume=15 14 13 12 11 10 9 8\narpeggio=-12 12 -7 7 0 -1 1 63 -64\npitch=" + std::string([] { std::string t; for (int i = 0; i < 40; ++i) t += std::to_string(i % 16 - 8) + " "; return t; }()) + "\ndutyEnvelope=" + std::string([] { std::string t; for (int i = 0; i < 100; ++i) t += std::to_string(i % 4) + " "; return t; }()) + "\n", preset, error);
    editor.getPanel(envelopeArpeggio).getTextField().onEscapeKey();
    processor.loadPreset(preset);
    editor.refreshFromProcessor();
    shoot("05_alignment");

    editor.getBrowser().importFile(juce::File(CHIPMUNK_PROJECTS_DIR).getChildFile("TestVRC6.fms"));
    processor.getKeyboardState().allNotesOff(1);
    editor.refreshFromProcessor();
    shoot("06_project_import");

    {
        TempFolder wavs;
        juce::StringArray paths;
        for (const auto* name : { "Kick", "Snare", "Closed Hat", "Tom", "Vocal Hit" })
            paths.add(writeTestWav(wavs.folder.getChildFile(juce::String(name) + ".wav"), 0.1 + 0.08 * paths.size(), 44100.0, 1, 90.0 + 90.0 * paths.size()).getFullPathName());
        editor.filesDropped(paths, 0, 0);
        processor.getKeyboardState().allNotesOff(1);
        editor.refreshFromProcessor();
        editor.resized();
        shoot("07_dpcm_kit");
    }

    for (const auto& entry : processor.getLibrary().getEntries())
        if (entry.preset.name == "S5B Static Hit")
            processor.loadPreset(entry.preset);
    render(processor, 512);
    editor.refreshFromProcessor();
    shoot("08_s5b");

    for (const auto& entry : processor.getLibrary().getEntries())
        if (entry.preset.name == "VRC6 Sweep")
            processor.loadPreset(entry.preset);
    render(processor, 512);
    render(processor, 4800, noteAt(0, true));
    editor.refreshFromProcessor();
    shoot("09_vrc6");

    for (const auto& entry : processor.getLibrary().getEntries())
        if (entry.preset.name == "Saw Bass")
            processor.loadPreset(entry.preset);
    render(processor, 512);
    editor.refreshFromProcessor();
    shoot("10_saw");

    for (const auto& [sound, shot] : std::vector<std::pair<const char*, const char*>> {
             { "FDS Warble", "11_fds" }, { "N163 Saw", "12_n163" }, { "VRC7 Bell", "13_vrc7" }, { "EPSM FM Bass", "14_epsm_fm" }, { "EPSM Hat", "15_epsm_square" }, { "EPSM Rhythm Kit", "16_epsm_rhythm" } })
    {
        for (const auto& entry : processor.getLibrary().getEntries())
            if (entry.preset.name == sound)
                processor.loadPreset(entry.preset);

        render(processor, 512);
        processor.applyMachineChange();
        render(processor, 4800, noteAt(0, true, 60));
        editor.refreshFromProcessor();
        shoot(shot);
        render(processor, 512, noteAt(0, false, 60));
    }

    // A Drum Kit with its list as tall as it goes, and the narrowest window's header.
    for (const auto& preset : factoryPresets())
        if (preset.channel == ChannelType::drumKit)
            processor.loadPreset(preset);

    render(processor, 512);
    editor.refreshFromProcessor();

    if (const auto kit = processor.getDocument().kit())
        for (int value = noteMusicalMax; value >= noteMusicalMin; --value)
            if (kit->mappings[(size_t) value].noise != nullptr)
                processor.getDocument().selectKitKey(value);

    editor.setSize(1100, 760);
    editor.refreshFromProcessor();
    processor.setKitListShare(1.0f);
    editor.resized();
    shoot("20_kit_tall_list");

    processor.setKitListShare(0.0f);
    editor.resized();
    shoot("21_kit_short_list");
    processor.setKitListShare(0.4f);
    processor.setChannelIndex(0);
    editor.setSize(1000, 640);
    editor.refreshFromProcessor();
    shoot("22_narrow_header");

    // Notes from every source lit at once, with the letters the standalone app shows.
    editor.setSize(1180, 800);
    editor.setComputerKeyboardEnabled(true);
    processor.getKeyboardState().noteOn(1, 60, 0.9f);
    processor.getKeyboardState().noteOn(1, 66, 0.9f);
    {
        juce::MidiBuffer chord;
        for (const auto note : { 72, 75, 79 })
            chord.addEvent(juce::MidiMessage::noteOn(1, note, (juce::uint8) 100), 0);
        render(processor, 512, chord);
    }
    editor.refreshFromProcessor();
    shoot("23_keys_down");
    editor.setComputerKeyboardEnabled(false);

    editor.getAboutPanel().setVisible(true);
    editor.getAboutPanel().toFront(false);
    shoot("24_about");
    editor.setSize(1000, 640);
    shoot("25_about_smallest");
    editor.getAboutPanel().setVisible(false);
}
} // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;

    testStateRestoresIdentically();
    testAudioPath();
    testEnvelopeGestures();
    testNumbersLineUpWithColumns();
    testBrowser();
    testProjectImport();
    testDpcm();
    testChips();
    testFactorySounds();
    testMachine();
    testActivityDisplay();
    testLiveTemplates();
    testMidiEffects();
    testLoadingIsQuiet();
    testPolyphony();
    testDrumKit();
    testNewChips();
    testResizing();
    testComputerKeyboard();
    testFmNotesEnd();
    testLegalNotices();

    if (argc > 1)
        writeScreenshots(juce::File(argv[1]));

    std::printf(failures == 0 ? "\nAll plugin tests passed\n" : "\n%d checks failed\n", failures);
    return failures == 0 ? 0 : 1;
}
