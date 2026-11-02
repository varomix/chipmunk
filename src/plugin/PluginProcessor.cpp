#include "plugin/PluginProcessor.h"

#include "ui/PluginEditor.h"

#include <algorithm>
#include <cmath>

namespace
{
const juce::ParameterID channelId { "channel", 1 };
const juce::ParameterID dutyId { "duty", 1 };
const juce::ParameterID gainId { "gain", 1 };
const juce::ParameterID velocityId { "velocity", 1 };
const juce::ParameterID legatoId { "legato", 1 };
const juce::ParameterID glideId { "glide", 1 };
const juce::ParameterID bendRangeId { "bendRange", 1 };
const juce::ParameterID vibratoSpeedId { "vibratoSpeed", 1 };
const juce::ParameterID authenticGridId { "authenticGrid", 1 };
const juce::ParameterID machineId { "machine", 1 };
const juce::ParameterID n163ChannelsId { "n163Channels", 1 };
const juce::ParameterID voicesId { "voices", 2 };

constexpr const char* instrumentStateType = "Instrument";
constexpr const char* instrumentTextProperty = "preset";

constexpr ChannelType channelTypes[] { ChannelType::pulse1, ChannelType::triangle, ChannelType::noise, ChannelType::dpcm,
                                       ChannelType::vrc6Pulse1, ChannelType::vrc6Saw, ChannelType::mmc5Pulse1, ChannelType::s5bSquare1,
                                       ChannelType::fdsWave, ChannelType::n163Wave1, ChannelType::vrc7Fm1,
                                       ChannelType::epsmSquare1, ChannelType::epsmFm1, ChannelType::epsmRhythm1, ChannelType::drumKit };

int channelTypeToIndex(ChannelType type)
{
    for (int i = 0; i < (int) std::size(channelTypes); ++i)
        if (channelTypes[i] == type)
            return i;

    return 0;
}

void setParameterWithGesture(juce::RangedAudioParameter& parameter, float plainValue)
{
    parameter.beginChangeGesture();
    parameter.setValueNotifyingHost(parameter.convertTo0to1(plainValue));
    parameter.endChangeGesture();
}
} // namespace

float ChipmunkProcessor::channelTrimDb(int channelIndex)
{
    // N163 and the FM chips are far louder than the rest on their own, as they expect to share a
    // mix with up to eight channels. These bring them near a pulse, and tools/ compensate for them.
    constexpr int n163 = 9, vrc7 = 10, epsmFm = 12, epsmRhythm = 13;

    return channelIndex == n163 ? -11.0f : channelIndex == vrc7 ? -7.0f : channelIndex == epsmFm ? -6.0f : channelIndex == epsmRhythm ? -4.0f : 0.0f;
}

juce::StringArray ChipmunkProcessor::channelNames()
{
    return { "Pulse", "Triangle", "Noise", "DPCM", "VRC6 Pulse", "VRC6 Saw", "MMC5 Pulse", "S5B Square",
             "FDS Wave", "N163 Wave", "VRC7 FM", "EPSM Square", "EPSM FM", "EPSM Rhythm", "Drum Kit" };
}

