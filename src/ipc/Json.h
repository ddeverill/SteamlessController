#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// A small hand-rolled JSON value, enough for the daemon<->CLI NDJSON
// protocol (flat command/response objects, one level of nesting for
// "args"/"data", and simple arrays of strings). Not a general-purpose JSON
// library — no streaming, no comments, no big-number handling beyond
// double/int64 — in the same spirit as the existing hand-rolled JSON reader
// in the Windows remap UI (RemapWindow.cpp): the wire shape here is simple
// enough that a dependency isn't worth it.
class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() : m_type(Type::Null) {}
    JsonValue(std::nullptr_t) : m_type(Type::Null) {}
    JsonValue(bool b) : m_type(Type::Bool), m_bool(b) {}
    JsonValue(double n) : m_type(Type::Number), m_number(n) {}
    JsonValue(int n) : m_type(Type::Number), m_number(n) {}
    JsonValue(int64_t n) : m_type(Type::Number), m_number(static_cast<double>(n)) {}
    JsonValue(uint32_t n) : m_type(Type::Number), m_number(n) {}
    JsonValue(const char* s) : m_type(Type::String), m_string(s) {}
    JsonValue(std::string s) : m_type(Type::String), m_string(std::move(s)) {}

    static JsonValue Array()  { JsonValue v; v.m_type = Type::Array;  return v; }
    static JsonValue Object() { JsonValue v; v.m_type = Type::Object; return v; }

    Type type() const { return m_type; }
    bool isNull()   const { return m_type == Type::Null; }
    bool isObject() const { return m_type == Type::Object; }
    bool isArray()  const { return m_type == Type::Array; }
    bool isString() const { return m_type == Type::String; }

    bool        asBool(bool def = false)     const { return m_type == Type::Bool ? m_bool : def; }
    double      asDouble(double def = 0)     const { return m_type == Type::Number ? m_number : def; }
    int         asInt(int def = 0)           const { return m_type == Type::Number ? static_cast<int>(m_number) : def; }
    uint32_t    asUInt(uint32_t def = 0)      const { return m_type == Type::Number ? static_cast<uint32_t>(m_number) : def; }
    std::string asString(std::string def = "") const { return m_type == Type::String ? m_string : def; }

    // Object access.
    JsonValue& operator[](const std::string& key);
    const JsonValue* find(const std::string& key) const;
    bool has(const std::string& key) const { return find(key) != nullptr; }

    // Array access.
    void push_back(JsonValue v);
    const std::vector<JsonValue>& items() const { return m_array; }

    // Object key iteration, e.g. for flattening one object's fields into another.
    const std::vector<std::pair<std::string, JsonValue>>& entries() const { return m_object; }

    std::string dump() const;
    static std::optional<JsonValue> parse(const std::string& text);

private:
    void dumpTo(std::string& out) const;

    Type   m_type = Type::Null;
    bool   m_bool = false;
    double m_number = 0;
    std::string m_string;
    std::vector<JsonValue> m_array;
    // Object: insertion-ordered, kept small (protocol messages have a
    // handful of keys) — linear find() is fine and keeps dump() stable.
    std::vector<std::pair<std::string, JsonValue>> m_object;
};
