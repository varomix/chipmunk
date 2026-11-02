#include "engine/VoiceController.h"

#include <algorithm>
#include <cmath>

#include "engine/EnvelopeEdit.h"

VoiceController::VoiceController(ChannelEngine& c, const Instrument& i)
    : channel(c),
      instrument(i)
{
}

void VoiceController::setSettings(const Settings& newSettings)
{
    const auto dutyChanged = newSettings.dutyCycle != settings.dutyCycle;

    settings = newSettings;

    if (dutyChanged)
    {
        ChannelNote note;
        note.hasDutyCycle = true;
        note.dutyCycle = settings.dutyCycle;
        channel.playNote(note);
    }
}

void VoiceController::playDpcmKey(int value)
{
    ChannelNote note;
    note.value = value;
    note.instrument = &instrument;
    channel.playNote(note);
    currentValue = value;
}

void VoiceController::playHeldNote(int midiNote, bool retrigger, int velocity)
{
    auto value = midiNoteToNoteValue(midiNote);
    currentInstrument = &instrument;

    if (usesKeyMap && keyMap != nullptr)
    {
        const auto& mapping = keyMap->mappings[(size_t) value];
        value = mapping.noiseNote;
        currentInstrument = &mapping.noise->instrument;
    }

    ChannelNote note;
    note.value = value;
    note.hasAttack = retrigger;
    note.instrument = currentInstrument;
    note.hasDutyCycle = true;
    note.dutyCycle = effectDuty >= 0 ? effectDuty : settings.dutyCycle;

    if (velocity > 0)
    {
        currentVelocity = velocity;
        note.hasVolume = true;
        note.volume = settings.velocitySensitive ? std::clamp((int) std::lround(velocity / 127.0 * 15.0), 1, 15) : 15;
    }

    int slidePitch = 0, slideStep = 0;

    const auto slideFrames = effectSlideFrames > 0 ? effectSlideFrames : settings.glideFrames;

    if (! retrigger && slideFrames > 0 && currentValue != 0 && currentValue != value)
    {
        channel.computeSlide(currentValue, value, slideFrames, slidePitch, slideStep);
        note.slideTarget = value;
    }

    channel.playNote(note, slidePitch, slideStep);
    currentValue = value;
    released = false;
}

bool VoiceController::removeHeldNote(int midiNote)
{
    for (int i = 0; i < numHeldNotes; ++i)
    {
        if (heldNotes[(size_t) i] == midiNote)
        {
            std::copy(heldNotes.begin() + i + 1, heldNotes.begin() + numHeldNotes, heldNotes.begin() + i);
            --numHeldNotes;
            return true;
        }
    }

    return false;
}

void VoiceController::noteOn(int midiNote, int velocity)
{
    const auto value = midiNoteToNoteValue(midiNote);

    if (value < noteMusicalMin || value > noteMusicalMax)
        return;

    if (usesKeyMap && (keyMap == nullptr || keyMap->mappings[(size_t) value].noise == nullptr))
        return;

    if (channel.getType() == ChannelType::dpcm)
    {
        playDpcmKey(value);
        return;
    }

    // Each hit is a new attack, and the drums ring out by themselves.
    if (channel.isRhythm())
    {
        ChannelNote note;
        note.value = value;
        note.instrument = &instrument;
        note.hasVolume = true;
        note.volume = settings.velocitySensitive ? std::clamp((int) std::lround(velocity / 127.0 * 15.0), 1, 15) : 15;
        channel.playNote(note);
        currentValue = value;
        return;
    }

    // Move the note to the top of the stack, dropping the oldest one if it's full.
    if (! removeHeldNote(midiNote) && numHeldNotes == maxHeldNotes)
        removeHeldNote(heldNotes[0]);

    const auto wasHolding = numHeldNotes > 0;

    heldNotes[(size_t) numHeldNotes++] = midiNote;
    playHeldNote(midiNote, ! (wasHolding && settings.legato), velocity);
}

void VoiceController::noteOff(int midiNote)
{
    // Hits ring out to the end of the sample, however short the MIDI note. Only loops need stopping.
    if (channel.isRhythm())
        return;

    if (channel.getType() == ChannelType::dpcm)
    {
        const auto value = midiNoteToNoteValue(midiNote);

        if (value == currentValue && channel.isDpcmKeyLooping(value))
            allNotesOff();

        return;
    }

    const auto wasTop = numHeldNotes > 0 && heldNotes[(size_t) numHeldNotes - 1] == midiNote;

    if (! removeHeldNote(midiNote))
        return;

    if (! wasTop)
        return;

    if (numHeldNotes > 0)
    {
        playHeldNote(heldNotes[(size_t) numHeldNotes - 1], ! settings.legato, settings.legato ? 0 : currentVelocity);
    }
    else if (currentInstrument->hasReleaseEnvelope() || channel.ringsOutOnRelease())
    {
        ChannelNote note;
        note.value = noteRelease;
        channel.playNote(note);

        // FM operators that ring out fade by themselves, so nothing needs to stop them.
        released = ! channel.ringsOutOnRelease();
    }
    else
    {
        allNotesOff();
    }
}