juce::AudioProcessorValueTreeState::ParameterLayout ChipmunkProcessor::createParameters()
{
    using namespace juce;

    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add(std::make_unique<AudioParameterChoice>(channelId, "Channel", channelNames(), 0));
    layout.add(std::make_unique<AudioParameterChoice>(dutyId, "Duty", StringArray { "Duty 1", "Duty 2", "Duty 3", "Duty 4", "Duty 5", "Duty 6", "Duty 7", "Duty 8" }, 2));
    layout.add(std::make_unique<AudioParameterFloat>(gainId, "Output", NormalisableRange<float>(-24.0f, 24.0f, 0.1f), 6.0f,
                                                     AudioParameterFloatAttributes().withLabel("dB")));
    layout.add(std::make_unique<AudioParameterBool>(velocityId, "Velocity", true));
    layout.add(std::make_unique<AudioParameterBool>(legatoId, "Legato", true));
    layout.add(std::make_unique<AudioParameterInt>(glideId, "Glide", 0, 64, 0, AudioParameterIntAttributes().withLabel("frames")));
    layout.add(std::make_unique<AudioParameterInt>(bendRangeId, "Bend Range", 0, 24, 2, AudioParameterIntAttributes().withLabel("st")));
    layout.add(std::make_unique<AudioParameterInt>(vibratoSpeedId, "Vibrato Speed", 1, 12, 6));
    layout.add(std::make_unique<AudioParameterBool>(authenticGridId, "Authentic Grid", false));

    // Changing machine rebuilds the chip, which cuts the sound, so it can't be automated.
    layout.add(std::make_unique<AudioParameterChoice>(machineId, "Machine", StringArray { "NTSC", "PAL" }, 0,
                                                      AudioParameterChoiceAttributes().withAutomatable(false)));

    // How many channels the N163 divides its time between, which changes its sound and rebuilds the chip.
    layout.add(std::make_unique<AudioParameterInt>(n163ChannelsId, "N163 Channels", 1, 8, 1, AudioParameterIntAttributes().withAutomatable(false)));

    // Each voice is a whole chip, so changing this rebuilds them. One voice is the mono instrument.
    layout.add(std::make_unique<AudioParameterInt>(voicesId, "Voices", 1, PolyEngine::maxVoices, 1, AudioParameterIntAttributes().withAutomatable(false)));

    return layout;
}

ChipmunkProcessor::ChipmunkProcessor(const juce::File& presetFolder)
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "Chipmunk", createParameters()),
      library(presetFolder),
      channelParam(parameters.getRawParameterValue(channelId.getParamID())),
      dutyParam(parameters.getRawParameterValue(dutyId.getParamID())),
      gainParam(parameters.getRawParameterValue(gainId.getParamID())),
      velocityParam(parameters.getRawParameterValue(velocityId.getParamID())),
      legatoParam(parameters.getRawParameterValue(legatoId.getParamID())),
      glideParam(parameters.getRawParameterValue(glideId.getParamID())),
      bendRangeParam(parameters.getRawParameterValue(bendRangeId.getParamID())),
      vibratoSpeedParam(parameters.getRawParameterValue(vibratoSpeedId.getParamID())),
      authenticGridParam(parameters.getRawParameterValue(authenticGridId.getParamID())),
      machineParam(parameters.getRawParameterValue(machineId.getParamID())),
      n163ChannelsParam(parameters.getRawParameterValue(n163ChannelsId.getParamID())),
      voicesParam(parameters.getRawParameterValue(voicesId.getParamID()))
{
    for (auto& position : envelopePositions)
        position.store(-1, std::memory_order_relaxed);

    keyboardState.addListener(this);

    document.setListener([this](const Instrument& instrument, const InstrumentDocument::KitPointer& kit) {
        instrumentExchange.writeBuffer() = instrument;
        instrumentExchange.publish();

        if (kit != lastPublishedKit)
        {
            lastPublishedKit = kit;

            const auto generation = ++nextKitGeneration;

            liveKits.emplace_back(generation, kit);
            kitExchange.writeBuffer() = { kit.get(), generation };
            kitExchange.publish();

            // The audio thread never goes back to an older kit, so those can be released.
            const auto adopted = adoptedKitGeneration.load(std::memory_order_acquire);

            liveKits.erase(std::remove_if(liveKits.begin(), liveKits.end(), [&](const auto& entry) { return entry.first < adopted; }), liveKits.end());
        }
    });
}

ChipmunkProcessor::~ChipmunkProcessor()
{
    stopTimer();
    keyboardState.removeListener(this);
}

void ChipmunkProcessor::pushKeyboardEvent(int note, int velocity)
{
    keyboardFifo.write(1).forEach([&](int index) { keyboardEvents[(size_t) index] = { note, velocity }; });
}

void ChipmunkProcessor::handleNoteOn(juce::MidiKeyboardState*, int, int note, float velocity)
{
    pushKeyboardEvent(note, juce::jlimit(1, 127, (int) std::lround(velocity * 127.0f)));
}

