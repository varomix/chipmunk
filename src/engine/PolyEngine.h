#pragma once

#include "engine/NesEngine.h"

#include <array>
#include <cstdint>
#include <memory>

// Plays chords by giving each note its own chip, as if each were on its own track. With one
// voice, everything goes straight to that chip, which keeps its last-note priority and legato.
class PolyEngine
{
public:
    static constexpr int maxVoices = 8;

    PolyEngine();

    // Allocates, so call this outside the audio thread.
    void prepare(double sampleRate, bool pal, int extraChips, int numN163Channels, int numVoices);

    int getNumVoices() const { return numVoices; }
    bool canPlay(ChannelType type) const { return voices[0]->canPlay(type); }
    int getNumN163Channels() const { return voices[0]->getNumN163Channels(); }
    bool isStereo() const { return voices[0]->isStereo(); }

    void setChannelType(ChannelType type);
    void setVoiceSettings(const VoiceController::Settings& settings);
    void setAuthenticGrid(bool shouldUseGrid);
    void setInstrument(const Instrument& newInstrument);
    void setDpcmKit(const DpcmKit* kit);

    void noteOn(int midiNote, int velocity);
    void noteOff(int midiNote);
    void allNotesOff();
    void setPitchBend(double normalized);
    void setController(int controller, int value);
    void setTransportPlaying(bool playing);

    void render(float* left, float* right, int numSamples);

    // These show the newest voice that's still sounding.
    int getEnvelopePosition(int type) const { return shownVoice().getEnvelopePosition(type); }
    int getSoundingNote() const { return shownVoice().getSoundingNote(); }
    int getSoundingVolume() const { return shownVoice().getSoundingVolume(); }

private:
    struct VoiceState
    {
        int heldKey = -1;      // The key holding this voice, or -1 once released.
        int lastKey = -1;      // The key it played last, which may still be ringing.
        int64_t startedAt = 0; // When it took its key, to find the oldest.
        int64_t releasedAt = 0;
    };

    int chooseVoice(int midiNote) const;
    const NesEngine& shownVoice() const;

    static constexpr int scratchSamples = 256;

    std::array<std::unique_ptr<NesEngine>, maxVoices> voices;
    std::array<VoiceState, maxVoices> states {};
    int numVoices = 1;
    int64_t eventCount = 0;

    // New voices have to catch up with these.
    VoiceController::Settings voiceSettings;
    Instrument instrument;
    const DpcmKit* dpcmKit = nullptr;
    ChannelType channelType = ChannelType::pulse1;
    bool authenticGrid = false;
    bool transportPlaying = false;

    std::array<float, scratchSamples> scratchLeft {}, scratchRight {};
};
