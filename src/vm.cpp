// Annota - vm.cpp : the bytecode virtual machine.
#include "vm.hpp"
#include "builtins.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace annota {

// numeric width conversion (defined below, used by OP_CONVERT)
static Value convertNum(VM& vm, const Value& v, NumKind k, const char* what);
static Value convertNumCode(VM& vm, const Value& v, uint8_t raw, const char* what);
__int128 parseWide(const std::string& text, bool isUnsigned);
std::string wideText(__int128 v, bool isUnsigned);

static const int kMaxFrames = 512;

VM::VM() {}

// ---------------------------------------------------------------- output
void VM::write(const std::string& s) {
    if (capture) { stdoutText += s; return; }
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
}

void VM::throwError(const std::string& msg) { throw VMError(msg); }

// ---------------------------------------------------------------- printing
std::string VM::toStr(const Value& v) {
    switch (v.t) {
        case VT::Null:  return "null";
        case VT::Bool:  return v.b ? "true" : "false";
        case VT::Int: {
            // an unsigned width prints its full range (uint64(0) - uint64(1) is 2^64-1)
            if (numIsUnsigned(v.numKind())) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)(uint64_t)v.i);
                return buf;
            }
            return formatInt(v.i);
        }
        case VT::Float: return formatDouble(v.f);
        case VT::Wide:  return wideText(v.o ? v.o->wide : 0, v.wideUnsigned());
        case VT::LongDouble: {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.17Lg", v.o ? v.o->wideF : 0.0L);
            return buf;
        }
        case VT::Str:   return v.o->str;
        case VT::Bytes: {
            std::string s = "b\"";
            for (uint8_t b : v.o->bytes) {
                if (b == '"' || b == '\\') { s += '\\'; s += (char)b; }
                else if (b >= 32 && b < 127) s += (char)b;
                else { char buf[8]; std::snprintf(buf, sizeof(buf), "\\x%02x", b); s += buf; }
            }
            return s + "\"";
        }
        case VT::Color: {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "#%06x", (unsigned)(v.o->color & 0xFFFFFF));
            return buf;
        }
        case VT::Array: {
            // nested rendering driven by the shape
            std::function<std::string(const Value&, const Value&)> render =
                [&](const Value& arr, const Value& depth) -> std::string {
                (void)depth;
                if (arr.o->dims.size() <= 1) {
                    std::string s = "[";
                    int64_t n = arr.o->dims.empty() ? 0 : arr.o->dims[0];
                    for (int64_t i = 0; i < n; i++) {
                        if (i) s += ", ";
                        s += toStr(getIndex(arr, Value::integer(i)));
                    }
                    return s + "]";
                }
                std::string s = "[";
                int64_t n = arr.o->dims[0];
                for (int64_t i = 0; i < n; i++) {
                    if (i) s += ", ";
                    s += render(arr.arrayView(arr.o->offset + i * arr.o->strides[0], 1), Value::null());
                }
                return s + "]";
            };
            return render(v, Value::null());
        }
        case VT::List: {
            std::string s = "[";
            for (size_t i = 0; i < v.o->items.size(); i++) {
                if (i) s += ", ";
                s += repr(v.o->items[i]);
            }
            return s + "]";
        }
        case VT::Tuple: {
            std::string s = "(";
            for (size_t i = 0; i < v.o->items.size(); i++) {
                if (i) s += ", ";
                s += repr(v.o->items[i]);
            }
            if (v.o->items.size() == 1) s += ",";
            return s + ")";
        }
        case VT::Map: {
            std::string s = "{";
            bool first = true;
            for (auto& kv : v.o->map) {
                if (!first) s += ", ";
                first = false;
                s += kv.first + ": " + repr(kv.second);
            }
            return s + "}";
        }
        case VT::UiNode: {
            std::string s = "<" + v.o->str;
            for (auto& kv : v.o->map) s += " " + kv.first + "=" + repr(kv.second);
            if (!v.o->items.empty()) s += " (" + formatInt((int64_t)v.o->items.size()) + " children)";
            return s + ">";
        }
        case VT::Function: return "<fn " + v.o->str + ">";
        case VT::Native:   return "<native " + v.o->str + ">";
        case VT::Bound:    return "<bound " + (v.o->items.size() > 1 ? repr(v.o->items[1]) : std::string("?")) + ">";
        case VT::Class:    return "<class " + v.o->klass->name + ">";
        case VT::Instance: {
            Value out;
            if (callMagic(v, "__str__", {}, out)) return toStr(out);
            return "<" + v.o->klass->name + ">";
        }
        case VT::Iter:   return "<iterator>";
        case VT::Module: return "<module " + v.o->str + ">";
    }
    return "?";
}

std::string VM::repr(const Value& v) {
    if (v.t == VT::Str) return "\"" + v.o->str + "\"";
    if (v.t == VT::Instance) {
        Value out;
        if (callMagic(v, "__repr__", {}, out)) return toStr(out);
    }
    return toStr(v);
}

// ---------------------------------------------------------------- truthiness
bool VM::truthy(const Value& v) {
    if (v.t == VT::Instance) {
        Value out;
        if (callMagic(v, "__bool__", {}, out)) return out.isTruthyRaw();
        if (callMagic(v, "__len__", {}, out)) return out.t == VT::Int ? out.i != 0 : out.isTruthyRaw();
        return true;
    }
    return v.isTruthyRaw();
}

// ---------------------------------------------------------------- globals
void VM::defineGlobal(const std::string& name, const Value& v) {
    auto it = globals.find(name);
    if (it != globals.end() && it->second) { *it->second = v; }
    else globals[name] = std::make_shared<Value>(v);
    if (stateNames.count(name) && onStateChange) onStateChange(*this);
}

bool VM::getGlobal(const std::string& name, Value& out) {
    auto it = globals.find(name);
    if (it == globals.end() || !it->second) return false;
    out = *it->second;
    return true;
}

Value VM::makeNative(const std::string& name, std::function<Value(VM&, std::vector<Value>&)> fn) {
    return Value::native(name, std::move(fn));
}

// ---------------------------------------------------------------- classes
void VM::resolveParent(ClassInfo* ci) {
    if (ci->parent || ci->parentName.empty()) return;
    Value pv;
    if (getGlobal(ci->parentName, pv) && pv.t == VT::Class) {
        ci->parent = pv.o->klass;
        ci->buildLayout();
    }
}

bool VM::findMethod(const Value& obj, const std::string& name, Value& out) {
    if (obj.t == VT::Instance && obj.o->klass) {
        MethodInfo* m = obj.o->klass->findMethod(name);
        if (m) { out = Value::bound(obj, m->fn); return true; }
    }
    if (obj.t == VT::Class && obj.o->klass) {
        Value* s = obj.o->klass->findStatic(name);
        if (s) { out = *s; return true; }
    }
    return false;
}

bool VM::callMagic(const Value& obj, const char* name, std::vector<Value> args, Value& out) {
    if (obj.t != VT::Instance || !obj.o->klass) return false;
    MethodInfo* m = obj.o->klass->findMethod(name);
    if (!m) return false;
    Value b = Value::bound(obj, m->fn);
    out = callSync(b, std::move(args));
    return true;
}

Value VM::getField(const Value& obj, const std::string& name) {
    if (obj.t == VT::Instance) {
        ClassInfo* k = obj.o->klass.get();
        int idx = k->findField(name);
        if (idx >= 0 && idx < (int)obj.o->fields.size()) return obj.o->fields[idx];
        MethodInfo* m = k->findMethod(name);
        if (m) return Value::bound(obj, m->fn);
        Value* s = k->findStatic(name);
        if (s) return *s;
        throwError("'" + k->name + "' has no member '" + name + "'");
    }
    if (obj.t == VT::Class) {
        ClassInfo* k = obj.o->klass.get();
        Value* s = k->findStatic(name);
        if (s) return *s;
        return Value::null();
    }
    if (obj.t == VT::Module || obj.t == VT::UiNode || obj.t == VT::Map) {
        auto it = obj.o->map.find(name);
        if (it != obj.o->map.end()) return it->second;
        return Value::null();
    }
    Value m = builtinMethod(*this, obj, name);
    if (m.t != VT::Null) return m;
    throwError(std::string(obj.typeName()) + " has no member '" + name + "'");
}

void VM::setField(Value& obj, const std::string& name, const Value& v, int line) {
    if (obj.t == VT::Instance) {
        ClassInfo* k = obj.o->klass.get();
        int idx = k->findField(name);
        if (idx < 0) {
            resolveParent(k);
            k->buildLayout();
            idx = k->findField(name);
        }
        if (idx >= 0 && idx < (int)obj.o->fields.size()) {
            obj.o->fields[idx] = deepCopy(v);
            return;
        }
        // extra dynamic field
        if (obj.o->fields.size() < k->fieldNames.size()) obj.o->fields.resize(k->fieldNames.size());
        obj.o->fields.push_back(deepCopy(v));
        k->fieldIndex[name] = (int)k->fieldNames.size();
        k->fieldNames.push_back(name);
        k->fieldDefaults.push_back(Value::null());
        return;
    }
    if (obj.t == VT::Class) {
        ClassInfo* k = obj.o->klass.get();
        for (auto& m : k->statics) {
            if (m.name == name) { m.fn = deepCopy(v); return; }
        }
        k->statics.push_back({name, deepCopy(v), true});
        return;
    }
    if (obj.t == VT::Module || obj.t == VT::UiNode || obj.t == VT::Map) {
        obj.o->map[name] = deepCopy(v);
        return;
    }
    throwError(std::string("cannot set member '") + name + "' on " + obj.typeName());
    (void)line;
}

