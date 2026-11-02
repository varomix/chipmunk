#include "engine/InstrumentDocument.h"

void InstrumentDocument::setListener(Listener newListener)
{
    const std::scoped_lock lock(mutex);
    listener = std::move(newListener);

    if (listener)
        listener(sound.instrument, sound.kit);
}

Instrument InstrumentDocument::snapshot() const
{
    const std::scoped_lock lock(mutex);
    return sound.instrument;
}

InstrumentDocument::KitPointer InstrumentDocument::kit() const
{
    const std::scoped_lock lock(mutex);
    return sound.kit;
}

std::string InstrumentDocument::name() const
{
    const std::scoped_lock lock(mutex);
    return currentName;
}

void InstrumentDocument::setName(const std::string& newName)
{
    const std::scoped_lock lock(mutex);
    currentName = newName;
    changeCount.fetch_add(1, std::memory_order_relaxed);
}

void InstrumentDocument::pushUndoState()
{
    if (undoStack.size() == maxUndoSteps)
        undoStack.erase(undoStack.begin());

    undoStack.push_back(sound);
    redoStack.clear();
}

void InstrumentDocument::changed()
{
    changeCount.fetch_add(1, std::memory_order_relaxed);

    if (listener)
        listener(sound.instrument, sound.kit);
}

void InstrumentDocument::beginGesture()
{
    const std::scoped_lock lock(mutex);
    inGesture = true;
    gestureHasUndoState = false;
}

void InstrumentDocument::endGesture()
{
    const std::scoped_lock lock(mutex);
    inGesture = false;
}

void InstrumentDocument::edit(const std::function<void(Instrument&)>& change)
{
    const std::scoped_lock lock(mutex);

    if (! inGesture || ! gestureHasUndoState)
    {
        pushUndoState();
        gestureHasUndoState = true;
    }

    change(sound.instrument);
    saveSelectedKitSound();
    changed();
}

void InstrumentDocument::editKit(const std::function<void(DpcmKit&)>& change)
{
    const std::scoped_lock lock(mutex);

    pushUndoState();

    auto edited = sound.kit != nullptr ? std::make_shared<DpcmKit>(*sound.kit) : std::make_shared<DpcmKit>();
    change(*edited);
    sound.kit = std::move(edited);

    changed();
}

void InstrumentDocument::selectKitKey(int noteValue)
{
    const std::scoped_lock lock(mutex);
    kitKey = noteValue;
    loadSelectedKitSound();
    changed();
}

int InstrumentDocument::selectedKitKey() const
{
    const std::scoped_lock lock(mutex);
    return kitKey;
}

void InstrumentDocument::loadSelectedKitSound()
{
    if (kitKey >= 0 && sound.kit != nullptr && sound.kit->mappings[(size_t) kitKey].noise != nullptr)
        sound.instrument = sound.kit->mappings[(size_t) kitKey].noise->instrument;
}

void InstrumentDocument::saveSelectedKitSound()
{
    if (kitKey < 0 || sound.kit == nullptr || sound.kit->mappings[(size_t) kitKey].noise == nullptr)
        return;

    const auto old = sound.kit->mappings[(size_t) kitKey].noise;
    auto updated = std::make_shared<KitSound>(*old);
    updated->instrument = sound.instrument;

    auto edited = std::make_shared<DpcmKit>(*sound.kit);

    for (auto& mapping : edited->mappings)
        if (mapping.noise == old)
            mapping.noise = updated;

    sound.kit = std::move(edited);
}

void InstrumentDocument::load(const Instrument& newInstrument, const std::string& newName, KitPointer newKit)
{
    const std::scoped_lock lock(mutex);
    pushUndoState();
    sound = { newInstrument, std::move(newKit) };
    currentName = newName;
    kitKey = -1;
    changed();
}

void InstrumentDocument::restore(const Instrument& newInstrument, const std::string& newName, KitPointer newKit)
{
    const std::scoped_lock lock(mutex);
    undoStack.clear();
    redoStack.clear();
    sound = { newInstrument, std::move(newKit) };
    currentName = newName;
    kitKey = -1;
    changed();
}

bool InstrumentDocument::undo()
{
    const std::scoped_lock lock(mutex);

    if (undoStack.empty())
        return false;

    redoStack.push_back(sound);
    sound = undoStack.back();
    undoStack.pop_back();
    loadSelectedKitSound();
    changed();
    return true;
}

bool InstrumentDocument::redo()
{
    const std::scoped_lock lock(mutex);

    if (redoStack.empty())
        return false;

    undoStack.push_back(sound);
    sound = redoStack.back();
    redoStack.pop_back();
    loadSelectedKitSound();
    changed();
    return true;
}

bool InstrumentDocument::canUndo() const
{
    const std::scoped_lock lock(mutex);
    return ! undoStack.empty();
}

bool InstrumentDocument::canRedo() const
{
    const std::scoped_lock lock(mutex);
    return ! redoStack.empty();
}
