#pragma once

#include "engine/InstrumentDocument.h"
#include "engine/NesEngine.h"
#include "engine/PolyEngine.h"
#include "engine/Preset.h"
#include "engine/TripleBuffer.h"
#include "plugin/PresetLibrary.h"
#include "plugin/ScopeBuffer.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

class ChipmunkProcessor final : public juce::AudioProcessor,
                               private juce::Timer,
                               private juce::AsyncUpdater,
                               private juce::MidiKeyboardState::Listener
{
public:
    explicit ChipmunkProcessor(const juce::File& presetFolder = PresetLibrary::defaultFolder());
    ~ChipmunkProcessor() override;

    InstrumentDocument& getDocument() { return document; }
    PresetLibrary& getLibrary() { return library; }
    juce::AudioProcessorValueTreeState& getParameters() { return parameters; }
    juce::MidiKeyboardState& getKeyboardState() { return keyboardState; }

    Preset currentPreset() const;

    // Applies a whole sound: envelopes, channel and playing settings.
    void loadPreset(const Preset&);

    // Plays a short note so a sound can be judged while browsing.
    void audition();
    void auditionKey(int midiNote);

    void setChannelIndex(int index);

    int getChannelIndex() const { return (int) channelParam->load(); }

    // How much of the height a Drum Kit's key list takes. It saves with the Set.
    float getKitListShare() const { return (float) parameters.state.getProperty("kitListShare", 0.4f); }
    void setKitListShare(float share) { parameters.state.setProperty("kitListShare", share, nullptr); }
    static juce::StringArray channelNames();
    static float channelTrimDb(int channelIndex);

    // Envelope step last played, or -1 when silent. Safe to read from any thread.
    int getEnvelopePosition(int type) const { return envelopePositions[(size_t) type].load(std::memory_order_relaxed); }

    // What's playing, for display. Safe to read from any thread.
    const ScopeBuffer& getScope() const { return scope; }
    int getSoundingNote() const { return soundingNote.load(std::memory_order_relaxed); }
    int getSoundingVolume() const { return soundingVolume.load(std::memory_order_relaxed); }

    // Rebuilds the chip if the NTSC/PAL setting has changed. Call from the message thread.
    // Rebuilding allocates, so the audio thread only asks for this to happen.
    void applyMachineChange();
    bool isEnginePal() const { return enginePal.load(std::memory_order_relaxed); }

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void reset() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameters();

    void applyParameters();
    void handleMidi(const juce::MidiMessage&);
    void timerCallback() override;
    void handleAsyncUpdate() override { applyMachineChange(); }
    ChannelType selectedChannelType() const;
    bool needsChipRebuild() const;
    void stopAudition();
    void handleNoteOn(juce::MidiKeyboardState*, int channel, int note, float velocity) override;
    void handleNoteOff(juce::MidiKeyboardState*, int channel, int note, float velocity) override;
    void pushKeyboardEvent(int note, int velocity);

    PolyEngine engine;
    juce::AudioProcessorValueTreeState parameters;
    InstrumentDocument document;
    TripleBuffer<Instrument> instrumentExchange;

    // Kits are too big to copy across, so the audio thread gets a pointer. Each published kit
    // stays alive here until the audio thread says it has moved on to a newer one.
    struct PublishedKit { const DpcmKit* kit; int generation; };
    TripleBuffer<PublishedKit> kitExchange;
    std::atomic<int> adoptedKitGeneration { 0 };
    int nextKitGeneration = 0;
    InstrumentDocument::KitPointer lastPublishedKit;
    std::vector<std::pair<int, InstrumentDocument::KitPointer>> liveKits;
    PresetLibrary library;
    juce::MidiKeyboardState keyboardState;

    // Carries on-screen keyboard notes to the audio thread without locks.
    struct KeyboardEvent { int note, velocity; };
    std::array<KeyboardEvent, 128> keyboardEvents {};
    juce::AbstractFifo keyboardFifo { 128 };

    std::array<std::atomic<int>, numEnvelopeTypes> envelopePositions;
    ScopeBuffer scope;
    std::atomic<int> soundingNote { -1 };
    std::atomic<int> soundingVolume { 0 };
    std::atomic<bool> enginePal { false };
    double preparedSampleRate = 0.0;
    int auditionNote = -1;

    std::atomic<float>* channelParam;
    std::atomic<float>* dutyParam;
    std::atomic<float>* gainParam;
    std::atomic<float>* velocityParam;
    std::atomic<float>* legatoParam;
    std::atomic<float>* glideParam;
    std::atomic<float>* bendRangeParam;
    std::atomic<float>* vibratoSpeedParam;
    std::atomic<float>* authenticGridParam;
    std::atomic<float>* machineParam;
    std::atomic<float>* n163ChannelsParam;
    std::atomic<float>* voicesParam;

    juce::SmoothedValue<float> gain;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChipmunkProcessor)
};
