// Annota - builtins.cpp : built-in functions, pseudo methods, GUI components, std modules.
#include "builtins.hpp"
#include "ffi.hpp"
#include "sys_api.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>

namespace annota {

namespace fs = std::filesystem;

Value makeUiNode(const std::string& type, std::unordered_map<std::string, Value> attrs) {
    Value v;
    v.t = VT::UiNode;
    v.o = std::make_shared<Obj>();
    v.o->str = type;
    v.o->map = std::move(attrs);
    return v;
}

static std::unordered_map<std::string, Value>* namedOf(VM& vm) {
    return vm.pendingNamed.o ? &vm.pendingNamed.o->map : nullptr;
}

static Value argAt(std::vector<Value>& a, size_t i) {
    return i < a.size() ? a[i] : Value::null();
}

static int64_t asIndex(VM& vm, const Value& v, const char* what) {
    if (v.t != VT::Int) vm.throwError(std::string(what) + " expects an int index");
    return v.i;
}

static std::string asStr(VM& vm, const Value& v, const char* what) {
    if (v.t != VT::Str) vm.throwError(std::string(what) + " expects a string");
    return v.o->str;
}

// ---------------------------------------------------------------- type conversion
static bool tryToNumber(VM& vm, const Value& v, double& out) {
    (void)vm;
    switch (v.t) {
        case VT::Int: out = (double)v.i; return true;
        case VT::Float: out = v.f; return true;
        case VT::Bool: out = v.b ? 1 : 0; return true;
        case VT::Str: {
            const std::string& s = v.o->str;
            if (s.empty()) return false;
            char* end = nullptr;
            double d = std::strtod(s.c_str(), &end);
            if (end == s.c_str() || *end != '\0') return false;
            out = d;
            return true;
        }
        default: return false;
    }
}

static Value convertTo(VM& vm, const std::string& type, const Value& v) {
    if (type == "int") {
        // the boxed widths (`long long`, `long double`) convert through the same path
        if (v.t == VT::Wide || v.t == VT::LongDouble) return Value::integer(v.asInt());
        switch (v.t) {
            case VT::Int: return v;
            case VT::Float: return Value::integer((int64_t)v.f);
            case VT::Bool: return Value::integer(v.b ? 1 : 0);
            case VT::Str: {
                double d;
                if (!tryToNumber(vm, v, d)) vm.throwError("int(): cannot convert \"" + v.o->str + "\" to int");
                return Value::integer((int64_t)d);
            }
            case VT::Null: return Value::integer(0);
            case VT::Instance: {
                Value out;
                if (vm.callMagic(v, "__int__", {}, out)) return convertTo(vm, "int", out);
                break;
            }
            default: break;
        }
        vm.throwError(std::string("int(): cannot convert ") + v.typeName() + " to int");
    }
    if (type == "float") {
        double d;
        if (v.t == VT::Wide || v.t == VT::LongDouble) return Value::real(v.asFloat());
        if (v.t == VT::Float) return v;
        if (v.t == VT::Instance) {
            Value out;
            if (vm.callMagic(v, "__float__", {}, out)) return convertTo(vm, "float", out);
        }
        if (tryToNumber(vm, v, d)) return Value::real(d);
        vm.throwError(std::string("float(): cannot convert ") + v.typeName() + " to float");
    }
    if (type == "bool") {
        if (v.t == VT::Bool) return v;
        return Value::boolean(vm.truthy(v));
    }
    if (type == "str" || type == "String") return Value::str(vm.toStr(v));
    if (type == "Bytes") {
        if (v.t == VT::Bytes) return v;
        if (v.t == VT::List) {
            std::vector<uint8_t> bs;
            for (auto& x : v.o->items) {
                if (x.t != VT::Int) vm.throwError("Bytes(): list elements must be ints");
                bs.push_back((uint8_t)(x.i & 0xff));
            }
            return Value::bytes(bs);
        }
        if (v.t == VT::Str) return Value::bytes(std::vector<uint8_t>(v.o->str.begin(), v.o->str.end()));
        if (v.t == VT::Int) return Value::bytes(std::vector<uint8_t>((size_t)v.i, 0));
        vm.throwError("Bytes(): unsupported conversion");
    }
    if (type == "List") {
        if (v.t == VT::List) return v;
        if (v.t == VT::Tuple) return Value::list(v.o->items);
        if (v.t == VT::Str) {
            std::vector<Value> items;
            for (char c : v.o->str) items.push_back(Value::str(std::string(1, c)));
            return Value::list(items);
        }
        if (v.t == VT::Iter) {
            Value it = v;
            std::vector<Value> items;
            Obj* io = it.o.get();
            if (io->iterKind == IterKind::Range) {
                while (io->iterStep > 0 ? io->iterCur <= io->iterStop : io->iterCur >= io->iterStop) {
                    items.push_back(Value::integer(io->iterCur));
                    io->iterCur += io->iterStep;
                }
            } else {
                items = io->items;
            }
            return Value::list(items);
        }
        std::vector<Value> one{v};
        return Value::list(one);
    }
    if (type == "Tuple") {
        if (v.t == VT::Tuple) return v;
        if (v.t == VT::List) return Value::tuple(v.o->items);
        std::vector<Value> one{v};
        return Value::tuple(one);
    }
    vm.throwError("unknown type '" + type + "'");
}


static std::vector<Value> itemsOf(VM& vm, const Value& v) {
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

// ---------------------------------------------------------------- pseudo methods
static Value methodSize(VM& vm, const Value& self, std::vector<Value>&) {
    switch (self.t) {
        case VT::Array: return Value::integer(self.o->dims.empty() ? 0 : self.o->dims[0]);
        case VT::List: case VT::Tuple: return Value::integer((int64_t)self.o->items.size());
        case VT::Str: return Value::integer((int64_t)self.o->str.size());
        case VT::Bytes: return Value::integer((int64_t)self.o->bytes.size());
        case VT::UiNode: return Value::integer((int64_t)self.o->items.size());
        default: vm.throwError("size() unsupported"); 
    }
}

Value builtinMethod(VM& vm, const Value& obj, const std::string& name) {
    auto bind = [&](std::function<Value(VM&, const Value&, std::vector<Value>&)> fn) {
        Value self = obj;
        return vm.makeNative(name, [fn, self](VM& v, std::vector<Value>& args) {
            return fn(v, self, args);
        });
    };
    // `.size` / `.count` also behave as the length itself when used as a value
    auto bindSized = [&](std::function<Value(VM&, const Value&, std::vector<Value>&)> fn) {
        Value m = bind(fn);
        if (obj.t == VT::Array) m.o->implicit = Value::integer(obj.o->dims.empty() ? 0 : obj.o->dims[0]);
        else if (obj.t == VT::List || obj.t == VT::Tuple) m.o->implicit = Value::integer((int64_t)obj.o->items.size());
        else if (obj.t == VT::Str) m.o->implicit = Value::integer((int64_t)obj.o->str.size());
        else if (obj.t == VT::Bytes) m.o->implicit = Value::integer((int64_t)obj.o->bytes.size());
        else if (obj.t == VT::UiNode) m.o->implicit = Value::integer((int64_t)obj.o->items.size());
        return m;
    };
    bool isSeq = obj.t == VT::List || obj.t == VT::Tuple || obj.t == VT::Str ||
                  obj.t == VT::Bytes || obj.t == VT::Array;
    if (!isSeq) {
        if (obj.t == VT::UiNode) {
            if (name == "size") return bind(methodSize);
            if (name == "get") return bind([](VM& vm, const Value& s, std::vector<Value>& a) {
                int64_t i = asIndex(vm, argAt(a, 0), "get()");
                if (i < 0 || i >= (int64_t)s.o->items.size()) vm.throwError("index out of range");
                return s.o->items[(size_t)i];
            });
        }
        return Value::null();
    }
    if (name == "size" || name == "len" || name == "count") return bindSized(methodSize);
    if (name == "get") return bind([](VM& vm, const Value& s, std::vector<Value>& a) {
        return vm.getIndex(s, argAt(a, 0));
    });
    if (name == "contains" || name == "includes") return bind([](VM& vm, const Value& s, std::vector<Value>& a) {
        Value needle = argAt(a, 0);
        if (s.t == VT::Str) return Value::boolean(s.o->str.find(vm.toStr(needle)) != std::string::npos);
        for (auto& x : s.o->items) if (valueEquals(x, needle)) return Value::boolean(true);
        return Value::boolean(false);
    });
    if (obj.t == VT::List || obj.t == VT::Bytes || obj.t == VT::Array) {
        if (name == "set") return bind([](VM& vm, const Value& s, std::vector<Value>& a) {
            Value self = s;
            vm.setIndex(self, argAt(a, 0), argAt(a, 1));
            return Value::null();
        });
        if (name == "push" || name == "append" || name == "add") return bind([](VM& vm, const Value& s, std::vector<Value>& a) {
            if (s.t == VT::Array) {
                if (!s.o->dynamic)
                    vm.throwError("append() on a fixed size array; declare it as T[] to grow");
                for (auto& x : a) {
                    s.o->buf->push_back(deepCopy(x));
                    if (!s.o->dims.empty()) s.o->dims[0]++;
                }
                return s;
            }
            if (s.t != VT::List) vm.throwError("append() is only supported by List");
            for (auto& x : a) s.o->items.push_back(deepCopy(x));
            return s;
        });
        if (name == "pop") return bind([](VM& vm, const Value& s, std::vector<Value>& a) {
            if (s.t == VT::Array) {
                if (!s.o->dynamic) vm.throwError("pop() on a fixed size array");
                if (s.o->buf->empty()) vm.throwError("pop() on an empty array");
                Value out = s.o->buf->back();
                s.o->buf->pop_back();
                if (!s.o->dims.empty()) s.o->dims[0]--;
                return out;
            }
            if (s.t != VT::List) vm.throwError("pop() is only supported by List");
            if (s.o->items.empty()) vm.throwError("pop() on an empty list");
            if (!a.empty()) {
                int64_t i = asIndex(vm, a[0], "pop()");
                if (i < 0) i += (int64_t)s.o->items.size();
                if (i < 0 || i >= (int64_t)s.o->items.size()) vm.throwError("index out of range");
                Value v = s.o->items[(size_t)i];
                s.o->items.erase(s.o->items.begin() + (long)i);
                return v;
            }
            Value v = s.o->items.back();
            s.o->items.pop_back();
            return v;
        });
        if (name == "insert") return bind([](VM& vm, const Value& s, std::vector<Value>& a) {
            int64_t i = asIndex(vm, argAt(a, 0), "insert()");
            if (i < 0) i += (int64_t)s.o->items.size() + 1;
            if (i < 0 || i > (int64_t)s.o->items.size()) vm.throwError("index out of range");
            s.o->items.insert(s.o->items.begin() + (long)i, deepCopy(argAt(a, 1)));
            return s;
        });
        if (name == "remove") return bind([](VM& vm, const Value& s, std::vector<Value>& a) {
            int64_t i = asIndex(vm, argAt(a, 0), "remove()");
            if (i < 0) i += (int64_t)s.o->items.size();
            if (i < 0 || i >= (int64_t)s.o->items.size()) vm.throwError("index out of range");
            Value v = s.o->items[(size_t)i];
            s.o->items.erase(s.o->items.begin() + (long)i);
            return v;
        });
        if (name == "clear") return bind([](VM&, const Value& s, std::vector<Value>&) {
            s.o->items.clear();
            return Value::null();
        });
        if (name == "index_of" || name == "find") return bind([](VM& vm, const Value& s, std::vector<Value>& a) {
            Value needle = argAt(a, 0);
            for (size_t i = 0; i < s.o->items.size(); i++)
                if (valueEquals(s.o->items[i], needle)) return Value::integer((int64_t)i);
            return Value::integer(-1);
        });
        if (name == "join") return bind([](VM& vm, const Value& s, std::vector<Value>& a) {
            std::string sep = a.empty() ? "" : vm.toStr(a[0]);
            std::string out;
            for (size_t i = 0; i < s.o->items.size(); i++) {
                if (i) out += sep;
                out += vm.toStr(s.o->items[i]);
            }
            return Value::str(out);
        });
        if (name == "slice") return bind([](VM& vm, const Value& s, std::vector<Value>& a) {
            int64_t n = (int64_t)s.o->items.size();
            int64_t start = a.empty() ? 0 : asIndex(vm, a[0], "slice()");
            int64_t stop = a.size() > 1 ? asIndex(vm, a[1], "slice()") : n;
            if (start < 0) start += n;
            if (stop < 0) stop += n;
            start = std::max<int64_t>(0, std::min(start, n));
            stop = std::max<int64_t>(0, std::min(stop, n));
            std::vector<Value> items;
            for (int64_t i = start; i < stop; i++) items.push_back(s.o->items[(size_t)i]);
            return s.t == VT::Bytes ? Value::bytes({}) : Value::list(items);
        });
    }
    if (obj.t == VT::Str) {
        if (name == "upper") return bind([](VM&, const Value& s, std::vector<Value>&) {
            std::string r = s.o->str;
            for (auto& c : r) c = (char)std::toupper((unsigned char)c);
            return Value::str(r);
        });
        if (name == "lower") return bind([](VM&, const Value& s, std::vector<Value>&) {
            std::string r = s.o->str;
            for (auto& c : r) c = (char)std::tolower((unsigned char)c);
            return Value::str(r);
        });
        if (name == "trim") return bind([](VM&, const Value& s, std::vector<Value>&) {
            return Value::str(trim(s.o->str));
        });
        if (name == "split") return bind([](VM& vm, const Value& s, std::vector<Value>& a) {
            std::string sep = a.empty() ? " " : vm.toStr(a[0]);
            std::vector<Value> items;
            if (sep.empty()) {
                for (char c : s.o->str) items.push_back(Value::str(std::string(1, c)));
            } else {
                size_t p = 0;
                while (true) {
                    size_t q = s.o->str.find(sep, p);
                    if (q == std::string::npos) { items.push_back(Value::str(s.o->str.substr(p))); break; }
                    items.push_back(Value::str(s.o->str.substr(p, q - p)));
                    p = q + sep.size();
                }
            }
            return Value::list(items);
        });
        if (name == "substr" || name == "substring") return bind([](VM& vm, const Value& s, std::vector<Value>& a) {
            int64_t n = (int64_t)s.o->str.size();
            int64_t start = asIndex(vm, argAt(a, 0), "substr()");
            int64_t stop = a.size() > 1 ? asIndex(vm, a[1], "substr()") : n;
            if (start < 0) start += n;
            if (stop < 0) stop += n;
            start = std::max<int64_t>(0, std::min(start, n));
            stop = std::max<int64_t>(0, std::min(stop, n));
            if (stop < start) stop = start;
            return Value::str(s.o->str.substr((size_t)start, (size_t)(stop - start)));
        });
        if (name == "find" || name == "index_of") return bind([](VM& vm, const Value& s, std::vector<Value>& a) {
            size_t q = s.o->str.find(vm.toStr(argAt(a, 0)));
            return Value::integer(q == std::string::npos ? -1 : (int64_t)q);
        });
        if (name == "replace") return bind([](VM& vm, const Value& s, std::vector<Value>& a) {
            std::string from = vm.toStr(argAt(a, 0)), to = vm.toStr(argAt(a, 1));
            if (from.empty()) return s;
            std::string r = s.o->str;
            size_t p = 0;
            while ((p = r.find(from, p)) != std::string::npos) {
                r.replace(p, from.size(), to);
                p += to.size();
            }
            return Value::str(r);
        });
        if (name == "starts_with") return bind([](VM& vm, const Value& s, std::vector<Value>& a) {
            std::string p = vm.toStr(argAt(a, 0));
            return Value::boolean(s.o->str.rfind(p, 0) == 0);
        });
        if (name == "ends_with") return bind([](VM& vm, const Value& s, std::vector<Value>& a) {
            std::string p = vm.toStr(argAt(a, 0));
            if (p.size() > s.o->str.size()) return Value::boolean(false);
            return Value::boolean(s.o->str.compare(s.o->str.size() - p.size(), p.size(), p) == 0);
        });
        if (name == "to_int") return bind([](VM& vm, const Value& s, std::vector<Value>&) {
            return convertTo(vm, "int", s);
        });
        if (name == "to_float") return bind([](VM& vm, const Value& s, std::vector<Value>&) {
            return convertTo(vm, "float", s);
        });
        if (name == "bytes") return bind([](VM&, const Value& s, std::vector<Value>&) {
            return Value::bytes(std::vector<uint8_t>(s.o->str.begin(), s.o->str.end()));
        });
    }
    return Value::null();
}

// ---------------------------------------------------------------- GUI components
static const char* kComponents[] = {
    "Text", "Button", "Input", "Image", "Column", "Row", "Stack", "Scroll", "Spacer", "Slider", "Checkbox", "Link"
};

bool isBuiltinComponent(const std::string& name) {
    for (auto* c : kComponents) if (name == c) return true;
    return false;
}

Value builtinComponent(VM& vm, const std::string& name) {
    return vm.makeNative(name, [name](VM& v, std::vector<Value>& args) {
        std::unordered_map<std::string, Value> attrs;
        if (auto* nm = namedOf(v)) attrs = *nm;
        Value node = makeUiNode(name, attrs);
        for (size_t i = 0; i < args.size(); i++) {
            if (args[i].isNull()) continue;
            node.o->map["arg" + formatInt((int64_t)i)] = args[i];
        }
        if (!args.empty() && args[0].t == VT::Str) {
            if (name == "Text" || name == "Input" || name == "Image" || name == "Link") node.o->map["text"] = args[0];
            else if (name == "Button" || name == "Slider" || name == "Checkbox") node.o->map["label"] = args[0];
        }
        return node;
    });
}

// ---------------------------------------------------------------- std modules
static Value makeModuleFn(VM& vm, const std::string& name, std::function<Value(VM&, std::vector<Value>&)> fn) {
    return vm.makeNative(name, std::move(fn));
}

static Value mathModule(VM& vm) {
    Value m = Value::module("math");
    auto put = [&](const std::string& n, std::function<Value(VM&, std::vector<Value>&)> fn) {
        m.o->map[n] = makeModuleFn(vm, "math." + n, std::move(fn));
    };
    m.o->map["pi"] = Value::real(3.14159265358979323846);
    m.o->map["e"] = Value::real(2.71828182845904523536);
    put("sqrt", [](VM& v, std::vector<Value>& a) {
        double d; if (!tryToNumber(v, argAt(a, 0), d)) v.throwError("math.sqrt expects a number");
        if (d < 0) v.throwError("math.sqrt of a negative number");
        return Value::real(std::sqrt(d));
    });
    put("abs", [](VM& v, std::vector<Value>& a) {
        Value x = argAt(a, 0);
        if (x.t == VT::Int) return Value::integer(x.i < 0 ? -x.i : x.i);
        double d; if (!tryToNumber(v, x, d)) v.throwError("math.abs expects a number");
        return Value::real(std::fabs(d));
    });
    put("floor", [](VM& v, std::vector<Value>& a) {
        double d; if (!tryToNumber(v, argAt(a, 0), d)) v.throwError("math.floor expects a number");
        return Value::integer((int64_t)std::floor(d));
    });
    put("ceil", [](VM& v, std::vector<Value>& a) {
        double d; if (!tryToNumber(v, argAt(a, 0), d)) v.throwError("math.ceil expects a number");
        return Value::integer((int64_t)std::ceil(d));
    });
    put("round", [](VM& v, std::vector<Value>& a) {
        double d; if (!tryToNumber(v, argAt(a, 0), d)) v.throwError("math.round expects a number");
        return Value::integer((int64_t)std::llround(d));
    });
    put("pow", [](VM& v, std::vector<Value>& a) {
        double x, y;
        if (!tryToNumber(v, argAt(a, 0), x) || !tryToNumber(v, argAt(a, 1), y)) v.throwError("math.pow expects numbers");
        return Value::real(std::pow(x, y));
    });
    put("min", [](VM& v, std::vector<Value>& a) {
        if (a.empty()) v.throwError("math.min expects at least one argument");
        Value best = a[0];
        for (auto& x : a) if (x.isNumber() && best.isNumber() && x.asFloat() < best.asFloat()) best = x;
        return best;
    });
    put("max", [](VM& v, std::vector<Value>& a) {
        if (a.empty()) v.throwError("math.max expects at least one argument");
        Value best = a[0];
        for (auto& x : a) if (x.isNumber() && best.isNumber() && x.asFloat() > best.asFloat()) best = x;
        return best;
    });
    put("sin", [](VM& v, std::vector<Value>& a) { double d; tryToNumber(v, argAt(a, 0), d); return Value::real(std::sin(d)); });
    put("cos", [](VM& v, std::vector<Value>& a) { double d; tryToNumber(v, argAt(a, 0), d); return Value::real(std::cos(d)); });
    put("tan", [](VM& v, std::vector<Value>& a) { double d; tryToNumber(v, argAt(a, 0), d); return Value::real(std::tan(d)); });
    put("log", [](VM& v, std::vector<Value>& a) { double d; tryToNumber(v, argAt(a, 0), d); return Value::real(std::log(d)); });
    put("exp", [](VM& v, std::vector<Value>& a) { double d; tryToNumber(v, argAt(a, 0), d); return Value::real(std::exp(d)); });
    put("random", [](VM&, std::vector<Value>&) { return Value::real((double)std::rand() / ((double)RAND_MAX + 1.0)); });
    put("random_int", [](VM& v, std::vector<Value>& a) {
        int64_t lo = asIndex(v, argAt(a, 0), "math.random_int");
        int64_t hi = asIndex(v, argAt(a, 1), "math.random_int");
        if (hi < lo) std::swap(lo, hi);
        return Value::integer(lo + (int64_t)(std::rand() % (unsigned)(hi - lo + 1)));
    });
    return m;
}

static Value ioModule(VM& vm) {
    Value m = Value::module("io");
    auto put = [&](const std::string& n, std::function<Value(VM&, std::vector<Value>&)> fn) {
        m.o->map[n] = makeModuleFn(vm, "io." + n, std::move(fn));
    };
    put("read_file", [](VM& v, std::vector<Value>& a) {
        std::string path = asStr(v, argAt(a, 0), "io.read_file");
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) v.throwError("io.read_file: cannot open " + path);
        std::string out;
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        std::fclose(f);
        return Value::str(out);
    });
    put("write_file", [](VM& v, std::vector<Value>& a) {
        std::string path = asStr(v, argAt(a, 0), "io.write_file");
        std::string text = v.toStr(argAt(a, 1));
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) v.throwError("io.write_file: cannot open " + path);
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
        return Value::null();
    });
    put("println", [](VM& v, std::vector<Value>& a) {
        v.write(v.toStr(argAt(a, 0)) + "\n");
        return Value::null();
    });
    return m;
}

