#include "engine/NoteTables.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr double ratioA4ToC1 = 1.0 / 13.454340859610068739450573644169;
constexpr double epsmClock = 8000000.0;

double clockFor(bool pal)
{
    return (pal ? cpuClockPal : cpuClockNtsc) / 16.0;
}

double noteFrequency(size_t note, int tuning)
{
    return tuning * ratioA4ToC1 * std::pow(2.0, (double) (note - 1) / 12.0);
}

// The FM chips set their octave separately, so higher octaves just shift the first octave's values.
template <typename FirstOctave>
NoteTable makeOctaveShiftedTable(FirstOctave firstOctave)
{
    NoteTable table {};

    for (size_t i = 1; i < table.size(); ++i)
    {
        const auto octave = (i - 1) / 12;
        table[i] = octave == 0 ? firstOctave(i) : (uint16_t) (table[(i - 1) % 12 + 1] << octave);
    }

    return table;
}
} // namespace

NoteTable makeNoteTable(bool pal, int tuning)
{
    NoteTable table {};

    for (size_t i = 1; i < table.size(); ++i)
        table[i] = (uint16_t) (clockFor(pal) / noteFrequency(i, tuning) - 0.5);

    return table;
}

NoteTable makeSawNoteTable(bool pal, int tuning)
{
    NoteTable table {};

    for (size_t i = 1; i < table.size(); ++i)
        table[i] = (uint16_t) ((clockFor(pal) * 16.0) / (noteFrequency(i, tuning) * 14.0) - 0.5);

    return table;
}

NoteTable makeFdsNoteTable(bool pal, int tuning)
{
    NoteTable table {};

    for (size_t i = 1; i < table.size(); ++i)
        table[i] = (uint16_t) ((noteFrequency(i, tuning) * 65536.0) / clockFor(pal) + 0.5);

    return table;
}

NoteTable makeN163NoteTable(bool pal, int numChannels, int tuning)
{
    NoteTable table {};

    for (size_t i = 1; i < table.size(); ++i)
        table[i] = (uint16_t) std::min(65535.0, ((noteFrequency(i, tuning) * numChannels * 983040.0) / clockFor(pal)) / 4.0);

    return table;
}

NoteTable makeVrc7NoteTable(int tuning)
{
    return makeOctaveShiftedTable([tuning](size_t i) { return (uint16_t) (noteFrequency(i, tuning) * 262144.0 / 49715.0 + 0.5); });
}

NoteTable makeEpsmSquareNoteTable(int tuning)
{
    NoteTable table {};

    for (size_t i = 1; i < table.size(); ++i)
        table[i] = (uint16_t) std::nearbyint((epsmClock / 32.0) / noteFrequency(i, tuning));

    return table;
}

NoteTable makeEpsmFmNoteTable(int tuning)
{
    return makeOctaveShiftedTable([tuning](size_t i) { return (uint16_t) ((144.0 * noteFrequency(i, tuning) * 1048576.0 / epsmClock) / 4.0); });
}