Value VM::getIndex(const Value& obj, const Value& idx) {
    if (obj.t == VT::Array) {
        int64_t i = idx.t == VT::Int ? idx.i : (int64_t)idx.asFloat();
        int64_t n = obj.o->dims.empty() ? 0 : obj.o->dims[0];
        if (i < 0 || i >= n)
            throwError("array index " + formatInt(i) + " out of range [0, " + formatInt(n) + ")");
        int64_t stride = obj.o->strides.empty() ? 1 : obj.o->strides[0];
        int64_t at = obj.o->offset + i * stride;
        if (obj.o->dims.size() == 1) {
            if (at < 0 || !obj.o->buf || at >= (int64_t)obj.o->buf->size()) return obj.defaultElement();
            return (*obj.o->buf)[(size_t)at];
        }
        return obj.arrayView(at, 1);            // row view shares the buffer
    }

    switch (obj.t) {
        case VT::List:
        case VT::Tuple: {
            if (idx.t != VT::Int) throwError("list index must be an int");
            int64_t i = idx.i;
            int64_t n = (int64_t)obj.o->items.size();
            if (i < 0) i += n;
            if (i < 0 || i >= n) throwError("index out of range: " + formatInt(idx.i) + " (size " + formatInt(n) + ")");
            return obj.o->items[(size_t)i];
        }
        case VT::Str: {
            if (idx.t != VT::Int) throwError("string index must be an int");
            int64_t i = idx.i;
            int64_t n = (int64_t)obj.o->str.size();
            if (i < 0) i += n;
            if (i < 0 || i >= n) throwError("index out of range: " + formatInt(idx.i));
            return Value::str(std::string(1, obj.o->str[(size_t)i]));
        }
        case VT::Bytes: {
            if (idx.t != VT::Int) throwError("bytes index must be an int");
            int64_t i = idx.i;
            int64_t n = (int64_t)obj.o->bytes.size();
            if (i < 0) i += n;
            if (i < 0 || i >= n) throwError("index out of range: " + formatInt(idx.i));
            return Value::integer(obj.o->bytes[(size_t)i]);
        }
        case VT::Map:
        case VT::Module:
        case VT::UiNode: {
            std::string key = toStr(idx);
            auto it = obj.o->map.find(key);
            if (it == obj.o->map.end()) return Value::null();
            return it->second;
        }
        case VT::Instance: {
            MethodInfo* m = obj.o->klass ? obj.o->klass->findMethod("__get__") : nullptr;
            if (!m) throwError("'" + obj.o->klass->name + "' does not support indexing");
            return callSync(Value::bound(obj, m->fn), {idx});
        }
        default:
            throwError(std::string(obj.typeName()) + " does not support indexing");
    }
}

void VM::setIndex(Value& obj, const Value& idx, const Value& v) {
    if (obj.t == VT::Array) {
        int64_t i = idx.t == VT::Int ? idx.i : (int64_t)idx.asFloat();
        int64_t n = obj.o->dims.empty() ? 0 : obj.o->dims[0];
        if (i < 0 || i >= n)
            throwError("array index " + formatInt(i) + " out of range [0, " + formatInt(n) + ")");
        int64_t stride = obj.o->strides.empty() ? 1 : obj.o->strides[0];
        int64_t at = obj.o->offset + i * stride;
        if (!obj.o->buf) obj.o->buf = std::make_shared<std::vector<Value>>();
        if (at >= 0 && at < (int64_t)obj.o->buf->size()) (*obj.o->buf)[(size_t)at] = deepCopy(v);
        return;
    }

    switch (obj.t) {
        case VT::List: {
            if (idx.t != VT::Int) throwError("list index must be an int");
            int64_t i = idx.i;
            int64_t n = (int64_t)obj.o->items.size();
            if (i < 0) i += n;
            if (i < 0 || i >= n) throwError("index out of range: " + formatInt(idx.i));
            obj.o->items[(size_t)i] = deepCopy(v);
            return;
        }
        case VT::Bytes: {
            if (idx.t != VT::Int) throwError("bytes index must be an int");
            int64_t i = idx.i;
            int64_t n = (int64_t)obj.o->bytes.size();
            if (i < 0) i += n;
            if (i < 0 || i >= n) throwError("index out of range: " + formatInt(idx.i));
            if (v.t != VT::Int) throwError("bytes element must be an int");
            obj.o->bytes[(size_t)i] = (uint8_t)(v.i & 0xff);
            return;
        }
        case VT::Map:
        case VT::Module:
        case VT::UiNode:
            obj.o->map[toStr(idx)] = deepCopy(v);
            return;
        case VT::Instance: {
            MethodInfo* m = obj.o->klass ? obj.o->klass->findMethod("__set__") : nullptr;
            if (!m) throwError("'" + obj.o->klass->name + "' does not support index assignment");
            callSync(Value::bound(obj, m->fn), {idx, v});
            return;
        }
        default:
            throwError(std::string(obj.typeName()) + " does not support index assignment");
    }
}

// ---------------------------------------------------------------- iterators
Value VM::makeIter(const Value& v) {
    if (v.t == VT::Array) {
        std::vector<Value> out;
        int64_t n = v.o->dims.empty() ? 0 : v.o->dims[0];
        if (v.o->dims.size() <= 1) {
            for (int64_t i = 0; i < n; i++) out.push_back(getIndex(v, Value::integer(i)));
        } else {
            for (int64_t i = 0; i < n; i++) out.push_back(v.arrayView(v.o->offset + i * v.o->strides[0], 1));
        }
        return Value::iterList(out, false);
    }

    switch (v.t) {
        case VT::Iter: return v;
        case VT::List: return Value::iterList(v.o->items, false);
        case VT::Tuple: return Value::iterList(v.o->items, true);
        case VT::Str: {
            std::vector<Value> chars;
            chars.reserve(v.o->str.size());
            for (char c : v.o->str) chars.push_back(Value::str(std::string(1, c)));
            return Value::iterList(chars, false);
        }
        case VT::Bytes: {
            std::vector<Value> bs;
            for (uint8_t b : v.o->bytes) bs.push_back(Value::integer(b));
            return Value::iterList(bs, false);
        }
        case VT::Instance: {
            Value out;
            if (callMagic(v, "__iter__", {}, out)) return makeIter(out);
            throwError("'" + v.o->klass->name + "' is not iterable");
        }
        default:
            throwError(std::string(v.typeName()) + " is not iterable");
    }
}

// ---------------------------------------------------------------- ui
void VM::uiAppend(const Value& listValue, const Value& node) {
    if (node.isNull()) return;
    if (node.t == VT::List || node.t == VT::Tuple) {
        for (auto& it : node.o->items) uiAppend(listValue, it);
        return;
    }
    if (node.t != VT::UiNode) {
        throwError("expected a component but got " + std::string(node.typeName()));
    }
    listValue.o->items.push_back(node);
}

void VM::uiAppendChildren(Value& node, const Value& children) {
    if (node.t != VT::UiNode) return;
    if (children.t == VT::List || children.t == VT::Tuple) {
        for (auto& c : children.o->items) {
            if (c.isNull()) continue;
            if (c.t == VT::List || c.t == VT::Tuple) uiAppendChildren(node, c);
            else if (c.t == VT::UiNode) node.o->items.push_back(c);
        }
    } else if (children.t == VT::UiNode) {
        node.o->items.push_back(children);
    }
}

void VM::applyStyle(Value& node) {
    if (node.t != VT::UiNode) return;
    auto it = node.o->map.find("style");
    if (it == node.o->map.end()) return;
    Value st = it->second;
    if (st.t != VT::Class || !st.o->klass) return;
    ClassInfo* k = st.o->klass.get();
    resolveParent(k);
    k->buildLayout();
    for (size_t i = 0; i < k->fieldNames.size(); i++) {
        const std::string& n = k->fieldNames[i];
        if (node.o->map.count(n)) continue;
        node.o->map[n] = i < st.o->klass->fieldDefaults.size() ? st.o->klass->fieldDefaults[i] : Value::null();
    }
    node.o->map.erase("style");
}

