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

enum class VT : uint8_t {
    Null, Bool, Int, Float, Str, Bytes, Color,
    List, Tuple, Function, Native, Class, Instance, Bound, Iter, Module, UiNode, Map
};

// iterator flavours
enum class IterKind : uint8_t { Range, List, Tuple, Str, Instance, Map };

struct Value {
    VT t = VT::Null;
    bool b = false;
    int64_t i = 0;
    double f = 0.0;
    std::shared_ptr<Obj> o;

    Value() = default;
    static Value null()                     { return Value(); }
    static Value boolean(bool v)            { Value r; r.t = VT::Bool; r.b = v; return r; }
    static Value integer(int64_t v)         { Value r; r.t = VT::Int; r.i = v; return r; }
    static Value real(double v)             { Value r; r.t = VT::Float; r.f = v; return r; }
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
    bool isNumber() const { return t == VT::Int || t == VT::Float; }
    bool isString() const { return t == VT::Str; }
    bool isCallable() const {
        return t == VT::Function || t == VT::Native || t == VT::Class ||
               t == VT::Bound || t == VT::Instance;
    }
    int64_t asInt() const { return t == VT::Float ? (int64_t)f : i; }
    double asFloat() const { return t == VT::Int ? (double)i : f; }
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
