// Annota - value.cpp
#include <algorithm>
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

// ---------------------------------------------------------------- numeric kinds
const char* numKindName(NumKind k) {
    switch (k) {
        case NumKind::I8:  return "int8";
        case NumKind::I16: return "int16";
        case NumKind::I32: return "int32";
        case NumKind::I64: return "int64";
        case NumKind::U8:  return "uint8";
        case NumKind::U16: return "uint16";
        case NumKind::U32: return "uint32";
        case NumKind::U64: return "uint64";
        case NumKind::F32: return "float32";
        case NumKind::F64: return "float";
        case NumKind::None: default: return "int";
    }
}

NumKind numKindByName(const std::string& n) {
    // the declarable numeric types: int=32, long=64, longlong=128, uint/ulong, the explicit
    // widths, float=32, double=64, longdouble=80.  No other spellings.
    if (n == "int" || n == "int32") return NumKind::I32;
    if (n == "long" || n == "int64") return NumKind::I64;
    if (n == "int8") return NumKind::I8;
    if (n == "int16") return NumKind::I16;
    if (n == "uint" || n == "uint32") return NumKind::U32;
    if (n == "ulong" || n == "uint64") return NumKind::U64;
    if (n == "uint8") return NumKind::U8;
    if (n == "uint16") return NumKind::U16;
    if (n == "float") return NumKind::F32;
    if (n == "double") return NumKind::F64;
    return NumKind::None;
}

bool isWidthTypeName(const std::string& n) { return numKindByName(n) != NumKind::None; }

const std::vector<BuiltinTypeSpec>& builtinTypes() {
    static const std::vector<BuiltinTypeSpec> t = {
        {"int", NumKind::I32, false, "有符号整数 32 位（声明 new x:int = ...；转换 int8(x) 等）"},
        {"long", NumKind::I64, false, "有符号整数 64 位"},
        {"int8", NumKind::I8, false, "有符号整数 8 位"},
        {"int16", NumKind::I16, false, "有符号整数 16 位"},
        {"int32", NumKind::I32, false, "有符号整数 32 位"},
        {"int64", NumKind::I64, false, "有符号整数 64 位"},
        {"uint", NumKind::U32, false, "无符号整数 32 位"},
        {"ulong", NumKind::U64, false, "无符号整数 64 位"},
        {"uint8", NumKind::U8, false, "无符号整数 8 位"},
        {"uint16", NumKind::U16, false, "无符号整数 16 位"},
        {"uint32", NumKind::U32, false, "无符号整数 32 位"},
        {"uint64", NumKind::U64, false, "无符号整数 64 位"},
        {"longlong", NumKind::None, true, "有符号整数 128 位（盒式存储，精确运算）"},
        {"ulonglong", NumKind::None, true, "无符号整数 128 位"},
        {"float", NumKind::F32, false, "单精度浮点 32 位"},
        {"double", NumKind::F64, false, "双精度浮点 64 位"},
        {"longdouble", NumKind::None, true, "扩展精度浮点 80 位"},
        {"bool", NumKind::None, false, "布尔值 true / false"},
        {"String", NumKind::None, false, "字节串；len 是字节数"},
        {"List", NumKind::None, false, "可增长列表"},
        {"Tuple", NumKind::None, false, "定长元组"},
        {"Map", NumKind::None, false, "键值映射"},
        {"Bytes", NumKind::None, false, "字节缓冲区"},
        {"Color", NumKind::None, false, "#RRGGBB 颜色"},
    };
    return t;
}

const BuiltinTypeSpec* findBuiltinType(const std::string& n) {
    for (auto& t : builtinTypes()) if (n == t.name) return &t;
    return nullptr;
}

bool isBuiltinTypeName(const std::string& n) { return findBuiltinType(n) != nullptr; }

NumTraits numTraits(NumKind k) {
    switch (k) {
        case NumKind::I8:  return {8, false, true};
        case NumKind::I16: return {16, false, true};
        case NumKind::I32: return {32, false, true};
        case NumKind::U8:  return {8, false, false};
        case NumKind::U16: return {16, false, false};
        case NumKind::U32: return {32, false, false};
        case NumKind::U64: return {64, false, false};
        case NumKind::F32: return {32, true, true};
        case NumKind::F64: return {64, true, true};
        case NumKind::I64:
        case NumKind::None: default: return {64, false, true};
    }
}

bool numIsFloat(NumKind k) { return k == NumKind::F32 || k == NumKind::F64; }
bool numIsUnsigned(NumKind k) { return k == NumKind::U8 || k == NumKind::U16 || k == NumKind::U32 || k == NumKind::U64; }

