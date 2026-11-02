#pragma once

#include "engine/Dpcm.h"
#include "engine/Instrument.h"
#include "engine/NoteTables.h"

enum class ChannelType
{
    pulse1,
    pulse2,
    triangle,
    noise,
    dpcm,
    vrc6Pulse1,
    vrc6Pulse2,
    vrc6Saw,
    mmc5Pulse1,
    mmc5Pulse2,
    s5bSquare1,
    s5bSquare2,
    s5bSquare3,
    fdsWave,
    n163Wave1, n163Wave2, n163Wave3, n163Wave4, n163Wave5, n163Wave6, n163Wave7, n163Wave8,
    vrc7Fm1, vrc7Fm2, vrc7Fm3, vrc7Fm4, vrc7Fm5, vrc7Fm6,
    epsmSquare1, epsmSquare2, epsmSquare3,
    epsmFm1, epsmFm2, epsmFm3, epsmFm4, epsmFm5, epsmFm6,
    epsmRhythm1, epsmRhythm2, epsmRhythm3, epsmRhythm4, epsmRhythm5, epsmRhythm6,

    // Noise and DPCM together, with keys choosing what plays. NesEngine runs it as two channels.
    drumKit
};

// Registers S5B channels share, which the last channel writes. With no group a channel acts
// alone, which is how the plugin uses it. FamiStudio's songs run all three as a group.
struct S5bSharedState
{
    int toneReg = 0x38;
    int envPeriod = 0;
    int envShape = 0;
    bool envPeriodEffect = false;
    bool envAutoPitch = false;
    int envAutoOctave = 0;
    bool envReset = false;
    int noiseFreq = 0;
};

class RegisterSink
{
public:
    virtual ~RegisterSink() = default;
    virtual void writeRegister(int reg, int data) = 0;
    virtual void skipCycles(int cycles) = 0;

    // The DMC is about to play this mapping's sample. Memory reads come from it until the next call.
    virtual void selectDpcmSample(const DpcmSampleData*) {}
};

// Fields of FamiStudio's Note that reach a channel while it plays.
struct ChannelNote
{
    int value = noteInvalid;
    bool hasAttack = true;
    const Instrument* instrument = nullptr;
    const Envelope* arpeggio = nullptr;
    int slideTarget = 0;

    bool hasFinePitch = false;
    int finePitch = 0;
    bool hasVibrato = false;
    int vibratoSpeed = 0;
    int vibratoDepth = 0;
    bool hasDutyCycle = false;
    int dutyCycle = 0;
    bool hasVolume = false;
    int volume = 0;
    bool hasVolumeSlide = false;
    int volumeSlideTarget = 0;
    bool hasPhaseReset = false;
    bool hasDeltaCounter = false;
    int deltaCounter = 0;
    bool hasEnvelopePeriod = false;
    int envelopePeriod = 0;
    bool hasFdsModSpeed = false;
    int fdsModSpeed = 0;
    bool hasFdsModDepth = false;
    int fdsModDepth = 0;

    bool isValid() const { return value != noteInvalid; }
    bool isStop() const { return value == noteStop; }
    bool isRelease() const { return value == noteRelease; }
    bool isMusical() const { return isValid() && ! isStop() && ! isRelease(); }
};

// Port of FamiStudio's ChannelState and its 2A03 subclasses, without the song sequencing.
// Call playNote() as often as needed between frames and update() once per frame.
// Allocation-free, so all of it is safe on the audio thread.
class ChannelEngine
{
public:
    // The N163 divides its time between its channels, so its pitch depends on how many are in use.
    ChannelEngine(RegisterSink& sink, ChannelType type, bool pal = false, int numN163Channels = 1);

    void playNote(ChannelNote newNote, int noteSlidePitch = 0, int noteSlideStep = 0, int noteVolumeSlideStep = 0);
    void update();
    void postUpdate();

    // Slide parameters for playNote(), ported from Channel.ComputeSlideNoteParams.
    void computeSlide(int fromValue, int toValue, int numFrames, int& slidePitch, int& slideStep) const;

    void setVolumeSlideStep(int step) { volumeSlideStep = step; }

    // Picks up edited settings on the current note, which keeps using the same instrument object.
    void instrumentEdited() { loadInstrument(note.instrument); }

    // Which samples a DPCM channel plays. The kit must outlive its use here.
    void setDpcmKit(const DpcmKit* kit) { dpcmKit = kit; }
    bool isDpcmKeyLooping(int noteValue) const;

    // Joins S5B channels so they share registers. "isFirst" and "isLast" are the group's ends.
    void setS5bGroup(S5bSharedState* shared, bool isFirst, bool isLast);

    // VRC7 channels share one custom patch, so loading one makes the others load theirs again.
    // The list holds "count" channels, and can include this one. With none, a channel acts alone.
    void setVrc7Group(ChannelEngine* const* channels, int count);

