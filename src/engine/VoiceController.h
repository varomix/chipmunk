#pragma once

#include "engine/ChannelEngine.h"

#include <array>

// MIDI controllers for FamiStudio's note effects. They can be drawn in a clip's envelopes.
namespace MidiEffect
{
constexpr int vibratoDepth = 1;  // Mod wheel. 0 to 127 covers depths 0 to 15.
constexpr int finePitch = 3;     // 64 is none. Each step is one period step, up to +/-64.
constexpr int slideFrames = 5;   // Portamento time. Overlapping notes slide over this many frames, if not 0.
constexpr int volume = 11;       // Expression. 0 to 127 covers volumes 0 to 15, taking effect immediately.
constexpr int dutyCycle = 71;    // The value is the duty setting, from 0.
constexpr int phaseReset = 73;   // Resets the waveform's phase when it goes above 0.
constexpr int vibratoSpeed = 76; // The value is the speed, from 1 to 12.
constexpr int fdsModSpeed = 77;  // 0 to 127 covers 0 to 4064, in steps of 32.
constexpr int fdsModDepth = 78;  // The value is the depth, from 0 to 63.
constexpr int stopSample = 85;   // On a Drum Kit, stops DPCM samples when it goes above 0. Stop Note then only stops noise.
constexpr int stopNote = 86;     // Stops the sound when it goes above 0, for sounds that outlast their notes.
constexpr int holdRelease = 87;  // Above 0, a released note holds its last volume until a new note or a stop, as in FamiStudio, while the host is playing.
} // namespace MidiEffect

// Turns MIDI into notes for a single monophonic channel: last-note priority,
// legato, glide, pitch bend, mod wheel vibrato and velocity.
class VoiceController
{
public:
    struct Settings
    {
        bool velocitySensitive = true;
        bool legato = true;
        int glideFrames = 0;
        int bendRangeSemitones = 2;
        int vibratoSpeed = 6;
        int dutyCycle = 2;
    };

    VoiceController(ChannelEngine& channel, const Instrument& instrument);

    void setSettings(const Settings& newSettings);

    void noteOn(int midiNote, int velocity);
    void noteOff(int midiNote);
    void allNotesOff();
    void setPitchBend(double normalized); // -1 to 1
    void setModWheel(int value);          // 0 to 127
    void setController(int controller, int value);
    void setTransportPlaying(bool playing) { transportPlaying = playing; }

    // On a Drum Kit's noise channel, keys choose a sound and noise pitch from the kit, and other
    // keys do nothing. The kit must stay alive until the next call, which moves off the old one.
    void setKeyMap(const DpcmKit* kit, bool useKeyMap);

    // Call once per frame, before the channel's update().
    void beginFrame();

private:
    bool removeHeldNote(int midiNote);
    void applyVibrato();
    void applyBend();
    void playHeldNote(int midiNote, bool retrigger, int velocity);
    void playDpcmKey(int value);

    static constexpr int maxHeldNotes = 16;

    ChannelEngine& channel;
    const Instrument& instrument;
    const Instrument* currentInstrument = &instrument;
    const DpcmKit* keyMap = nullptr;
    bool usesKeyMap = false;
    Settings settings;

    std::array<int, maxHeldNotes> heldNotes {};
    int numHeldNotes = 0;
    int currentValue = 0;
    int currentVelocity = 127;
    bool released = false;
    double bend = 0.0;
    int vibratoDepth = 0;
    int appliedVibratoDepth = 0;
    int appliedVibratoSpeed = 0;

    // Set by MIDI controllers. Each overrides its setting while it's in use.
    int effectVibratoSpeed = 0;
    int effectSlideFrames = 0;
    int effectDuty = -1;
    bool effectHoldRelease = false;
    bool transportPlaying = false;
    int previousPhaseReset = 0;
    int previousStop = 0;
};