// ---------------------------------------------------------------- json
static Value jsonParseValue(const std::string& s, size_t& p, VM& vm);

static void jsonSkipWs(const std::string& s, size_t& p) {
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) p++;
}

static Value jsonParseValue(const std::string& s, size_t& p, VM& vm) {
    jsonSkipWs(s, p);
    if (p >= s.size()) vm.throwError("json.parse: unexpected end of input");
    char c = s[p];
    if (c == '{') {
        p++;
        std::unordered_map<std::string, Value> m;
        jsonSkipWs(s, p);
        if (p < s.size() && s[p] == '}') { p++; return Value::map(m); }
        while (true) {
            jsonSkipWs(s, p);
            if (p >= s.size() || s[p] != '"') vm.throwError("json.parse: expected a key");
            Value key = jsonParseValue(s, p, vm);
            jsonSkipWs(s, p);
            if (p >= s.size() || s[p] != ':') vm.throwError("json.parse: expected ':'");
            p++;
            m[key.o->str] = jsonParseValue(s, p, vm);
            jsonSkipWs(s, p);
            if (p < s.size() && s[p] == ',') { p++; continue; }
            if (p < s.size() && s[p] == '}') { p++; break; }
            vm.throwError("json.parse: expected ',' or '}'");
        }
        return Value::map(m);
    }
    if (c == '[') {
        p++;
        std::vector<Value> items;
        jsonSkipWs(s, p);
        if (p < s.size() && s[p] == ']') { p++; return Value::list(items); }
        while (true) {
            items.push_back(jsonParseValue(s, p, vm));
            jsonSkipWs(s, p);
            if (p < s.size() && s[p] == ',') { p++; continue; }
            if (p < s.size() && s[p] == ']') { p++; break; }
            vm.throwError("json.parse: expected ',' or ']'");
        }
        return Value::list(items);
    }
    if (c == '"') {
        p++;
        std::string out;
        while (p < s.size() && s[p] != '"') {
            if (s[p] == '\\' && p + 1 < s.size()) {
                p++;
                switch (s[p]) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    default: out += s[p]; break;
                }
            } else out += s[p];
            p++;
        }
        if (p >= s.size()) vm.throwError("json.parse: unterminated string");
        p++;
        return Value::str(out);
    }
    if (s.compare(p, 4, "true") == 0) { p += 4; return Value::boolean(true); }
    if (s.compare(p, 5, "false") == 0) { p += 5; return Value::boolean(false); }
    if (s.compare(p, 4, "null") == 0) { p += 4; return Value::null(); }
    size_t start = p;
    while (p < s.size() && (isdigit((unsigned char)s[p]) || s[p] == '-' || s[p] == '+' || s[p] == '.' ||
                            s[p] == 'e' || s[p] == 'E')) p++;
    std::string num = s.substr(start, p - start);
    if (num.empty()) vm.throwError("json.parse: unexpected character");
    if (num.find('.') != std::string::npos || num.find('e') != std::string::npos || num.find('E') != std::string::npos)
        return Value::real(std::strtod(num.c_str(), nullptr));
    return Value::integer(std::strtoll(num.c_str(), nullptr, 10));
}