void ChipmunkProcessor::handleNoteOff(juce::MidiKeyboardState*, int, int note, float)
{
    pushKeyboardEvent(note, 0);
}

Preset ChipmunkProcessor::currentPreset() const
{
    Preset preset;
    preset.name = document.name();
    preset.channel = channelTypes[(size_t) channelParam->load()];
    preset.voice.dutyCycle = (int) dutyParam->load();
    preset.voice.velocitySensitive = velocityParam->load() > 0.5f;
    preset.voice.legato = legatoParam->load() > 0.5f;
    preset.voice.glideFrames = (int) glideParam->load();
    preset.voice.bendRangeSemitones = (int) bendRangeParam->load();
    preset.voice.vibratoSpeed = (int) vibratoSpeedParam->load();
    preset.voices = (int) voicesParam->load();
    preset.instrument = document.snapshot();
    preset.kit = document.kit();
    return preset;
}

void ChipmunkProcessor::loadPreset(const Preset& preset)
{
    const auto set = [this](const juce::ParameterID& id, float value) {
        setParameterWithGesture(*parameters.getParameter(id.getParamID()), value);
    };

    set(channelId, (float) channelTypeToIndex(preset.channel));
    set(dutyId, (float) preset.voice.dutyCycle);
    set(velocityId, preset.voice.velocitySensitive ? 1.0f : 0.0f);
    set(legatoId, preset.voice.legato ? 1.0f : 0.0f);
    set(glideId, (float) preset.voice.glideFrames);
    set(bendRangeId, (float) preset.voice.bendRangeSemitones);
    set(vibratoSpeedId, (float) preset.voice.vibratoSpeed);
    set(voicesId, (float) preset.voices);

    document.load(preset.instrument, preset.name, preset.kit);
}

void ChipmunkProcessor::setChannelIndex(int index)
{
    setParameterWithGesture(*parameters.getParameter(channelId.getParamID()), (float) index);
}

void ChipmunkProcessor::stopAudition()
{
    stopTimer();

    if (auditionNote >= 0)
        keyboardState.noteOff(1, auditionNote, 0.0f);

    auditionNote = -1;
}

void ChipmunkProcessor::audition()
{
    // A kit plays its lowest key.
    const auto channelIndex = getChannelIndex();
    constexpr int s5b = 7, fds = 8, epsmSquare = 11;

    // These sound below their notes, as they do in FamiStudio, so play them higher to compare easily.
    auto note = channelIndex == fds ? 84 : channelIndex == 1 || channelIndex == s5b || channelIndex == epsmSquare ? 72 : 60;

    constexpr int dpcm = 3, drumKit = 14;

    if (channelIndex == dpcm || channelIndex == drumKit)
    {
        if (const auto kit = document.kit())
        {
            for (int value = noteMusicalMin; value <= noteMusicalMax; ++value)
            {
                if (kit->mappings[(size_t) value].sample != nullptr || (channelIndex == drumKit && kit->mappings[(size_t) value].noise != nullptr))
                {
                    note = value + 23;
                    break;
                }
            }
        }
    }

    auditionKey(note);
}

void ChipmunkProcessor::auditionKey(int midiNote)
{
    stopAudition();

    auditionNote = midiNote;
    keyboardState.noteOn(1, auditionNote, 0.9f);
    startTimer(350);
}

void ChipmunkProcessor::timerCallback()
{
    stopAudition();
}

void ChipmunkProcessor::prepareToPlay(double sampleRate, int)
{
    const auto pal = machineParam->load() > 0.5f;

    for (auto& word : midiHeldNotes)
        word.store(0, std::memory_order_relaxed);

    engine.prepare(sampleRate, pal, NesEngine::chipsNeededFor(selectedChannelType()), (int) n163ChannelsParam->load(), (int) voicesParam->load());
    enginePal.store(pal, std::memory_order_relaxed);
    preparedSampleRate = sampleRate;
    setLatencySamples(NesEngine::outputDelaySamples);

    gain.reset(sampleRate, 0.02);
    gain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(gainParam->load() + channelTrimDb(getChannelIndex())));
}

