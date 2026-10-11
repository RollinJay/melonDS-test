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

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "NetJson.h"

namespace melonDS::RollbackNet
{

static const JsonValue kNull;

const JsonValue& JsonValue::operator[](const std::string& key) const
{
    if (Kind != Type::Object) return kNull;
    for (const auto& m : Members)
    {
        if (m.first == key) return m.second;
    }
    return kNull;
}

const JsonValue& JsonValue::operator[](size_t index) const
{
    if (Kind != Type::Array || index >= Items.size()) return kNull;
    return Items[index];
}

size_t JsonValue::Size() const
{
    if (Kind == Type::Array) return Items.size();
    if (Kind == Type::Object) return Members.size();
    return 0;
}

void JsonValue::Set(const std::string& key, JsonValue value)
{
    if (Kind != Type::Object)
    {
        *this = MakeObject();
    }
    for (auto& m : Members)
    {
        if (m.first == key)
        {
            m.second = std::move(value);
            return;
        }
    }
    Members.emplace_back(key, std::move(value));
}

void JsonValue::Append(JsonValue value)
{
    if (Kind != Type::Array)
    {
        *this = MakeArray();
    }
    Items.push_back(std::move(value));
}

static void EscapeString(const std::string& s, std::string& out)
{
    out += '"';
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
                out += (char)c;
        }
    }
    out += '"';
}

void JsonValue::SerializeTo(std::string& out) const
{
    switch (Kind)
    {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += Boolean ? "true" : "false"; break;
    case Type::Number:
        {
            char buf[32];
            if (std::isfinite(Num) && Num == std::floor(Num) && std::fabs(Num) < 9.0e15)
                snprintf(buf, sizeof(buf), "%.0f", Num);
            else if (std::isfinite(Num))
                snprintf(buf, sizeof(buf), "%.17g", Num);
            else
                snprintf(buf, sizeof(buf), "null");
            out += buf;
        }
        break;
    case Type::String: EscapeString(Str, out); break;
    case Type::Array:
        out += '[';
        for (size_t i = 0; i < Items.size(); i++)
        {
            if (i) out += ',';
            Items[i].SerializeTo(out);
        }
        out += ']';
        break;
    case Type::Object:
        out += '{';
        for (size_t i = 0; i < Members.size(); i++)
        {
            if (i) out += ',';
            EscapeString(Members[i].first, out);
            out += ':';
            Members[i].second.SerializeTo(out);
        }
        out += '}';
        break;
    }
}

std::string JsonValue::Serialize() const
{
    std::string out;
    SerializeTo(out);
    return out;
}

class JsonParser
{
public:
    explicit JsonParser(const std::string& text) : S(text) {}

    bool ParseDocument(JsonValue& out)
    {
        SkipSpace();
        if (!ParseValue(out, 0)) return false;
        SkipSpace();
        return Pos == S.size();
    }

private:
    const std::string& S;
    size_t Pos = 0;

    void SkipSpace()
    {
        while (Pos < S.size() && (S[Pos] == ' ' || S[Pos] == '\t' || S[Pos] == '\n' || S[Pos] == '\r'))
            Pos++;
    }

    bool Literal(const char* word)
    {
        size_t n = strlen(word);
        if (S.compare(Pos, n, word) != 0) return false;
        Pos += n;
        return true;
    }