static std::string jsonStringify(VM& vm, const Value& v) {
    switch (v.t) {
        case VT::Null: return "null";
        case VT::Bool: return v.b ? "true" : "false";
        case VT::Int: return formatInt(v.i);
        case VT::Float: return formatDouble(v.f);
        case VT::Str: {
            std::string out = "\"";
            for (char c : v.o->str) {
                switch (c) {
                    case '"': out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n"; break;
                    case '\t': out += "\\t"; break;
                    case '\r': out += "\\r"; break;
                    default: out += c;
                }
            }
            return out + "\"";
        }
        case VT::List:
        case VT::Tuple: {
            std::string out = "[";
            for (size_t i = 0; i < v.o->items.size(); i++) {
                if (i) out += ",";
                out += jsonStringify(vm, v.o->items[i]);
            }
            return out + "]";
        }
        case VT::Map:
        case VT::Module: {
            std::string out = "{";
            bool first = true;
            for (auto& kv : v.o->map) {
                if (!first) out += ",";
                first = false;
                out += "\"" + kv.first + "\":" + jsonStringify(vm, kv.second);
            }
            return out + "}";
        }
        case VT::Instance: {
            std::string out = "{";
            bool first = true;
            for (size_t i = 0; i < v.o->klass->fieldNames.size() && i < v.o->fields.size(); i++) {
                if (!first) out += ",";
                first = false;
                out += "\"" + v.o->klass->fieldNames[i] + "\":" + jsonStringify(vm, v.o->fields[i]);
            }
            return out + "}";
        }
        default: return "null";
    }
}

