// Console diagnostic: round-trips bindings through both encodings and checks
// that values written before modifiers existed still decode to exactly what
// they used to mean. Those old values live in real users' registries, so
// "it compiles" is not evidence that their paddles still work.
#include "core/BackButtonConfig.h"
#include <cstdio>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) ++g_failures;
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
}

void RoundTrip(const char* what, BackButtonBinding b) {
    const BackButtonBinding viaPack = BackButtonBinding::Unpack(b.Pack());
    const BackButtonBinding viaId   = BackButtonBinding::FromId(b.Id());
    printf("  %-26s pack=0x%08X id=%-14s\n", what, b.Pack(), b.Id().c_str());
    Check(viaPack == b, "survives Pack/Unpack");
    Check(viaId   == b, "survives Id/FromId");
}

}  // namespace

int main() {
    using B = BackButtonBinding;

    printf("Round-trips\n");
    RoundTrip("plain key K",        B::FromKey('K'));
    RoundTrip("Ctrl+K",             B::FromKey('K', B::ModCtrl));
    RoundTrip("Ctrl+Alt+Shift+Win", B::FromKey('K', B::ModCtrl | B::ModAlt | B::ModShift | B::ModWin));
    RoundTrip("Win+G",              B::FromKey('G', B::ModWin));
    RoundTrip("action A",           B::FromAction(BackButtonAction::A));
    RoundTrip("touch keyboard",     B::FromAction(BackButtonAction::TouchKeyboard));
    RoundTrip("middle mouse",       B::FromMouseButton(B::MouseButtonCode::Middle));

    printf("\nValues written before modifiers existed\n");
    // Exactly what older builds persisted: (kind << 16) | code, top byte zero.
    const uint32_t oldPlainKey = (1u << 16) | 'K';
    const B decodedKey = B::Unpack(oldPlainKey);
    printf("  0x%08X -> kind=%d code=%u mods=%u\n", oldPlainKey,
           static_cast<int>(decodedKey.kind), decodedKey.code, decodedKey.mods);
    Check(decodedKey == B::FromKey('K'), "old key value still decodes unmodified");

    const uint32_t oldAction = static_cast<uint32_t>(BackButtonAction::LeftMouseButton);
    Check(B::Unpack(oldAction) == B::FromAction(BackButtonAction::LeftMouseButton),
          "bare-action value from the oldest builds still decodes");

    // The wire id an older page would have sent for a plain key.
    Check(B::FromId("key:75") == B::FromKey('K'), "old \"key:75\" id still decodes");
    Check(B::FromKey('K').Id() == "key:75", "unmodified key still emits the old id");

    printf("\nRejections\n");
    Check(B::FromId("key:99999999") == B{}, "absurdly large id rejected");
    Check(B::FromId("key:abc")      == B{}, "non-numeric id rejected");
    Check(B::Unpack((1u << 16) | 0) == B{}, "key with no virtual key rejected");
    // A future build inventing a fifth modifier must not poison the binding.
    const B futureMods = B::Unpack((0xF0u << 24) | (1u << 16) | 'K');
    Check(futureMods == B::FromKey('K'), "unknown modifier bits dropped, key kept");
    // What an older build sees in a profile that binds an action it predates:
    // unbound, never some other action that happens to hold the value now.
    const uint32_t futureAction = static_cast<uint32_t>(BackButtonAction::COUNT);
    Check(B::Unpack(futureAction) == B::FromAction(BackButtonAction::None),
          "action from a newer build decodes as unbound");

    printf("\n%s (%d failure(s))\n", g_failures ? "FAILED" : "All checks passed", g_failures);
    return g_failures ? 1 : 0;
}
