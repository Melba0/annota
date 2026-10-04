// Annota - json.cpp
#include "json.hpp"
#include "common.hpp"
#include <cstdio>
#include <cmath>

namespace annota {

static void escapeTo(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    out += '"';
}

static void dumpTo(const Json& j, std::string& out, bool pretty, int indent) {
    std::string pad, pad2;
    if (pretty) {
        pad.assign((size_t)indent * 2, ' ');
        pad2.assign((size_t)(indent + 1) * 2, ' ');
    }
    switch (j.t) {
        case Json::T::Null: out += "null"; break;
        case Json::T::Bool: out += j.b ? "true" : "false"; break;
        case Json::T::Num:
            if (j.isInt) out += formatInt((long long)j.num);
            else out += formatDouble(j.num);
            break;
        case Json::T::Str: escapeTo(j.str, out); break;
        case Json::T::Arr: {
            if (j.arr.empty()) { out += "[]"; break; }
            out += '[';
            for (size_t i = 0; i < j.arr.size(); i++) {
                if (i) out += ',';
                if (pretty) { out += '\n'; out += pad2; }
                dumpTo(j.arr[i], out, pretty, indent + 1);
            }
            if (pretty) { out += '\n'; out += pad; }
            out += ']';
            break;
        }
        case Json::T::Obj: {
            if (j.obj.empty()) { out += "{}"; break; }
            out += '{';
            for (size_t i = 0; i < j.obj.size(); i++) {
                if (i) out += ',';
                if (pretty) { out += '\n'; out += pad2; }
                escapeTo(j.obj[i].first, out);
                out += ':';
                if (pretty) out += ' ';
                dumpTo(j.obj[i].second, out, pretty, indent + 1);
            }
            if (pretty) { out += '\n'; out += pad; }
            out += '}';
            break;
        }
    }
}

std::string jsonDump(const Json& j, bool pretty, int indent) {
    std::string out;
    dumpTo(j, out, pretty, indent);
    return out;
}

namespace {
struct Parser {
    const std::string& s;
    size_t p = 0;
    std::string err;
    explicit Parser(const std::string& src) : s(src) {}

    void ws() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) p++;
    }
    bool fail(const std::string& m) { if (err.empty()) err = m; return false; }

    bool value(Json& out) {
        ws();
        if (p >= s.size()) return fail("unexpected end of input");
        char c = s[p];
        if (c == '{') return object(out);
        if (c == '[') return array(out);
        if (c == '"') {
            out.t = Json::T::Str;
            return string(out.str);
        }
        if (c == 't') { if (s.compare(p, 4, "true") == 0) { p += 4; out = Json::boolean(true); return true; } return fail("bad literal"); }
        if (c == 'f') { if (s.compare(p, 5, "false") == 0) { p += 5; out = Json::boolean(false); return true; } return fail("bad literal"); }
        if (c == 'n') { if (s.compare(p, 4, "null") == 0) { p += 4; out = Json::null(); return true; } return fail("bad literal"); }
        return number(out);
    }
    bool number(Json& out) {
        size_t start = p;
        if (p < s.size() && (s[p] == '-' || s[p] == '+')) p++;
        bool isInt = true;
        while (p < s.size() && (isdigit((unsigned char)s[p]) || s[p] == '.' || s[p] == 'e' || s[p] == 'E' ||
                                s[p] == '+' || s[p] == '-')) {
            if (s[p] == '.' || s[p] == 'e' || s[p] == 'E') isInt = false;
            p++;
        }
        if (p == start) return fail("expected a value");
        std::string num = s.substr(start, p - start);
        out.t = Json::T::Num;
        out.num = std::strtod(num.c_str(), nullptr);
        out.isInt = isInt;
        return true;
    }
    bool string(std::string& out) {
        if (p >= s.size() || s[p] != '"') return fail("expected a string");
        p++;
        while (p < s.size() && s[p] != '"') {
            if (s[p] == '\\' && p + 1 < s.size()) {
                p++;
                switch (s[p]) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case '/': out += '/'; break;
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case 'u': {
                        if (p + 4 >= s.size()) return fail("bad \\u escape");
                        unsigned cp = 0;
                        for (int i = 0; i < 4; i++) {
                            char h = s[p + 1 + (size_t)i];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                            else return fail("bad \\u escape");
                        }
                        p += 4;
                        // encode as UTF-8 (BMP only, which is all the IDE layer needs)
                        if (cp < 0x80) out += (char)cp;
                        else if (cp < 0x800) {
                            out += (char)(0xC0 | (cp >> 6));
                            out += (char)(0x80 | (cp & 0x3F));
                        } else {
                            out += (char)(0xE0 | (cp >> 12));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: out += s[p]; break;
                }
                p++;
                continue;
            }
            out += s[p++];
        }
        if (p >= s.size()) return fail("unterminated string");
        p++;
        return true;
    }
    bool array(Json& out) {
        out = Json::array();
        p++;   // '['
        ws();
        if (p < s.size() && s[p] == ']') { p++; return true; }
        while (true) {
            Json v;
            if (!value(v)) return false;
            out.arr.push_back(std::move(v));
            ws();
            if (p < s.size() && s[p] == ',') { p++; continue; }
            if (p < s.size() && s[p] == ']') { p++; return true; }
            return fail("expected ',' or ']'");
        }
    }
    bool object(Json& out) {
        out = Json::object();
        p++;   // '{'
        ws();
        if (p < s.size() && s[p] == '}') { p++; return true; }
        while (true) {
            ws();
            std::string key;
            if (!string(key)) return false;
            ws();
            if (p >= s.size() || s[p] != ':') return fail("expected ':'");
            p++;
            Json v;
            if (!value(v)) return false;
            out.obj.emplace_back(key, std::move(v));
            ws();
            if (p < s.size() && s[p] == ',') { p++; continue; }
            if (p < s.size() && s[p] == '}') { p++; return true; }
            return fail("expected ',' or '}'");
        }
    }
};
} // namespace

bool jsonParse(const std::string& text, Json& out, std::string* err) {
    Parser ps(text);
    if (!ps.value(out)) {
        if (err) *err = ps.err;
        return false;
    }
    ps.ws();
    if (ps.p != text.size()) {
        if (err) *err = "trailing content";
        return false;
    }
    return true;
}

} // namespace annota
