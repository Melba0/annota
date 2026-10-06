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

// Meyers singleton, defined once in src/ffi.cpp: an inline definition would give every plugin
// its own registry, and then a plugin's registration would never reach the interpreter.
FfiRegistry& ffiRegistry();

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
// install a single registered module/function (used for plugins loaded on demand)
inline void ffiInstallNamed(VM& vm, const std::string& name) {
    FfiRegistry& r = ffiRegistry();
    auto f = r.functions.find(name);
    if (f != r.functions.end() && !vm.globals.count(name))
        vm.globals[name] = std::make_shared<Value>(vm.makeNative(name, f->second));
    auto m = r.modules.find(name);
    if (m != r.modules.end() && !vm.globals.count(name)) {
        Value mod = Value::module(name);
        for (auto& p : m->second) mod.o->map[p.first] = vm.makeNative(name + "." + p.first, p.second);
        auto c = r.constants.find(name);
        if (c != r.constants.end())
            for (auto& p : c->second) mod.o->map[p.first] = p.second;
        vm.globals[name] = std::make_shared<Value>(mod);
    }
    // constants may arrive after the module object (or without one)
    auto it = vm.globals.find(name);
    auto c = r.constants.find(name);
    if (it != vm.globals.end() && it->second && it->second->t == VT::Module && c != r.constants.end())
        for (auto& p : c->second) it->second->o->map[p.first] = p.second;
}

inline bool ffiHasAny(const std::string& name) {
    return ffiHasModule(name) || ffiHasFunction(name);
}

inline void ffiInstallAll(VM& vm) {
    for (auto& n : ffiModuleNames()) ffiInstallNamed(vm, n);
    for (auto& n : ffiFunctionNames()) ffiInstallNamed(vm, n);
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
// Build a shared library that registers itself; `annota plugin build` produces one, and it is
// loaded automatically by `use <module>` (or explicitly with `--plugin` / `ANNOTA_PLUGIN`).
// Nothing in the core needs to know about it - see docs/ffi.md.
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

// Look for a plugin that provides `name` and load it.  `dirs` are tried in order; on Windows
// `<name>.dll`, elsewhere `lib<name>.so` (with and without a `plugins/` subdirectory).
inline bool ffiLoadPluginFor(const std::string& name, const std::vector<std::string>& dirs,
                             std::string* tried = nullptr) {
#ifdef _WIN32
    const char* exts[] = {".dll"};
#else
    const char* exts[] = {".so"};
#endif
    std::vector<std::string> cands;
    for (auto& d : dirs) {
        for (const char* e : exts) {
            // accept both spellings: `name.so` and the conventional `libname.so`
            for (const char* p : {"", "lib"}) {
                cands.push_back(d + "/" + p + name + e);
                cands.push_back(d + "/plugins/" + p + name + e);
            }
        }
    }
    for (auto& c : cands) {
        std::FILE* f = std::fopen(c.c_str(), "rb");
        if (!f) continue;
        std::fclose(f);
        std::string err;
        if (!ffiLoadPlugin(c, &err)) {
            if (tried) *tried = err;
            continue;
        }
        if (ffiHasAny(name)) return true;          // the plugin registered what we asked for
        if (tried) *tried = "插件 " + c + " 没有注册模块 '" + name + "'";
    }
    return false;
}

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
