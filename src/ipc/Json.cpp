#include "Json.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

JsonValue& JsonValue::operator[](const std::string& key) {
    m_type = Type::Object;
    for (auto& [k, v] : m_object)
        if (k == key) return v;
    m_object.emplace_back(key, JsonValue());
    return m_object.back().second;
}

const JsonValue* JsonValue::find(const std::string& key) const {
    if (m_type != Type::Object) return nullptr;
    for (auto& [k, v] : m_object)
        if (k == key) return &v;
    return nullptr;
}

void JsonValue::push_back(JsonValue v) {
    m_type = Type::Array;
    m_array.push_back(std::move(v));
}

namespace {

void DumpString(const std::string& s, std::string& out) {
    out += '"';
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

}  // namespace

void JsonValue::dumpTo(std::string& out) const {
    switch (m_type) {
    case Type::Null:  out += "null"; break;
    case Type::Bool:  out += m_bool ? "true" : "false"; break;
    case Type::Number: {
        if (m_number == static_cast<int64_t>(m_number)) {
            out += std::to_string(static_cast<int64_t>(m_number));
        } else {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%g", m_number);
            out += buf;
        }
        break;
    }
    case Type::String: DumpString(m_string, out); break;
    case Type::Array: {
        out += '[';
        for (size_t i = 0; i < m_array.size(); ++i) {
            if (i) out += ',';
            m_array[i].dumpTo(out);
        }
        out += ']';
        break;
    }
    case Type::Object: {
        out += '{';
        for (size_t i = 0; i < m_object.size(); ++i) {
            if (i) out += ',';
            DumpString(m_object[i].first, out);
            out += ':';
            m_object[i].second.dumpTo(out);
        }
        out += '}';
        break;
    }
    }
}

std::string JsonValue::dump() const {
    std::string out;
    dumpTo(out);
    return out;
}

namespace {

class Parser {
public:
    explicit Parser(const std::string& text) : m_text(text) {}

    std::optional<JsonValue> Parse() {
        SkipWs();
        auto v = ParseValue();
        if (!v) return std::nullopt;
        SkipWs();
        return v;  // trailing garbage is ignored — NDJSON framing already found the line end
    }

private:
    void SkipWs() { while (m_pos < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_pos]))) ++m_pos; }
    bool AtEnd() const { return m_pos >= m_text.size(); }
    char Peek() const { return AtEnd() ? '\0' : m_text[m_pos]; }

    bool Consume(char c) {
        if (Peek() != c) return false;
        ++m_pos;
        return true;
    }

    bool ConsumeLiteral(const char* lit) {
        const size_t len = std::strlen(lit);
        if (m_text.compare(m_pos, len, lit) != 0) return false;
        m_pos += len;
        return true;
    }

    std::optional<std::string> ParseString() {
        if (!Consume('"')) return std::nullopt;
        std::string out;
        while (!AtEnd() && Peek() != '"') {
            char c = m_text[m_pos++];
            if (c == '\\' && !AtEnd()) {
                char esc = m_text[m_pos++];
                switch (esc) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    if (m_pos + 4 > m_text.size()) return std::nullopt;
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = m_text[m_pos++];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                        else return std::nullopt;
                    }
                    // BMP-only (no surrogate-pair handling) — sufficient for
                    // this protocol's ASCII-shaped command/field names.
                    if (code < 0x80) {
                        out += static_cast<char>(code);
                    } else if (code < 0x800) {
                        out += static_cast<char>(0xC0 | (code >> 6));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (code >> 12));
                        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    break;
                }
                default: return std::nullopt;
                }
            } else {
                out += c;
            }
        }
        if (!Consume('"')) return std::nullopt;
        return out;
    }

    std::optional<JsonValue> ParseValue() {
        SkipWs();
        if (Peek() == '"') {
            auto s = ParseString();
            if (!s) return std::nullopt;
            return JsonValue(*s);
        }
        if (Peek() == '{') return ParseObject();
        if (Peek() == '[') return ParseArray();
        if (ConsumeLiteral("true"))  return JsonValue(true);
        if (ConsumeLiteral("false")) return JsonValue(false);
        if (ConsumeLiteral("null"))  return JsonValue(nullptr);
        return ParseNumber();
    }

    std::optional<JsonValue> ParseNumber() {
        const size_t start = m_pos;
        if (Peek() == '-') ++m_pos;
        while (!AtEnd() && (std::isdigit(static_cast<unsigned char>(Peek())) || Peek() == '.' ||
                            Peek() == 'e' || Peek() == 'E' || Peek() == '+' || Peek() == '-'))
            ++m_pos;
        if (m_pos == start) return std::nullopt;
        try {
            return JsonValue(std::stod(m_text.substr(start, m_pos - start)));
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<JsonValue> ParseObject() {
        if (!Consume('{')) return std::nullopt;
        JsonValue obj = JsonValue::Object();
        SkipWs();
        if (Consume('}')) return obj;
        for (;;) {
            SkipWs();
            auto key = ParseString();
            if (!key) return std::nullopt;
            SkipWs();
            if (!Consume(':')) return std::nullopt;
            auto value = ParseValue();
            if (!value) return std::nullopt;
            obj[*key] = *value;
            SkipWs();
            if (Consume(',')) continue;
            if (Consume('}')) break;
            return std::nullopt;
        }
        return obj;
    }

    std::optional<JsonValue> ParseArray() {
        if (!Consume('[')) return std::nullopt;
        JsonValue arr = JsonValue::Array();
        SkipWs();
        if (Consume(']')) return arr;
        for (;;) {
            auto value = ParseValue();
            if (!value) return std::nullopt;
            arr.push_back(*value);
            SkipWs();
            if (Consume(',')) continue;
            if (Consume(']')) break;
            return std::nullopt;
        }
        return arr;
    }

    const std::string& m_text;
    size_t             m_pos = 0;
};

}  // namespace

std::optional<JsonValue> JsonValue::parse(const std::string& text) {
    Parser p(text);
    return p.Parse();
}