// ---------------------------------------------------------------- calls
void VM::bindArgs(Frame& f, const std::shared_ptr<Chunk>& ch, const Value& pos, const Value& named) {
    size_t base = ch->isMethod ? 1 : 0;
    size_t nparams = ch->params.size();
    size_t nfixed = 0;
    int varargIdx = -1;
    for (size_t i = 0; i < nparams; i++) {
        if (ch->params[i].vararg) varargIdx = (int)i;
        else nfixed++;
    }
    const std::vector<Value>& pa = pos.o ? pos.o->items : std::vector<Value>{};
    f.provided.assign(nparams, 0);
    f.argc = (int)pa.size();
    size_t pi = 0;
    for (size_t i = 0; i < nparams; i++) {
        if ((int)i == varargIdx) continue;
        if (pi < pa.size()) {
            *f.locals[base + i] = deepCopy(pa[pi++]);
            f.provided[i] = 1;
        }
    }
    if (varargIdx >= 0) {
        std::vector<Value> rest;
        while (pi < pa.size()) rest.push_back(deepCopy(pa[pi++]));
        *f.locals[base + (size_t)varargIdx] = Value::list(rest);
        f.provided[(size_t)varargIdx] = 1;
    } else if (pi < pa.size()) {
        throwError("too many arguments: '" + ch->fnName + "' expects " + formatInt((int64_t)nfixed) +
                   " but got " + formatInt((int64_t)pa.size()));
    }
    if (named.o && !named.o->map.empty()) {
        for (auto& kv : named.o->map) {
            int idx = -1;
            for (size_t i = 0; i < nparams; i++) if (ch->params[i].name == kv.first) { idx = (int)i; break; }
            if (idx < 0) throwError("unexpected named argument '" + kv.first + "' for '" + ch->fnName + "'");
            if (f.provided[(size_t)idx]) throwError("argument '" + kv.first + "' given twice");
            *f.locals[base + (size_t)idx] = deepCopy(kv.second);
            f.provided[(size_t)idx] = 1;
        }
    }
    for (size_t i = 0; i < nparams; i++) {
        if (!f.provided[i] && !ch->params[i].hasDefault && (int)i != varargIdx)
            throwError("missing argument '" + ch->params[i].name + "' for '" + ch->fnName + "'");
    }
}

// A view/component instance carries window level attributes (`title`, `width`, `height`) as
// fields; copy them onto the root node of the tree it builds so hosts can size the window.
static void applyRootMeta(Value& result, const Value& inst) {

    if (inst.t != VT::Instance || !inst.o->klass) return;
    Value* node = nullptr;
    if (result.t == VT::UiNode) node = &result;
    else if ((result.t == VT::List || result.t == VT::Tuple) && result.o->items.size() == 1 &&
             result.o->items[0].t == VT::UiNode)
        node = &result.o->items[0];
    if (!node) return;
    ClassInfo* k = inst.o->klass.get();
    for (const char* key : {"title", "width", "height"}) {
        if (node->o->map.count(key)) continue;
        int idx = k->findField(key);
        if (idx < 0 || idx >= (int)inst.o->fields.size()) continue;
        const Value& v = inst.o->fields[(size_t)idx];
        if (!v.isNull()) node->o->map[key] = v;
    }
}

void VM::pushFrame(const Value& fn, const Cell& thisCell) {
    Obj* o = fn.o.get();
    if (!o || !o->chunk) throwError("corrupt function value");
    if (frames.size() >= (size_t)kMaxFrames) throwError("stack overflow (call depth " + formatInt((int64_t)frames.size()) + ")");
    Frame fr;
    fr.chunk = o->chunk;
    fr.ip = 0;
    fr.upvals = o->upvals;
    fr.locals.resize(fr.chunk->numLocals);
    for (auto& c : fr.locals) c = std::make_shared<Value>();
    if (thisCell && fr.chunk->isMethod) fr.locals[0] = thisCell;
    fr.stackBase = stack.size();
    frames.push_back(std::move(fr));
}

void VM::instantiate(const Value& cls, const Value& pos, const Value& named,
                     const Value& children, bool hasChildren) {
    auto klass = cls.o->klass;
    resolveParent(klass.get());
    klass->buildLayout();
    std::vector<Value> fields;
    fields.reserve(klass->fieldDefaults.size());
    for (auto& d : klass->fieldDefaults) fields.push_back(deepCopy(d));
    Value inst = Value::instance(klass, std::move(fields));
    if (hasChildren) {
        int idx = klass->findField("__children");
        if (idx >= 0 && idx < (int)inst.o->fields.size()) inst.o->fields[(size_t)idx] = children;
        else {
            klass->fieldIndex["__children"] = (int)klass->fieldNames.size();
            klass->fieldNames.push_back("__children");
            klass->fieldDefaults.push_back(Value::list({}));
            inst.o->fields.push_back(children);
        }
    }
    auto cell = std::make_shared<Value>(inst);

    if (klass->initFn.t == VT::Function) {
        pushFrame(klass->initFn, cell);
        bindArgs(frames.back(), klass->initFn.o->chunk, pos, named);
        frames.back().klass = klass;
        frames.back().buildOnReturn = true;
    } else {
        if (klass->buildFn.t == VT::Function) {
            pushFrame(klass->buildFn, cell);
            bindArgs(frames.back(), klass->buildFn.o->chunk, pos, named);
        } else {
            push(inst);
        }
    }
}

Value VM::callFunction(const Value& callee, const Value& pos, const Value& named,
                       const Cell& thisCell, const Value& children, bool hasChildren) {
    switch (callee.t) {
        case VT::Native: {
            pendingNamed = named;
            pendingSelf = thisCell ? *thisCell : Value::null();
            std::vector<Value>& args = pos.o->items;
            Value r = callee.o->fn(*this, args);
            pendingNamed = Value::map({});
            pendingSelf = Value::null();
            if (hasChildren && r.t == VT::UiNode) uiAppendChildren(r, children);
            if (r.t == VT::UiNode) applyStyle(r);
            push(r);
            return r;
        }
        case VT::Function: {
            pushFrame(callee, thisCell);
            bindArgs(frames.back(), callee.o->chunk, pos, named);
            return Value::null();
        }
        case VT::Bound: {
            Value recv = callee.o->items[0];
            Value fn = callee.o->items[1];
            if (fn.t == VT::Native) {
                pendingNamed = named;
                pendingSelf = recv;
                std::vector<Value>& args = pos.o->items;
                Value r = fn.o->fn(*this, args);
                pendingNamed = Value::map({});
                pendingSelf = Value::null();
                if (hasChildren && r.t == VT::UiNode) uiAppendChildren(r, children);
                if (r.t == VT::UiNode) applyStyle(r);
                push(r);
                return r;
            }
            auto cell = std::make_shared<Value>(recv);
            pushFrame(fn, cell);
            bindArgs(frames.back(), fn.o->chunk, pos, named);
            return Value::null();
        }
        case VT::Class: {
            instantiate(callee, pos, named, children, hasChildren);
            return Value::null();
        }
        case VT::Instance: {
            Value call;
            if (!findMethod(callee, "__call__", call))
                throwError("'" + callee.o->klass->name + "' is not callable");
            return callFunction(call, pos, named, thisCell, children, hasChildren);
        }
        default:
            throwError(std::string(callee.typeName()) + " is not callable");
    }
}

Value VM::callSync(const Value& callee, std::vector<Value> args) {
    size_t depth = frames.size();
    Value pos = Value::list(std::move(args));
    Value named = Value::map({});
    callFunction(callee, pos, named, nullptr, Value::null(), false);
    if (frames.size() > depth) return execute(depth);
    return pop();
}

// ---------------------------------------------------------------- exceptions
bool VM::unwind(const Value& err) {
    while (!tryFrames.empty()) {
        TryFrame t = tryFrames.back();
        tryFrames.pop_back();
        if (t.frameIndex >= frames.size()) continue;
        while (frames.size() > t.frameIndex + 1) frames.pop_back();
        Frame& f = frames.back();
        f.ip = t.handler;
        stack.resize(t.stackDepth);
        push(err);
        return true;
    }
    return false;
}

std::string VM::stackTrace() {
    std::string s;
    for (size_t i = frames.size(); i-- > 0;) {
        Frame& f = frames[i];
        size_t ip = f.ip > 0 ? f.ip - 1 : 0;
        int line = ip < f.chunk->lines.size() ? f.chunk->lines[ip] : 0;
        s += "  at " + f.chunk->fnName + " (" + f.chunk->file + ":" + formatInt(line) + ")\n";
    }
    return s;
}

// ---------------------------------------------------------------- the loop
Value VM::run() {
    Value mainFn = Value::function(mainChunk);
    pushFrame(mainFn, nullptr);
    return execute(0);
}

