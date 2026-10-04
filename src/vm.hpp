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
    std::vector<uint8_t> provided;
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
    Value pop() {
        if (stack.empty()) throw VMError("internal: value stack underflow");
        Value v = stack.back();
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
