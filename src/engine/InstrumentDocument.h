#pragma once

#include "engine/Dpcm.h"
#include "engine/Instrument.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// The editable instrument, with undo. Hosts may save and restore state from any thread,
// so every access is locked. The audio thread never touches this class.
class InstrumentDocument
{
public:
    using KitPointer = std::shared_ptr<const DpcmKit>;
    using Listener = std::function<void(const Instrument&, const KitPointer&)>;

    // Called whenever the instrument changes, with the document's lock held.
    void setListener(Listener newListener);

    Instrument snapshot() const;
    KitPointer kit() const;
    std::string name() const;
    void setName(const std::string&);

    // Everything between these calls is one undo step, such as a whole mouse stroke.
    void beginGesture();
    void endGesture();

    void edit(const std::function<void(Instrument&)>& change);

    // Edits a copy of the kit, which then replaces it.
    void editKit(const std::function<void(DpcmKit&)>& change);

    // Chooses which Drum Kit key's noise sound the instrument edits, or -1 for none. The choice isn't
    // an undo step, and edits then go to every key sharing that sound.
    void selectKitKey(int noteValue);
    int selectedKitKey() const;

    // Replaces the whole sound as a single undo step.
    void load(const Instrument&, const std::string& newName, KitPointer newKit = nullptr);

    // Replaces the sound and forgets the history, as when a project is reopened.
    void restore(const Instrument&, const std::string& newName, KitPointer newKit = nullptr);

    bool undo();
    bool redo();
    bool canUndo() const;
    bool canRedo() const;

    // Increases on every change, so views can notice changes by polling.
    int version() const { return changeCount.load(std::memory_order_relaxed); }

private:
    void pushUndoState();
    void changed();
    void loadSelectedKitSound();
    void saveSelectedKitSound();

    static constexpr size_t maxUndoSteps = 200;

    mutable std::mutex mutex;
    struct Sound
    {
        Instrument instrument;
        KitPointer kit;
    };

    Sound sound;
    int kitKey = -1;
    std::string currentName = "Init";
    std::vector<Sound> undoStack, redoStack;
    bool inGesture = false;
    bool gestureHasUndoState = false;
    Listener listener;
    std::atomic<int> changeCount { 0 };
};
