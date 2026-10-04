// Annota - common.hpp : error types and small helpers.
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <sstream>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace annota {

struct Span { int line = 1; int col = 1; };

// ---------------------------------------------------------------- diagnostics
struct AnnotaError {
    std::string message;
    int line = 0;
    std::string file;
    bool warning = false;
};

// lexical / syntax / macro / compile-time failure
struct CompileError {
    std::string message;
    int line = 1;
    CompileError(std::string m, int l = 1) : message(std::move(m)), line(l) {}
};

// run-time failure raised from bytecode or a native function
struct VMError {
    std::string message;
    int line = 1;
    VMError(std::string m, int l = 1) : message(std::move(m)), line(l) {}
};

// ---------------------------------------------------------------- helpers
inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

inline bool hasSuffix(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

// Format a double the way a scripting language would (shortest that round-trips).
inline std::string formatDouble(double d) {
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d < 0 ? "-inf" : "inf";
    if (d == (double)(int64_t)d && std::fabs(d) < 1e15) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.1f", d);
        return buf;
    }
    char buf[64];
    for (int prec = 15; prec <= 17; prec++) {
        std::snprintf(buf, sizeof(buf), "%.*g", prec, d);
        if (std::strtod(buf, nullptr) == d) break;
    }
    return buf;
}

// integer -> string
inline std::string formatInt(int64_t i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", (long long)i);
    return buf;
}

} // namespace annota