Value VM::execute(size_t stopDepth) {
    Value result;
    for (;;) {
        try {
            for (;;) {
                Frame& f = frames.back();
                std::vector<uint8_t>& code = f.chunk->code;
                if (f.ip >= code.size()) {
                    // implicit return
                    stack.resize(f.stackBase);
                    frames.pop_back();
                    if (frames.size() == stopDepth) return Value::null();
                    push(Value::null());
                    continue;
                }
                uint8_t op = code[f.ip++];
                int line = f.chunk->lines[f.ip - 1];
                (void)line;
                switch (op) {
                    case OP_NOP: break;
                    case OP_CONST: {
                        uint16_t k = (uint16_t)((code[f.ip] << 8) | code[f.ip + 1]); f.ip += 2;
                        push(f.chunk->consts[k]);
                        break;
                    }
                    case OP_NULL: push(Value::null()); break;
                    case OP_TRUE: push(Value::boolean(true)); break;
                    case OP_FALSE: push(Value::boolean(false)); break;
                    case OP_INT1: {
                        int8_t v = (int8_t)code[f.ip++];
                        push(Value::integer(v));
                        break;
                    }
                    case OP_POP: stack.pop_back(); break;
                    case OP_DUP: push(peek(0)); break;
                    case OP_DUP2: {
                        Value a = peek(1), b = peek(0);
                        push(a); push(b);
                        break;
                    }
                    case OP_SWAP: {
                        std::swap(stack[stack.size() - 1], stack[stack.size() - 2]);
                        break;
                    }
                    case OP_GET_LOCAL: {
                        uint8_t s = code[f.ip++];
                        Cell& c = f.locals[s];
                        if (!c) throwError("variable has been deleted");
                        push(*c);
                        break;
                    }
                    case OP_SET_LOCAL: {
                        uint8_t s = code[f.ip++];
                        Value v = pop();
                        Cell& c = f.locals[s];
                        if (!c) c = std::make_shared<Value>();
                        *c = deepCopy(v);
                        break;
                    }
                    case OP_INIT_LOCAL: {
                        uint8_t s = code[f.ip++];
                        Value v = pop();
                        f.locals[s] = std::make_shared<Value>(deepCopy(v));
                        break;
                    }
                    case OP_DEL_LOCAL: {
                        uint8_t s = code[f.ip++];
                        f.locals[s] = nullptr;
                        break;
                    }
                    case OP_GET_UPVAL: {
                        uint8_t s = code[f.ip++];
                        push(*f.upvals[s]);
                        break;
                    }
                    case OP_SET_UPVAL: {
                        uint8_t s = code[f.ip++];
                        Value v = pop();
                        *f.upvals[s] = deepCopy(v);
                        break;
                    }
                    case OP_GET_GLOBAL: {
                        uint16_t k = (uint16_t)((code[f.ip] << 8) | code[f.ip + 1]); f.ip += 2;
                        const std::string& name = f.chunk->consts[k].o->str;
                        Value out;
                        if (getGlobal(name, out)) { push(out); break; }
                        // fall back to a member of `this` (fields, then methods, then statics)
                        if (f.chunk->isMethod && f.locals.size() && f.locals[0] &&
                            f.locals[0]->t == VT::Instance) {
                            ClassInfo* k = f.locals[0]->o->klass.get();
                            int idx = k->findField(name);
                            if (idx >= 0 && idx < (int)f.locals[0]->o->fields.size()) {
                                push(f.locals[0]->o->fields[(size_t)idx]);
                                break;
                            }
                            MethodInfo* m = k->findMethod(name);
                            if (m) { push(Value::bound(*f.locals[0], m->fn)); break; }
                            Value* s = k->findStatic(name);
                            if (s) { push(*s); break; }
                        }
                        throwError("undefined variable '" + name + "'");
                    }
                    case OP_SET_GLOBAL: {
                        uint16_t k = (uint16_t)((code[f.ip] << 8) | code[f.ip + 1]); f.ip += 2;
                        const std::string& name = f.chunk->consts[k].o->str;
                        Value v = pop();
                        auto it = globals.find(name);
                        if (it != globals.end() && it->second) {
                            *it->second = deepCopy(v);
                            if (stateNames.count(name) && onStateChange) onStateChange(*this);
                            break;
                        }
                        if (f.chunk->isMethod && f.locals.size() && f.locals[0] &&
                            f.locals[0]->t == VT::Instance) {
                            int idx = f.locals[0]->o->klass->findField(name);
                            if (idx >= 0 && idx < (int)f.locals[0]->o->fields.size()) {
                                f.locals[0]->o->fields[(size_t)idx] = deepCopy(v);
                                break;
                            }
                        }
                        throwError("assignment to undefined variable '" + name + "'");
                    }
                    case OP_DEF_GLOBAL: {
                        uint16_t k = (uint16_t)((code[f.ip] << 8) | code[f.ip + 1]); f.ip += 2;
                        const std::string& name = f.chunk->consts[k].o->str;
                        defineGlobal(name, deepCopy(pop()));
                        break;
                    }
                    case OP_DEL_GLOBAL: {
                        uint16_t k = (uint16_t)((code[f.ip] << 8) | code[f.ip + 1]); f.ip += 2;
                        globals.erase(f.chunk->consts[k].o->str);
                        break;
                    }
                    case OP_GET_FIELD: {
                        uint16_t k = (uint16_t)((code[f.ip] << 8) | code[f.ip + 1]); f.ip += 2;
                        Value obj = pop();
                        push(getField(obj, f.chunk->consts[k].o->str));
                        break;
                    }
                    case OP_SET_FIELD: {
                        uint16_t k = (uint16_t)((code[f.ip] << 8) | code[f.ip + 1]); f.ip += 2;
                        Value v = pop();
                        Value obj = pop();
                        setField(obj, f.chunk->consts[k].o->str, v, line);
                        break;
                    }
                    case OP_NEW_ARRAY: {
                        uint8_t nd = code[f.ip++];
                        uint8_t info = code[f.ip++];
                        bool eltStr = (info & 2) != 0;
                        bool dyn = (info & 4) != 0;
                        int kindCode = (info >> 3);
                        NumKind ek = kindCode > 0 ? (NumKind)(kindCode - 1) : NumKind::None;
                        // the initialiser list sits on top of the dimensions
                        Value init;
                        bool hasInit = (info & 1) != 0;
                        if (hasInit) init = pop();
                        std::vector<int64_t> dims((size_t)nd, 0);
                        for (int i = (int)nd - 1; i >= 0; i--) {
                            Value d = pop();
                            dims[(size_t)i] = d.t == VT::Int ? d.i : (int64_t)d.asFloat();
                        }
                        int64_t total = 1;
                        for (int64_t d : dims) total *= d < 0 ? 0 : d;
                        auto buf = std::make_shared<std::vector<Value>>();
                        Value proto = Value::array(buf, dims, {}, 0, ek, eltStr, dyn);
                        buf->assign((size_t)total, proto.defaultElement());
                        if (hasInit && total > 0) {
                            std::vector<Value> src;
                            if (init.t == VT::List || init.t == VT::Tuple) src = init.o->items;
                            else if (init.t == VT::Array && init.o->buf) src = *init.o->buf;
                            for (size_t i = 0; i < src.size() && i < (size_t)total; i++)
                                (*buf)[i] = deepCopy(src[i]);
                        }
                        std::vector<int64_t> strides((size_t)nd, 1);
                        for (int i = (int)nd - 2; i >= 0; i--)
                            strides[(size_t)i] = strides[(size_t)i + 1] * dims[(size_t)i + 1];
                        push(Value::array(buf, dims, strides, 0, ek, eltStr, dyn));
                        break;
                    }
                    case OP_LOCAL_ADD_IMM: {
                        uint8_t sl = code[f.ip++];
                        int8_t imm = (int8_t)code[f.ip++];
                        Cell& c = f.locals[sl];
                        if (!c) c = std::make_shared<Value>(Value::integer(0));
                        // the original GET_LOCAL/INT1/ADD/SET_LOCAL sequence leaves the stack
                        // unchanged, so the fused form must store the result, never push it
                        if (c->t == VT::Int) *c = Value::typedInt(c->i + imm, c->k);
                        else *c = binaryResult(OP_ADD, *c, Value::integer(imm), "+");
                        break;
                    }
                    case OP_LOCAL_SUB_IMM: {
                        uint8_t sl = code[f.ip++];
                        int8_t imm = (int8_t)code[f.ip++];
                        Cell& c = f.locals[sl];
                        if (!c) c = std::make_shared<Value>(Value::integer(0));
                        if (c->t == VT::Int) *c = Value::typedInt(c->i - imm, c->k);
                        else *c = binaryResult(OP_SUB, *c, Value::integer(imm), "-");
                        break;
                    }
                    case OP_LOCAL_ADD_LOCAL: {
                        uint8_t sl = code[f.ip++];
                        uint8_t src = code[f.ip++];
                        Cell& c = f.locals[sl];
                        Value sv = f.locals[src] ? *f.locals[src] : Value::integer(0);
                        if (!c) c = std::make_shared<Value>(Value::integer(0));
                        if (c->t == VT::Int && sv.t == VT::Int)
                            *c = Value::typedInt(c->i + sv.i, promoteNum(c->k, sv.k));
                        else *c = binaryResult(OP_ADD, *c, sv, "+");
                        break;
                    }
                    case OP_GET_INDEX_FAST: {
                        Value idx = pop();
                        Value obj = pop();
                        if (obj.t == VT::Array) {
                            int64_t i = idx.t == VT::Int ? idx.i : (int64_t)idx.asFloat();
                            int64_t stride = obj.o->strides.empty() ? 1 : obj.o->strides[0];
                            int64_t at = obj.o->offset + i * stride;
                            if (obj.o->dims.size() == 1) {
                                push(obj.o->buf && at >= 0 && at < (int64_t)obj.o->buf->size()
                                         ? (*obj.o->buf)[(size_t)at] : obj.defaultElement());
                            } else {
                                push(obj.arrayView(at, 1));
                            }
                            break;
                        }
                        push(getIndex(obj, idx));
                        break;
                    }
                    case OP_GET_INDEX: {
                        Value idx = pop();
                        Value obj = pop();
                        push(getIndex(obj, idx));
                        break;
                    }
                    case OP_SET_INDEX: {
                        Value v = pop();
                        Value idx = pop();
                        Value obj = pop();
                        setIndex(obj, idx, v);
                        break;
                    }
                    case OP_GET_SUPER: {
                        uint16_t k = (uint16_t)((code[f.ip] << 8) | code[f.ip + 1]); f.ip += 2;
                        uint16_t ck = (uint16_t)((code[f.ip] << 8) | code[f.ip + 1]); f.ip += 2;
                        ClassInfo* owner = f.chunk->consts[ck].o->klass.get();
                        resolveParent(owner);
                        ClassInfo* par = owner->parent.get();
                        MethodInfo* m = par ? par->findMethod(f.chunk->consts[k].o->str) : nullptr;
                        if (!m) throwError("super has no method '" + f.chunk->consts[k].o->str + "'");
                        Value self = (f.locals.size() && f.locals[0]) ? *f.locals[0] : Value::null();
                        push(Value::bound(self, m->fn));
                        break;
                    }
                    case OP_SUPER_INIT: {
                        uint16_t ck = (uint16_t)((code[f.ip] << 8) | code[f.ip + 1]); f.ip += 2;
                        Value named = pop();
                        Value pos = pop();
                        ClassInfo* owner = f.chunk->consts[ck].o->klass.get();
                        resolveParent(owner);
                        ClassInfo* par = owner->parent.get();
                        if (!par || par->initFn.t != VT::Function)
                            throwError("base class has no constructor body");
                        Cell self = (f.locals.size() && f.locals[0]) ? f.locals[0] : nullptr;
                        pushFrame(par->initFn, self);
                        bindArgs(frames.back(), par->initFn.o->chunk, pos, named);
                        continue;
                    }
                    case OP_CLASS: {
                        uint16_t k = (uint16_t)((code[f.ip] << 8) | code[f.ip + 1]); f.ip += 2;
                        auto tmpl = f.chunk->consts[k].o->klass;
                        auto copy = tmpl->shallowCopy();
                        copy->templateOf = tmpl;
                        resolveParent(copy.get());
                        copy->buildLayout();
                        push(Value::klass(copy));
                        break;
                    }
                    case OP_INIT_CLASS: {
                        Value cls = pop();
                        auto k = cls.o->klass;
                        if (k->staticInitFn.t == VT::Function) {
                            auto cell = std::make_shared<Value>(cls);
                            pushFrame(k->staticInitFn, cell);
                            frames.back().discardResult = true;
                        }
                        break;
                    }
                    case OP_CLOSURE: {
                        uint16_t k = (uint16_t)((code[f.ip] << 8) | code[f.ip + 1]); f.ip += 2;
                        Value proto = f.chunk->consts[k];
                        Value fn = Value::function(proto.o->chunk);
                        for (auto& d : proto.o->chunk->upvals) {
                            if (d.fromLocal) fn.o->upvals.push_back(f.locals[d.index]);
                            else fn.o->upvals.push_back(f.upvals[d.index]);
                        }
                        push(fn);
                        break;
                    }
                    case OP_JUMP: {
                        int16_t j = (int16_t)((code[f.ip] << 8) | code[f.ip + 1]); f.ip += 2;
                        f.ip += j;
                        break;
                    }
                    case OP_JUMP_IF_FALSE: {
                        int16_t j = (int16_t)((code[f.ip] << 8) | code[f.ip + 1]); f.ip += 2;
                        Value v = pop();
                        if (!truthy(v)) f.ip += j;
                        break;
                    }
                    case OP_JUMP_IF_FALSE_KEEP: {
                        int16_t j = (int16_t)((code[f.ip] << 8) | code[f.ip + 1]); f.ip += 2;
                        if (!truthy(peek(0))) f.ip += j;
                        break;
                    }
                    case OP_JUMP_IF_TRUE_KEEP: {
                        int16_t j = (int16_t)((code[f.ip] << 8) | code[f.ip + 1]); f.ip += 2;
                        if (truthy(peek(0))) f.ip += j;
                        break;
                    }
                    case OP_LOOP: {
                        int16_t j = (int16_t)((code[f.ip] << 8) | code[f.ip + 1]); f.ip += 2;
                        f.ip += j;
                        break;
                    }
                    case OP_JUMP_IF_PROVIDED: {
                        uint8_t idx = code[f.ip++];
                        int16_t j = (int16_t)((code[f.ip] << 8) | code[f.ip + 1]); f.ip += 2;
                        if (idx < f.provided.size() && f.provided[idx]) f.ip += j;
                        break;
                    }
                    case OP_COLLECT_VARARGS: {
                        f.ip += 2;      // handled during argument binding
                        break;
                    }
                    case OP_BUILD_LIST: {
                        uint16_t n = (uint16_t)((code[f.ip] << 8) | code[f.ip + 1]); f.ip += 2;
                        std::vector<Value> items(n);
                        for (int i = (int)n - 1; i >= 0; i--) items[(size_t)i] = pop();
                        push(Value::list(std::move(items)));
                        break;
                    }
                    case OP_BUILD_TUPLE: {
                        uint16_t n = (uint16_t)((code[f.ip] << 8) | code[f.ip + 1]); f.ip += 2;
                        std::vector<Value> items(n);
                        for (int i = (int)n - 1; i >= 0; i--) items[(size_t)i] = pop();
                        push(Value::tuple(std::move(items)));
                        break;
                    }
                    case OP_LIST_APPEND: {
                        Value v = pop();
                        peek(0).o->items.push_back(v);
                        break;
                    }
                    case OP_LIST_EXTEND: {
                        Value v = pop();
                        Value& target = peek(0);
                        if (v.t == VT::Iter) {
                            Value it = v;
                            Value e;
                            while (true) {
                                Obj* io = it.o.get();
                                if (io->iterKind == IterKind::Range) {
                                    bool done = io->iterStep > 0 ? io->iterCur > io->iterStop
                                                                 : io->iterCur < io->iterStop;
                                    if (done) break;
                                    target.o->items.push_back(Value::integer(io->iterCur));
                                    io->iterCur += io->iterStep;
                                } else {
                                    if (io->iterIdx >= io->items.size()) break;
                                    target.o->items.push_back(io->items[io->iterIdx++]);
                                }
                            }
                        } else {
                            Value it = makeIter(v);
                            Obj* io = it.o.get();
                            while (true) {
                                if (io->iterKind == IterKind::Range) {
                                    bool done = io->iterStep > 0 ? io->iterCur > io->iterStop
                                                                 : io->iterCur < io->iterStop;
                                    if (done) break;
                                    target.o->items.push_back(Value::integer(io->iterCur));
                                    io->iterCur += io->iterStep;
                                } else {
                                    if (io->iterIdx >= io->items.size()) break;
                                    target.o->items.push_back(io->items[io->iterIdx++]);
                                }
                            }
                        }
                        break;
                    }
                    case OP_BUILD_NAMED: {
                        uint16_t n = (uint16_t)((code[f.ip] << 8) | code[f.ip + 1]); f.ip += 2;
                        std::vector<Value> vals(2 * (size_t)n);
                        for (int i = (int)n * 2 - 1; i >= 0; i--) vals[(size_t)i] = pop();
                        std::unordered_map<std::string, Value> m;
                        for (size_t i = 0; i < n; i++) m[toStr(vals[i * 2])] = vals[i * 2 + 1];
                        push(Value::map(std::move(m)));
                        break;
                    }
                    case OP_ITER_RANGE: {
                        Value stop = pop();
                        Value start = pop();
                        if (start.t != VT::Int || stop.t != VT::Int)
                            throwError("'to' expects integer bounds");
                        // `a to b` always counts upwards (the documentation writes `step -1`
                        // explicitly for descending ranges), so `0 to len-1` is empty when len == 0
                        push(Value::iterRange(start.i, stop.i, 1));
                        break;
                    }
                    case OP_ITER_RANGE_STEP: {
                        Value step = pop();
                        Value stop = pop();
                        Value start = pop();
                        if (start.t != VT::Int || stop.t != VT::Int || step.t != VT::Int)
                            throwError("'to'/'step' expect integer bounds");
                        if (step.i == 0) throwError("'step' must not be zero");
                        push(Value::iterRange(start.i, stop.i, step.i));
                        break;
                    }
                    case OP_ITER_VALUE: {
                        Value v = pop();
                        push(makeIter(v));
                        break;
                    }
                    case OP_ITER_NEXT: {
                        int16_t j = (int16_t)((code[f.ip] << 8) | code[f.ip + 1]); f.ip += 2;
                        Value& it = peek(0);
                        Obj* io = it.o.get();
                        if (io->iterKind == IterKind::Range) {
                            bool done = io->iterStep > 0 ? io->iterCur > io->iterStop
                                                         : io->iterCur < io->iterStop;
                            if (done) { f.ip += j; break; }
                            push(Value::integer(io->iterCur));
                            io->iterCur += io->iterStep;
                        } else {
                            if (io->iterIdx >= io->items.size()) { f.ip += j; break; }
                            push(io->items[io->iterIdx++]);
                        }
                        break;
                    }
                    case OP_ITER_BIND2: {
                        uint8_t sa = code[f.ip++];
                        uint8_t sb = code[f.ip++];
                        Value elem = pop();
                        Value a, b;
                        if ((elem.t == VT::List || elem.t == VT::Tuple) && elem.o->items.size() >= 2) {
                            a = elem.o->items[0];
                            b = elem.o->items[1];
                        } else {
                            Value& it = peek(0);
                            Obj* io = it.o.get();
                            int64_t idx = 0;
                            if (io->iterKind == IterKind::Range) {
                                idx = io->iterStep != 0 ? (io->iterCur - io->iterStep - io->iterStart) / io->iterStep : 0;
                            } else {
                                idx = (int64_t)io->iterIdx - 1;
                            }
                            a = Value::integer(idx);
                            b = elem;
                        }
                        f.locals[sa] = std::make_shared<Value>(deepCopy(a));
                        f.locals[sb] = std::make_shared<Value>(deepCopy(b));
                        break;
                    }
                    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: case OP_POW:
                    case OP_EQ: case OP_NE: case OP_LT: case OP_GT: case OP_LE: case OP_GE:
                    case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR: {
                        Value b = pop();
                        Value a = pop();
                        push(binaryResult((Op)op, a, b, opName(op)));
                        break;
                    }
                    case OP_NEG: {
                        Value a = pop();
                        if (a.t == VT::Int) push(Value::integer(-a.i));
                        else if (a.t == VT::Float) push(Value::real(-a.f));
                        else if (a.t == VT::Wide) push(Value::wide(-a.asWide(), a.wideUnsigned()));
                        else if (a.t == VT::LongDouble) push(Value::longDouble(-a.asLongDouble()));
                        else {
                            Value out;
                            if (callMagic(a, "__neg__", {}, out)) push(out);
                            else throwError("cannot negate " + std::string(a.typeName()));
                        }
                        break;
                    }
                    case OP_NOT: {
                        Value a = pop();
                        push(Value::boolean(!truthy(a)));
                        break;
                    }
                    case OP_BNOT: {
                        Value a = pop();
                        if (a.t == VT::Int) push(Value::integer(~a.i));
                        else if (a.t == VT::Bool) push(Value::integer(~(int64_t)(a.b ? 1 : 0)));
                        else {
                            Value out;
                            if (callMagic(a, "__invert__", {}, out)) push(out);
                            else throwError("cannot apply '~' to " + std::string(a.typeName()));
                        }
                        break;
                    }
                    case OP_CALL: {
                        Value named = pop();
                        Value pos = pop();
                        Value callee = pop();
                        callFunction(callee, pos, named, nullptr, Value::null(), false);
                        continue;
                    }
                    case OP_CALL_UI: {
                        Value children = pop();
                        Value named = pop();
                        Value pos = pop();
                        Value callee = pop();
                        callFunction(callee, pos, named, nullptr, children, true);
                        continue;
                    }
                    case OP_RETURN: {
                        Value r = deepCopy(pop());
                        Frame fr = frames.back();
                        frames.pop_back();
                        while (!tryFrames.empty() && tryFrames.back().frameIndex >= frames.size())
                            tryFrames.pop_back();
                        stack.resize(fr.stackBase);
                        if (fr.buildOnReturn && fr.klass) {
                            Cell self = fr.locals.size() ? fr.locals[0] : nullptr;
                            if (fr.klass->buildFn.t == VT::Function) {
                                pushFrame(fr.klass->buildFn, self);
                                frames.back().klass = fr.klass;
                                frames.back().isBuild = true;

                                continue;
                            }
                            if (self) r = *self;
                        }
                        if (fr.isBuild && fr.klass && fr.locals.size() && fr.locals[0])
                            applyRootMeta(r, *fr.locals[0]);

                        if (fr.discardResult) {
                            if (frames.size() == stopDepth) return Value::null();
                            continue;
                        }
                        if (frames.size() == stopDepth) return r;
                        push(r);
                        break;
                    }
                    case OP_RETURN_NULL: {
                        Frame fr = frames.back();
                        frames.pop_back();
                        while (!tryFrames.empty() && tryFrames.back().frameIndex >= frames.size())
                            tryFrames.pop_back();
                        stack.resize(fr.stackBase);
                        if (fr.buildOnReturn && fr.klass) {
                            Cell self = fr.locals.size() ? fr.locals[0] : nullptr;
                            if (fr.klass->buildFn.t == VT::Function) {
                                pushFrame(fr.klass->buildFn, self);
                                frames.back().klass = fr.klass;
                                frames.back().isBuild = true;
                                continue;
                            }
                            Value r = self ? *self : Value::null();
                            if (frames.size() == stopDepth) return r;
                            push(r);
                            break;
                        }
                        if (fr.discardResult) {
                            if (frames.size() == stopDepth) return Value::null();
                            continue;
                        }
                        if (frames.size() == stopDepth) return Value::null();
                        push(Value::null());
                        break;
                    }
                    case OP_THROW: {
                        Value v = pop();
                        if (!unwind(v)) {
                            VMError e("uncaught: " + toStr(v), line);
                            throw e;
                        }
                        continue;
                    }
                    case OP_TRY: {
                        int16_t j = (int16_t)((code[f.ip] << 8) | code[f.ip + 1]); f.ip += 2;
                        TryFrame t;
                        t.handler = f.ip + (size_t)j;
                        t.stackDepth = stack.size();
                        t.frameIndex = frames.size() - 1;
                        tryFrames.push_back(t);
                        break;
                    }
                    case OP_POP_TRY: {
                        if (!tryFrames.empty()) tryFrames.pop_back();
                        break;
                    }
                    case OP_PRINT: {
                        uint8_t n = code[f.ip++];
                        uint8_t hasSep = code[f.ip++];
                        std::string sep = " ";
                        if (hasSep) sep = toStr(pop());
                        std::vector<Value> vals((size_t)n);
                        for (int i = (int)n - 1; i >= 0; i--) vals[(size_t)i] = pop();
                        std::string out;
                        for (size_t i = 0; i < vals.size(); i++) {
                            if (i) out += sep;
                            out += toStr(vals[i]);
                        }
                        out += "\n";
                        write(out);
                        break;
                    }
                    case OP_INPUT: {
                        // the operand names the variable being read, for the GUI prompt
                        uint16_t k = (uint16_t)((code[f.ip] << 8) | code[f.ip + 1]);
                        f.ip += 2;
                        std::string hint = k < f.chunk->consts.size() ? f.chunk->consts[k].o->str
                                                                      : std::string("input");
                        if (inputProvider) {
                            push(Value::str(inputProvider(hint)));
                            break;
                        }
                        std::string lineStr;
                        if (capture) { push(Value::str("")); break; }
                        int ch;
                        while ((ch = std::fgetc(stdin)) != EOF && ch != '\n') lineStr += (char)ch;
                        if (!lineStr.empty() && lineStr.back() == '\r') lineStr.pop_back();
                        push(Value::str(lineStr));
                        break;
                    }
                    case OP_UI_APPEND: {
                        uint8_t slot = code[f.ip++];
                        Value v = pop();
                        Cell& c = f.locals[slot];
                        if (!c) { c = std::make_shared<Value>(Value::list({})); }
                        if (c->t != VT::List) { *c = Value::list({}); }
                        uiAppend(*c, v);
                        break;
                    }
                    case OP_ASSERT: {
                        uint16_t k = (uint16_t)((code[f.ip] << 8) | code[f.ip + 1]); f.ip += 2;
                        Value v = pop();
                        if (!truthy(v)) {
                            std::string msg = f.chunk->consts[k].o->str;
                            Value e = Value::str(msg);
                            if (!unwind(e)) throw VMError(msg, line);
                            continue;
                        }
                        break;
                    }
                    case OP_CONVERT: {
                        uint8_t kk = code[f.ip++];
                        Value v = pop();
                        push(convertNumCode(*this, v, kk, "convert"));
                        break;
                    }
                    case OP_DEEPCOPY: {
                        Value v = pop();
                        push(deepCopy(v));
                        break;
                    }
                    default:
                        throwError("unknown opcode " + formatInt(op));
                }
            }
        } catch (const VMError& e) {
            if (!unwind(Value::str(e.message))) throw;
        } catch (const std::bad_alloc&) {
            throw VMError("out of memory", 0);
        }
    }
    return result;
}

