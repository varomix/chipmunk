#pragma once

#include "engine/Instrument.h"

#include <string>

// Editing rules from FamiStudio's Envelope.cs, plus the text form used for typing envelopes.
struct EnvelopeRange
{
    int min, max;
};

// Duty has 4 values on most pulse channels and 8 on VRC6, which is the only range that varies.
constexpr int envelopeDutyMaxVrc6 = 7;

constexpr EnvelopeRange envelopeRange(int type, int maxDuty = 3)
{
    return type == envelopeVolume      ? EnvelopeRange { 0, 15 }
         : type == envelopeDutyCycle   ? EnvelopeRange { 0, maxDuty }
         : type == envelopeS5bMixer    ? EnvelopeRange { 0, 3 }
         : type == envelopeS5bNoise    ? EnvelopeRange { 0, 31 }
                                       : EnvelopeRange { -64, 63 };
}

constexpr bool envelopeCanRelease(int type) { return type == envelopeVolume; }

void setEnvelopeValue(Envelope&, int type, int index, int value, int maxDuty = 3);

// New steps repeat the last value, or the type's default in an empty envelope.
void setEnvelopeLength(Envelope&, int type, int length);

// Passing -1 clears. Clearing the loop also clears the release, which depends on it.
void setEnvelopeLoop(Envelope&, int index);

// A release needs a loop to hold on, so one is added just before it if there is none.
void setEnvelopeRelease(Envelope&, int type, int index);

void clearEnvelope(Envelope&);

// Clamps loaded data to valid values and marker positions.
void sanitizeEnvelope(Envelope&, int type, int maxDuty = envelopeDutyMaxVrc6);

// Text form: values separated by spaces, with | marking the loop start and / the release.
// For example "15 12 | 9 8 / 4 2".
std::string formatEnvelope(const Envelope&);

// Leaves "out" untouched and fills "error" if the text is invalid. Keeps out.relative.
bool parseEnvelope(const std::string& text, int type, Envelope& out, std::string& error, int maxDuty = 3);
