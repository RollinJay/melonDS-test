/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef NETJSON_H
#define NETJSON_H

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace melonDS::RollbackNet
{

// A small JSON value, for the peer-to-peer handshake and its signals (the
// frontend uses Qt's JSON for everything else). Parse accepts standard JSON;
// numbers are doubles; strings are UTF-8.
class JsonValue
{
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() = default;
    JsonValue(bool b) : Kind(Type::Bool), Boolean(b) {}
    JsonValue(double n) : Kind(Type::Number), Num(n) {}
    JsonValue(int n) : Kind(Type::Number), Num(n) {}
    JsonValue(const char* s) : Kind(Type::String), Str(s) {}
    JsonValue(std::string s) : Kind(Type::String), Str(std::move(s)) {}

    static JsonValue MakeArray() { JsonValue v; v.Kind = Type::Array; return v; }
    static JsonValue MakeObject() { JsonValue v; v.Kind = Type::Object; return v; }

    Type GetType() const { return Kind; }
    bool IsObject() const { return Kind == Type::Object; }
    bool IsArray() const { return Kind == Type::Array; }

    // object member (Null if absent or not an object)
    const JsonValue& operator[](const std::string& key) const;
    // array element (Null if out of range or not an array)
    const JsonValue& operator[](size_t index) const;
    size_t Size() const;

    std::string AsString(const std::string& def = "") const { return Kind == Type::String ? Str : def; }
    double AsNumber(double def = 0) const { return Kind == Type::Number ? Num : def; }
    bool AsBool(bool def = false) const { return Kind == Type::Bool ? Boolean : def; }

    // building
    void Set(const std::string& key, JsonValue value);
    void Append(JsonValue value);

    std::string Serialize() const;
    // false if text is not one valid JSON value (nesting is limited to 64 levels)
    static bool Parse(const std::string& text, JsonValue& out);

private:
    Type Kind = Type::Null;
    bool Boolean = false;
    double Num = 0;
    std::string Str;
    std::vector<JsonValue> Items;
    std::vector<std::pair<std::string, JsonValue>> Members;   // in insertion order

    void SerializeTo(std::string& out) const;
    friend class JsonParser;
};

}

#endif // NETJSON_H
