// Annota - value.hpp : run-time values.
#pragma once
#include "common.hpp"
#include <memory>
#include <functional>
#include <unordered_map>
#include <map>

namespace annota {

struct Obj;
struct ClassInfo;
struct Chunk;
struct VM;
struct Value;

using Cell = std::shared_ptr<Value>;

// ---------------------------------------------------------------- numeric kinds
// C like fixed widths.  `None` means "untyped": such a value keeps the interpreter's default
// 64 bit integer / double precision, which is what every program written before the width
// table existed relies on.
enum class NumKind : uint8_t {
    None = 0,
    I8, I16, I32, I64,
    U8, U16, U32, U64,
    F32, F64
};
const char* numKindName(NumKind k);                 // "int8" ... "uint64" / "float32"
NumKind numKindByName(const std::string& name);     // None when the name is not a width type
bool isWidthTypeName(const std::string& name);
struct NumTraits { int bits; bool isFloat; bool isSigned; };
NumTraits numTraits(NumKind k);
NumKind promoteNum(NumKind a, NumKind b);           // C usual arithmetic conversions
int64_t wrapToKind(int64_t v, NumKind k);           // wrap into the kind's width/sign
double roundToKind(double v, NumKind k);            // f32 rounds through float
bool numIsFloat(NumKind k);
// `OP_CONVERT` operand codes for the widths that live in a boxed payload
constexpr uint8_t kConvertWideS = 200;      // -> long long / int128 (signed)
constexpr uint8_t kConvertWideU = 201;      // -> ulonglong / uint128
constexpr uint8_t kConvertLongDouble = 202; // -> long double
uint8_t boxedConvertCode(const std::string& typeName);   // 0 when the name is not boxed

// The built-in type names, declared once here and used by the parser (which names are types),
// the analyzer (hover/completion/diagnostics) and the tooling docs.  Deliberately short: one
// name per type, no `i8`/`u64`/`f32` style aliases.
struct BuiltinTypeSpec {
    const char* name;
    NumKind kind;              // None for the non numeric types
    bool boxed;                // lives in the boxed payload (128 bit / long double)
    const char* summary;
};
const std::vector<BuiltinTypeSpec>& builtinTypes();
const BuiltinTypeSpec* findBuiltinType(const std::string& name);   // nullptr when not a type
bool isBuiltinTypeName(const std::string& name);
__int128 parseWide(const std::string& text, bool isUnsigned);   // 128 bit decimal parse
bool numIsUnsigned(NumKind k);

enum class VT : uint8_t {
    Null, Bool, Int, Float, Str, Bytes, Color,
    List, Tuple, Function, Native, Class, Instance, Bound, Iter, Module, UiNode, Map,
    // boxed scalars for the widths that do not fit the inline payload: `long long` (128 bit,
    // signed or unsigned) and `long double` (80 bit on x86).  They are immutable, so copying a
    // value only copies the pointer.
    Wide, LongDouble,
    Array               // fixed size / typed array (also used for row views)
};

// iterator flavours
enum class IterKind : uint8_t { Range, List, Tuple, Str, Instance, Map };

struct Value {
    VT t = VT::Null;
    NumKind k = NumKind::None;      // width of an Int/Float value (None = interpreter default)
    bool b = false;
    int64_t i = 0;
    double f = 0.0;
    std::shared_ptr<Obj> o;

    Value() = default;
    static Value null()                     { return Value(); }
    static Value boolean(bool v)            { Value r; r.t = VT::Bool; r.b = v; return r; }
    static Value integer(int64_t v)         { Value r; r.t = VT::Int; r.i = v; return r; }
    static Value real(double v)             { Value r; r.t = VT::Float; r.f = v; return r; }
    static Value wide(__int128 v, bool isUnsigned);
    static Value longDouble(long double v);
    // `items` holds the (contiguous) elements this view can reach; `dims`/`strides` describe the
    // shape, `offset` where the view starts.  A row of `T[m][n]` is a view of the same buffer.
    static Value array(std::shared_ptr<std::vector<Value>> buf, std::vector<int64_t> dims,
                       std::vector<int64_t> strides, int64_t offset, NumKind eltKind,
                       bool eltIsStr, bool dynamic);
    Value arrayView(int64_t newOffset, size_t dropDims) const;   // O(1) sub-view of a buffer
    static Value typedInt(int64_t v, NumKind kind) {
        Value r; r.t = VT::Int; r.i = wrapToKind(v, kind); r.k = kind; return r;
    }
    static Value typedReal(double v, NumKind kind) {
        Value r; r.t = VT::Float; r.f = roundToKind(v, kind); r.k = kind; return r;
    }
    static Value str(const std::string& s);
    static Value color(uint32_t rgb);
    static Value bytes(std::vector<uint8_t> b);
    static Value list(std::vector<Value> items = {});
    static Value tuple(std::vector<Value> items = {});
    static Value map(std::unordered_map<std::string, Value> m = {});
    static Value module(const std::string& name);
    static Value function(std::shared_ptr<Chunk> chunk);
    static Value native(const std::string& name,
                        std::function<Value(VM&, std::vector<Value>&)> fn);
    static Value klass(std::shared_ptr<ClassInfo> ci);
    static Value instance(std::shared_ptr<ClassInfo> ci, std::vector<Value> fields);
    static Value bound(Value recv, Value callee);
    static Value iterRange(int64_t start, int64_t stop, int64_t step);
    static Value iterList(std::vector<Value> snapshot, bool isTuple);

