// Annota - native/seq_native.cpp : the native kernels behind lib/seq.mod.
//
// These used to be builtins compiled into the interpreter.  They now live here and are
// registered through the FFI (src/ffi.hpp), which is the layering the project wants:
//
//     language core        lexer / parser / compiler / VM
//     runtime primitives   the small `_`-prefixed layer (I/O, time, sockets, ...)
//     native kernels       this file: hot algorithms, linked in, no core change needed
//     script standard lib  lib/*.mod wraps the kernels into the friendly API
//
// `lib/seq.mod` calls `seqnative.sort`-style members, so the dependency is explicit.  Adding an
// algorithm here does not touch src/builtins.cpp at all.
#include "../src/ffi.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>

using namespace annota;

namespace {

// ordering used by every kernel here: numbers by value, strings lexicographically, then any
// user type that provides `__lt__`
bool orderLess(VM& vm, const Value& a, const Value& b, bool& comparable) {
    if (a.isNumber() && b.isNumber()) return a.asFloat() < b.asFloat();
    if (a.t == VT::Str && b.t == VT::Str) return a.o->str < b.o->str;
    if (a.t == VT::Bool && b.t == VT::Bool) return (a.b ? 1 : 0) < (b.b ? 1 : 0);
    Value lt;
    if (a.t == VT::Instance && vm.callMagic(a, "__lt__", {b}, lt)) return vm.truthy(lt);
    comparable = false;
    return false;
}

bool reverseRequested(VM& vm, std::vector<Value>& a, size_t positional) {
    bool reverse = a.size() > positional && vm.truthy(a[positional]);
    if (auto* named = ffiNamed(vm)) {
        auto it = named->find("reverse");
        if (it != named->end()) reverse = vm.truthy(it->second);
    }
    return reverse;
}

} // namespace

ANNOTA_MODULE(seqnative)
    // stable sort (numbers / strings / `__lt__`), optionally descending
    mod.fn("sorted", [](VM& vm, ValueList& a) {
        if (a.empty()) vm.throwError("seqnative.sorted expects a sequence");
        std::vector<Value> items = ffiItems(vm, a[0]);
        bool reverse = reverseRequested(vm, a, 1);
        bool comparable = true;
        std::stable_sort(items.begin(), items.end(), [&](const Value& x, const Value& y) {
            bool r = orderLess(vm, x, y, comparable);
            if (!comparable) return false;
            return r;
        });
        if (!comparable) vm.throwError("seqnative.sorted: elements are not comparable");
        if (reverse) std::reverse(items.begin(), items.end());
        return Value::list(items);
    });

    // k-th smallest in O(n) average, without sorting the whole sequence
    mod.fn("nth", [](VM& vm, ValueList& a) {
        if (a.size() < 2) vm.throwError("seqnative.nth expects a sequence and an index");
        std::vector<Value> items = ffiItems(vm, a[0]);
        if (items.empty()) return Value::null();
        int64_t k = ffiInt(vm, a[1], "seqnative.nth");
        if (k < 0) k = 0;
        if (k >= (int64_t)items.size()) k = (int64_t)items.size() - 1;
        bool comparable = true;
        std::nth_element(items.begin(), items.begin() + k, items.end(),
                         [&](const Value& x, const Value& y) {
                             bool r = orderLess(vm, x, y, comparable);
                             if (!comparable) return false;
                             return r;
                         });
        if (!comparable) vm.throwError("seqnative.nth: elements are not comparable");
        return items[(size_t)k];
    });

    // stable permutation that sorts the sequence: lets a script sort *by any key* natively
    mod.fn("argsort", [](VM& vm, ValueList& a) {
        if (a.empty()) vm.throwError("seqnative.argsort expects a sequence");
        std::vector<Value> items = ffiItems(vm, a[0]);
        bool reverse = reverseRequested(vm, a, 1);
        std::vector<int64_t> idx(items.size());
        for (size_t i = 0; i < idx.size(); i++) idx[i] = (int64_t)i;
        bool comparable = true;
        std::stable_sort(idx.begin(), idx.end(), [&](int64_t x, int64_t y) {
            bool r = reverse ? orderLess(vm, items[(size_t)y], items[(size_t)x], comparable)
                             : orderLess(vm, items[(size_t)x], items[(size_t)y], comparable);
            if (!comparable) return false;
            return r;
        });
        if (!comparable) vm.throwError("seqnative.argsort: elements are not comparable");
        std::vector<Value> out;
        out.reserve(idx.size());
        for (int64_t i : idx) out.push_back(Value::integer(i));
        return Value::list(out);
    });

    // binary search on an already sorted sequence
    // Binary search on an already sorted sequence.  `ffiItems` would copy the whole sequence on
    // every call (the standard library borrows its argument with `lend`, so nothing else copies
    // it) - instead probe the container in place, which is log2(n) reads and no allocation.
    auto at = [](VM& vm, const Value& seq, size_t i) -> Value {
        if (seq.t == VT::List || seq.t == VT::Tuple) return seq.o->items[i];
        return vm.getIndex(seq, Value::integer((int64_t)i));
    };
    auto seqSize = [](VM& vm, const Value& seq) -> size_t {
        if (seq.t == VT::List || seq.t == VT::Tuple || seq.t == VT::Iter) return seq.o->items.size();
        if (seq.t == VT::Str) return seq.o->str.size();
        if (seq.t == VT::Bytes) return seq.o->bytes.size();
        if (seq.t == VT::Array) return (size_t)seq.arrayCount();
        vm.throwError(std::string("expected a sequence but got ") + seq.typeName());
    };

    mod.fn("lower_bound", [at, seqSize](VM& vm, ValueList& a) {
        if (a.size() < 2) vm.throwError("seqnative.lower_bound expects a sequence and a value");
        const Value& seq = a[0];
        const Value& key = a[1];
        size_t lo = 0, hi = seqSize(vm, seq);
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            bool comparable = true;
            bool less = orderLess(vm, at(vm, seq, mid), key, comparable);
            if (!comparable) vm.throwError("seqnative.lower_bound: elements are not comparable");
            if (less) lo = mid + 1;
            else hi = mid;
        }
        return Value::integer((int64_t)lo);
    });

    mod.fn("upper_bound", [at, seqSize](VM& vm, ValueList& a) {
        if (a.size() < 2) vm.throwError("seqnative.upper_bound expects a sequence and a value");
        const Value& seq = a[0];
        const Value& key = a[1];
        size_t lo = 0, hi = seqSize(vm, seq);
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            bool comparable = true;
            bool less = orderLess(vm, key, at(vm, seq, mid), comparable);
            if (!comparable) vm.throwError("seqnative.upper_bound: elements are not comparable");
            if (!less) lo = mid + 1;
            else hi = mid;
        }
        return Value::integer((int64_t)lo);
    });
ANNOTA_END_MODULE