ChannelType ChipmunkProcessor::selectedChannelType() const
{
    return channelTypes[(size_t) channelParam->load()];
}

bool ChipmunkProcessor::needsChipRebuild() const
{
    return (machineParam->load() > 0.5f) != isEnginePal()
        || (int) voicesParam->load() != engine.getNumVoices()
        || ! engine.canPlay(selectedChannelType())
        || (selectedChannelType() == ChannelType::n163Wave1 && (int) n163ChannelsParam->load() != engine.getNumN163Channels());
}

void ChipmunkProcessor::applyMachineChange()
{
    const auto pal = machineParam->load() > 0.5f;

    if (! needsChipRebuild() || preparedSampleRate <= 0.0)
        return;

    // Hosts skip the processor while it's suspended, so the chip can be rebuilt safely.
    suspendProcessing(true);
    engine.prepare(preparedSampleRate, pal, NesEngine::chipsNeededFor(selectedChannelType()), (int) n163ChannelsParam->load(), (int) voicesParam->load());
    enginePal.store(pal, std::memory_order_relaxed);
    suspendProcessing(false);
}

void ChipmunkProcessor::reset()
{
    engine.allNotesOff();
}

bool ChipmunkProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto output = layouts.getMainOutputChannelSet();
    return output == juce::AudioChannelSet::mono() || output == juce::AudioChannelSet::stereo();
}

void ChipmunkProcessor::applyParameters()
{
    // A channel on a chip that's off keeps the old one playing until the rebuild, which is moments away.
    if (engine.canPlay(selectedChannelType()))
        engine.setChannelType(selectedChannelType());

    engine.setAuthenticGrid(authenticGridParam->load() > 0.5f);

    VoiceController::Settings settings;
    settings.dutyCycle = (int) dutyParam->load();
    settings.velocitySensitive = velocityParam->load() > 0.5f;
    settings.legato = legatoParam->load() > 0.5f;
    settings.glideFrames = (int) glideParam->load();
    settings.bendRangeSemitones = (int) bendRangeParam->load();
    settings.vibratoSpeed = (int) vibratoSpeedParam->load();
    engine.setVoiceSettings(settings);

    gain.setTargetValue(juce::Decibels::decibelsToGain(gainParam->load() + channelTrimDb(getChannelIndex())));
}

std::bitset<128> ChipmunkProcessor::takeMidiNotes()
{
    std::bitset<128> notes;

    for (size_t word = 0; word < midiHeldNotes.size(); ++word)
    {
        const auto bits = midiHeldNotes[word].load(std::memory_order_relaxed) | midiStruckNotes[word].exchange(0, std::memory_order_relaxed);

        for (int bit = 0; bit < 64; ++bit)
            if ((bits >> bit) & 1)
                notes.set(word * 64 + (size_t) bit);
    }

    return notes;
}

void ChipmunkProcessor::handleMidi(const juce::MidiMessage& message)
{
    const auto noteNumber = message.getNoteNumber() & 127;
    const auto noteWord = (size_t) (noteNumber >> 6);
    const auto noteBit = (uint64_t) 1 << (noteNumber & 63);

    if (message.isNoteOn())
    {
        midiHeldNotes[noteWord].fetch_or(noteBit, std::memory_order_relaxed);
        midiStruckNotes[noteWord].fetch_or(noteBit, std::memory_order_relaxed);
        engine.noteOn(message.getNoteNumber(), message.getVelocity());
    }
    else if (message.isNoteOff())
    {
        midiHeldNotes[noteWord].fetch_and(~noteBit, std::memory_order_relaxed);
        engine.noteOff(message.getNoteNumber());
    }
    else if (message.isAllNotesOff() || message.isAllSoundOff())
    {
        for (auto& word : midiHeldNotes)
            word.store(0, std::memory_order_relaxed);

        engine.allNotesOff();
    }
    else if (message.isPitchWheel())
        engine.setPitchBend((message.getPitchWheelValue() - 8192) / 8192.0);
    else if (message.isController())
        engine.setController(message.getControllerNumber(), message.getControllerValue());
}

void ChipmunkProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    if (needsChipRebuild())
        triggerAsyncUpdate();

    applyParameters();

    // A held release tail mustn't drone on once the host stops.
    const auto hostPosition = getPlayHead() != nullptr ? getPlayHead()->getPosition() : juce::Optional<juce::AudioPlayHead::PositionInfo> {};
    engine.setTransportPlaying(hostPosition.hasValue() && hostPosition->getIsPlaying());

    if (instrumentExchange.fetch())
        engine.setInstrument(instrumentExchange.readBuffer());

    if (kitExchange.fetch())
    {
        const auto& published = kitExchange.readBuffer();

        engine.setDpcmKit(published.kit);
        adoptedKitGeneration.store(published.generation, std::memory_order_release);
    }

    auto* output = buffer.getWritePointer(0);
    auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
    const auto numSamples = buffer.getNumSamples();
    int position = 0;

    // Notes from the on-screen keyboard start with the block.
    keyboardFifo.read(keyboardFifo.getNumReady()).forEach([this](int index) {
        const auto& event = keyboardEvents[(size_t) index];

        if (event.velocity > 0)
            engine.noteOn(event.note, event.velocity);
        else
            engine.noteOff(event.note);
    });

    for (const auto metadata : midi)
    {
        const auto eventPosition = juce::jlimit(position, numSamples, metadata.samplePosition);

        engine.render(output + position, right != nullptr ? right + position : nullptr, eventPosition - position);
        position = eventPosition;

        handleMidi(metadata.getMessage());
    }

    engine.render(output + position, right != nullptr ? right + position : nullptr, numSamples - position);

    // The right side has to follow the same fade, so it takes the gain sample by sample with the left.
    if (right != nullptr && engine.isStereo())
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const auto value = gain.getNextValue();
            output[i] *= value;
            right[i] *= value;
        }
    }
    else
    {
        gain.applyGain(output, numSamples);
    }

    scope.write(output, numSamples);

    soundingNote.store(engine.getSoundingNote(), std::memory_order_relaxed);
    soundingVolume.store(engine.getSoundingVolume(), std::memory_order_relaxed);

    for (int type = 0; type < numEnvelopeTypes; ++type)
        envelopePositions[(size_t) type].store(engine.getEnvelopePosition(type), std::memory_order_relaxed);

    for (int channel = engine.isStereo() ? 2 : 1; channel < buffer.getNumChannels(); ++channel)
        buffer.copyFrom(channel, 0, buffer, 0, 0, numSamples);
}

juce::AudioProcessorEditor* ChipmunkProcessor::createEditor()
{
    return new ChipmunkEditor(*this);
}

void ChipmunkProcessor::getStateInformation(juce::MemoryBlock& destination)
{
    auto state = parameters.copyState();

    state.removeChild(state.getChildWithName(instrumentStateType), nullptr);

    juce::ValueTree instrumentState(instrumentStateType);
    instrumentState.setProperty(instrumentTextProperty, juce::String(formatPreset(currentPreset())), nullptr);
    state.appendChild(instrumentState, nullptr);

    if (const auto xml = state.createXml())
        copyXmlToBinary(*xml, destination);
}

void ChipmunkProcessor::setStateInformation(const void* data, int size)
{
    const auto xml = getXmlFromBinary(data, size);

    if (xml == nullptr)
        return;

    // Sets saved before the rename use the old name.
    if (xml->hasTagName("VaroNES"))
        xml->setTagName(parameters.state.getType());

    if (! xml->hasTagName(parameters.state.getType()))
        return;

    const auto state = juce::ValueTree::fromXml(*xml);
    parameters.replaceState(state);

    // Sets saved before instruments could be edited have no instrument, and keep the default.
    const auto instrumentState = state.getChildWithName(instrumentStateType);

    if (instrumentState.isValid())
    {
        Preset preset;
        std::string error;

        if (parsePreset(instrumentState[instrumentTextProperty].toString().toStdString(), preset, error))
            document.restore(preset.instrument, preset.name, preset.kit);
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ChipmunkProcessor();
}