// ---------------------------------------------------------------- operators
static bool isStrLike(const Value& a, const Value& b) {
    return a.t == VT::Str || b.t == VT::Str;
}

// ---------------------------------------------------------------- numeric width conversion
// `int8(x)` / `OP_CONVERT` wrap an integer into the target width and truncate a float toward
// zero, which is what C does for a narrowing conversion.
// the same helper also serves the boxed widths (`long long`, `long double`); `raw` carries their
// conversion code when it is one of kConvertWide*
static Value convertNumCode(VM& vm, const Value& v, uint8_t raw, const char* what) {
    if (raw == kConvertWideS || raw == kConvertWideU) {
        bool uns = raw == kConvertWideU;
        if (v.t == VT::Str) return Value::wide(parseWide(v.o->str, uns), uns);
        if (v.isANumber() || v.t == VT::Bool) return Value::wide(v.asWide(), uns);
        vm.throwError(std::string(what) + ": cannot convert " + v.typeName());
    }
    if (raw == kConvertLongDouble) {
        if (v.t == VT::Str) return Value::longDouble(std::strtold(v.o->str.c_str(), nullptr));
        if (v.isANumber() || v.t == VT::Bool) return Value::longDouble(v.asLongDouble());
        vm.throwError(std::string(what) + ": cannot convert " + v.typeName());
    }
    return convertNum(vm, v, (NumKind)raw, what);
}

