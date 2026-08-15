#include "ModifierTracker.h"
#include "core/iface/IInputInjector.h"

void ModifierTracker::Apply(uint8_t mods, bool down) {
    size_t count = 0;
    const ModifierKeyInfo* table = ModifierKeyTable(count);

    for (size_t i = 0; i < count && i < kMaxModifiers; ++i) {
        const auto& m = table[i];
        if (!(mods & m.flag)) continue;

        if (down) {
            // Only the first binding to want it does anything; the rest just
            // join the count so the key outlives their individual releases.
            if (m_holds[i]++ > 0) continue;
            // Already down under the user's own finger — leave it theirs, and
            // remember not to release what we never pressed.
            m_sentByUs[i] = !m_injector.IsPhysicallyHeld(m.keyId);
            if (m_sentByUs[i]) m_injector.Key(m.keyId, true);
        } else {
            if (m_holds[i] == 0) continue;       // never pressed, or already undone
            if (--m_holds[i] > 0) continue;      // another binding still wants it
            if (!m_sentByUs[i]) continue;
            m_sentByUs[i] = false;
            m_injector.Key(m.keyId, false);
        }
    }
}
