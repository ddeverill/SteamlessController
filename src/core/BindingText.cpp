#include "BindingText.h"
#include "KeyNames.h"

namespace {

bool ConsumePrefix(std::string& s, const char* prefix) {
    const std::string p = prefix;
    if (s.compare(0, p.size(), p) != 0) return false;
    s.erase(0, p.size());
    return true;
}

bool IsAllDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

}  // namespace

std::optional<BackButtonBinding> BindingText::Parse(const std::string& text) {
    if (text.rfind("key:", 0) == 0) {
        std::string rest = text.substr(4);

        // Legacy packed numeric form — always accepted so old config values
        // and the wire format keep working.
        if (IsAllDigits(rest))
            return BackButtonBinding::FromId(text);

        // Readable form: key:[ctrl+][alt+][shift+][win+]<KeyName>
        uint8_t mods = BackButtonBinding::ModNone;
        bool changed = true;
        while (changed) {
            changed = false;
            if (ConsumePrefix(rest, "ctrl+"))  { mods |= BackButtonBinding::ModCtrl;  changed = true; }
            if (ConsumePrefix(rest, "alt+"))   { mods |= BackButtonBinding::ModAlt;   changed = true; }
            if (ConsumePrefix(rest, "shift+")) { mods |= BackButtonBinding::ModShift; changed = true; }
            if (ConsumePrefix(rest, "win+"))   { mods |= BackButtonBinding::ModWin;   changed = true; }
        }
        const uint16_t keyId = KeyIdFromJsCode(rest);
        if (keyId == 0) return std::nullopt;
        return BackButtonBinding::FromKey(keyId, mods);
    }

    if (text == "mouse:middle") return BackButtonBinding::FromMouseButton(BackButtonBinding::MouseButtonCode::Middle);
    if (text == "mouse:x1")     return BackButtonBinding::FromMouseButton(BackButtonBinding::MouseButtonCode::X1);
    if (text == "mouse:x2")     return BackButtonBinding::FromMouseButton(BackButtonBinding::MouseButtonCode::X2);

    const BackButtonAction action = BackButtonActionFromId(text);
    // BackButtonActionFromId degrades unrecognised text to None — only accept
    // that as a real answer when the caller actually asked for "none".
    if (action != BackButtonAction::None || text == "none")
        return BackButtonBinding::FromAction(action);
    return std::nullopt;
}

std::string BindingText::Format(const BackButtonBinding& b) {
    if (b.kind != BackButtonBinding::Kind::Key)
        return b.Id();

    std::string out = "key:";
    size_t count = 0;
    const ModifierKeyInfo* table = ModifierKeyTable(count);
    for (size_t i = 0; i < count; ++i) {
        if (b.mods & table[i].flag) {
            out += table[i].name;
            out += '+';
        }
    }
    // Lowercase the modifier names to match the grammar ("ctrl+", not "Ctrl+").
    for (auto& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    // ...except the "key:" prefix itself, which the lowercase pass above left
    // untouched since it already was lowercase.

    const std::string jsCode = JsCodeFromKeyId(b.code);
    if (!jsCode.empty()) {
        out += jsCode;
        return out;
    }

    // No catalog entry for this key id (arrived via the legacy packed form
    // with a code this build doesn't recognise a name for) — fall back to
    // the lossless packed wire format so the binding still round-trips.
    return b.Id();
}
