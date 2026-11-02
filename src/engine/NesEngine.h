#pragma once

#include "engine/ChannelEngine.h"
#include "engine/VoiceController.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

class Simple_Apu;

// One emulated chip playing one channel. Note and controller methods take effect at the
// current render position, so the caller splits its block at MIDI events for sample-accurate timing.
class NesEngine final : private RegisterSink
{
public:
    // Delay of the emulator's band-limited synthesis, the same at every sample rate.
    static constexpr int outputDelaySamples = 8;

    NesEngine();
    ~NesEngine() override;

    // Chips that are only switched on when needed, as they're the costly ones to emulate.
    static constexpr int chipFds = 1, chipN163 = 2, chipVrc7 = 4, chipEpsm = 8;
    static int chipsNeededFor(ChannelType type);

    // Allocates, so call this outside the audio thread. "numN163Channels" sets the N163's time
    // slicing, which is part of its sound: with more channels it runs slower and whines more.
    void prepare(double sampleRate, bool pal = false, int extraChips = 0, int numN163Channels = 1);

    bool canPlay(ChannelType type) const { return (chipsNeededFor(type) & ~enabledChips) == 0; }
    int getEnabledChips() const { return enabledChips; }
    int getNumN163Channels() const { return numN163; }

    // True while the EPSM is on, which is the one chip that outputs stereo.
    bool isStereo() const { return (enabledChips & chipEpsm) != 0; }

    // The type must be one that canPlay() allows.
    void setChannelType(ChannelType type);
    void setVoiceSettings(const VoiceController::Settings& settings);

    // When on, the frame clock runs freely as on real hardware and notes wait for the
    // next frame. When off, notes start immediately and restart the frame clock.
    void setAuthenticGrid(bool shouldUseGrid) { authenticGrid = shouldUseGrid; }

    // Takes effect on the sounding note, so edits can be heard while playing.
    void setInstrument(const Instrument& newInstrument);

    // The kit must stay alive until the next call. A playing sample continues if the new kit
    // still has it, and otherwise stops, so nothing here refers to the old kit afterwards.
    void setDpcmKit(const DpcmKit* kit);

    void noteOn(int midiNote, int velocity);
    void noteOff(int midiNote);
    void allNotesOff();
    void setPitchBend(double normalized);
    void setModWheel(int value);
    void setController(int controller, int value);
    void setTransportPlaying(bool playing);

    // Fills both sides when the engine is stereo. With no right side, they're mixed into the left.
    void render(float* left, float* right, int numSamples);
    void render(float* output, int numSamples) { render(output, nullptr, numSamples); }

    int64_t getFrameCount() const { return frameCount; }

    // Envelope step last played, or -1 if the channel is silent.
    int getEnvelopePosition(int type) const { return channel->isSounding() ? channel->playedEnvelopeStep(type) : -1; }

    // The note playing as MIDI, or -1 if the channel is silent, and its volume from 0 to 15.
    int getSoundingNote() const
    {
        return channel->isSounding() ? channel->currentNote().value + midiToNoteValueOffset
             : sampleChannel.has_value() && sampleChannel->isSounding() ? sampleChannel->currentNote().value + midiToNoteValueOffset : -1;
    }

    int getSoundingVolume() const
    {
        return channel->isSounding() ? channel->currentVolume() : sampleChannel.has_value() && sampleChannel->isSounding() ? sampleChannel->currentVolume() : 0;
    }

private:
    void writeRegister(int reg, int data) override;
    void skipCycles(int cycles) override;
    void selectDpcmSample(const DpcmSampleData* sample) override;
    static int readDmcMemory(void* engine, unsigned address);

    void createChannel(ChannelType type);
    void discardAudio(int numSamples);
    void restDac();
    void runFrame();
    void renderChunk(float* left, float* right, int count);
    void noteEventHappened();
    VoiceController& voiceForKey(int midiNote);

    static constexpr int maxChunkSamples = 256;
    static constexpr int midiToNoteValueOffset = 23;

    std::unique_ptr<Simple_Apu> apu;
    Instrument instrument;
    const DpcmKit* dpcmKit = nullptr;
    const DpcmSampleData* dmcSample = nullptr;
    std::optional<ChannelEngine> channel;
    std::optional<VoiceController> voice;
    ChannelType currentType = ChannelType::pulse1;
    int dacRestLevel = 0;

    // A Drum Kit's DPCM side. The main channel and voice above are its noise.
    std::optional<ChannelEngine> sampleChannel;
    std::optional<VoiceController> sampleVoice;
    VoiceController::Settings voiceSettings;
    bool isPal = false;
    int enabledChips = 0;
    int numN163 = 1;
    bool authenticGrid = false;

    int64_t samplePosition = 0;
    int64_t emulatedCycles = 0;
    int64_t frameCount = 0;
    double samplesPerFrame = 0.0;
    double nextFramePosition = 0.0;
    bool frameIsDue = false;

    // Register writes advance the emulator, so a chunk can yield a few samples
    // more than requested. The extras wait here for the next render call.
    std::array<int16_t, 4096> pending {};
    int numPending = 0;
};