static Value jsonModule(VM& vm) {
    Value m = Value::module("json");
    m.o->map["parse"] = makeModuleFn(vm, "json.parse", [](VM& v, std::vector<Value>& a) {
        std::string s = asStr(v, argAt(a, 0), "json.parse");
        size_t p = 0;
        return jsonParseValue(s, p, v);
    });
    m.o->map["stringify"] = makeModuleFn(vm, "json.stringify", [](VM& v, std::vector<Value>& a) {
        return Value::str(jsonStringify(v, argAt(a, 0)));
    });
    return m;
}

Value builtinModule(const std::string& name) {
    (void)name;
    return Value::null();
}
bool isBuiltinModule(const std::string& name) {
    // native modules come from two places: the ones the core ships, and anything a linked C++
    // file (or a plugin) registered through the FFI - so `use fast` works without core changes
    if (ffiHasModule(name) || ffiHasFunction(name)) return true;
    return name == "math" || name == "io" || name == "json" || name == "time" || name == "net" ||
           name == "os" || name == "thread" || name == "system" || name == "slice";
}

// ---------------------------------------------------------------- low level file I/O
//
// These `_`-prefixed primitives are the raw layer of the runtime: they talk to the operating
// system directly and raise on error.  `lib/file.mod` wraps them into the higher level
// File / Dir / Path API that normal programs should use.
namespace {

std::string pathOf(VM& v, std::vector<Value>& a, size_t i, const char* what) {
    Value x = argAt(a, i);
    if (x.t != VT::Str) v.throwError(std::string(what) + " expects a path string");
    return x.o->str;
}

std::string portable(const fs::path& p);

// The interpreter deals in UTF-8 everywhere, while the file system on Windows speaks UTF-16,
// so convert explicitly to keep non-ASCII paths working.
std::wstring utf8ToWide(const std::string& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        unsigned cp = 0;
        int extra = 0;
        if (c < 0x80) cp = c;
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; extra = 3; }
        else cp = 0xFFFD;
        i++;
        for (int k = 0; k < extra && i < s.size(); k++, i++) cp = (cp << 6) | ((unsigned char)s[i] & 0x3Fu);
        if (sizeof(wchar_t) == 2 && cp > 0xFFFF) {
            cp -= 0x10000;
            out += (wchar_t)(0xD800 + (cp >> 10));
            out += (wchar_t)(0xDC00 + (cp & 0x3FF));
        } else {
            out += (wchar_t)cp;
        }
    }
    return out;
}

