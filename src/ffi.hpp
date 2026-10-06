// Annota - ffi.hpp : the C++ linking interface.
//
// The point of this file is that **new native capability no longer needs a language core
// change**.  A C++ file (linked into the binary, or built as a plugin and loaded at start-up)
// registers globals and modules; Annota then just does `use <module>` and calls
// `<module>.<member>(...)`, exactly like a script module.
//
//   ANNOTA_MODULE(fast)
//       mod.fn("sum", [](VM& vm, std::vector<Value>& a) {
//           int64_t total = 0;
//           for (auto& x : a[0].o->items) total += x.asInt();
//           return Value::integer(total);
//       });
//   ANNOTA_END_MODULE
//
// Registration goes through a function-local static registry, so it is safe from any static
// initialiser order.  `ffiInstallAll(vm)` is called while the interpreter installs its builtins.
#pragma once
#include "vm.hpp"
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace annota {

using ValueList = std::vector<Value>;        // native functions take one of these
using FfiFn = std::function<Value(VM&, ValueList&)>;

struct FfiRegistry {
    std::map<std::string, FfiFn> functions;                                     // plain globals
    std::map<std::string, std::vector<std::pair<std::string, FfiFn>>> modules;  // module members
    std::map<std::string, std::vector<std::pair<std::string, Value>>> constants;
};

// Meyers singleton: a static registrar may run before any other global, but this is always ready
inline FfiRegistry& ffiRegistry() {
    static FfiRegistry r;
    return r;
}

// ---------------------------------------------------------------- registration
inline void ffiFunction(const std::string& name, FfiFn fn) {
    ffiRegistry().functions[name] = std::move(fn);
}

inline void ffiModuleMember(const std::string& module, const std::string& member, FfiFn fn) {
    auto& v = ffiRegistry().modules[module];
    for (auto& p : v) {
        if (p.first == member) { p.second = std::move(fn); return; }
    }
    v.push_back({member, std::move(fn)});
}

inline void ffiModuleValue(const std::string& module, const std::string& member, Value v) {
    ffiRegistry().constants[module].push_back({member, std::move(v)});
}

inline bool ffiHasModule(const std::string& name) {
    FfiRegistry& r = ffiRegistry();
    return r.modules.count(name) > 0 || r.constants.count(name) > 0;
}

inline bool ffiHasFunction(const std::string& name) {
    return ffiRegistry().functions.count(name) > 0;
}

inline std::vector<std::string> ffiFunctionNames() {
    std::vector<std::string> out;
    for (auto& kv : ffiRegistry().functions) out.push_back(kv.first);
    return out;
}

// members of one module (used by `annota ide docs`)
inline std::vector<std::string> ffiModuleMembers(const std::string& module) {
    std::vector<std::string> out;
    auto it = ffiRegistry().modules.find(module);
    if (it != ffiRegistry().modules.end())
        for (auto& p : it->second) out.push_back(p.first);
    auto ic = ffiRegistry().constants.find(module);
    if (ic != ffiRegistry().constants.end())
        for (auto& p : ic->second) out.push_back(p.first);
    return out;
}

inline std::vector<std::string> ffiModuleNames() {
    std::vector<std::string> out;
    for (auto& kv : ffiRegistry().modules) out.push_back(kv.first);
    for (auto& kv : ffiRegistry().constants) {
        bool seen = false;
        for (auto& s : out) if (s == kv.first) seen = true;
        if (!seen) out.push_back(kv.first);
    }
    return out;
}

// ---------------------------------------------------------------- installation
// Every registered global/module lands in the VM's globals, so `use <module>` resolves and
// `<module>.<member>(...)` is an ordinary field call.
inline void ffiInstallAll(VM& vm) {
    FfiRegistry& r = ffiRegistry();
    for (auto& kv : r.functions) {
        vm.globals[kv.first] = std::make_shared<Value>(vm.makeNative(kv.first, kv.second));
    }
    for (auto& kv : r.modules) {
        Value m = Value::module(kv.first);
        for (auto& p : kv.second) {
            m.o->map[p.first] = vm.makeNative(kv.first + "." + p.first, p.second);
        }
        vm.globals[kv.first] = std::make_shared<Value>(m);
    }
    for (auto& kv : r.constants) {
        Value* mod = nullptr;
        auto it = vm.globals.find(kv.first);
        if (it != vm.globals.end() && it->second->t == VT::Module) mod = it->second.get();
        for (auto& p : kv.second) {
            if (mod) mod->o->map[p.first] = p.second;
            else vm.globals[kv.first + "." + p.first] = std::make_shared<Value>(p.second);
        }
    }
}

// A convenient registration handle used by the ANNOTA_MODULE macro.
struct FfiModule {
    std::string name;
    explicit FfiModule(std::string n) : name(std::move(n)) {}
    FfiModule& fn(const std::string& member, FfiFn f) {
        ffiModuleMember(name, member, std::move(f));
        return *this;
    }
    FfiModule& value(const std::string& member, Value v) {
        ffiModuleValue(name, member, std::move(v));
        return *this;
    }
};

// ---------------------------------------------------------------- plugins
// Build a shared library that registers itself; load it at start-up with `--plugin <file>` or
// `ANNOTA_PLUGIN=<file>`.  Nothing in the core needs to know about it - see docs/ffi.md.
#ifdef _WIN32
inline int ffiLoadPlugin(const std::string& path, std::string* error = nullptr) {
    HMODULE h = LoadLibraryA(path.c_str());
    if (!h) {
        if (error) *error = "cannot load plugin '" + path + "'";
        return 0;
    }
    // an optional entry point; a plugin that registers from a static initialiser needs none
    using InitFn = void (*)();
    if (auto init = (InitFn)GetProcAddress(h, "annota_plugin_init")) init();
    return 1;
}
#else
inline int ffiLoadPlugin(const std::string& path, std::string* error = nullptr) {
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        if (error) *error = std::string("cannot load plugin '") + path + "': " + dlerror();
        return 0;
    }
    using InitFn = void (*)();
    if (auto init = (InitFn)dlsym(h, "annota_plugin_init")) init();
    return 1;
}
#endif