    bool isNull() const { return t == VT::Null; }
    bool isNumber() const {
        return t == VT::Int || t == VT::Float || t == VT::Wide || t == VT::LongDouble;
    }
    bool isString() const { return t == VT::Str; }
    bool isCallable() const {
        return t == VT::Function || t == VT::Native || t == VT::Class ||
               t == VT::Bound || t == VT::Instance;
    }
    int64_t asInt() const;           // boxed kinds included (defined in value.cpp)
    double asFloat() const;
    bool isWideInt() const { return t == VT::Wide; }
    bool isLongDouble() const { return t == VT::LongDouble; }
    bool isArray() const { return t == VT::Array; }
    int64_t arrayCount() const;              // number of elements visible through this view
    Value defaultElement() const;            // 0 / "" / [] / null for the element type
    bool isANumber() const { return t == VT::Int || t == VT::Float || t == VT::Wide || t == VT::LongDouble; }
    __int128 asWide() const;
    bool wideUnsigned() const;
    long double asLongDouble() const;
    NumKind numKind() const { return (t == VT::Int || t == VT::Float) ? k : NumKind::None; }
    bool isTruthyRaw() const;        // no magic-method dispatch
    const char* typeName() const;
};

// fat object: one allocation per heap value, fields reused per kind
struct Obj {
    // Str / Color? / Module name / Function name / UiNode type / ClassInfo name
    std::string str;
    // Bytes payload
    std::vector<uint8_t> bytes;
    // List / Tuple / UiNode children / Bound(receiver, callee) / Iter snapshot
    std::vector<Value> items;
    // Module members / UiNode attributes / Map (named args)
    std::unordered_map<std::string, Value> map;

    // Function
    std::shared_ptr<Chunk> chunk;
    std::vector<Cell> upvals;              // captured cells
    int arity = 0;
    // Native
    std::function<Value(VM&, std::vector<Value>&)> fn;
    // some pseudo methods (`.size` on a list) also carry their value so that the
    // documentation's `result.size == len` contract reads naturally
    Value implicit;

    // Array: shape and element type of this view (the storage is `items`)
    std::shared_ptr<std::vector<Value>> buf;   // Array storage (shared by views)
    std::vector<int64_t> dims;
    std::vector<int64_t> strides;
    int64_t offset = 0;
    NumKind eltKind = NumKind::None;
    bool eltIsStr = false;
    bool dynamic = false;                    // `T[]` may grow through push/append

    // boxed scalars: Wide (128 bit, `unsigned` flag) and LongDouble (80 bit)
    __int128 wide = 0;
    long double wideF = 0;
    bool wideUnsigned = false;

    // Class / Instance
    std::shared_ptr<ClassInfo> klass;
    std::vector<Value> fields;             // Instance fields

    // Iter
    IterKind iterKind = IterKind::Range;
    int64_t iterCur = 0, iterStart = 0, iterStop = 0, iterStep = 1;
    size_t iterIdx = 0;
    Value iterTarget;                      // instance being iterated

    uint32_t color = 0;
};

// Method entry inside a class
struct MethodInfo {
    std::string name;
    Value fn;
    bool isStatic = false;
};

struct ClassInfo {
    std::string name;
    std::string parentName;                         // `:parent(...)` clause
    std::shared_ptr<ClassInfo> parent;
    // flattened instance field layout (inherited fields first)
    std::vector<std::string> fieldNames;
    std::unordered_map<std::string, int> fieldIndex;
    std::vector<Value> fieldDefaults;
    // declarations of this class only (used by the constructor body)
    std::vector<std::string> ownFields;
    std::vector<Value> ownDefaults;
    // behaviour
    Value initFn;                                   // constructor body
    Value buildFn;                                  // view / component builder
    Value staticInitFn;                             // [[static]] initialisers
    std::vector<MethodInfo> methods;                // instance methods
    std::vector<MethodInfo> statics;                // [[static]] methods & fields
    std::shared_ptr<ClassInfo> templateOf;          // null unless this is a copy
    bool isComponent = false;                       // declared with `view` or builds UI
    bool isStyle = false;                           // plain `Name=(...)` style class
    std::vector<std::string> paramNames;
    std::vector<std::string> exposed;               // [[expose: ...]]
    std::vector<std::string> annotations;           // raw annotation names
    MethodInfo* findMethod(const std::string& n);
    Value* findStatic(const std::string& n);
    int findField(const std::string& n) const;
    void buildLayout();                             // recompute field slots
    std::shared_ptr<ClassInfo> shallowCopy();
};

// ---------------------------------------------------------------- deep copy
Value deepCopy(const Value& v);

// equality used by `==`
bool valueEquals(const Value& a, const Value& b);

} // namespace annota
