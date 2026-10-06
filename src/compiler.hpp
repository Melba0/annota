// Annota - compiler.hpp
#pragma once
#include "ast.hpp"
#include "bytecode.hpp"
#include <memory>
#include <set>

namespace annota {

struct CompileResult {
    std::shared_ptr<Chunk> main;
    std::vector<std::shared_ptr<ClassInfo>> classes;
    std::vector<AnnotationRecord> annotations;
    std::vector<std::string> views;
    std::vector<std::string> states;
    std::vector<std::string> moduleNames;
};

class Compiler {
public:
    Compiler(Program prog, std::string file, bool contracts);
    CompileResult compile();
    const std::vector<std::string>& warnings() const { return warnings_; }
    const std::vector<std::string>& errors() const { return errors_; }

private:
    struct Local {
        std::string name;
        int slot = 0;
        int depth = 0;
        bool isConst = false;
        std::string type;                    // declared type name ("" when untyped)
        std::vector<int64_t> fixedDims;      // declared `T[n][m]` shape, when all sizes are known
        bool captured = false;
    };
    struct LoopCtx {
        std::vector<int> breaks;
        std::vector<int> continues;
        size_t tryDepth = 0;
        std::vector<ExprP> invariants;
    };
    struct FuncState {
        std::shared_ptr<Chunk> chunk;
        std::vector<Local> locals;
        std::vector<UpvalDesc> upvals;
        FuncState* parent = nullptr;
        int scopeDepth = 0;
        int nextSlot = 0;
        int maxSlot = 0;              // high-water mark of allocated slots
        std::shared_ptr<ClassInfo> cls;
        int clsConstIdx = -1;
        int tryDepth = 0;
        std::vector<LoopCtx> loops;
        bool isTopLevel = false;
        std::string origin;           // module file, when the chunk came from `use`
        bool hasResult = false;
        int resultSlot = -1;
        std::vector<ExprP> ensures;
        std::vector<std::string> ensureTexts;
        std::string fnName;
    };

    Program prog_;
    std::string file_;
    std::string stmtOrigin_;          // source file of the statement being compiled
    bool contracts_ = false;
    FuncState* fs_ = nullptr;
    std::vector<std::unique_ptr<FuncState>> pool_;
    CompileResult res_;
    int uiSlot_ = -1;
    std::vector<int> uiStack_;
    std::vector<std::string> warnings_;
    std::vector<std::string> errors_;
    std::set<std::string> constGlobals_;

    // ---- chunk emission
    int emit(Op op, int line);
    void emitByte(uint8_t b, int line);
    void emitU16(uint16_t v, int line);
    int emitJump(Op op, int line);
    void patchJump(int at);
    void patchJumpTo(int at, int target);
    void emitLoop(int target);
    int addConst(const Value& v);
    int addString(const std::string& s);
    int here() const { return (int)fs_->chunk->code.size(); }

    // ---- scopes & names
    void beginScope();
    void endScope();
    void endScopeKeep();
    int declareLocal(const std::string& name, bool isConst);
    int resolveLocal(FuncState* f, const std::string& name);
    int addUpval(FuncState* f, UpvalDesc d);
    int resolveUpval(FuncState* f, const std::string& name);
    bool isGlobalScope() const { return fs_->isTopLevel && fs_->scopeDepth == 0; }
    bool isOwnField(const std::string& name) const;

    // ---- statements
    void stmt(const StmtP& s);
    void stmtBlock(const BlockP& b);
    void stmtIf(const StmtP& s);
    void stmtWhile(const StmtP& s);
    void stmtFor(const StmtP& s);
    void stmtNew(const StmtP& s);
    void emitLend(const StmtP& s);
    void stmtAssign(const StmtP& s);
    void stmtCompound(const StmtP& s);
    void stmtFuncDef(const StmtP& s);
    void stmtClassDef(const StmtP& s);
    void stmtReturn(const StmtP& s);
    void stmtUse(const StmtP& s);
    void compileContracts(const std::vector<Annotation>& anns, const char* names[], int n);
    std::vector<Annotation> pendingAssert(const std::vector<Annotation>& anns);

    // ---- expressions
    void expr(const ExprP& e);
    void exprCall(const ExprP& e);
    void storeTarget(const ExprP& target, bool isDecl, bool isConst);
    void compileArgs(const ExprP& e);          // pushes posList, namedMap
    int compileUIBlock(const BlockP& b);       // pushes a fresh list, returns its slot
    void uiAppend(const ExprP& e);

    // ---- function / class
    std::shared_ptr<Chunk> compileFunction(const std::string& name, const std::vector<Param>& params,
                                           const BlockP& body, bool isMethod,
                                           const std::shared_ptr<ClassInfo>& cls,
                                           const std::vector<Annotation>& anns);
    bool stmtUnsafe_ = false;      // the statement being compiled carries [[unsafe]]
    Value makeDefault(const std::string& type);
    void emitNewArray(const StmtP& s);
    void optimizeChunk(const std::shared_ptr<Chunk>& ch);   // [[jit]]: fold + fuse this function
    void noteLocalShape(const std::string& name, const std::string& type,
                        const std::vector<ExprP>& dims);
    bool hasAnn(const std::vector<Annotation>& anns, const char* name) const;
    const Annotation* findAnn(const std::vector<Annotation>& anns, const char* name) const;
    Value constEval(const ExprP& e, bool& ok);

    FuncState* pushState(const std::string& fnName, bool isTopLevel);
    void popState();
    void error(const std::string& msg, int line);
    void warn(const std::string& msg);
};

} // namespace annota