    static void AppendUTF8(std::string& out, unsigned cp)
    {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800)
        {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
        else
        {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool Hex4(unsigned& v)
    {
        if (Pos + 4 > S.size()) return false;
        v = 0;
        for (int i = 0; i < 4; i++)
        {
            char c = S[Pos++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    }

    bool ParseString(std::string& out)
    {
        if (Pos >= S.size() || S[Pos] != '"') return false;
        Pos++;
        while (Pos < S.size())
        {
            char c = S[Pos++];
            if (c == '"') return true;
            if ((unsigned char)c < 0x20) return false;
            if (c != '\\')
            {
                out += c;
                continue;
            }
            if (Pos >= S.size()) return false;
            char e = S[Pos++];
            switch (e)
            {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u':
                {
                    unsigned cp;
                    if (!Hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00)
                    {
                        unsigned lo;
                        if (Pos + 2 > S.size() || S[Pos] != '\\' || S[Pos + 1] != 'u') return false;
                        Pos += 2;
                        if (!Hex4(lo) || lo < 0xDC00 || lo >= 0xE000) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    else if (cp >= 0xDC00 && cp < 0xE000)
                        return false;
                    AppendUTF8(out, cp);
                }
                break;
            default:
                return false;
            }
        }
        return false;
    }

    bool ParseNumber(double& out)
    {
        size_t start = Pos;
        if (Pos < S.size() && S[Pos] == '-') Pos++;
        if (Pos >= S.size() || !isdigit((unsigned char)S[Pos])) return false;
        while (Pos < S.size() && isdigit((unsigned char)S[Pos])) Pos++;
        if (Pos < S.size() && S[Pos] == '.')
        {
            Pos++;
            if (Pos >= S.size() || !isdigit((unsigned char)S[Pos])) return false;
            while (Pos < S.size() && isdigit((unsigned char)S[Pos])) Pos++;
        }
        if (Pos < S.size() && (S[Pos] == 'e' || S[Pos] == 'E'))
        {
            Pos++;
            if (Pos < S.size() && (S[Pos] == '+' || S[Pos] == '-')) Pos++;
            if (Pos >= S.size() || !isdigit((unsigned char)S[Pos])) return false;
            while (Pos < S.size() && isdigit((unsigned char)S[Pos])) Pos++;
        }
        out = strtod(S.substr(start, Pos - start).c_str(), nullptr);
        return true;
    }

    bool ParseValue(JsonValue& out, int depth)
    {
        if (depth > 64 || Pos >= S.size()) return false;
        char c = S[Pos];
        if (c == '{')
        {
            Pos++;
            out = JsonValue::MakeObject();
            SkipSpace();
            if (Pos < S.size() && S[Pos] == '}') { Pos++; return true; }
            for (;;)
            {
                SkipSpace();
                std::string key;
                if (!ParseString(key)) return false;
                SkipSpace();
                if (Pos >= S.size() || S[Pos] != ':') return false;
                Pos++;
                SkipSpace();
                JsonValue v;
                if (!ParseValue(v, depth + 1)) return false;
                out.Set(key, std::move(v));
                SkipSpace();
                if (Pos < S.size() && S[Pos] == ',') { Pos++; continue; }
                if (Pos < S.size() && S[Pos] == '}') { Pos++; return true; }
                return false;
            }
        }
        if (c == '[')
        {
            Pos++;
            out = JsonValue::MakeArray();
            SkipSpace();
            if (Pos < S.size() && S[Pos] == ']') { Pos++; return true; }
            for (;;)
            {
                SkipSpace();
                JsonValue v;
                if (!ParseValue(v, depth + 1)) return false;
                out.Append(std::move(v));
                SkipSpace();
                if (Pos < S.size() && S[Pos] == ',') { Pos++; continue; }
                if (Pos < S.size() && S[Pos] == ']') { Pos++; return true; }
                return false;
            }
        }
        if (c == '"')
        {
            std::string s;
            if (!ParseString(s)) return false;
            out = JsonValue(std::move(s));
            return true;
        }
        if (c == 't') { if (!Literal("true")) return false; out = JsonValue(true); return true; }
        if (c == 'f') { if (!Literal("false")) return false; out = JsonValue(false); return true; }
        if (c == 'n') { if (!Literal("null")) return false; out = JsonValue(); return true; }
        double d;
        if (!ParseNumber(d)) return false;
        out = JsonValue(d);
        return true;
    }
};

bool JsonValue::Parse(const std::string& text, JsonValue& out)
{
    JsonParser p(text);
    JsonValue v;
    if (!p.ParseDocument(v)) return false;
    out = std::move(v);
    return true;
}

}