    // Lets one EPSM rhythm channel play all six drums, chosen by the note.
    void setRhythmFollowsNote(bool shouldFollow) { rhythmFollowsNote = shouldFollow; }

    // Extra period offset for pitch bend, which FamiStudio doesn't have.
    void setBendPeriodOffset(int offset) { bendPeriodOffset = offset; }

    // Period offset that bends a note by a number of semitones.
    int periodOffsetForBend(int noteValue, double semitones) const;

    ChannelType getType() const { return type; }
    const ChannelNote& currentNote() const { return note; }

    bool isSounding() const { return note.isMusical(); }
    bool isRhythm() const { return (int) type >= (int) ChannelType::epsmRhythm1 && (int) type <= (int) ChannelType::epsmRhythm6; }
    bool ringsOutOnRelease() const;
    int currentVolume() const { return getVolume(); }
    int playedEnvelopeStep(int type) const { return playedSteps[(size_t) type]; }

    // True once a released note's volume envelope has nothing left to play.
    bool isVolumeEnvelopeFinished() const;

private:
    void updateEnvelopes();
    void updateSlide();
    void updateVolumeSlide();
    void updateApu();
    void updatePulse();
    void updateTriangle();
    void updateNoise();
    void updateDpcm();
    void updateVrc6Pulse();
    void updateVrc6Saw();
    void updateMmc5Pulse();
    void updateS5b();
    void updateFds();
    void updateN163();
    void updateVrc7();
    void updateEpsmSquare();
    void updateEpsmFm();
    void updateEpsmRhythm();
    void updateWaveRepeat();
    void loadFdsInstrument(const Instrument&);
    void loadN163Instrument(const Instrument&);
    void loadVrc7Instrument(const Instrument&);
    void conditionalLoadFdsWave();
    void conditionalLoadN163Wave();
    void resetFdsModulation();
    void writeN163(int reg, int data, int skip = 0);
    void writeVrc7(int reg, int data);
    void writeEpsm(int reg, int data, bool highBank = false, int extraCycles = 0);
    void writeWithSkip(int reg, int data, int skip);
    void loadInstrument(const Instrument*);

    int getPeriod() const;
    int getVolume() const;
    int slideOffset() const;
    void buildVibratoEnvelope(int speed, int depth);

    RegisterSink& sink;
    ChannelType type;
    NoteTable noteTable;
    int slideShift;
    int pitchShift;
    int channelGroupIndex;
    int numN163Channels;

    ChannelNote note;
    int dutyCycle = 0;
    bool pitchEnvelopeOverride = false;
    bool arpeggioEnvelopeOverride = false;
    bool resetInstrumentOnNextAttack = false;
    std::array<const Envelope*, numEnvelopeTypes> envelopes {};
    std::array<int, numEnvelopeTypes> envelopeIdx {};
    std::array<int, numEnvelopeTypes> envelopeValues {};
    std::array<int, numEnvelopeTypes> playedSteps {};
    Envelope vibratoEnvelope;
    bool resetPhase = false;
    int slideStep = 0;
    int slidePitch = 0;
    int volume = 15 << 4;
    int volumeSlideStep = 0;
    int volumeSlideTarget = 0;
    int prevPeriodHi = 0x80;
    int bendPeriodOffset = 0;
    bool noteTriggered = false;
    bool noteReleased = false;
    bool forceInstrumentReload = false;
    const DpcmKit* dpcmKit = nullptr;
    int maximumPeriod;
    int periodHiForReset = 0;

    S5bSharedState ownS5bState;
    S5bSharedState* s5b = &ownS5bState;
    bool s5bIsFirst = true, s5bIsLast = true;
    int instEnvShape = 0;
    bool instAutoPitch = false;
    int instAutoOctave = 0;
    int instEnvPeriod = 0;

    // Waveform animation, for FDS and N163. It counts in frames left on the current waveform.
    const Instrument* waveInstrument = nullptr;
    int waveRepeatIndex = 0;
    int waveRepeatCounter = 0;
    int loadedWaveIndex = -1;

    int fdsModDelayCounter = 0;
    int fdsModDepth = 0;
    int fdsModSpeed = 0;
    int fdsMasterVolume = 0;
    std::array<int8_t, 32> fdsPreviousModTable {};

    int n163WaveLengthRegister = 0;
    int n163WavePosition = 0;

    int vrc7InstrumentBits = 0;
    int vrc7PreviousPeriodHi = 0;
    bool vrc7OverrideStop = false;
    ChannelEngine* const* vrc7Group = nullptr;
    int vrc7GroupSize = 0;

    const uint8_t* epsmPendingPatch = nullptr;
    int epsmStereoFlags = 0;
    int epsmAlgorithm = 0;
    int epsmLastVolume = 0;
    bool epsmStopped = false;
    bool rhythmFollowsNote = false;
    std::array<int, 4> epsmOperatorVolumes {};
};
