// Annota - ast.hpp
#pragma once
#include "lexer.hpp"
#include <memory>

namespace annota {

struct Expr; struct Stmt; struct Block;
using ExprP = std::shared_ptr<Expr>;
using StmtP = std::shared_ptr<Stmt>;
using BlockP = std::shared_ptr<Block>;

enum class EK : uint8_t {
    Null, Bool, Int, Float, Str, Color,
    Ident, This, Super, Children,
    List, Tuple, Unary, Binary, Logical, Call, Index, Field,
    Piecewise, Lambda, Iter
};

enum class SK : uint8_t {
    Expr, New, Const, Del, Assign, CompoundAssign,
    If, While, For, Break, Continue, Throw, Print, Input, Return,
    Block, FuncDef, ClassDef, FieldDecl, ParentInit,
    Use, MacroDef, Annot, State, ViewDef
};

struct Param {
    std::string name;
    std::string type;
    ExprP def;          // optional default
    bool vararg = false;
    std::vector<ExprP> typeDims;   // `T[n][m]`, `[]` -> null entry
};

// one `[[name: a, k=v]]` annotation; arguments kept as raw text and, when possible, as expressions
struct AnnotArg {
    std::string key;        // empty -> positional
    std::string text;       // verbatim source text
    ExprP expr;             // may be null when the argument is not an expression
    bool hasKey = false;
};

struct Annotation {
    std::string name;
    std::vector<AnnotArg> args;
    int line = 0;
};

struct AnnotationRecord {
    Annotation ann;
    std::string target;     // human readable target description
    int targetLine = 0;
};

struct Arg {
    ExprP value;
    std::string name;       // empty -> positional
    bool spread = false;
};

struct Block {
    std::vector<StmtP> stmts;
    BlockP exceptBody;              // `except name( ... )`
    std::string exceptName;
    int exceptLine = 0;
    bool isUI = false;              // children list of a view/component
    int line = 0;
};

struct Expr {
    EK kind = EK::Null;
    int line = 0;
    // literals
    int64_t ival = 0;
    bool wideLiteral = false;   // the literal only fits in 128 bits
    double fval = 0.0;
    bool bval = false;
    std::string sval;
    uint32_t color = 0;
    // identifiers / field names / operator text
    std::string name;
    T op = T::End;
    // children
    ExprP a, b;                                     // operands
    std::vector<ExprP> items;                       // list/tuple elements, call args
    std::vector<Arg> args;                          // call arguments
    std::vector<std::pair<ExprP, ExprP>> cases;     // piecewise
    ExprP elseVal;                                  // piecewise else
    BlockP children;                                // component children
    std::vector<Param> params;                      // lambda parameters
    BlockP body;                                    // lambda block body
    ExprP bodyExpr;                                 // lambda single expression body
};

struct MacroDef {
    std::string name;
    std::vector<Token> pattern;
    std::vector<Token> body;
    int line = 0;
    std::string file;
};

// recorded at expansion time so the analyzer can warn about duplicated side effects
struct MacroExpansion {
    std::string macro;
    int line = 0;
    std::string argument;             // the duplicated placeholder's text
    bool duplicatedArgWithCall = false;
};

struct Stmt {
    SK kind = SK::Expr;
    int line = 0;
    std::string origin;         // set when the statement came from a `use`d module file
    // expressions
    ExprP expr;                 // Expr / Throw / Return / Print arg holder
    std::vector<ExprP> args;    // print arguments / call-ish
    ExprP sep;                  // print sep=
    ExprP target;               // assignment target / input target
    ExprP value;                // assignment value
    T op = T::End;              // compound assignment operator
    // declarations
    std::vector<std::string> names;     // new / destructuring / class params
    std::vector<std::string> types;     // parallel type annotations
    std::string name;                   // single name
    std::string type;
    std::vector<ExprP> typeDims;   // `T[n][m]`                   // single type
    std::string module;                 // use
    bool isConst = false;
    bool isLend = false;        // `lend a = b`: `a` shares `b`'s storage instead of copying it
    bool isStatic = false;
    bool isPublic = false;
    bool isPrivate = false;
    // control flow
    ExprP cond;
    BlockP body;
    BlockP elseBody;
    ExprP iterable;
    std::vector<std::string> loopVars;  // for
    // functions / classes
    std::vector<Param> params;
    bool isMethod = false;
    bool isView = false;
    bool isStyle = false;
    std::string parentName;
    std::vector<ExprP> parentArgs;
    std::vector<StmtP> members;         // class body
    bool hasParent = false;
    // annotations
    std::vector<Annotation> annotations;
    Annotation ann;                     // for SK::Annot
    // macro
    std::shared_ptr<MacroDef> macro;
    ExprP initExpr;                     // state / field initialiser
};

struct Program {
    std::string file;
    std::vector<StmtP> stmts;
    std::vector<MacroDef> macros;       // macros declared anywhere in this file (for `use`)
    std::vector<AnnotationRecord> annotationIndex;
    std::vector<MacroExpansion> expansions;
    int macroDepth = 256;
};

} // namespace annota
