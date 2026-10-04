// Annota - value.cpp
#include "value.hpp"
#include "bytecode.hpp"
#include <unordered_set>

namespace annota {

static std::shared_ptr<Obj> newObj(VT t, Value& v) {
    v.t = t;
    v.o = std::make_shared<Obj>();
    return v.o;
}

Value Value::str(const std::string& s) {
    Value v; auto o = newObj(VT::Str, v); o->str = s; return v;
}
Value Value::color(uint32_t rgb) {
    Value v; auto o = newObj(VT::Color, v); o->color = rgb; return v;
}
Value Value::bytes(std::vector<uint8_t> b) {
    Value v; auto o = newObj(VT::Bytes, v); o->bytes = std::move(b); return v;
}
Value Value::list(std::vector<Value> items) {
    Value v; auto o = newObj(VT::List, v); o->items = std::move(items); return v;
}
Value Value::tuple(std::vector<Value> items) {
    Value v; auto o = newObj(VT::Tuple, v); o->items = std::move(items); return v;
}
Value Value::map(std::unordered_map<std::string, Value> m) {
    Value v; auto o = newObj(VT::Map, v); o->map = std::move(m); return v;
}
Value Value::module(const std::string& name) {
    Value v; auto o = newObj(VT::Module, v); o->str = name; return v;
}
Value Value::function(std::shared_ptr<Chunk> c) {
    Value v; auto o = newObj(VT::Function, v);
    o->chunk = std::move(c);
    o->str = o->chunk ? o->chunk->fnName : "<fn>";
    o->arity = o->chunk ? (int)o->chunk->params.size() : 0;
    return v;
}
Value Value::native(const std::string& name,
                    std::function<Value(VM&, std::vector<Value>&)> fn) {
    Value v; auto o = newObj(VT::Native, v); o->str = name; o->fn = std::move(fn); return v;
}
Value Value::klass(std::shared_ptr<ClassInfo> ci) {
    Value v; auto o = newObj(VT::Class, v); o->klass = std::move(ci); o->str = o->klass->name; return v;
}
Value Value::instance(std::shared_ptr<ClassInfo> ci, std::vector<Value> fields) {
    Value v; auto o = newObj(VT::Instance, v);
    o->klass = std::move(ci); o->fields = std::move(fields); o->str = o->klass->name;
    return v;
}
Value Value::bound(Value recv, Value callee) {
    Value v; auto o = newObj(VT::Bound, v);
    o->items.push_back(std::move(recv));
    o->items.push_back(std::move(callee));
    o->str = "<bound>";
    return v;
}
Value Value::iterRange(int64_t start, int64_t stop, int64_t step) {
    Value v; auto o = newObj(VT::Iter, v);
    o->iterKind = IterKind::Range;
    o->iterStart = start; o->iterStop = stop; o->iterStep = step; o->iterCur = start;
    return v;
}
Value Value::iterList(std::vector<Value> snapshot, bool isTuple) {
    Value v; auto o = newObj(VT::Iter, v);
    o->iterKind = isTuple ? IterKind::Tuple : IterKind::List;
    o->items = std::move(snapshot);
    o->iterIdx = 0;
    return v;
}

bool Value::isTruthyRaw() const {
    switch (t) {
        case VT::Null:  return false;
        case VT::Bool:  return b;
        case VT::Int:   return i != 0;
        case VT::Float: return f != 0.0;
        case VT::Str:   return o && !o->str.empty();
        case VT::Bytes: return o && !o->bytes.empty();
        case VT::Color: return true;
        case VT::List:
        case VT::Tuple: return o && !o->items.empty();
        case VT::Map:   return o && !o->map.empty();
        case VT::UiNode:return true;
        default:        return true;   // functions, classes, instances, modules
    }
}

const char* Value::typeName() const {
    switch (t) {
        case VT::Null:     return "null";
        case VT::Bool:     return "bool";
        case VT::Int:      return "int";
        case VT::Float:    return "float";
        case VT::Str:      return "String";
        case VT::Bytes:    return "Bytes";
        case VT::Color:    return "Color";
        case VT::List:     return "List";
        case VT::Tuple:    return "Tuple";
        case VT::Function: return "Fn";
        case VT::Native:   return "Fn";
        case VT::Class:    return "Class";
        case VT::Instance: return o && o->klass ? o->klass->name.c_str() : "Instance";
        case VT::Bound:    return "Fn";
        case VT::Iter:     return "Iter";
        case VT::Module:   return "Module";
        case VT::UiNode:   return "UiNode";
        case VT::Map:      return "Map";
    }
    return "?";
}

// ---------------------------------------------------------------- ClassInfo
MethodInfo* ClassInfo::findMethod(const std::string& n) {
    for (auto& m : methods) if (m.name == n) return &m;
    if (parent) return parent->findMethod(n);
    return nullptr;
}
Value* ClassInfo::findStatic(const std::string& n) {
    for (auto& m : statics) if (m.name == n) return &m.fn;
    if (parent) return parent->findStatic(n);
    return nullptr;
}
int ClassInfo::findField(const std::string& n) const {
    auto it = fieldIndex.find(n);
    return it == fieldIndex.end() ? -1 : it->second;
}
void ClassInfo::buildLayout() {
    fieldNames.clear(); fieldIndex.clear(); fieldDefaults.clear();
    if (parent) {
        fieldNames = parent->fieldNames;
        fieldDefaults = parent->fieldDefaults;
    }
    for (size_t k = 0; k < ownFields.size(); k++) {
        const std::string& n = ownFields[k];
        if (fieldIndex.count(n)) { fieldDefaults[fieldIndex[n]] = ownDefaults[k]; continue; }
        fieldIndex[n] = (int)fieldNames.size();
        fieldNames.push_back(n);
        fieldDefaults.push_back(ownDefaults[k]);
    }
}
std::shared_ptr<ClassInfo> ClassInfo::shallowCopy() {
    auto c = std::make_shared<ClassInfo>();
    *c = *this;
    c->templateOf = nullptr;
    return c;
}

// ---------------------------------------------------------------- deep copy
static Value copyRec(const Value& v, std::unordered_map<const Obj*, Value>& memo, int depth) {
    if (depth > 512) return v;
    switch (v.t) {
        case VT::List:
        case VT::Tuple: {
            if (!v.o) return v;
            auto it = memo.find(v.o.get());
            if (it != memo.end()) return it->second;
            Value out = (v.t == VT::List) ? Value::list({}) : Value::tuple({});
            out.o->items.resize(v.o->items.size());
            memo[v.o.get()] = out;
            for (size_t k = 0; k < v.o->items.size(); k++)
                out.o->items[k] = copyRec(v.o->items[k], memo, depth + 1);
            return out;
        }
        case VT::Map: {
            if (!v.o) return v;
            auto it = memo.find(v.o.get());
            if (it != memo.end()) return it->second;
            Value out = Value::map({});
            memo[v.o.get()] = out;
            for (auto& kv : v.o->map) out.o->map[kv.first] = copyRec(kv.second, memo, depth + 1);
            return out;
        }
        case VT::Bytes: {
            Value out = Value::bytes(v.o ? v.o->bytes : std::vector<uint8_t>{});
            return out;
        }
        case VT::Instance: {
            if (!v.o) return v;
            auto it = memo.find(v.o.get());
            if (it != memo.end()) return it->second;
            Value out = Value::instance(v.o->klass, {});
            out.o->fields.resize(v.o->fields.size());
            memo[v.o.get()] = out;
            for (size_t k = 0; k < v.o->fields.size(); k++)
                out.o->fields[k] = copyRec(v.o->fields[k], memo, depth + 1);
            return out;
        }
        case VT::UiNode: {
            if (!v.o) return v;
            auto it = memo.find(v.o.get());
            if (it != memo.end()) return it->second;
            Value out;
            out.t = VT::UiNode;
            out.o = std::make_shared<Obj>();
            out.o->str = v.o->str;
            memo[v.o.get()] = out;
            for (size_t k = 0; k < v.o->items.size(); k++)
                out.o->items.push_back(copyRec(v.o->items[k], memo, depth + 1));
            for (auto& kv : v.o->map) out.o->map[kv.first] = copyRec(kv.second, memo, depth + 1);
            return out;
        }
        default:
            return v;   // scalars, functions, natives, classes, iterations: share
    }
}

Value deepCopy(const Value& v) {
    std::unordered_map<const Obj*, Value> memo;
    return copyRec(v, memo, 0);
}

// ---------------------------------------------------------------- equality
bool valueEquals(const Value& a, const Value& b) {
    if (a.t == VT::Int && b.t == VT::Float) return (double)a.i == b.f;
    if (a.t == VT::Float && b.t == VT::Int) return a.f == (double)b.i;
    if (a.t != b.t) return false;
    switch (a.t) {
        case VT::Null:  return true;
        case VT::Bool:  return a.b == b.b;
        case VT::Int:   return a.i == b.i;
        case VT::Float: return a.f == b.f;
        case VT::Color: return a.o->color == b.o->color;
        case VT::Str:   return a.o->str == b.o->str;
        case VT::Bytes: return a.o->bytes == b.o->bytes;
        case VT::List:
        case VT::Tuple: {
            if (a.o->items.size() != b.o->items.size()) return false;
            for (size_t k = 0; k < a.o->items.size(); k++)
                if (!valueEquals(a.o->items[k], b.o->items[k])) return false;
            return true;
        }
        case VT::Instance: {
            if (a.o->klass != b.o->klass) return false;
            if (a.o->fields.size() != b.o->fields.size()) return false;
            for (size_t k = 0; k < a.o->fields.size(); k++)
                if (!valueEquals(a.o->fields[k], b.o->fields[k])) return false;
            return true;
        }
        default:
            return a.o == b.o;
    }
}

} // namespace annota
