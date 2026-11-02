#include "engine/PolyEngine.h"

#include <algorithm>

PolyEngine::PolyEngine()
{
    voices[0] = std::make_unique<NesEngine>();
}

void PolyEngine::prepare(double sampleRate, bool pal, int extraChips, int numN163Channels, int newNumVoices)
{
    numVoices = std::clamp(newNumVoices, 1, maxVoices);
    states = {};
    eventCount = 0;
    channelType = ChannelType::pulse1;

    for (int i = 0; i < maxVoices; ++i)
    {
        auto& voice = voices[(size_t) i];

        if (i >= numVoices)
        {
            voice.reset();
            continue;
        }

        if (voice == nullptr)
            voice = std::make_unique<NesEngine>();

        voice->prepare(sampleRate, pal, extraChips, numN163Channels);
        voice->setVoiceSettings(voiceSettings);
        voice->setAuthenticGrid(authenticGrid);
        voice->setInstrument(instrument);
        voice->setDpcmKit(dpcmKit);
        voice->setTransportPlaying(transportPlaying);
    }
}

void PolyEngine::setChannelType(ChannelType type)
{
    if (type == channelType)
        return;

    channelType = type;
    states = {};

    for (int i = 0; i < numVoices; ++i)
        voices[(size_t) i]->setChannelType(type);
}

void PolyEngine::setVoiceSettings(const VoiceController::Settings& settings)
{
    voiceSettings = settings;

    for (int i = 0; i < numVoices; ++i)
        voices[(size_t) i]->setVoiceSettings(settings);
}

void PolyEngine::setAuthenticGrid(bool shouldUseGrid)
{
    authenticGrid = shouldUseGrid;

    for (int i = 0; i < numVoices; ++i)
        voices[(size_t) i]->setAuthenticGrid(shouldUseGrid);
}

void PolyEngine::setInstrument(const Instrument& newInstrument)
{
    instrument = newInstrument;

    for (int i = 0; i < numVoices; ++i)
        voices[(size_t) i]->setInstrument(newInstrument);
}

void PolyEngine::setDpcmKit(const DpcmKit* kit)
{
    dpcmKit = kit;

    for (int i = 0; i < numVoices; ++i)
        voices[(size_t) i]->setDpcmKit(kit);
}

int PolyEngine::chooseVoice(int midiNote) const
{
    int silent = -1, released = -1, held = 0;

    for (int i = 0; i < numVoices; ++i)
    {
        const auto& state = states[(size_t) i];

        // The same key takes its own voice back, so it can't double up with its own tail.
        if (state.lastKey == midiNote)
            return i;

        if (state.heldKey >= 0)
        {
            if (state.startedAt < states[(size_t) held].startedAt)
                held = i;
        }
        else if (voices[(size_t) i]->getSoundingNote() < 0)
        {
            if (silent < 0 || state.releasedAt < states[(size_t) silent].releasedAt)
                silent = i;
        }
        else if (released < 0 || state.releasedAt < states[(size_t) released].releasedAt)
        {
            released = i;
        }
    }

    // Keep what's still heard for as long as possible: silent voices first, then tails.
    return silent >= 0 ? silent : released >= 0 ? released : held;
}

void PolyEngine::noteOn(int midiNote, int velocity)
{
    const auto index = numVoices == 1 ? 0 : chooseVoice(midiNote);
    auto& state = states[(size_t) index];
    auto& voice = *voices[(size_t) index];

    // Letting go first keeps a stolen note from coming back when the new one ends.
    if (numVoices > 1 && state.heldKey >= 0)
        voice.noteOff(state.heldKey);

    voice.noteOn(midiNote, velocity);

    state.heldKey = state.lastKey = midiNote;
    state.startedAt = ++eventCount;
}

void PolyEngine::noteOff(int midiNote)
{
    if (numVoices == 1)
    {
        voices[0]->noteOff(midiNote);
        return;
    }

    // A note whose voice was stolen has nothing left to release.
    for (int i = 0; i < numVoices; ++i)
    {
        auto& state = states[(size_t) i];

        if (state.heldKey == midiNote)
        {
            voices[(size_t) i]->noteOff(midiNote);
            state.heldKey = -1;
            state.releasedAt = ++eventCount;
        }
    }
}

void PolyEngine::allNotesOff()
{
    states = {};

    for (int i = 0; i < numVoices; ++i)
        voices[(size_t) i]->allNotesOff();
}

void PolyEngine::setPitchBend(double normalized)
{
    for (int i = 0; i < numVoices; ++i)
        voices[(size_t) i]->setPitchBend(normalized);
}

void PolyEngine::setController(int controller, int value)
{
    for (int i = 0; i < numVoices; ++i)
        voices[(size_t) i]->setController(controller, value);
}

void PolyEngine::setTransportPlaying(bool playing)
{
    transportPlaying = playing;

    for (int i = 0; i < numVoices; ++i)
        voices[(size_t) i]->setTransportPlaying(playing);
}

void PolyEngine::render(float* left, float* right, int numSamples)
{
    voices[0]->render(left, right, numSamples);

    if (numVoices == 1)
        return;

    for (int position = 0; position < numSamples; position += scratchSamples)
    {
        const auto count = std::min(numSamples - position, scratchSamples);

        for (int i = 1; i < numVoices; ++i)
        {
            voices[(size_t) i]->render(scratchLeft.data(), right != nullptr ? scratchRight.data() : nullptr, count);

            for (int sample = 0; sample < count; ++sample)
                left[position + sample] += scratchLeft[(size_t) sample];

            if (right != nullptr && isStereo())
                for (int sample = 0; sample < count; ++sample)
                    right[position + sample] += scratchRight[(size_t) sample];
        }
    }
}

const NesEngine& PolyEngine::shownVoice() const
{
    auto shown = 0;
    int64_t newest = -1;

    for (int i = 0; i < numVoices; ++i)
    {
        if (voices[(size_t) i]->getSoundingNote() >= 0 && states[(size_t) i].startedAt > newest)
        {
            newest = states[(size_t) i].startedAt;
            shown = i;
        }
    }

    return *voices[(size_t) shown];
}