static Value convertNum(VM& vm, const Value& v, NumKind k, const char* what) {
    if (k == NumKind::None) return v;
    if (v.t == VT::Int)   return Value::typedInt(v.i, k);
    if (v.t == VT::Float) {
        if (numIsFloat(k)) return Value::typedReal(v.f, k);
        return Value::typedInt((int64_t)v.f, k);
    }
    if (v.t == VT::Bool)  return Value::typedInt(v.b ? 1 : 0, k);
    vm.throwError(std::string(what) + ": cannot convert " + v.typeName() + " to " + numKindName(k));
    return Value::null();
}

Value VM::binaryResult(Op op, const Value& a0, const Value& b0, const char* name) {
    // `.size`-style pseudo methods also carry their value
    const Value& a = (a0.t == VT::Native && a0.o && a0.o->implicit.t != VT::Null) ? a0.o->implicit : a0;
    const Value& b = (b0.t == VT::Native && b0.o && b0.o->implicit.t != VT::Null) ? b0.o->implicit : b0;
    // user-defined operators first
    if (a.t == VT::Instance) {
        const char* magic = nullptr;
        switch (op) {
            case OP_ADD: magic = "__add__"; break;
            case OP_SUB: magic = "__sub__"; break;
            case OP_MUL: magic = "__mul__"; break;
            case OP_DIV: magic = "__div__"; break;
            case OP_MOD: magic = "__mod__"; break;
            case OP_POW: magic = "__pow__"; break;
            case OP_EQ:  magic = "__eq__"; break;
            case OP_NE:  magic = "__ne__"; break;
            case OP_LT:  magic = "__lt__"; break;
            case OP_GT:  magic = "__gt__"; break;
            case OP_LE:  magic = "__le__"; break;
            case OP_GE:  magic = "__ge__"; break;
            case OP_BAND: magic = "__and__"; break;
            case OP_BOR:  magic = "__or__"; break;
            case OP_BXOR: magic = "__xor__"; break;
            case OP_SHL:  magic = "__lshift__"; break;
            case OP_SHR:  magic = "__rshift__"; break;
            default: break;
        }
        if (magic) {
            Value out;
            if (callMagic(a, magic, {b}, out)) return out;
            if (op == OP_NE) {
                Value eq;
                if (callMagic(a, "__eq__", {b}, eq)) return Value::boolean(!truthy(eq));
            }
            if (op == OP_EQ && b.t == VT::Instance) {
                Value out2;
                if (callMagic(b, "__eq__", {a}, out2)) return out2;
            }
        }
    }
    if (b.t == VT::Instance && (op == OP_EQ)) {
        Value out;
        if (callMagic(b, "__eq__", {a}, out)) return out;
    }
    if (b.t == VT::Instance) {
        const char* magic = nullptr;
        switch (op) {
            case OP_ADD: magic = "__radd__"; break;
            case OP_SUB: magic = "__rsub__"; break;
            case OP_MUL: magic = "__rmul__"; break;
            case OP_DIV: magic = "__rdiv__"; break;
            default: break;
        }
        if (magic) {
            Value out;
            if (callMagic(b, magic, {a}, out)) return out;
        }
    }

    switch (op) {
        case OP_EQ: return Value::boolean(valueEquals(a, b));
        case OP_NE: return Value::boolean(!valueEquals(a, b));
        default: break;
    }

    // string concatenation with any value
    if (op == OP_ADD && isStrLike(a, b)) {
        if (a.t == VT::Str || b.t == VT::Str) return Value::str(toStr(a) + toStr(b));
    }
    if (op == OP_MUL) {
        if (a.t == VT::Str && b.t == VT::Int) {
            std::string s;
            for (int64_t i = 0; i < b.i; i++) s += a.o->str;
            return Value::str(s);
        }
        if (a.t == VT::List && b.t == VT::Int) {
            std::vector<Value> items;
            for (int64_t i = 0; i < b.i; i++) for (auto& x : a.o->items) items.push_back(x);
            return Value::list(items);
        }
        if (a.t == VT::Int && b.t == VT::Str) {
            std::string s;
            for (int64_t i = 0; i < a.i; i++) s += b.o->str;
            return Value::str(s);
        }
    }
    if (op == OP_ADD) {
        if (a.t == VT::List && b.t == VT::List) {
            std::vector<Value> items = a.o->items;
            for (auto& x : b.o->items) items.push_back(x);
            return Value::list(items);
        }
        if (a.t == VT::Tuple && b.t == VT::Tuple) {
            std::vector<Value> items = a.o->items;
            for (auto& x : b.o->items) items.push_back(x);
            return Value::tuple(items);
        }
        if (a.t == VT::Bytes && b.t == VT::Bytes) {
            std::vector<uint8_t> bs = a.o->bytes;
            for (auto x : b.o->bytes) bs.push_back(x);
            return Value::bytes(bs);
        }
    }

    // numeric
    bool an = a.isNumber(), bn = b.isNumber();
    switch (op) {
        case OP_LT: case OP_GT: case OP_LE: case OP_GE: {
            if (an && bn) {
                if ((a.t == VT::Wide || b.t == VT::Wide) &&
                    (a.t != VT::LongDouble && b.t != VT::LongDouble && a.t != VT::Float && b.t != VT::Float)) {
                    __int128 x = a.asWide(), y = b.asWide();
                    bool uns = a.wideUnsigned() || b.wideUnsigned();
                    bool r;
                    if (uns) {
                        unsigned __int128 ux = (unsigned __int128)x, uy = (unsigned __int128)y;
                        r = op == OP_LT ? ux < uy : op == OP_GT ? ux > uy : op == OP_LE ? ux <= uy : ux >= uy;
                    } else {
                        r = op == OP_LT ? x < y : op == OP_GT ? x > y : op == OP_LE ? x <= y : x >= y;
                    }
                    return Value::boolean(r);
                }
                if (a.t == VT::Int && b.t == VT::Int) {
                    // C comparison: if either side is unsigned at the same or wider rank, the
                    // comparison happens in the unsigned domain
                    bool useUnsigned = numIsUnsigned(promoteNum(a.numKind(), b.numKind()));
                    bool r;
                    if (useUnsigned) {
                        uint64_t x = (uint64_t)a.i, y = (uint64_t)b.i;
                        r = op == OP_LT ? x < y : op == OP_GT ? x > y : op == OP_LE ? x <= y : x >= y;
                    } else {
                        int64_t x = a.i, y = b.i;
                        r = op == OP_LT ? x < y : op == OP_GT ? x > y : op == OP_LE ? x <= y : x >= y;
                    }
                    return Value::boolean(r);
                }
                double x = a.asFloat(), y = b.asFloat();
                bool r = op == OP_LT ? x < y : op == OP_GT ? x > y : op == OP_LE ? x <= y : x >= y;
                return Value::boolean(r);
            }
            if (a.t == VT::Str && b.t == VT::Str) {
                int c = a.o->str.compare(b.o->str);
                bool r = op == OP_LT ? c < 0 : op == OP_GT ? c > 0 : op == OP_LE ? c <= 0 : c >= 0;
                return Value::boolean(r);
            }
            throwError(std::string("cannot compare ") + a.typeName() + " and " + b.typeName() + " with '" + name + "'");
        }
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: case OP_POW: {
            // 128 bit and long double operands: compute in the boxed domain (no precision loss)
            if (a.t == VT::Wide || b.t == VT::Wide || a.t == VT::LongDouble || b.t == VT::LongDouble) {
                bool wantFloat = a.t == VT::LongDouble || b.t == VT::LongDouble ||
                                 a.t == VT::Float || b.t == VT::Float;
                bool uns = a.wideUnsigned() || b.wideUnsigned();
                if (!wantFloat) {
                    __int128 x = a.asWide(), y = b.asWide();
                    switch (op) {
                        case OP_ADD: return Value::wide(x + y, uns);
                        case OP_SUB: return Value::wide(x - y, uns);
                        case OP_MUL: return Value::wide(x * y, uns);
                        case OP_DIV:
                            if (y == 0) throwError("division by zero");
                            return Value::wide(x / y, uns);
                        case OP_MOD:
                            if (y == 0) throwError("modulo by zero");
                            return Value::wide(x % y, uns);
                        case OP_POW: {
                            if (y < 0) return Value::longDouble(::powl(x, (long double)y));
                            __int128 r = 1, base = x, e = y;
                            while (e > 0) { if (e & 1) r *= base; base *= base; e >>= 1; }
                            return Value::wide(r, uns);
                        }
                        default: break;
                    }
                }
                long double x = a.asLongDouble(), y = b.asLongDouble();
                switch (op) {
                    case OP_ADD: return Value::longDouble(x + y);
                    case OP_SUB: return Value::longDouble(x - y);
                    case OP_MUL: return Value::longDouble(x * y);
                    case OP_DIV:
                        if (y == 0) throwError("division by zero");
                        return Value::longDouble(x / y);
                    case OP_MOD:
                        if (y == 0) throwError("modulo by zero");
                        return Value::longDouble(::fmodl(x, y));
                    case OP_POW: return Value::longDouble(::powl(x, y));
                    default: break;
                }
            }
            if (!an || !bn)
                throwError(std::string("unsupported operand types for '") + name + "': " +
                           a.typeName() + " and " + b.typeName());
            bool intMode = (a.t == VT::Int && b.t == VT::Int);
            NumKind kr = promoteNum(a.numKind(), b.numKind());
            if (op == OP_ADD && intMode) return Value::typedInt(a.i + b.i, kr);
            if (op == OP_SUB && intMode) return Value::typedInt(a.i - b.i, kr);
            if (op == OP_MUL && intMode) return Value::typedInt(a.i * b.i, kr);
            if (op == OP_MOD) {
                if (intMode) {
                    if (b.i == 0) throwError("modulo by zero");
                    return Value::typedInt(a.i % b.i, kr);
                }
                if (b.asFloat() == 0.0) throwError("modulo by zero");
                return Value::real(std::fmod(a.asFloat(), b.asFloat()));
            }
            if (op == OP_POW && intMode && b.i >= 0) {
                int64_t r = 1, base = a.i, e = b.i;
                while (e) { if (e & 1) r *= base; base *= base; e >>= 1; }
                return Value::typedInt(r, kr);
            }
            double x = a.asFloat(), y = b.asFloat();
            switch (op) {
                case OP_ADD: return Value::real(x + y);
                case OP_SUB: return Value::real(x - y);
                case OP_MUL: return Value::real(x * y);
                case OP_DIV: {
                    // C semantics: int / int truncates toward zero; a float on either side
                    // makes it a floating point division
                    if (intMode) {
                        if (b.i == 0) throwError("division by zero");
                        if (a.i == std::numeric_limits<int64_t>::min() && b.i == -1)
                            return Value::typedInt(std::numeric_limits<int64_t>::min(), kr);
                        return Value::typedInt(a.i / b.i, kr);
                    }
                    if (y == 0.0) throwError("division by zero");
                    return Value::typedReal(x / y, kr);
                }
                case OP_POW: return Value::real(std::pow(x, y));
                default: break;
            }
            throwError("bad arithmetic");
        }
        case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR: {
            auto toI = [&](const Value& v) -> int64_t {
                if (v.t == VT::Int) return v.i;
                if (v.t == VT::Bool) return v.b ? 1 : 0;
                throwError(std::string("bitwise operator expects ints, got ") + v.typeName());
            };
            int64_t x = toI(a), y = toI(b);
            switch (op) {
                case OP_BAND: return Value::integer(x & y);
                case OP_BOR:  return Value::integer(x | y);
                case OP_BXOR: return Value::integer(x ^ y);
                case OP_SHL:  return Value::integer(y >= 64 ? 0 : (x << y));
                default:      return Value::integer(y >= 64 ? (x < 0 ? -1 : 0) : (x >> y));
            }
        }
        default: break;
    }
    throwError(std::string("unsupported operator '") + name + "'");
}

