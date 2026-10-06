// Annota - plugins/hello.cpp : a plugin you build separately, no interpreter rebuild.
//
//   annota plugin build plugins/hello.cpp      -> build/plugins/hello.{dll,so}
//   annota run examples/plugin.ant             -> `use hello` loads it
//
// The file may live anywhere; only src/ffi.hpp (the linking interface) is needed.
#include "../src/ffi.hpp"
#include <algorithm>
#include <cctype>
#include <string>

using namespace annota;

ANNOTA_MODULE(hello)
    mod.value("version", Value::str("1.0"));
    mod.value("origin", Value::str("plugins/hello.cpp (动态加载，没有重编译解释器)"));

    mod.fn("greet", [](VM& vm, ValueList& a) {
        std::string who = a.empty() ? std::string("world") : vm.toStr(a[0]);
        return Value::str("hello, " + who + "!");
    });

    // a tiny native kernel, to show the speed difference is real
    mod.fn("sum_squares", [](VM& vm, ValueList& a) {
        int64_t n = ffiInt(vm, ffiArg(a, 0), "hello.sum_squares");
        int64_t total = 0;
        for (int64_t i = 1; i <= n; i++) total += i * i;
        return Value::integer(total);
    });

    mod.fn("shout_all", [](VM& vm, ValueList& a) {
        std::vector<Value> xs = ffiItems(vm, ffiArg(a, 0));
        std::vector<Value> out;
        out.reserve(xs.size());
        for (auto& x : xs) {
            std::string s = vm.toStr(x);
            for (auto& c : s) c = (char)std::toupper((unsigned char)c);
            out.push_back(Value::str(s));
        }
        return Value::list(out);
    });
ANNOTA_END_MODULE
