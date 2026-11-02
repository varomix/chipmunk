#pragma once

#include <array>
#include <cstdint>

constexpr int cpuClockNtsc = 1789773;
constexpr int cpuClockPal = 1662607;
constexpr double framesPerSecondNtsc = 60.0988;
constexpr double framesPerSecondPal = 50.0070;

// Note values match FamiStudio: 0 is stop, 1 is C1 (MIDI 24), 96 is B8 (MIDI 119).
constexpr int noteStop = 0x00;
constexpr int noteMusicalMin = 0x01;
constexpr int noteMusicalMax = 0x60;
constexpr int noteRelease = 0x80;
constexpr int noteInvalid = 0xff;

constexpr int midiNoteToNoteValue(int midiNote) { return midiNote - 23; }

using NoteTable = std::array<uint16_t, noteMusicalMax + 1>;

// Pulse and triangle periods, ported from NesApu.GetOrCreateNoteTableSet.
NoteTable makeNoteTable(bool pal, int tuning = 440);

// The VRC6 saw has a 14-step cycle, so it gets its own table.
NoteTable makeSawNoteTable(bool pal, int tuning = 440);

// Tables of the other expansion chips, which each count pitch in their own way.
NoteTable makeFdsNoteTable(bool pal, int tuning = 440);
NoteTable makeN163NoteTable(bool pal, int numChannels, int tuning = 440);
NoteTable makeVrc7NoteTable(int tuning = 440);
NoteTable makeEpsmSquareNoteTable(int tuning = 440);
NoteTable makeEpsmFmNoteTable(int tuning = 440);
