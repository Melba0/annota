// Annota - native/fast.cpp : an example of the C++ linking interface.
//
// This file is a normal translation unit: it is linked into the interpreter (or built into a
// plugin, see docs/ffi.md) and registers the `fast` module.  Nothing in the language core knows
// about it - `use fast` finds it because the FFI registry says so, and `fast.sum(xs)` is an
// ordinary module member call.  That is the intended way to make the standard library faster:
// write the hot kernel in C++, link it, done.
#include "../src/ffi.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace annota;

ANNOTA_MODULE(fast)
    mod.value("version", Value::str("1.0"));
    mod.value("author", Value::str("annota ffi example"));

    // ---------------------------------------------------------------- numeric kernels
    mod.fn("sum", [](VM& vm, std::vector<Value>& a) {
        std::vector<Value> xs = ffiList(vm, ffiArg(a, 0), "fast.sum");
        int64_t total = 0;
        double ftotal = 0.0;
        bool isFloat = false;
        for (auto& x : xs) {
            if (!x.isNumber()) vm.throwError("fast.sum expects numbers");
            if (x.t == VT::Float) { isFloat = true; ftotal += x.f; }
            else { total += x.asInt(); ftotal += (double)x.asInt(); }
        }
        return isFloat ? Value::real(ftotal) : Value::integer(total);
    });

    mod.fn("dot", [](VM& vm, std::vector<Value>& a) {
        std::vector<Value> xs = ffiList(vm, ffiArg(a, 0), "fast.dot");
        std::vector<Value> ys = ffiList(vm, ffiArg(a, 1), "fast.dot");
        size_t n = std::min(xs.size(), ys.size());
        double acc = 0.0;
        bool allInt = true;
        for (size_t i = 0; i < n; i++) {
            if (!xs[i].isNumber() || !ys[i].isNumber()) vm.throwError("fast.dot expects numbers");
            acc += xs[i].asFloat() * ys[i].asFloat();
            if (xs[i].t != VT::Int || ys[i].t != VT::Int) allInt = false;
        }
        return allInt ? Value::integer((int64_t)acc) : Value::real(acc);
    });

    mod.fn("prefix_sums", [](VM& vm, std::vector<Value>& a) {
        std::vector<Value> xs = ffiList(vm, ffiArg(a, 0), "fast.prefix_sums");
        std::vector<Value> out;
        out.reserve(xs.size());
        int64_t acc = 0;
        for (auto& x : xs) {
            if (x.t != VT::Int) vm.throwError("fast.prefix_sums expects ints");
            acc += x.i;
            out.push_back(Value::integer(acc));
        }
        return Value::list(out);
    });

    mod.fn("clamp_all", [](VM& vm, std::vector<Value>& a) {
        std::vector<Value> xs = ffiList(vm, ffiArg(a, 0), "fast.clamp_all");
        int64_t lo = ffiInt(vm, ffiArg(a, 1), "fast.clamp_all");
        int64_t hi = ffiInt(vm, ffiArg(a, 2), "fast.clamp_all");
        std::vector<Value> out;
        out.reserve(xs.size());
        for (auto& x : xs) {
            int64_t v = ffiInt(vm, x, "fast.clamp_all");
            out.push_back(Value::integer(v < lo ? lo : (v > hi ? hi : v)));
        }
        return Value::list(out);
    });

    mod.fn("moving_average", [](VM& vm, std::vector<Value>& a) {
        std::vector<Value> xs = ffiList(vm, ffiArg(a, 0), "fast.moving_average");
        int64_t w = ffiInt(vm, ffiArg(a, 1), "fast.moving_average");
        std::vector<Value> out;
        if (w <= 0 || (size_t)w > xs.size()) return Value::list(out);
        double acc = 0.0;
        for (size_t i = 0; i < xs.size(); i++) {
            acc += xs[i].asFloat();
            if (i >= (size_t)w) acc -= xs[i - (size_t)w].asFloat();
            if (i + 1 >= (size_t)w) out.push_back(Value::real(acc / (double)w));
        }
        return Value::list(out);
    });

    mod.fn("histogram", [](VM& vm, std::vector<Value>& a) {
        std::vector<Value> xs = ffiList(vm, ffiArg(a, 0), "fast.histogram");
        int64_t buckets = ffiInt(vm, ffiArg(a, 1), "fast.histogram");
        if (buckets <= 0) vm.throwError("fast.histogram expects a positive bucket count");
        if (xs.empty()) return Value::list({});
        double lo = xs[0].asFloat(), hi = xs[0].asFloat();
        for (auto& x : xs) {
            double v = x.asFloat();
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        std::vector<int64_t> counts((size_t)buckets, 0);
        if (hi == lo) {
            counts[0] = (int64_t)xs.size();
        } else {
            double width = (hi - lo) / (double)buckets;
            for (auto& x : xs) {
                int64_t idx = (int64_t)std::floor((x.asFloat() - lo) / width);
                if (idx >= buckets) idx = buckets - 1;
                if (idx < 0) idx = 0;
                counts[(size_t)idx]++;
            }
        }
        std::vector<Value> out;
        for (int64_t c : counts) out.push_back(Value::integer(c));
        return Value::list(out);
    });

    // a real kernel: a sieve of Eratosthenes in C++ (the script version is ~50x slower)
    mod.fn("prime_count", [](VM& vm, std::vector<Value>& a) {
        int64_t limit = ffiInt(vm, ffiArg(a, 0), "fast.prime_count");
        if (limit < 2) return Value::integer(0);
        if (limit > 200000000) vm.throwError("fast.prime_count: limit too large");
        std::vector<uint8_t> flags((size_t)limit + 1, 1);
        flags[0] = flags[1] = 0;
        for (int64_t p = 2; p * p <= limit; p++) {
            if (!flags[(size_t)p]) continue;
            for (int64_t m = p * p; m <= limit; m += p) flags[(size_t)m] = 0;
        }
        int64_t c = 0;
        for (int64_t i = 2; i <= limit; i++) c += flags[(size_t)i];
        return Value::integer(c);
    });

    mod.fn("primes", [](VM& vm, std::vector<Value>& a) {
        int64_t limit = ffiInt(vm, ffiArg(a, 0), "fast.primes");
        std::vector<Value> out;
        if (limit < 2) return Value::list(out);
        if (limit > 200000000) vm.throwError("fast.primes: limit too large");
        std::vector<uint8_t> flags((size_t)limit + 1, 1);
        flags[0] = flags[1] = 0;
        for (int64_t p = 2; p * p <= limit; p++) {
            if (!flags[(size_t)p]) continue;
            for (int64_t m = p * p; m <= limit; m += p) flags[(size_t)m] = 0;
        }
        for (int64_t i = 2; i <= limit; i++)
            if (flags[(size_t)i]) out.push_back(Value::integer(i));
        return Value::list(out);
    });

    mod.fn("checksum", [](VM& vm, std::vector<Value>& a) {
        const Value& s = ffiArg(a, 0);
        if (s.t != VT::Str) vm.throwError("fast.checksum expects a string");
        uint64_t h = 1469598103934665603ull;
        for (unsigned char c : s.o->str) {
            h ^= c;
            h *= 1099511628211ull;
        }
        return Value::integer((int64_t)h);
    });

    // ---------------------------------------------------------------- talking back to Annota
    // A C++ kernel can call an Annota closure: this is the "communication" direction that makes
    // the interface useful for callbacks, comparators and timing harnesses.
    mod.fn("count_if", [](VM& vm, std::vector<Value>& a) {
        std::vector<Value> xs = ffiList(vm, ffiArg(a, 0), "fast.count_if");
        const Value& pred = ffiArg(a, 1);
        int64_t n = 0;
        for (auto& x : xs) {
            Value r = vm.callSync(pred, {x});
            if (vm.truthy(r)) n++;
        }
        return Value::integer(n);
    });

    mod.fn("map_call", [](VM& vm, std::vector<Value>& a) {
        std::vector<Value> xs = ffiList(vm, ffiArg(a, 0), "fast.map_call");
        const Value& f = ffiArg(a, 1);
        std::vector<Value> out;
        out.reserve(xs.size());
        for (auto& x : xs) out.push_back(vm.callSync(f, {x}));
        return Value::list(out);
    });

    mod.fn("reduce_call", [](VM& vm, std::vector<Value>& a) {
        std::vector<Value> xs = ffiList(vm, ffiArg(a, 0), "fast.reduce_call");
        const Value& f = ffiArg(a, 1);
        Value acc = ffiArg(a, 2);
        for (auto& x : xs) acc = vm.callSync(f, {acc, x});
        return acc;
    });

    // time an Annota closure from C++ (monotonic ms)
    mod.fn("time_call", [](VM& vm, std::vector<Value>& a) {
        const Value& f = ffiArg(a, 0);
        int64_t iters = a.size() > 1 ? ffiInt(vm, a[1], "fast.time_call") : 1;
        auto now = []() {
            using namespace std::chrono;
            return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
        };
        double t0 = now();
        for (int64_t i = 0; i < iters; i++) vm.callSync(f, {});
        return Value::real(now() - t0);
    });
ANNOTA_END_MODULE
