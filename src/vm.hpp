// Annota - vm.hpp
#pragma once
#include "bytecode.hpp"
#include <set>
#include <functional>
#include <iostream>

namespace annota {

struct Frame {
    std::shared_ptr<Chunk> chunk;
    size_t ip = 0;
    std::vector<Cell> locals;
    std::vector<Cell> upvals;
    uint64_t providedMask = 0;         // parameter i was supplied (i < 64)
    int argc = 0;
    size_t stackBase = 0;
    bool buildOnReturn = false;
    bool discardResult = false;
    bool isBuild = false;              // this frame runs a component/view builder
    std::shared_ptr<ClassInfo> klass;
};

struct TryFrame {
    size_t handler = 0;
    size_t stackDepth = 0;
    size_t frameIndex = 0;
};

class VM {
public:
    VM();

    std::shared_ptr<Chunk> mainChunk;
    std::unordered_map<std::string, Cell> globals;
    std::vector<Value> stack;
    std::vector<Frame> frames;
    std::vector<TryFrame> tryFrames;

    // Frames are recycled: allocating `numLocals` shared cells (and one Value each) used to
    // dominate call overhead.  Cells captured by a closure are detached instead of pooled.
    std::unordered_map<size_t, std::vector<std::vector<Cell>>> cellPool;
    void recycleFrame(Frame& fr);
    Value emptyNamedArgs();            // one shared empty dict, instead of one per call

    bool contracts = false;
    std::set<std::string> stateNames;
    std::function<void(VM&)> onStateChange;
    // How `input` (and `_stdin_line`) obtains a line. Left empty in console programs, where a
    // line is read from stdin; a GUI host installs a provider that asks the user instead.
    std::function<std::string(const std::string& hint)> inputProvider;
    std::string stdoutText;              // when capturing is enabled
    bool capture = false;
    // named arguments / receiver of the native call currently in progress
    Value pendingNamed;
    Value pendingSelf;
    // style objects (classes) whose fields are merged into component attributes
    std::set<std::string> styleClasses;

    // ---- entry points
    Value run();
    Value execute(size_t stopDepth);
    Value callSync(const Value& callee, std::vector<Value> args);
    Value callFunction(const Value& callee, const Value& pos, const Value& named,
                       const Cell& thisCell, const Value& children, bool hasChildren);

    // ---- helpers shared with natives
    void push(const Value& v) { stack.push_back(v); }
    void push(Value&& v) { stack.push_back(std::move(v)); }
    Value pop() {
        if (stack.empty()) throw VMError("internal: value stack underflow");
        Value v = std::move(stack.back());
        stack.pop_back();
        return v;
    }
    Value& peek(size_t back = 0) { return stack[stack.size() - 1 - back]; }

    bool truthy(const Value& v);
    std::string toStr(const Value& v);
    std::string repr(const Value& v);
    void write(const std::string& s);
    [[noreturn]] void throwError(const std::string& msg);

    void defineGlobal(const std::string& name, const Value& v);
    bool getGlobal(const std::string& name, Value& out);
    Value makeNative(const std::string& name, std::function<Value(VM&, std::vector<Value>&)> fn);

    // object protocol
    Value getField(const Value& obj, const std::string& name);
    void setField(Value& obj, const std::string& name, const Value& v, int line);
    Value getIndex(const Value& obj, const Value& idx);
    void setIndex(Value& obj, const Value& idx, const Value& v);
    // `a[i][j]` fused: same semantics, no intermediate row view for arrays
    Value getIndex2(const Value& obj, const Value& i, const Value& j);
    void setIndex2(Value& obj, const Value& i, const Value& j, const Value& v);
    // OP_CALL_DIRECT: bind arguments that live on the operand stack, then run the callee
    Value directCallee(Chunk& ch, uint16_t nameIdx, Cell& selfOut);
    void invokeFunction(const Value& fn, const Value* args, size_t n, const Value& named,
                        const Cell& selfCell);
    // resolves (and if needed compiles) a callee so the JIT can emit a native call
    JitResolver jitResolver();
    bool runNativeIfReady(const Value& fn, Frame& fr);
    void bindArgsSpan(Frame& f, const std::shared_ptr<Chunk>& ch, const Value* args, size_t n,
                      const Value& named);
    bool findMethod(const Value& obj, const std::string& name, Value& out);
    bool callMagic(const Value& obj, const char* name, std::vector<Value> args, Value& out);
    Value makeIter(const Value& v);
    void uiAppend(const Value& listCellValue, const Value& node);
    void uiAppendChildren(Value& node, const Value& children);
    void applyStyle(Value& node);

    // report an uncaught error (with a stack trace) and exit
    std::string stackTrace();

private:
    void resetTryFrames();
    void bindArgs(Frame& f, const std::shared_ptr<Chunk>& ch, const Value& pos, const Value& named);
    bool unwind(const Value& err);
    void resolveParent(ClassInfo* ci);
    void instantiate(const Value& cls, const Value& pos, const Value& named,
                     const Value& children, bool hasChildren);
    Value binaryResult(Op op, const Value& a, const Value& b, const char* opName);
    void pushFrame(const Value& fn, const Cell& thisCell);
};

} // namespace annota