std::string wideToUtf8(const std::wstring& w) {
    std::string out;
    for (size_t i = 0; i < w.size(); i++) {
        unsigned cp = (unsigned)w[i];
        if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size()) {
            unsigned lo = (unsigned)w[i + 1];
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i++;
        }
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

fs::path nativePath(const std::string& p) {
    if (sizeof(wchar_t) == 2) return fs::path(utf8ToWide(p));
    return fs::path(p);
}

std::string portable(const fs::path& p) {
    if (sizeof(wchar_t) == 2) return wideToUtf8(p.wstring());
    return p.string();
}

std::string osError() {
    return std::strerror(errno);
}

// Gather the bytes of a string / Bytes / any value (stringified) for writing.
std::string dataOf(VM& v, const Value& data, const char* what) {
    switch (data.t) {
        case VT::Str: return data.o->str;
        case VT::Bytes: return std::string((const char*)data.o->bytes.data(), data.o->bytes.size());
        case VT::Null: v.throwError(std::string(what) + " needs text or bytes to write");
        default: return v.toStr(data);
    }
}

void registerFilePrimitives(VM& vm) {
    auto reg = [&](const std::string& name, std::function<Value(VM&, std::vector<Value>&)> fn) {
        vm.globals[name] = std::make_shared<Value>(vm.makeNative(name, std::move(fn)));
    };

    reg("_file_read", [](VM& v, std::vector<Value>& a) {
        std::string path = pathOf(v, a, 0, "_file_read");
        std::ifstream in(nativePath(path), std::ios::binary);
        if (!in) v.throwError("_file_read: cannot open '" + path + "' (" + osError() + ")");
        std::string out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return Value::str(out);
    });
    reg("_file_read_bytes", [](VM& v, std::vector<Value>& a) {
        std::string path = pathOf(v, a, 0, "_file_read_bytes");
        std::ifstream in(nativePath(path), std::ios::binary);
        if (!in) v.throwError("_file_read_bytes: cannot open '" + path + "' (" + osError() + ")");
        std::string out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return Value::bytes(std::vector<uint8_t>(out.begin(), out.end()));
    });
    reg("_file_write", [](VM& v, std::vector<Value>& a) {
        std::string path = pathOf(v, a, 0, "_file_write");
        std::string data = dataOf(v, argAt(a, 1), "_file_write");
        std::FILE* f = std::fopen(nativePath(path).string().c_str(), "wb");
        if (!f) v.throwError("_file_write: cannot open '" + path + "' for writing (" + osError() + ")");
        size_t n = std::fwrite(data.data(), 1, data.size(), f);
        std::fclose(f);
        return Value::integer((int64_t)n);
    });
    reg("_file_append", [](VM& v, std::vector<Value>& a) {
        std::string path = pathOf(v, a, 0, "_file_append");
        std::string data = dataOf(v, argAt(a, 1), "_file_append");
        std::FILE* f = std::fopen(nativePath(path).string().c_str(), "ab");
        if (!f) v.throwError("_file_append: cannot open '" + path + "' for writing (" + osError() + ")");
        size_t n = std::fwrite(data.data(), 1, data.size(), f);
        std::fclose(f);
        return Value::integer((int64_t)n);
    });
    reg("_file_exists", [](VM& v, std::vector<Value>& a) {
        std::error_code ec;
        return Value::boolean(fs::exists(nativePath(pathOf(v, a, 0, "_file_exists")), ec));
    });
    reg("_file_is_dir", [](VM& v, std::vector<Value>& a) {
        std::error_code ec;
        return Value::boolean(fs::is_directory(nativePath(pathOf(v, a, 0, "_file_is_dir")), ec));
    });
    reg("_file_size", [](VM& v, std::vector<Value>& a) {
        std::error_code ec;
        auto sz = fs::file_size(nativePath(pathOf(v, a, 0, "_file_size")), ec);
        return Value::integer(ec ? -1 : (int64_t)sz);
    });
    reg("_file_mtime", [](VM& v, std::vector<Value>& a) {
        std::string path = pathOf(v, a, 0, "_file_mtime");
        std::error_code ec;
        auto t = fs::last_write_time(nativePath(path), ec);
        if (ec) return Value::integer(-1);
        return Value::integer((int64_t)std::chrono::duration_cast<std::chrono::seconds>(
                                  t.time_since_epoch()).count());
    });
    reg("_file_remove", [](VM& v, std::vector<Value>& a) {
        std::string path = pathOf(v, a, 0, "_file_remove");
        std::error_code ec;
        bool ok = fs::remove(nativePath(path), ec);
        return Value::boolean(ok && !ec);
    });
    reg("_file_rename", [](VM& v, std::vector<Value>& a) {
        std::string from = pathOf(v, a, 0, "_file_rename");
        std::string to = pathOf(v, a, 1, "_file_rename");
        std::error_code ec;
        fs::rename(nativePath(from), nativePath(to), ec);
        if (ec) v.throwError("_file_rename: cannot move '" + from + "' to '" + to + "' (" + ec.message() + ")");
        return Value::boolean(true);
    });
    reg("_file_copy", [](VM& v, std::vector<Value>& a) {
        std::string from = pathOf(v, a, 0, "_file_copy");
        std::string to = pathOf(v, a, 1, "_file_copy");
        bool overwrite = a.size() > 2 ? argAt(a, 2).isTruthyRaw() : true;
        std::error_code ec;
        auto flags = overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::none;
        bool ok = fs::copy_file(nativePath(from), nativePath(to), flags, ec);
        if (ec) v.throwError("_file_copy: cannot copy '" + from + "' (" + ec.message() + ")");
        return Value::boolean(ok);
    });
    reg("_file_write_bytes", [](VM& v, std::vector<Value>& a) {
        std::string path = pathOf(v, a, 0, "_file_write_bytes");
        Value data = argAt(a, 1);
        if (data.t != VT::Bytes) v.throwError("_file_write_bytes expects Bytes");
        std::FILE* f = std::fopen(nativePath(path).string().c_str(), "wb");
        if (!f) v.throwError("_file_write_bytes: cannot open '" + path + "'");
        size_t n = std::fwrite(data.o->bytes.data(), 1, data.o->bytes.size(), f);
        std::fclose(f);
        return Value::integer((int64_t)n);
    });

    // ---- directories
    reg("_dir_list", [](VM& v, std::vector<Value>& a) {
        std::string path = pathOf(v, a, 0, "_dir_list");
        std::error_code ec;
        std::vector<Value> names;
        fs::directory_iterator it(nativePath(path), ec), end;
        if (ec) v.throwError("_dir_list: cannot list '" + path + "' (" + ec.message() + ")");
        for (; it != end; it.increment(ec)) {
            if (ec) break;
            names.push_back(Value::str(portable(it->path().filename())));
        }
        std::sort(names.begin(), names.end(),
                  [](const Value& x, const Value& y) { return x.o->str < y.o->str; });
        return Value::list(names);
    });
    reg("_dir_make", [](VM& v, std::vector<Value>& a) {
        std::string path = pathOf(v, a, 0, "_dir_make");
        std::error_code ec;
        fs::create_directories(nativePath(path), ec);
        if (ec) v.throwError("_dir_make: cannot create '" + path + "' (" + ec.message() + ")");
        return Value::boolean(true);
    });
    reg("_dir_remove", [](VM& v, std::vector<Value>& a) {
        std::string path = pathOf(v, a, 0, "_dir_remove");
        bool recursive = a.size() > 1 && argAt(a, 1).isTruthyRaw();
        std::error_code ec;
        if (recursive) fs::remove_all(nativePath(path), ec);
        else fs::remove(nativePath(path), ec);
        if (ec) v.throwError("_dir_remove: cannot remove '" + path + "' (" + ec.message() + ")");
        return Value::boolean(true);
    });
    reg("_dir_walk", [](VM& v, std::vector<Value>& a) {
        std::string path = pathOf(v, a, 0, "_dir_walk");
        std::error_code ec;
        std::vector<Value> out;
        fs::recursive_directory_iterator it(nativePath(path), ec), end;
        if (ec) v.throwError("_dir_walk: cannot walk '" + path + "' (" + ec.message() + ")");
        for (; it != end; it.increment(ec)) {
            if (ec) break;
            out.push_back(Value::str(portable(it->path().lexically_relative(nativePath(path)))));
        }
        std::sort(out.begin(), out.end(), [](const Value& x, const Value& y) { return x.o->str < y.o->str; });
        return Value::list(out);
    });

    // ---- paths
    reg("_path_join", [](VM& v, std::vector<Value>& a) {
        fs::path p;
        for (size_t i = 0; i < a.size(); i++) {
            if (argAt(a, i).t != VT::Str) v.throwError("_path_join expects strings");
            p /= nativePath(argAt(a, i).o->str);
        }
        return Value::str(portable(p));
    });
    reg("_path_dirname", [](VM& v, std::vector<Value>& a) {
        fs::path p = nativePath(pathOf(v, a, 0, "_path_dirname")).parent_path();
        return Value::str(p.empty() ? "." : portable(p));
    });
    reg("_path_basename", [](VM& v, std::vector<Value>& a) {
        return Value::str(portable(nativePath(pathOf(v, a, 0, "_path_basename")).filename()));
    });
    reg("_path_ext", [](VM& v, std::vector<Value>& a) {
        return Value::str(portable(nativePath(pathOf(v, a, 0, "_path_ext")).extension()));
    });
    reg("_path_abs", [](VM& v, std::vector<Value>& a) {
        std::error_code ec;
        fs::path p = fs::absolute(nativePath(pathOf(v, a, 0, "_path_abs")), ec);
        if (ec) return Value::str(pathOf(v, a, 0, "_path_abs"));
        return Value::str(portable(p.lexically_normal()));
    });
    reg("_path_normalize", [](VM& v, std::vector<Value>& a) {
        return Value::str(portable(nativePath(pathOf(v, a, 0, "_path_normalize")).lexically_normal()));
    });

    // ---- process environment
    reg("_cwd", [](VM&, std::vector<Value>&) {
        std::error_code ec;
        fs::path p = fs::current_path(ec);
        return Value::str(ec ? std::string(".") : portable(p));
    });
    reg("_chdir", [](VM& v, std::vector<Value>& a) {
        std::string path = pathOf(v, a, 0, "_chdir");
        std::error_code ec;
        fs::current_path(nativePath(path), ec);
        if (ec) v.throwError("_chdir: cannot enter '" + path + "' (" + ec.message() + ")");
        return Value::boolean(true);
    });
    reg("_stdin_line", [](VM& v, std::vector<Value>&) {
        // a GUI host answers through the input provider, a console reads stdin
        if (v.inputProvider) return Value::str(v.inputProvider("_stdin_line"));
        std::string line;
        if (!std::getline(std::cin, line)) return Value::null();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return Value::str(line);
    });
    reg("_stdin_all", [](VM&, std::vector<Value>&) {
        std::string out((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
        return Value::str(out);
    });
}

} // namespace

// ---------------------------------------------------------------- registration

// ---------------------------------------------------------------- numeric width conversions
// `int8(x)`, `uint32(x)`, `float32(x)` ... produce a value carrying that width, so arithmetic
// follows C conversion/wrapping rules.  `typeof` reports the width name.
static void registerNumericConversions(VM& vm) {
    static const struct { const char* name; NumKind kind; } tbl[] = {
        {"int8", NumKind::I8},     {"int16", NumKind::I16},   {"int32", NumKind::I32},
        {"int64", NumKind::I64},   {"uint8", NumKind::U8},    {"uint16", NumKind::U16},
        {"uint32", NumKind::U32},  {"uint64", NumKind::U64},  {"uint", NumKind::U32},
        {"ulong", NumKind::U64},   {"long", NumKind::I64},    {"float32", NumKind::F32},
        {"float64", NumKind::F64},
    };
    // widths that need the boxed representation
    static const struct { const char* name; bool isFloat; bool isUnsigned; } wide[] = {
        {"longlong", false, false}, {"ulonglong", false, true}, {"longdouble", true, false},
    };
    for (auto& w : wide) {
        std::string nm = w.name;
        bool isFloat = w.isFloat, isUnsigned = w.isUnsigned;
        vm.globals[w.name] = std::make_shared<Value>(vm.makeNative(nm, [nm, isFloat, isUnsigned](
            VM& v, std::vector<Value>& a) {
            Value x = a.empty() ? Value::integer(0) : a[0];
            if (x.t == VT::Str) {
                if (isFloat) return Value::longDouble(std::strtold(x.o->str.c_str(), nullptr));
                return Value::wide(parseWide(x.o->str, isUnsigned), isUnsigned);
            }
            if (!x.isANumber()) v.throwError(nm + ": cannot convert " + x.typeName());
            if (isFloat) return Value::longDouble(x.asLongDouble());
            return Value::wide(x.asWide(), isUnsigned);
        }));
    }
    for (auto& e : tbl) {
        NumKind k = e.kind;
        std::string nm = e.name;
        vm.globals[e.name] = std::make_shared<Value>(vm.makeNative(e.name, [k, nm](VM& v, std::vector<Value>& a) {
            Value x = a.empty() ? Value::integer(0) : a[0];
            if (x.t == VT::Int)   return Value::typedInt(x.i, k);
            if (x.t == VT::Float) return numIsFloat(k) ? Value::typedReal(x.f, k)
                                                       : Value::typedInt((int64_t)x.f, k);
            if (x.t == VT::Bool)  return Value::typedInt(x.b ? 1 : 0, k);
            if (x.t == VT::Str) {
                // keep the string parsing behaviour of int()/float()
                Value base = numIsFloat(k) ? Value::real(std::atof(x.o->str.c_str()))
                                           : Value::integer((int64_t)std::strtoll(x.o->str.c_str(), nullptr, 10));
                return numIsFloat(k) ? Value::typedReal(base.f, k) : Value::typedInt(base.i, k);
            }
            if (numIsFloat(k)) v.throwError(nm + ": cannot convert " + x.typeName());
            v.throwError(nm + ": cannot convert " + x.typeName());
            return Value::null();
        }));
    }
}

void registerBuiltins(VM& vm) {
    auto reg = [&](const std::string& name, std::function<Value(VM&, std::vector<Value>&)> fn) {
        vm.globals[name] = std::make_shared<Value>(vm.makeNative(name, std::move(fn)));
    };

    // type conversion functions
    for (const char* t : {"int", "float", "bool", "str", "String", "Bytes", "List", "Tuple"}) {
        std::string type = t;
        reg(type, [type](VM& v, std::vector<Value>& a) {
            if (a.empty()) v.throwError(type + "() expects one argument");
            return convertTo(v, type, a[0]);
        });
    }
    reg("len", [](VM& v, std::vector<Value>& a) {
        Value x = argAt(a, 0);
        switch (x.t) {
            case VT::Array: return Value::integer(x.o->dims.empty() ? 0 : x.o->dims[0]);
            case VT::List: case VT::Tuple: return Value::integer((int64_t)x.o->items.size());
            case VT::Str: return Value::integer((int64_t)x.o->str.size());
            case VT::Bytes: return Value::integer((int64_t)x.o->bytes.size());
            case VT::Map: return Value::integer((int64_t)x.o->map.size());
            case VT::Instance: {
                Value out;
                if (v.callMagic(x, "__len__", {}, out)) return out;
                v.throwError("len(): '" + x.o->klass->name + "' has no __len__");
            }
            default: break;
        }
        v.throwError(std::string("len() is not supported for ") + x.typeName());
    });
    reg("typeof", [](VM&, std::vector<Value>& a) {
        Value x = argAt(a, 0);
        return Value::str(x.t == VT::Instance ? x.o->klass->name : x.typeName());
    });
    reg("print", [](VM& v, std::vector<Value>& a) {
        std::string out;
        for (size_t i = 0; i < a.size(); i++) {
            if (i) out += " ";
            out += v.toStr(a[i]);
        }
        v.write(out + "\n");
        return Value::null();
    });
    reg("alloc", [](VM& v, std::vector<Value>& a) {
        int64_t n = asIndex(v, argAt(a, 0), "alloc()");
        if (n < 0) v.throwError("alloc(): negative size");
        return Value::list(std::vector<Value>((size_t)n, Value::integer(0)));
    });
    reg("raw_copy", [](VM& v, std::vector<Value>& a) {
        Value dst = argAt(a, 0), src = argAt(a, 1);
        int64_t n = a.size() > 2 ? asIndex(v, a[2], "raw_copy()") : (int64_t)src.o->items.size();
        if (dst.t != VT::List || src.t != VT::List) v.throwError("raw_copy expects lists");
        for (int64_t i = 0; i < n && i < (int64_t)src.o->items.size() && i < (int64_t)dst.o->items.size(); i++)
            dst.o->items[(size_t)i] = deepCopy(src.o->items[(size_t)i]);
        return dst;
    });
    reg("pairs", [](VM& v, std::vector<Value>& a) {
        Value x = argAt(a, 0);
        std::vector<Value> out;
        if (x.t == VT::List || x.t == VT::Tuple) {
            for (size_t i = 0; i < x.o->items.size(); i++)
                out.push_back(Value::tuple({Value::integer((int64_t)i), x.o->items[i]}));
        } else if (x.t == VT::Map || x.t == VT::Module) {
            for (auto& kv : x.o->map) out.push_back(Value::tuple({Value::str(kv.first), kv.second}));
        } else if (x.t == VT::Instance) {
            for (size_t i = 0; i < x.o->klass->fieldNames.size() && i < x.o->fields.size(); i++)
                out.push_back(Value::tuple({Value::str(x.o->klass->fieldNames[i]), x.o->fields[i]}));
        } else v.throwError("pairs() expects a list, map or instance");
        return Value::list(out);
    });
    reg("range", [](VM& v, std::vector<Value>& a) {
        int64_t start = asIndex(v, argAt(a, 0), "range()");
        int64_t stop = a.size() > 1 ? asIndex(v, a[1], "range()") : start;
        int64_t step = a.size() > 2 ? asIndex(v, a[2], "range()") : 1;
        if (a.size() == 1) start = 0;
        if (step == 0) v.throwError("range() step must not be zero");
        return Value::iterRange(start, stop, step);
    });
    reg("abs", [](VM& v, std::vector<Value>& a) {
        Value x = argAt(a, 0);
        if (x.t == VT::Int) return Value::integer(x.i < 0 ? -x.i : x.i);
        if (x.t == VT::Float) return Value::real(std::fabs(x.f));
        v.throwError("abs() expects a number");
    });
    reg("min", [](VM& v, std::vector<Value>& a) {
        if (a.empty()) v.throwError("min() expects at least one argument");
        Value best = a[0];
        for (auto& x : a) if (x.isNumber() && best.isNumber() && x.asFloat() < best.asFloat()) best = x;
        return best;
    });
    reg("max", [](VM& v, std::vector<Value>& a) {
        if (a.empty()) v.throwError("max() expects at least one argument");
        Value best = a[0];
        for (auto& x : a) if (x.isNumber() && best.isNumber() && x.asFloat() > best.asFloat()) best = x;
        return best;
    });
    reg("Ok", [](VM&, std::vector<Value>& a) { return argAt(a, 0); });
    reg("Err", [](VM&, std::vector<Value>& a) { return argAt(a, 0); });
    reg("is_null", [](VM&, std::vector<Value>& a) { return Value::boolean(argAt(a, 0).isNull()); });

    // (the hot sequence kernels - sorted / nth / argsort / lower_bound / upper_bound -
    //  live in native/seq_native.cpp and are registered through the FFI)
    reg("sum", [](VM& v, std::vector<Value>& a) {
        std::vector<Value> items = itemsOf(v, argAt(a, 0));
        Value acc = a.size() > 1 ? a[1] : Value::integer(0);
        for (auto& x : items) {
            if (!x.isNumber() || !acc.isNumber()) v.throwError("sum(): elements must be numbers");
            if (acc.t == VT::Int && x.t == VT::Int) acc = Value::integer(acc.i + x.i);
            else acc = Value::real(acc.asFloat() + x.asFloat());
        }
        return acc;
    });
    reg("zip", [](VM& v, std::vector<Value>& a) {
        if (a.size() < 2) v.throwError("zip() expects at least two sequences");
        std::vector<std::vector<Value>> seqs;
        size_t n = (size_t)-1;
        for (auto& x : a) {
            seqs.push_back(itemsOf(v, x));
            n = std::min(n, seqs.back().size());
        }
        std::vector<Value> out;
        for (size_t i = 0; i < n; i++) {
            std::vector<Value> row;
            for (auto& s : seqs) row.push_back(s[i]);
            out.push_back(Value::tuple(row));
        }
        return Value::list(out);
    });
    reg("enumerate", [](VM& v, std::vector<Value>& a) {
        std::vector<Value> items = itemsOf(v, argAt(a, 0));
        int64_t start = a.size() > 1 ? asIndex(v, a[1], "enumerate()") : 0;
        std::vector<Value> out;
        for (size_t i = 0; i < items.size(); i++)
            out.push_back(Value::tuple({Value::integer(start + (int64_t)i), items[i]}));
        return Value::list(out);
    });
    reg("ord", [](VM& v, std::vector<Value>& a) {
        Value x = argAt(a, 0);
        if (x.t != VT::Str || x.o->str.empty()) v.throwError("ord() expects a non-empty string");
        return Value::integer((unsigned char)x.o->str[0]);
    });
    reg("chr", [](VM& v, std::vector<Value>& a) {
        int64_t c = asIndex(v, argAt(a, 0), "chr()");
        if (c < 0 || c > 255) v.throwError("chr() expects a byte value (0..255)");
        return Value::str(std::string(1, (char)c));
    });

    // low level file / directory / path primitives (`lib/file.mod` wraps them)
    registerFilePrimitives(vm);

    // GUI attribute vocabulary
    for (const char* c : {"bold", "normal", "light", "center", "left", "right", "top", "bottom",
                          "start", "end", "row", "column", "horizontal", "vertical", "wrap", "nowrap"})
        vm.globals[c] = std::make_shared<Value>(Value::str(c));

    // GUI components
    for (const char* c : kComponents)
        vm.globals[c] = std::make_shared<Value>(builtinComponent(vm, c));

    // standard modules
    vm.globals["math"] = std::make_shared<Value>(mathModule(vm));
    vm.globals["io"] = std::make_shared<Value>(ioModule(vm));
    vm.globals["json"] = std::make_shared<Value>(jsonModule(vm));
    // time / os / thread / net come from the unified native registry (native_api.hpp); the
    // `system` facade mirrors every namespace, including the ones registered above.
    registerNumericConversions(vm);
    registerSysPrimitives(vm);

    // everything a linked C++ file or a plugin registered (see src/ffi.hpp, docs/ffi.md)
    ffiInstallAll(vm);
}

Value builtinFunction(VM& vm, const std::string& name) {
    Value out;
    if (vm.getGlobal(name, out)) return out;
    return Value::null();
}

} // namespace annota