NumKind promoteNum(NumKind a, NumKind b) {
    // untyped numbers are the interpreter's 64 bit `long`, so mixing them with a narrow declared
    // width promotes to the wider kind, exactly like C does
    if (a == NumKind::None) a = NumKind::I64;
    if (b == NumKind::None) b = NumKind::I64;
    NumTraits ta = numTraits(a), tb = numTraits(b);
    if (ta.isFloat || tb.isFloat) {
        if (ta.isFloat && tb.isFloat) return ta.bits >= tb.bits ? a : b;
        return ta.isFloat ? a : b;
    }
    // integer promotion: the wider kind wins, unsigned wins at equal width
    if (ta.bits != tb.bits) return ta.bits > tb.bits ? a : b;
    bool ua = numIsUnsigned(a), ub = numIsUnsigned(b);
    if (ua != ub) return ua ? a : b;
    return a;
}

uint8_t boxedConvertCode(const std::string& n) {
    if (n == "longlong") return kConvertWideS;
    if (n == "ulonglong") return kConvertWideU;
    if (n == "longdouble") return kConvertLongDouble;
    return 0;
}

// decimal -> 128 bit (strtoll would stop at 64 bits)
__int128 parseWide(const std::string& text, bool isUnsigned) {
    size_t i = 0;
    bool neg = false;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) i++;
    if (i < text.size() && (text[i] == '-' || text[i] == '+')) { neg = text[i] == '-'; i++; }
    int base = 10;
    if (i + 1 < text.size() && text[i] == '0') {
        char b = text[i + 1];
        if (b == 'x' || b == 'X') { base = 16; i += 2; }
        else if (b == 'o' || b == 'O') { base = 8; i += 2; }
        else if (b == 'b' || b == 'B') { base = 2; i += 2; }
    }
    unsigned __int128 u = 0;
    for (; i < text.size(); i++) {
        char c = text[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else continue;
        if (d >= base) continue;
        u = u * (unsigned)base + (unsigned)d;
    }
    if (isUnsigned) return (__int128)u;
    return neg ? -(__int128)u : (__int128)u;
}

int64_t wrapToKind(int64_t v, NumKind k) {
    switch (k) {
        case NumKind::I8:  return (int8_t)v;
        case NumKind::I16: return (int16_t)v;
        case NumKind::I32: return (int32_t)v;
        case NumKind::U8:  return (int64_t)(uint8_t)v;
        case NumKind::U16: return (int64_t)(uint16_t)v;
        case NumKind::U32: return (int64_t)(uint32_t)v;
        case NumKind::U64: return (int64_t)(uint64_t)v;
        case NumKind::None:
        case NumKind::I64:
        default: return v;
    }
}

double roundToKind(double v, NumKind k) {
    if (k == NumKind::F32) return (double)(float)v;
    return v;
}

// ---------------------------------------------------------------- boxed scalars
Value Value::wide(__int128 v, bool isUnsigned) {
    Value r; auto o = newObj(VT::Wide, r); o->wide = v; o->wideUnsigned = isUnsigned; return r;
}
Value Value::longDouble(long double v) {
    Value r; auto o = newObj(VT::LongDouble, r); o->wideF = v; return r;
}
__int128 Value::asWide() const {
    if (t == VT::Wide) return o ? o->wide : 0;
    if (t == VT::Int) return (__int128)i;
    if (t == VT::Float) return (__int128)f;
    if (t == VT::Bool) return b ? 1 : 0;
    return 0;
}
bool Value::wideUnsigned() const { return t == VT::Wide && o && o->wideUnsigned; }
long double Value::asLongDouble() const {
    if (t == VT::LongDouble) return o ? o->wideF : 0;
    if (t == VT::Wide) return (long double)(o ? o->wide : 0);
    if (t == VT::Int) return (long double)i;
    if (t == VT::Float) return (long double)f;
    if (t == VT::Bool) return b ? 1 : 0;
    return 0;
}
int64_t Value::asInt() const {
    if (t == VT::Float) return (int64_t)f;
    if (t == VT::Wide) return (int64_t)(o ? o->wide : 0);
    if (t == VT::LongDouble) return (int64_t)(o ? o->wideF : 0);
    if (t == VT::Bool) return b ? 1 : 0;
    return i;
}
double Value::asFloat() const {
    if (t == VT::Int) return (double)i;
    if (t == VT::Wide) return (double)(o ? o->wide : 0);
    if (t == VT::LongDouble) return (double)(o ? o->wideF : 0);
    if (t == VT::Bool) return b ? 1.0 : 0.0;
    return f;
}
// decimal text for a 128 bit integer (no library helper exists for __int128)
std::string wideText(__int128 v, bool isUnsigned) {
    if (isUnsigned) {
        unsigned __int128 u = (unsigned __int128)v;
        if (u == 0) return "0";
        std::string out;
        while (u > 0) { out += (char)('0' + (int)(u % 10)); u /= 10; }
        std::reverse(out.begin(), out.end());
        return out;
    }
    if (v == 0) return "0";
    bool neg = v < 0;
    unsigned __int128 u = neg ? (unsigned __int128)(-v) : (unsigned __int128)v;
    std::string out;
    while (u > 0) { out += (char)('0' + (int)(u % 10)); u /= 10; }
    if (neg) out += '-';
    std::reverse(out.begin(), out.end());
    return out;
}

