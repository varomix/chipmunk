// Plays a Chipmunk Live Set's arrangement through the real processor and writes a WAV.
// It leaves out return tracks and Live's own devices, and Live's pan law is approximated.
//
//     render_als <set.als> <output.wav>

#include "plugin/PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <vector>

namespace
{
constexpr double sampleRate = 48000.0;
constexpr int blockSize = 512;

// Live's controller slots start with pitch bend and channel pressure, so CC 0 is ControllerTargets.2.
constexpr int controllerSlotOfCc0 = 2;

struct Note { double start, end; int key, velocity; };
struct ControllerEvent { double time; int controller, value; };

struct PlayingHead final : juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying(true);
        return info;
    }
};

const juce::XmlElement* findFirst(const juce::XmlElement& element, const juce::String& tag)
{
    if (element.hasTagName(tag))
        return &element;

    for (auto* child : element.getChildIterator())
        if (const auto* found = findFirst(*child, tag))
            return found;

    return nullptr;
}

const juce::XmlElement* at(const juce::XmlElement* element, std::initializer_list<const char*> path)
{
    for (const auto* name : path)
        element = element != nullptr ? element->getChildByName(name) : nullptr;

    return element;
}

juce::MemoryOutputStream* readState(const juce::XmlElement& track, juce::MemoryOutputStream& state)
{
    juce::MemoryBlock plistData;
    plistData.loadFromHexString(findFirst(track, "Buffer")->getAllSubText());

    const auto plist = plistData.toString();
    const auto start = plist.indexOf(plist.indexOf("jucePluginState"), "<data>") + 6;

    juce::Base64::convertFromBase64(state, plist.substring(start, plist.indexOf(start, "</data>")).removeCharacters(" \t\r\n"));
    return &state;
}

// A clip's MIDI controller envelopes. Each event holds until the next, so only changes matter.
std::vector<ControllerEvent> readControllers(const juce::XmlElement& track)
{
    std::map<juce::String, int> controllerForTarget;
    const auto* targets = at(&track, { "DeviceChain", "MainSequencer", "MidiControllers" });

    for (auto* target : targets->getChildIterator())
        if (target->getTagName().startsWith("ControllerTargets."))
            controllerForTarget[target->getStringAttribute("Id")] = target->getTagName().fromFirstOccurrenceOf(".", false, false).getIntValue() - controllerSlotOfCc0;

    std::vector<ControllerEvent> result;
    const auto* events = at(&track, { "DeviceChain", "MainSequencer", "ClipTimeable", "ArrangerAutomation", "Events" });

    for (auto* clip : events->getChildWithTagNameIterator("MidiClip"))
    {
        const auto clipStart = clip->getDoubleAttribute("Time");

        for (auto* envelope : at(clip, { "Envelopes", "Envelopes" })->getChildIterator())
        {
            const auto controller = controllerForTarget.find(at(envelope, { "EnvelopeTarget", "PointeeId" })->getStringAttribute("Value"));

            if (controller == controllerForTarget.end())
                continue;

            auto previous = -1;

            for (auto* event : at(envelope, { "Automation", "Events" })->getChildIterator())
            {
                const auto value = event->getIntAttribute("Value");

                if (value != previous)
                    result.push_back({ std::max(0.0, clipStart + event->getDoubleAttribute("Time")), controller->second, value });

                previous = value;
            }
        }
    }

    return result;
}

