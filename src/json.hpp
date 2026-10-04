// Annota - json.hpp : a tiny ordered JSON value used by the analysis reports and the LSP layer.
#pragma once
#include <string>
#include <vector>
#include <utility>

namespace annota {

struct Json {
    enum class T { Null, Bool, Num, Str, Arr, Obj };
    T t = T::Null;
    bool b = false;
    double num = 0;
    bool isInt = false;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    Json() = default;
    static Json null() { return Json(); }
    static Json boolean(bool v) { Json j; j.t = T::Bool; j.b = v; return j; }
    static Json number(double v) { Json j; j.t = T::Num; j.num = v; return j; }
    static Json integer(long long v) { Json j; j.t = T::Num; j.num = (double)v; j.isInt = true; return j; }
    static Json string(const std::string& s) { Json j; j.t = T::Str; j.str = s; return j; }
    static Json array() { Json j; j.t = T::Arr; return j; }
    static Json object() { Json j; j.t = T::Obj; return j; }

    bool isNull() const { return t == T::Null; }
    Json& set(const std::string& key, Json v) {
        for (auto& kv : obj) if (kv.first == key) { kv.second = std::move(v); return *this; }
        obj.emplace_back(key, std::move(v));
        return *this;
    }
    Json& push(Json v) { arr.push_back(std::move(v)); return *this; }
    const Json* find(const std::string& key) const {
        for (auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string s(const std::string& key, const std::string& def = "") const {
        const Json* v = find(key);
        return (v && v->t == T::Str) ? v->str : def;
    }
    long long i(const std::string& key, long long def = 0) const {
        const Json* v = find(key);
        return (v && v->t == T::Num) ? (long long)v->num : def;
    }
    std::string asString() const { return t == T::Str ? str : ""; }
};

// Compact (or pretty) serialisation; strings are escaped, output is UTF-8.
std::string jsonDump(const Json& j, bool pretty = false, int indent = 0);
// Parses UTF-8 JSON; returns false and fills `err` on failure.
bool jsonParse(const std::string& text, Json& out, std::string* err = nullptr);

} // namespace annota