const char* opName(uint8_t op) {
    switch (op) {
        case OP_ADD: return "+";
        case OP_SUB: return "-";
        case OP_MUL: return "*";
        case OP_DIV: return "/";
        case OP_MOD: return "%";
        case OP_POW: return "**";
        case OP_EQ: return "==";
        case OP_NE: return "!=";
        case OP_LT: return "<";
        case OP_GT: return ">";
        case OP_LE: return "<=";
        case OP_GE: return ">=";
        case OP_BAND: return "&";
        case OP_BOR: return "|";
        case OP_BXOR: return "^";
        case OP_SHL: return "<<";
        case OP_SHR: return ">>";
        case OP_NEG: return "-";
        case OP_NOT: return "!";
        case OP_BNOT: return "~";
        case OP_CONST: return "const";
        case OP_NULL: return "null";
        case OP_TRUE: return "true";
        case OP_FALSE: return "false";
        case OP_POP: return "pop";
        case OP_DUP: return "dup";
        case OP_DUP2: return "dup2";
        case OP_SWAP: return "swap";
        case OP_GET_LOCAL: return "get_local";
        case OP_SET_LOCAL: return "set_local";
        case OP_INIT_LOCAL: return "init_local";
        case OP_DEL_LOCAL: return "del_local";
        case OP_GET_UPVAL: return "get_upval";
        case OP_SET_UPVAL: return "set_upval";
        case OP_GET_GLOBAL: return "get_global";
        case OP_SET_GLOBAL: return "set_global";
        case OP_DEF_GLOBAL: return "def_global";
        case OP_DEL_GLOBAL: return "del_global";
        case OP_GET_FIELD: return "get_field";
        case OP_SET_FIELD: return "set_field";
        case OP_GET_INDEX: return "get_index";
        case OP_SET_INDEX: return "set_index";
        case OP_GET_SUPER: return "get_super";
        case OP_SUPER_INIT: return "super_init";
        case OP_JUMP: return "jump";
        case OP_JUMP_IF_FALSE: return "jump_if_false";
        case OP_JUMP_IF_FALSE_KEEP: return "jump_if_false_keep";
        case OP_JUMP_IF_TRUE_KEEP: return "jump_if_true_keep";
        case OP_LOOP: return "loop";
        case OP_CALL: return "call";
        case OP_CALL_UI: return "call_ui";
        case OP_RETURN: return "return";
        case OP_RETURN_NULL: return "return_null";
        case OP_CLOSURE: return "closure";
        case OP_BUILD_LIST: return "build_list";
        case OP_BUILD_TUPLE: return "build_tuple";
        case OP_LIST_APPEND: return "list_append";
        case OP_LIST_EXTEND: return "list_extend";
        case OP_BUILD_NAMED: return "build_named";
        case OP_ITER_RANGE: return "iter_range";
        case OP_ITER_RANGE_STEP: return "iter_range_step";
        case OP_ITER_VALUE: return "iter_value";
        case OP_ITER_NEXT: return "iter_next";
        case OP_ITER_BIND2: return "iter_bind2";
        case OP_THROW: return "throw";
        case OP_TRY: return "try";
        case OP_POP_TRY: return "pop_try";
        case OP_PRINT: return "print";
        case OP_INPUT: return "input";
        case OP_JUMP_IF_PROVIDED: return "jump_if_provided";
        case OP_COLLECT_VARARGS: return "collect_varargs";
        case OP_UI_APPEND: return "ui_append";
        case OP_ASSERT: return "assert";
        case OP_CLASS: return "class";
        case OP_INIT_CLASS: return "init_class";
        case OP_DEEPCOPY: return "deepcopy";
        case OP_CONVERT: return "convert";
        case OP_NEW_ARRAY: return "new-array";
        case OP_GET_INDEX_FAST: return "index-fast";
        case OP_NOP: return "nop";
        case OP_LOCAL_ADD_IMM: return "local+=";
        case OP_LOCAL_SUB_IMM: return "local-=";
        case OP_LOCAL_ADD_LOCAL: return "local+=local";
        case OP_INT1: return "int1";
        default: return "?";
    }
}

} // namespace annota