Value Value::array(std::shared_ptr<std::vector<Value>> buf, std::vector<int64_t> dims,
                  std::vector<int64_t> strides, int64_t offset, NumKind eltKind,
                  bool eltIsStr, bool dynamic) {
    Value r; auto o = newObj(VT::Array, r);
    o->buf = std::move(buf);
    if (!o->buf) o->buf = std::make_shared<std::vector<Value>>();
    o->dims = std::move(dims);
    o->strides = std::move(strides);
    o->offset = offset;
    o->eltKind = eltKind;
    o->eltIsStr = eltIsStr;
    o->dynamic = dynamic;
    return r;
}

Value Value::arrayView(int64_t newOffset, size_t dropDims) const {
    if (t != VT::Array || !o) return *this;
    Value r = *this;
    r.o = std::make_shared<Obj>(*o);          // shallow: the buffer stays shared
    for (size_t i = 0; i < dropDims && !r.o->dims.empty(); i++) {
        r.o->dims.erase(r.o->dims.begin());
        if (!r.o->strides.empty()) r.o->strides.erase(r.o->strides.begin());
    }
    r.o->offset = newOffset;
    return r;
}

int64_t Value::arrayCount() const {
    if (t != VT::Array || !o) return 0;
    int64_t n = 1;
    for (int64_t d : o->dims) n *= d < 0 ? 0 : d;
    return n;
}

Value Value::defaultElement() const {
    if (!o) return Value::null();
    if (o->eltIsStr) return Value::str("");
    NumKind k = o->eltKind;
    if (k != NumKind::None) return numIsFloat(k) ? Value::typedReal(0.0, k) : Value::typedInt(0, k);
    return Value::null();
}

const char* Value::typeName() const {
    switch (t) {
        case VT::Null:     return "null";
        case VT::Bool:     return "bool";
        case VT::Int:      return k == NumKind::None ? "int" : numKindName(k);
        case VT::Float:    return k == NumKind::None ? "float" : numKindName(k);
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
        case VT::Wide:     return wideUnsigned() ? "ulonglong" : "longlong";
        case VT::LongDouble: return "long double";
        case VT::Array:    return "Array";
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
        case VT::Array: {
            if (!v.o) return v;
            int64_t rows = v.o->dims.empty() ? 0 : v.o->dims[0];
            int64_t rest = 1;
            for (size_t d = 1; d < v.o->dims.size(); d++) rest *= v.o->dims[d];
            auto buf = std::make_shared<std::vector<Value>>();
            buf->reserve((size_t)v.arrayCount());
            for (int64_t r = 0; r < rows; r++) {
                int64_t base = v.o->offset + r * (v.o->strides.empty() ? 1 : v.o->strides[0]);
                for (int64_t k = 0; k < rest; k++) {
                    int64_t at = v.o->strides.size() > 1 ? base + k * v.o->strides[1] : base + k;
                    buf->push_back(at >= 0 && v.o->buf && at < (int64_t)v.o->buf->size()
                                       ? copyRec((*v.o->buf)[(size_t)at], memo, depth + 1)
                                       : v.defaultElement());
                }
            }
            std::vector<int64_t> strides(v.o->dims.size(), 1);
            for (int64_t d = (int64_t)v.o->dims.size() - 2; d >= 0; d--)
                strides[(size_t)d] = strides[(size_t)d + 1] * v.o->dims[(size_t)d + 1];
            return Value::array(buf, v.o->dims, strides, 0, v.o->eltKind, v.o->eltIsStr, v.o->dynamic);
        }
        case VT::Bytes: {
            Value out = Value::bytes(v.o ? v.o->bytes : std::vector<uint8_t>{});
            return out;
        }
        case VT::List:
        case VT::Tuple: {
            if (!v.o) return v;
            auto it = memo.find(v.o.get());
            if (it != memo.end()) return it->second;          // shared/cyclic structures
            Value out = v.t == VT::List ? Value::list({}) : Value::tuple({});
            memo[v.o.get()] = out;
            out.o->items.reserve(v.o->items.size());
            for (auto& x : v.o->items) out.o->items.push_back(copyRec(x, memo, depth + 1));
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
    // boxed scalars: compare exactly (128 bit) unless a floating kind takes part
    if (a.t == VT::Wide || b.t == VT::Wide || a.t == VT::LongDouble || b.t == VT::LongDouble) {
        if (!a.isNumber() || !b.isNumber()) return false;
        bool floatish = a.t == VT::LongDouble || b.t == VT::LongDouble ||
                        a.t == VT::Float || b.t == VT::Float;
        if (floatish) return a.asLongDouble() == b.asLongDouble();
        return a.asWide() == b.asWide();
    }
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
