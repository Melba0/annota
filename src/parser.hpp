// Annota - parser.hpp
#pragma once
#include "ast.hpp"
#include <set>

namespace annota {

// resolves `use <name>` / `use "<path>"` to a token stream
struct ModuleLoader {
    virtual ~ModuleLoader() = default;
    virtual bool loadModule(const std::string& spec, std::vector<Token>& toks, std::string& file) = 0;
};

struct MacroRegistry {
    std::vector<MacroDef> macros;      // registration order == matching priority
    std::set<std::string> loaded;      // module specs already pulled in (idempotent `use`)
};

class Parser {
public:
    Parser(std::vector<Token> toks, std::string file, ModuleLoader* loader, MacroRegistry* reg);

    Program parse();

    // parse a standalone expression from a token range (used for annotation arguments)
    static ExprP exprFromTokens(std::vector<Token> toks, const std::string& file);

private:
    struct Ctx {
        bool func = false;         // inside a function/method body -> `=expr` returns
        bool classBody = false;    // direct top level of a class/view body
        bool ui = false;           // view body / component children
        bool method = false;       // inside a method -> `this` available
    };

    std::vector<Token> t_;
    std::vector<int> depth_;       // macro expansion depth, parallel to t_
    size_t p_ = 0;
    std::string file_;
    ModuleLoader* loader_ = nullptr;
    MacroRegistry* reg_ = nullptr;
    std::set<std::string> knownTypes_;
    std::vector<AnnotationRecord>* annIndex_ = nullptr;
    std::vector<MacroExpansion>* expansions_ = nullptr;
    size_t blockParen_ = (size_t)-1;
    size_t stmtStart_ = (size_t)-1;
    int macroLimit_ = 256;
    bool noMacro_ = false;
    int macroBurst_ = 0;

    // ---- token helpers
    const Token& at(size_t i) const { return i < t_.size() ? t_[i] : t_.back(); }
    const Token& cur() const { return at(p_); }
    T type(size_t i = 0) const { return at(p_ + i).type; }
    bool check(T t) const { return cur().type == t; }
    bool accept(T t) { if (check(t)) { p_++; return true; } return false; }
    Token expect(T t, const char* what = nullptr);
    [[noreturn]] void error(const std::string& msg);
    [[noreturn]] void errorAt(size_t idx, const std::string& msg);
    void skipNL() { while (check(T::Newline)) p_++; }
    bool atEnd() const { return check(T::End); }
    void expectStmtEnd();
    size_t matchBracket(size_t open) const;    // open = index of '(' '[' '{'
    size_t findBlockParen(size_t from) const;
    bool isTypeName(const std::string& s) const;
    bool startsOperand(T t, bool allowSign) const;

    // ---- macro machinery
    bool tryMacro();
    bool matchMacroAt(const MacroDef& m, size_t start, std::vector<std::pair<std::string, std::pair<size_t, size_t>>>& binds,
                      size_t& end) const;
    bool literalMatches(const Token& pat, const Token& tok) const;
    size_t skipBalanced(size_t i) const;
    void spliceMacro(size_t start, size_t end, const std::vector<Token>& body,
                     const std::vector<std::pair<std::string, std::pair<size_t, size_t>>>& binds,
                     const std::vector<int>& bodyDepth);

    // ---- lookahead helpers
    bool looksLikeParamList(size_t a, size_t b) const;      // a..b exclusive inside parens
    bool looksLikeClassBody(size_t openIdx) const;          // '(' of `=( ... )`
    bool looksLikeStyleBody(size_t openIdx) const;
    bool looksLikeLambda(size_t openIdx, size_t closeIdx) const;
    size_t parenClose(size_t openIdx) const { return matchBracket(openIdx); }
    void markBlockParen(size_t idx);

    // ---- statements
    void stmtList(Ctx ctx, std::vector<StmtP>& out, T terminator);
    std::vector<StmtP> statement(Ctx ctx);
    StmtP statementCore(Ctx ctx);
    void describeAndRecord(const std::vector<Annotation>& anns, const StmtP& s);
    Annotation parseAnnotation();
    std::vector<Annotation> parseAnnotations(bool& any);
    BlockP parseBlock(Ctx ctx, bool ui = false);
    std::vector<Param> parseParams();
    StmtP parseFuncDef(Ctx ctx, const std::string& name, const std::vector<Param>& params, bool isMethod);
    StmtP parseClassDef(const std::string& name, const std::vector<Param>& params, bool isView, bool isStyle);
    StmtP parseNew();
    StmtP parseSimpleDecl(SK kind, bool isConst);
    StmtP parseIf(Ctx ctx);
    StmtP parseWhile(Ctx ctx);
    StmtP parseFor(Ctx ctx);
    StmtP parsePrint();
    StmtP parseUse();
    StmtP parseMacro();
    StmtP parseView();
    StmtP parseState();
    std::vector<Token> bodyTokens(size_t openIdx) const;

    // ---- expressions
    ExprP parseExpr();
    ExprP parseOr();
    ExprP parseAnd();
    ExprP parseBitOr();
    ExprP parseBitXor();
    ExprP parseBitAnd();
    ExprP parseEquality();
    ExprP parseComparison();
    ExprP parseShift();
    ExprP parseAdditive();
    ExprP parseMultiplicative();
    ExprP parseUnary();
    ExprP parsePower();
    ExprP parsePostfix();
    ExprP parsePrimary();
    ExprP parsePiecewise();
    ExprP parseLambdaOrCastOrGroup();
    ExprP parseListLiteral();
    ExprP parseCallOrChildren(ExprP callee);
    std::vector<Arg> parseCallArgs(T closer);

    ExprP mkExpr(EK k, int line);
    static std::string joinTokens(const std::vector<Token>& toks, size_t a, size_t b);
    std::string describeStmt(const StmtP& s) const;
};

} // namespace annota