// ---------------------------------------------------------------- small helpers for authors
inline const Value& ffiArg(std::vector<Value>& a, size_t i) {
    static const Value nil;
    return i < a.size() ? a[i] : nil;
}

// keyword arguments a caller passed, or null when there are none
inline const std::unordered_map<std::string, Value>* ffiNamed(VM& vm) {
    return vm.pendingNamed.o ? &vm.pendingNamed.o->map : nullptr;
}

inline std::vector<Value> ffiItems(VM& vm, const Value& v) {
    if (v.t == VT::List || v.t == VT::Tuple) return v.o->items;
    if (v.t == VT::Str || v.t == VT::Bytes || v.t == VT::Iter) {
        Value it = vm.makeIter(v);
        return it.o->items;
    }
    if (v.t == VT::Map || v.t == VT::Module) {
        std::vector<Value> out;
        for (auto& kv : v.o->map) out.push_back(Value::tuple({Value::str(kv.first), kv.second}));
        return out;
    }
    vm.throwError(std::string("expected a sequence but got ") + v.typeName());
}

inline int64_t ffiInt(VM& vm, const Value& v, const char* what) {
    if (!v.isNumber()) vm.throwError(std::string(what) + " expects a number");
    return v.asInt();
}

inline double ffiFloat(VM& vm, const Value& v, const char* what) {
    if (!v.isNumber()) vm.throwError(std::string(what) + " expects a number");
    return v.asFloat();
}

inline std::vector<Value> ffiList(VM& vm, const Value& v, const char* what) {
    if (v.t == VT::List || v.t == VT::Tuple) return v.o->items;
    if (v.t == VT::Array) {
        std::vector<Value> out;
        int64_t n = v.arrayCount();
        for (int64_t i = 0; i < n; i++) out.push_back(vm.getIndex(v, Value::integer(i)));
        return out;
    }
    vm.throwError(std::string(what) + " expects a list");
}

} // namespace annota

// Define a native module:
//     ANNOTA_MODULE(name) { mod.fn("member", fn); ... } ANNOTA_END_MODULE
#define ANNOTA_MODULE(NAME)                                                              \
    static ::annota::FfiModule annota_module_##NAME(#NAME);                              \
    static const bool annota_module_registered_##NAME = []() {                           \
        ::annota::FfiModule& mod = annota_module_##NAME;                                 \
        (void)mod;

#define ANNOTA_END_MODULE                                                                \
        return true;                                                                     \
    }();

// Define a global native function:
//     ANNOTA_FUNCTION(name, [](VM& vm, std::vector<Value>& a) { ... });
#define ANNOTA_FUNCTION(NAME, FN)                                                        \
    static const bool annota_function_registered_##NAME = []() {                         \
        ::annota::ffiFunction(#NAME, FN);                                                \
        return true;                                                                     \
    }();