void VoiceController::allNotesOff()
{
    numHeldNotes = 0;
    currentValue = 0;
    released = false;
    currentInstrument = &instrument;

    ChannelNote note;
    note.value = noteStop;
    channel.playNote(note);
}

void VoiceController::setKeyMap(const DpcmKit* kit, bool useKeyMap)
{
    keyMap = kit;
    usesKeyMap = useKeyMap;

    if (! usesKeyMap)
        return;

    // Held keys pick up their edited sounds. Anything else has to stop, as it may point into the old kit.
    for (auto i = numHeldNotes - 1; i >= 0; --i)
    {
        const auto key = heldNotes[(size_t) i];

        if (kit == nullptr || kit->mappings[(size_t) midiNoteToNoteValue(key)].noise == nullptr)
            removeHeldNote(key);
    }

    if (numHeldNotes > 0)
        playHeldNote(heldNotes[(size_t) numHeldNotes - 1], false, 0);
    else
        allNotesOff();
}

void VoiceController::setPitchBend(double normalized)
{
    bend = normalized;
}

void VoiceController::setModWheel(int value)
{
    vibratoDepth = value == 0 ? 0 : std::clamp((value * 15 + 63) / 127, 1, 15);
}

void VoiceController::setController(int controller, int value)
{
    ChannelNote note;

    switch (controller)
    {
        case MidiEffect::vibratoDepth:
            setModWheel(value);
            return;

        case MidiEffect::vibratoSpeed:
            effectVibratoSpeed = std::clamp(value, 0, 12);
            return;

        case MidiEffect::slideFrames:
            effectSlideFrames = value;
            return;

        case MidiEffect::holdRelease:
            effectHoldRelease = value > 0;
            return;

        case MidiEffect::volume:
            note.hasVolume = true;
            note.volume = (value * 15 + 63) / 127;
            break;

        case MidiEffect::finePitch:
            note.hasFinePitch = true;
            note.finePitch = value - 64;
            break;

        case MidiEffect::dutyCycle:
            effectDuty = std::clamp(value, 0, envelopeDutyMaxVrc6);
            note.hasDutyCycle = true;
            note.dutyCycle = effectDuty;
            break;

        case MidiEffect::phaseReset:
            note.hasPhaseReset = value > 0 && previousPhaseReset == 0;
            previousPhaseReset = value;
            if (! note.hasPhaseReset)
                return;
            break;

        case MidiEffect::fdsModSpeed:
            note.hasFdsModSpeed = true;
            note.fdsModSpeed = value * 32;
            break;

        case MidiEffect::fdsModDepth:
            note.hasFdsModDepth = true;
            note.fdsModDepth = std::min(value, 63);
            break;

        case MidiEffect::stopNote:
            if (value > 0 && previousStop == 0)
                allNotesOff();
            previousStop = value;
            return;

        default:
            return;
    }

    channel.playNote(note);
}

void VoiceController::applyVibrato()
{
    const auto speed = effectVibratoSpeed > 0 ? effectVibratoSpeed : settings.vibratoSpeed;

    if (vibratoDepth == appliedVibratoDepth && (vibratoDepth == 0 || speed == appliedVibratoSpeed))
        return;

    ChannelNote note;
    note.hasVibrato = true;
    note.vibratoSpeed = speed;
    note.vibratoDepth = vibratoDepth;
    channel.playNote(note);

    appliedVibratoDepth = vibratoDepth;
    appliedVibratoSpeed = speed;
}

void VoiceController::applyBend()
{
    // Noise and DPCM have no period to bend.
    if (channel.getType() == ChannelType::noise || channel.getType() == ChannelType::dpcm || channel.isRhythm() || currentValue == 0)
    {
        channel.setBendPeriodOffset(0);
        return;
    }

    channel.setBendPeriodOffset(bend == 0.0 ? 0 : channel.periodOffsetForBend(currentValue, bend * settings.bendRangeSemitones));
}

void VoiceController::beginFrame()
{
    // A released note ends with its volume envelope, otherwise it would hold its final value forever.
    if (released && ! (effectHoldRelease && transportPlaying) && channel.isVolumeEnvelopeFinished())
        allNotesOff();

    applyVibrato();
    applyBend();
}