std::vector<Note> readNotes(const juce::XmlElement& track)
{
    std::vector<Note> notes;
    const auto* events = at(&track, { "DeviceChain", "MainSequencer", "ClipTimeable", "ArrangerAutomation", "Events" });

    for (auto* clip : events->getChildWithTagNameIterator("MidiClip"))
    {
        const auto clipStart = clip->getDoubleAttribute("Time");

        for (auto* keyTrack : at(clip, { "Notes", "KeyTracks" })->getChildIterator())
        {
            const auto key = keyTrack->getChildByName("MidiKey")->getIntAttribute("Value");

            for (auto* event : keyTrack->getChildByName("Notes")->getChildIterator())
            {
                const auto start = clipStart + event->getDoubleAttribute("Time");
                notes.push_back({ start, start + event->getDoubleAttribute("Duration"), key, event->getIntAttribute("Velocity") });
            }
        }
    }

    return notes;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::puts("usage: render_als <set.als> <output.wav>");
        return 1;
    }

    juce::ScopedJuceInitialiser_GUI gui;

    juce::FileInputStream raw { juce::File(juce::CharPointer_UTF8(argv[1])) };
    juce::GZIPDecompressorInputStream unzipped(&raw, false, juce::GZIPDecompressorInputStream::gzipFormat);
    const auto xml = juce::parseXML(unzipped.readEntireStreamAsString());

    if (xml == nullptr)
    {
        std::puts("can't read the Set");
        return 1;
    }

    const auto* liveSet = xml->getChildByName("LiveSet");
    const auto tempo = findFirst(*liveSet->getChildByName("MainTrack"), "Tempo")->getChildByName("Manual")->getDoubleAttribute("Value");
    const auto samplesPerBeat = sampleRate * 60.0 / tempo;

    const auto tempFolder = juce::File::createTempFile("chipmunk_render").getSiblingFile("chipmunk_render_presets");
    tempFolder.createDirectory();

    std::vector<std::vector<float>> renderedLeft, renderedRight;
    std::vector<std::pair<float, float>> gains;
    int64_t totalSamples = 0;
    auto problems = 0;

    const auto tailSeconds = 2.0;
    double lastBeat = 0.0;

    for (auto* track : liveSet->getChildByName("Tracks")->getChildWithTagNameIterator("MidiTrack"))
        for (const auto& note : readNotes(*track))
            lastBeat = std::max(lastBeat, note.end);

    totalSamples = (int64_t) ((lastBeat * samplesPerBeat) + tailSeconds * sampleRate);

    for (auto* track : liveSet->getChildByName("Tracks")->getChildWithTagNameIterator("MidiTrack"))
    {
        const auto name = at(track, { "Name", "EffectiveName" })->getStringAttribute("Value");
        const auto notes = readNotes(*track);

        // At one moment, controllers come first, then ends of notes, then new notes.
        struct Event { int64_t sample; int order; juce::MidiMessage message; };
        std::vector<Event> events;

        for (const auto& note : notes)
        {
            events.push_back({ (int64_t) std::llround(note.start * samplesPerBeat), 2, juce::MidiMessage::noteOn(1, note.key, (juce::uint8) note.velocity) });
            events.push_back({ (int64_t) std::llround(note.end * samplesPerBeat), 1, juce::MidiMessage::noteOff(1, note.key) });
        }

        for (const auto& controller : readControllers(*track))
            events.push_back({ (int64_t) std::llround(controller.time * samplesPerBeat), 0, juce::MidiMessage::controllerEvent(1, controller.controller, controller.value) });

        std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.sample != b.sample ? a.sample < b.sample : a.order < b.order; });

        juce::MemoryOutputStream state;
        readState(*track, state);

        ChipmunkProcessor processor(tempFolder);
        processor.setStateInformation(state.getData(), (int) state.getDataSize());
        processor.prepareToPlay(sampleRate, blockSize);
        PlayingHead playHead;
        processor.setPlayHead(&playHead);

        std::vector<float> audio((size_t) totalSamples), audioRight((size_t) totalSamples);
        juce::AudioBuffer<float> buffer(2, blockSize);
        size_t nextEvent = 0;

        for (int64_t position = 0; position < totalSamples; position += blockSize)
        {
            const auto count = (int) std::min((int64_t) blockSize, totalSamples - position);
            juce::MidiBuffer midi;

            while (nextEvent < events.size() && events[nextEvent].sample < position + count)
            {
                const auto& event = events[nextEvent++];
                midi.addEvent(event.message, (int) (event.sample - position));
            }

            buffer.setSize(2, count, false, false, true);
            buffer.clear();
            processor.processBlock(buffer, midi);
            std::copy_n(buffer.getReadPointer(0), count, audio.data() + position);
            std::copy_n(buffer.getReadPointer(1), count, audioRight.data() + position);
        }

        const auto* mixer = at(track, { "DeviceChain", "Mixer" });
        const auto volume = (float) at(mixer, { "Volume", "Manual" })->getDoubleAttribute("Value");
        const auto pan = (float) at(mixer, { "Pan", "Manual" })->getDoubleAttribute("Value");

        auto peak = 0.0f;
        auto sum = 0.0;

        // Measures both sides, as FM sounds can be panned hard to either.
        for (size_t i = 0; i < audio.size(); ++i)
        {
            const auto sample = std::max(std::abs(audio[i]), std::abs(audioRight[i])) * volume;
            peak = std::max(peak, sample);
            sum += sample * sample;
        }

        std::printf("  %-14s %4d notes  plays %-18s rms %6.1f dB  peak %6.1f dB\n", name.toRawUTF8(), (int) notes.size(), processor.currentPreset().name.c_str(),
                    juce::Decibels::gainToDecibels(std::sqrt(sum / (double) audio.size())), juce::Decibels::gainToDecibels(peak));

        if (notes.empty() || peak < 0.01f)
            ++problems;

        renderedLeft.push_back(std::move(audio));
        renderedRight.push_back(std::move(audioRight));
        gains.emplace_back(volume * std::min(1.0f, 1.0f - pan), volume * std::min(1.0f, 1.0f + pan));
    }

    juce::AudioBuffer<float> mix(2, (int) totalSamples);
    mix.clear();

    for (size_t track = 0; track < renderedLeft.size(); ++track)
    {
        mix.addFrom(0, 0, renderedLeft[track].data(), (int) totalSamples, gains[track].first);
        mix.addFrom(1, 0, renderedRight[track].data(), (int) totalSamples, gains[track].second);
    }

    const auto peak = mix.getMagnitude(0, (int) totalSamples);

    std::printf("  mix: %.0f s, peak %.1f dB, rms %.1f dB\n", (double) totalSamples / sampleRate, juce::Decibels::gainToDecibels(peak),
                juce::Decibels::gainToDecibels(mix.getRMSLevel(0, 0, (int) totalSamples)));

    if (peak >= 1.0f)
        ++problems;

    const juce::File output { juce::CharPointer_UTF8(argv[2]) };
    output.deleteFile();

    juce::WavAudioFormat wav;
    auto stream = std::unique_ptr<juce::OutputStream>(output.createOutputStream());
    auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions().withSampleRate(sampleRate).withNumChannels(2).withBitsPerSample(16));

    if (writer == nullptr || ! writer->writeFromAudioSampleBuffer(mix, 0, mix.getNumSamples()))
    {
        std::puts("can't write the WAV");
        ++problems;
    }

    tempFolder.deleteRecursively();

    std::printf(problems == 0 ? "  ok\n" : "  %d problems\n", problems);
    return problems == 0 ? 0 : 1;
}
