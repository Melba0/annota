// Annota - compiler.cpp : AST -> bytecode.
#include "compiler.hpp"
#include <algorithm>

namespace annota {

Compiler::Compiler(Program prog, std::string file, bool contracts)
    : prog_(std::move(prog)), file_(std::move(file)), contracts_(contracts) {}

void Compiler::error(const std::string& msg, int line) {
    throw CompileError(file_ + ":" + formatInt(line) + ": " + msg, line);
}
void Compiler::warn(const std::string& msg) { warnings_.push_back(msg); }

// ---------------------------------------------------------------- emission
int Compiler::emit(Op op, int line) {
    int at = (int)fs_->chunk->code.size();
    fs_->chunk->code.push_back((uint8_t)op);
    fs_->chunk->lines.push_back(line);
    return at;
}
void Compiler::emitByte(uint8_t b, int line) {
    fs_->chunk->code.push_back(b);
    fs_->chunk->lines.push_back(line);
}
void Compiler::emitU16(uint16_t v, int line) {
    emitByte((uint8_t)(v >> 8), line);
    emitByte((uint8_t)(v & 0xff), line);
}
int Compiler::emitJump(Op op, int line) {
    emit(op, line);
    emitByte(0xff, line);
    emitByte(0xff, line);
    return (int)fs_->chunk->code.size() - 2;
}
void Compiler::patchJump(int at) {
    int jump = (int)fs_->chunk->code.size() - (at + 2);
    fs_->chunk->code[at] = (uint8_t)((jump >> 8) & 0xff);
    fs_->chunk->code[at + 1] = (uint8_t)(jump & 0xff);
}
void Compiler::patchJumpTo(int at, int target) {
    int jump = target - (at + 2);
    fs_->chunk->code[at] = (uint8_t)((jump >> 8) & 0xff);
    fs_->chunk->code[at + 1] = (uint8_t)(jump & 0xff);
}
void Compiler::emitLoop(int target) {
    emit(OP_LOOP, 0);
    int offset = target - ((int)fs_->chunk->code.size() + 2);
    emitByte((uint8_t)((offset >> 8) & 0xff), 0);
    emitByte((uint8_t)(offset & 0xff), 0);
}
int Compiler::addConst(const Value& v) {
    fs_->chunk->consts.push_back(v);
    return (int)fs_->chunk->consts.size() - 1;
}
int Compiler::addString(const std::string& s) {
    for (size_t i = 0; i < fs_->chunk->consts.size(); i++) {
        const Value& v = fs_->chunk->consts[i];
        if (v.t == VT::Str && v.o && v.o->str == s) return (int)i;
    }
    return addConst(Value::str(s));
}

// ---------------------------------------------------------------- states
Compiler::FuncState* Compiler::pushState(const std::string& fnName, bool isTopLevel) {
    auto f = std::make_unique<FuncState>();
    f->chunk = std::make_shared<Chunk>();
    f->chunk->file = file_;
    f->chunk->fnName = fnName;
    f->parent = fs_;
    f->isTopLevel = isTopLevel;
    f->fnName = fnName;
    FuncState* raw = f.get();
    pool_.push_back(std::move(f));
    fs_ = raw;
    return raw;
}
void Compiler::popState() {
    fs_->chunk->numLocals = fs_->maxSlot;
    fs_ = fs_->parent;
}

void Compiler::beginScope() { fs_->scopeDepth++; }
void Compiler::endScope() {
    int newDepth = fs_->scopeDepth - 1;
    while (!fs_->locals.empty() && fs_->locals.back().depth > newDepth) {
        fs_->locals.pop_back();
        fs_->nextSlot--;
    }
    fs_->scopeDepth = newDepth;
}
int Compiler::declareLocal(const std::string& name, bool isConst) {
    Local l;
    l.name = name;
    l.slot = fs_->nextSlot++;
    l.depth = fs_->scopeDepth;
    l.isConst = isConst;
    fs_->locals.push_back(l);
    if (fs_->nextSlot > fs_->maxSlot) fs_->maxSlot = fs_->nextSlot;
    return l.slot;
}
int Compiler::resolveLocal(FuncState* f, const std::string& name) {
    for (int i = (int)f->locals.size() - 1; i >= 0; i--)
        if (f->locals[i].name == name) return f->locals[i].slot;
    return -1;
}
int Compiler::addUpval(FuncState* f, UpvalDesc d) {
    for (size_t i = 0; i < f->upvals.size(); i++)
        if (f->upvals[i].fromLocal == d.fromLocal && f->upvals[i].index == d.index) return (int)i;
    f->upvals.push_back(d);
    f->chunk->upvals = f->upvals;
    return (int)f->upvals.size() - 1;
}
bool Compiler::isOwnField(const std::string& name) const {
    if (!fs_->cls || !fs_->chunk->isMethod) return false;
    return fs_->cls->findField(name) >= 0;
}

int Compiler::resolveUpval(FuncState* f, const std::string& name) {
    if (!f->parent) return -1;
    int local = resolveLocal(f->parent, name);
    if (local >= 0) {
        for (auto& l : f->parent->locals) if (l.slot == local) l.captured = true;
        return addUpval(f, {(uint8_t)1, (uint8_t)local});
    }
    int up = resolveUpval(f->parent, name);
    if (up >= 0) return addUpval(f, {(uint8_t)0, (uint8_t)up});
    return -1;
}

bool Compiler::hasAnn(const std::vector<Annotation>& anns, const char* name) const {
    for (auto& a : anns) if (a.name == name) return true;
    return false;
}
const Annotation* Compiler::findAnn(const std::vector<Annotation>& anns, const char* name) const {
    for (auto& a : anns) if (a.name == name) return &a;
    return nullptr;
}

Value Compiler::makeDefault(const std::string& type) {
    if (type == "int") return Value::integer(0);
    if (type == "float") return Value::real(0.0);
    if (type == "bool") return Value::boolean(false);
    if (type == "List") return Value::list({});
    if (type == "Bytes") return Value::bytes({});
    return Value::null();
}

Value Compiler::constEval(const ExprP& e, bool& ok) {
    ok = true;
    if (!e) { ok = false; return Value::null(); }
    switch (e->kind) {
        case EK::Int:   return Value::integer(e->ival);
        case EK::Float: return Value::real(e->fval);
        case EK::Str:   return Value::str(e->sval);
        case EK::Bool:  return Value::boolean(e->bval);
        case EK::Null:  return Value::null();
        case EK::Color: return Value::color(e->color);
        case EK::Unary: {
            if (e->op != T::Minus) { ok = false; return Value::null(); }
            bool k = false;
            Value v = constEval(e->a, k);
            if (!k) { ok = false; return Value::null(); }
            if (v.t == VT::Int) return Value::integer(-v.i);
            if (v.t == VT::Float) return Value::real(-v.f);
            ok = false;
            return Value::null();
        }
        case EK::List:
        case EK::Tuple: {
            std::vector<Value> items;
            for (auto& it : e->items) {
                bool k = false;
                Value v = constEval(it, k);
                if (!k) { ok = false; return Value::null(); }
                items.push_back(v);
            }
            return e->kind == EK::List ? Value::list(items) : Value::tuple(items);
        }
        default: ok = false; return Value::null();
    }
}

// ---------------------------------------------------------------- program
CompileResult Compiler::compile() {
    pushState("<main>", true);
    fs_->chunk->file = file_;
    for (auto& s : prog_.stmts) stmt(s);
    emit(OP_RETURN_NULL, 0);
    popState();
    res_.main = pool_[0]->chunk;
    res_.annotations = prog_.annotationIndex;
    return res_;
}

// ---------------------------------------------------------------- statements
void Compiler::compileContracts(const std::vector<Annotation>& anns, const char* names[], int n) {
    if (!contracts_) return;
    for (auto& a : anns) {
        bool match = false;
        for (int i = 0; i < n; i++) if (a.name == names[i]) match = true;
        if (!match) continue;
        if (a.args.empty()) continue;
        const AnnotArg& g = a.args[0];
        if (!g.expr) continue;
        std::string msg = a.name + " failed";
        if (!g.text.empty()) msg += ": " + g.text;
        expr(g.expr);
        emit(OP_ASSERT, a.line);
        emitU16((uint16_t)addString(msg), a.line);
    }
}

void Compiler::stmt(const StmtP& s) {
    if (!s) return;
    switch (s->kind) {
        case SK::Annot: {
            if (contracts_) {
                const char* names[] = {"assert", "assume"};
                compileContracts({s->ann}, names, s->ann.name == "assume" ? 0 : 2);
            }
            return;
        }
        case SK::Expr: {
            if (contracts_) {
                const char* names[] = {"assert"};
                compileContracts(s->annotations, names, 1);
            }
            if (uiSlot_ >= 0 && !s->expr) return;
            if (uiSlot_ >= 0) { uiAppend(s->expr); }
            else if (s->expr) { expr(s->expr); emit(OP_POP, s->line); }
            return;
        }
        case SK::New:
        case SK::Const: stmtNew(s); return;
        case SK::State: {
            if (s->initExpr) expr(s->initExpr);
            else emit(OP_NULL, s->line);
            emit(OP_DEF_GLOBAL, s->line);
            emitU16((uint16_t)addString(s->name), s->line);
            res_.states.push_back(s->name);
            return;
        }
        case SK::Assign: stmtAssign(s); return;
        case SK::CompoundAssign: stmtCompound(s); return;
        case SK::Del: {
            for (auto& nm : s->names) {
                if (constGlobals_.count(nm) && resolveLocal(fs_, nm) < 0)
                    error("cannot delete constant '" + nm + "'", s->line);
                int slot = resolveLocal(fs_, nm);
                if (slot >= 0) { emit(OP_DEL_LOCAL, s->line); emitByte((uint8_t)slot, s->line); continue; }
                int up = resolveUpval(fs_, nm);
                if (up >= 0) { emit(OP_GET_UPVAL, s->line); emitByte((uint8_t)up, s->line); continue; }
                emit(OP_DEL_GLOBAL, s->line);
                emitU16((uint16_t)addString(nm), s->line);
            }
            return;
        }
        case SK::If: stmtIf(s); return;
        case SK::While: stmtWhile(s); return;
        case SK::For: stmtFor(s); return;
        case SK::Break: {
            if (fs_->loops.empty()) error("'break' outside a loop", s->line);
            LoopCtx& lp = fs_->loops.back();
            for (size_t i = lp.tryDepth; i < (size_t)fs_->tryDepth; i++) emit(OP_POP_TRY, s->line);
            lp.breaks.push_back(emitJump(OP_JUMP, s->line));
            return;
        }
        case SK::Continue: {
            if (fs_->loops.empty()) error("'continue' outside a loop", s->line);
            LoopCtx& lp = fs_->loops.back();
            for (size_t i = lp.tryDepth; i < (size_t)fs_->tryDepth; i++) emit(OP_POP_TRY, s->line);
            lp.continues.push_back(emitJump(OP_JUMP, s->line));
            return;
        }
        case SK::Throw: {
            expr(s->expr);
            emit(OP_THROW, s->line);
            return;
        }
        case SK::Print: {
            for (auto& a : s->args) expr(a);
            uint8_t n = (uint8_t)s->args.size();
            if (s->sep) expr(s->sep);
            emit(OP_PRINT, s->line);
            emitByte(n, s->line);
            emitByte(s->sep ? 1 : 0, s->line);
            return;
        }
        case SK::Input: {
            ExprP t = s->target;
            if (t->kind != EK::Ident) error("'input' requires a variable name", s->line);
            // the variable name travels with the instruction so that a GUI host can use it as
            // the prompt of its input dialog
            emit(OP_INPUT, s->line);
            emitU16((uint16_t)addString(t->name), s->line);
            bool declared = resolveLocal(fs_, t->name) >= 0 || resolveUpval(fs_, t->name) >= 0;
            storeTarget(t, !declared, false);
            return;
        }
        case SK::Return: stmtReturn(s); return;
        case SK::Block: stmtBlock(s->body); return;
        case SK::FuncDef: stmtFuncDef(s); return;
        case SK::ClassDef: stmtClassDef(s); return;
        case SK::Use: stmtUse(s); return;
        case SK::MacroDef: return;                       // compile-time only
        case SK::FieldDecl: {
            if (!fs_->cls) error("field declaration outside a class body", s->line);
            emit(OP_GET_LOCAL, s->line);
            emitByte(0, s->line);                        // this
            int slot = resolveLocal(fs_, s->name);
            // `_x:Type` inside a class whose constructor takes `x` initialises the field from it
            if (slot < 0 && !s->name.empty() && s->name[0] == '_')
                slot = resolveLocal(fs_, s->name.substr(1));
            if (slot >= 0) { emit(OP_GET_LOCAL, s->line); emitByte((uint8_t)slot, s->line); }
            else if (s->initExpr) expr(s->initExpr);
            else {
                bool ok = false;
                Value v = makeDefault(s->type);
                emit(OP_CONST, s->line);
                emitU16((uint16_t)addConst(v), s->line);
                (void)ok;
            }
            emit(OP_SET_FIELD, s->line);
            emitU16((uint16_t)addString(s->name), s->line);
            return;
        }
        case SK::ParentInit: {
            if (!fs_->cls) error("inheritance clause outside a class body", s->line);
            // build the argument package
            emit(OP_BUILD_LIST, s->line);
            emitU16(0, s->line);
            for (auto& a : s->parentArgs) {
                expr(a);
                emit(OP_LIST_APPEND, s->line);
            }
            emit(OP_BUILD_NAMED, s->line);
            emitU16(0, s->line);
            emit(OP_SUPER_INIT, s->line);
            emitU16((uint16_t)fs_->clsConstIdx, s->line);
            return;
        }
        default:
            error("unsupported statement", s->line);
    }
}

void Compiler::stmtBlock(const BlockP& b) {
    if (!b) return;
    beginScope();
    if (b->exceptBody) {
        int at = emit(OP_TRY, b->exceptLine);
        emitU16(0xffff, b->exceptLine);
        fs_->tryDepth++;
        for (auto& s : b->stmts) stmt(s);
        emit(OP_POP_TRY, b->exceptLine);
        int skip = emitJump(OP_JUMP, b->exceptLine);
        int handlerPos = here();
        patchJump(at + 1);
        fs_->tryDepth--;
        int slot = declareLocal(b->exceptName, false);
        emit(OP_INIT_LOCAL, b->exceptLine);
        emitByte((uint8_t)slot, b->exceptLine);
        for (auto& s : b->exceptBody->stmts) stmt(s);
        endScope();
        patchJump(skip);
        (void)handlerPos;
    } else {
        for (auto& s : b->stmts) stmt(s);
        endScope();
    }
}

void Compiler::stmtIf(const StmtP& s) {
    expr(s->cond);
    int elseJump = emitJump(OP_JUMP_IF_FALSE, s->line);
    stmtBlock(s->body);
    if (s->elseBody) {
        int endJump = emitJump(OP_JUMP, s->line);
        patchJump(elseJump);
        stmtBlock(s->elseBody);
        patchJump(endJump);
    } else {
        patchJump(elseJump);
    }
}

void Compiler::stmtWhile(const StmtP& s) {
    int loopStart = here();
    fs_->loops.push_back(LoopCtx());
    fs_->loops.back().tryDepth = (size_t)fs_->tryDepth;
    if (contracts_) {
        const Annotation* inv = findAnn(s->annotations, "invariant");
        if (inv && inv->args.size() && inv->args[0].expr) fs_->loops.back().invariants.push_back(inv->args[0].expr);
    }
    expr(s->cond);
    int exitJump = emitJump(OP_JUMP_IF_FALSE, s->line);
    stmtBlock(s->body);
    if (contracts_) {
        const char* names[] = {"invariant"};
        compileContracts(s->annotations, names, 1);
    }
    emitLoop(loopStart);
    patchJump(exitJump);
    LoopCtx lp = fs_->loops.back();
    fs_->loops.pop_back();
    for (int j : lp.breaks) patchJump(j);
    for (int j : lp.continues) {
        int save = (int)fs_->chunk->code.size();
        (void)save;
        // continue jumps to the loop condition, which is loopStart
        int jump = loopStart - (j + 2);
        fs_->chunk->code[j] = (uint8_t)((jump >> 8) & 0xff);
        fs_->chunk->code[j + 1] = (uint8_t)(jump & 0xff);
    }
}

void Compiler::stmtFor(const StmtP& s) {
    beginScope();
    // iterator on the stack
    if (s->iterable->kind == EK::Iter) {
        expr(s->iterable->a);
        expr(s->iterable->b);
        if (s->iterable->bodyExpr) {
            expr(s->iterable->bodyExpr);
            emit(OP_ITER_RANGE_STEP, s->line);
        } else {
            emit(OP_ITER_RANGE, s->line);
        }
    } else {
        expr(s->iterable);
        emit(OP_ITER_VALUE, s->line);
    }
    // loop variables
    std::vector<int> slots;
    for (auto& v : s->loopVars) slots.push_back(declareLocal(v, false));

    fs_->loops.push_back(LoopCtx());
    fs_->loops.back().tryDepth = (size_t)fs_->tryDepth;
    int loopStart = here();
    int exitJump = emitJump(OP_ITER_NEXT, s->line);
    if (slots.size() == 1) {
        emit(OP_INIT_LOCAL, s->line);
        emitByte((uint8_t)slots[0], s->line);
    } else {
        emit(OP_ITER_BIND2, s->line);
        emitByte((uint8_t)slots[0], s->line);
        emitByte((uint8_t)slots[1], s->line);
    }
    stmtBlock(s->body);
    if (contracts_) {
        const char* names[] = {"invariant"};
        compileContracts(s->annotations, names, 1);
    }
    emitLoop(loopStart);
    int popPos = here();
    emit(OP_POP, s->line);
    patchJumpTo(exitJump, popPos);
    LoopCtx lp = fs_->loops.back();
    fs_->loops.pop_back();
    for (int j : lp.breaks) {
        int jump = popPos - (j + 2);
        fs_->chunk->code[j] = (uint8_t)((jump >> 8) & 0xff);
        fs_->chunk->code[j + 1] = (uint8_t)(jump & 0xff);
    }
    for (int j : lp.continues) {
        int jump = loopStart - (j + 2);
        fs_->chunk->code[j] = (uint8_t)((jump >> 8) & 0xff);
        fs_->chunk->code[j + 1] = (uint8_t)(jump & 0xff);
    }
    endScope();
}

void Compiler::storeTarget(const ExprP& target, bool isDecl, bool isConst) {
    if (target->kind != EK::Ident) {
        error("invalid declaration target", target->line);
    }
    const std::string& name = target->name;
    if (isDecl && !isGlobalScope()) {
        int fresh = declareLocal(name, isConst);
        emit(OP_INIT_LOCAL, target->line);
        emitByte((uint8_t)fresh, target->line);
        return;
    }
    int slot = resolveLocal(fs_, name);
    if (slot >= 0) {
        emit(OP_SET_LOCAL, target->line);
        emitByte((uint8_t)slot, target->line);
        return;
    }
    int up = resolveUpval(fs_, name);
    if (up >= 0) {
        emit(OP_SET_UPVAL, target->line);
        emitByte((uint8_t)up, target->line);
        return;
    }
    if (isOwnField(name)) {
        emit(OP_GET_LOCAL, target->line);
        emitByte(0, target->line);
        emit(OP_SWAP, target->line);
        emit(OP_SET_FIELD, target->line);
        emitU16((uint16_t)addString(name), target->line);
        return;
    }
    if (isDecl) {
        emit(OP_DEF_GLOBAL, target->line);
        emitU16((uint16_t)addString(name), target->line);
        return;
    }
    emit(OP_SET_GLOBAL, target->line);
    emitU16((uint16_t)addString(name), target->line);
}

void Compiler::stmtNew(const StmtP& s) {
    bool isConst = (s->kind == SK::Const);
    if (s->names.size() == 1) {
        if (s->initExpr) expr(s->initExpr);
        else emit(OP_NULL, s->line);
        ExprP id = std::make_shared<Expr>();
        id->kind = EK::Ident;
        id->name = s->names[0];
        id->line = s->line;
        storeTarget(id, true, isConst);
        // mark the local constant
        if (isConst) {
            for (auto& l : fs_->locals) if (l.name == s->names[0]) l.isConst = true;
            if (isGlobalScope()) constGlobals_.insert(s->names[0]);
        }
        return;
    }
    expr(s->initExpr);
    for (size_t i = 0; i < s->names.size(); i++) {
        emit(OP_DUP, s->line);
        emit(OP_CONST, s->line);
        emitU16((uint16_t)addConst(Value::integer((int64_t)i)), s->line);
        emit(OP_GET_INDEX, s->line);
        ExprP id = std::make_shared<Expr>();
        id->kind = EK::Ident;
        id->name = s->names[i];
        id->line = s->line;
        storeTarget(id, true, isConst);
    }
    emit(OP_POP, s->line);
}

void Compiler::stmtAssign(const StmtP& s) {
    // const protection
    if (s->target->kind == EK::Ident) {
        const std::string& nm = s->target->name;
        int slot = resolveLocal(fs_, nm);
        for (auto& l : fs_->locals) if (l.slot == slot && l.isConst) error("cannot assign to constant '" + l.name + "'", s->line);
        if (slot < 0 && resolveUpval(fs_, nm) < 0 && constGlobals_.count(nm))
            error("cannot assign to constant '" + nm + "'", s->line);
    }
    if (s->target->kind == EK::Ident) {
        expr(s->value);
        storeTarget(s->target, false, false);
    } else if (s->target->kind == EK::Field) {
        expr(s->target->a);
        expr(s->value);
        emit(OP_SET_FIELD, s->line);
        emitU16((uint16_t)addString(s->target->name), s->line);
    } else if (s->target->kind == EK::Index) {
        expr(s->target->a);
        expr(s->target->b);
        expr(s->value);
        emit(OP_SET_INDEX, s->line);
    } else {
        error("invalid assignment target", s->line);
    }
}

void Compiler::stmtCompound(const StmtP& s) {
    static const std::map<T, Op> binOps = {
        {T::PlusA, OP_ADD}, {T::MinusA, OP_SUB}, {T::StarA, OP_MUL}, {T::SlashA, OP_DIV},
        {T::PercentA, OP_MOD}, {T::StarStarA, OP_POW}, {T::AmpA, OP_BAND}, {T::PipeA, OP_BOR},
        {T::CaretA, OP_BXOR}, {T::ShlA, OP_SHL}, {T::ShrA, OP_SHR},
    };
    Op op = binOps.at(s->op);
    if (s->target->kind == EK::Ident) {
        expr(s->target);
        expr(s->value);
        emit(op, s->line);
        storeTarget(s->target, false, false);
    } else if (s->target->kind == EK::Field) {
        expr(s->target->a);
        emit(OP_DUP, s->line);
        emit(OP_GET_FIELD, s->line);
        emitU16((uint16_t)addString(s->target->name), s->line);
        expr(s->value);
        emit(op, s->line);
        emit(OP_SET_FIELD, s->line);
        emitU16((uint16_t)addString(s->target->name), s->line);
        emit(OP_POP, s->line);
    } else if (s->target->kind == EK::Index) {
        expr(s->target->a);
        expr(s->target->b);
        emit(OP_DUP2, s->line);
        emit(OP_GET_INDEX, s->line);
        expr(s->value);
        emit(op, s->line);
        emit(OP_SET_INDEX, s->line);
    } else {
        error("invalid compound assignment target", s->line);
    }
}

void Compiler::stmtReturn(const StmtP& s) {
    if (s->expr) expr(s->expr);
    else emit(OP_NULL, s->line);
    if (fs_->hasResult) {
        emit(OP_DUP, s->line);
        emit(OP_SET_LOCAL, s->line);
        emitByte((uint8_t)fs_->resultSlot, s->line);
        for (size_t i = 0; i < fs_->ensures.size(); i++) {
            expr(fs_->ensures[i]);
            emit(OP_ASSERT, s->line);
            emitU16((uint16_t)addString("ensure failed: " + fs_->ensureTexts[i]), s->line);
        }
    }
    emit(OP_RETURN, s->line);
}

void Compiler::stmtUse(const StmtP& s) {
    res_.moduleNames.push_back(s->module);
    if (s->body) for (auto& st : s->body->stmts) stmt(st);
}

void Compiler::stmtFuncDef(const StmtP& s) {
    std::shared_ptr<Chunk> ch = compileFunction(s->name, s->params, s->body, s->isMethod, fs_->cls, s->annotations);
    int k = addConst(Value::function(ch));
    emit(OP_CLOSURE, s->line);
    emitU16((uint16_t)k, s->line);
    if (isGlobalScope()) {
        emit(OP_DEF_GLOBAL, s->line);
        emitU16((uint16_t)addString(s->name), s->line);
    } else {
        int slot = declareLocal(s->name, false);
        emit(OP_INIT_LOCAL, s->line);
        emitByte((uint8_t)slot, s->line);
    }
}

void Compiler::stmtClassDef(const StmtP& s) {
    auto ci = std::make_shared<ClassInfo>();
    ci->name = s->name;
    ci->isComponent = s->isView;
    ci->isStyle = s->isStyle;
    for (auto& p : s->params) ci->paramNames.push_back(p.name);
    for (auto& a : s->annotations) {
        ci->annotations.push_back(a.name);
        if (a.name == "expose")
            for (auto& g : a.args) if (!g.hasKey) ci->exposed.push_back(g.text);
    }
    for (auto& m : s->members) {
        if (m->kind == SK::FieldDecl || m->kind == SK::FuncDef) {
            for (auto& a : m->annotations) ci->annotations.push_back(a.name);
        }
    }

    std::vector<StmtP> initStmts, uiStmts, staticStmts;
    std::vector<StmtP> methodStmts, staticFns;
    StmtP initBody;                     // explicit `__init__`, if the class declares one
    // ---- pass 1: classify members and collect the field layout, so that methods know
    //              which bare names refer to fields of the enclosing class
    for (auto& m : s->members) {
        if (m->kind == SK::Annot) continue;
        if (m->kind == SK::FieldDecl) {
            if (hasAnn(m->annotations, "static")) {
                staticStmts.push_back(m);
                bool ok = false;
                Value dv = m->initExpr ? constEval(m->initExpr, ok) : Value::null();
                ci->statics.push_back({m->name, ok ? dv : Value::null(), true});
            } else {
                ci->ownFields.push_back(m->name);
                bool ok = false;
                Value dv = m->initExpr ? constEval(m->initExpr, ok) : Value::null();
                if (!ok) dv = makeDefault(m->type);
                ci->ownDefaults.push_back(dv);
                initStmts.push_back(m);
            }
        } else if (m->kind == SK::ParentInit) {
            ci->parentName = m->parentName;
            initStmts.push_back(m);
        } else if (m->kind == SK::FuncDef) {
            // `__init__()` is the constructor body: it runs first inside the constructor and
            // sees the class parameters (`tuple(a, b)` -> `a`, `b`) as locals.
            if (m->name == "__init__" && !hasAnn(m->annotations, "static")) {
                if (!m->params.empty())
                    error("'__init__' takes no parameters; declare them on the class instead "
                          "(e.g. `Name(a, b)=( __init__()( ... ) )`)", m->line);
                initBody = m;
                continue;
            }
            if (hasAnn(m->annotations, "static")) staticFns.push_back(m);
            else methodStmts.push_back(m);
        } else {
            uiStmts.push_back(m);
        }
    }
    if (!uiStmts.empty()) ci->isComponent = true;
    // constructor parameters are implicit fields (this makes bare `name`/`pad` work inside
    // methods and view builders, as the documentation's components rely on)
    for (auto& p : s->params) {
        bool declared = false;
        for (auto& f : ci->ownFields) if (f == p.name) { declared = true; break; }
        if (declared) continue;
        ci->ownFields.push_back(p.name);
        bool ok = false;
        Value dv = p.def ? constEval(p.def, ok) : Value::null();
        if (!ok) dv = makeDefault(p.type);
        ci->ownDefaults.push_back(dv);
        StmtP f = std::make_shared<Stmt>();
        f->kind = SK::FieldDecl;
        f->name = p.name;
        f->line = s->line;
        initStmts.insert(initStmts.begin(), f);
    }
    if (ci->isComponent) {
        if (ci->fieldIndex.find("__children") == ci->fieldIndex.end()) {
            ci->ownFields.push_back("__children");
            ci->ownDefaults.push_back(Value::list({}));
        }
    }
    ci->buildLayout();

    // ---- pass 2: now that the layout is known, compile the behaviour
    for (auto& m : methodStmts) {
        std::shared_ptr<Chunk> ch = compileFunction(m->name, m->params, m->body, true, ci, m->annotations);
        ci->methods.push_back({m->name, Value::function(ch), false});
    }
    for (auto& m : staticFns) {
        std::shared_ptr<Chunk> ch = compileFunction(m->name, m->params, m->body, false, ci, m->annotations);
        ci->statics.push_back({m->name, Value::function(ch), true});
    }

    // constructor: declared field defaults (and the automatic `param -> field` copies) first,
    // then the explicit `__init__`, so that `__init__` can override them.  `__init__` sees the
    // class parameters (`tuple(a, b)` -> `a`, `b`) as locals.
    {
        std::vector<StmtP> ctorStmts;
        for (auto& st : initStmts) ctorStmts.push_back(st);
        if (initBody && initBody->body)
            for (auto& st : initBody->body->stmts) ctorStmts.push_back(st);
        BlockP ib = std::make_shared<Block>();
        ib->stmts = ctorStmts;
        ib->line = s->line;
        std::shared_ptr<Chunk> ch = compileFunction(s->name, s->params, ib, true, ci, s->annotations);
        ch->fnName = s->name;
        ci->initFn = Value::function(ch);
    }
    // view / component builder
    if (ci->isComponent) {
        BlockP bb = std::make_shared<Block>();
        bb->stmts = uiStmts;
        bb->isUI = true;
        bb->line = s->line;
        std::shared_ptr<Chunk> ch = compileFunction(s->name + ".build", {}, bb, true, ci, {});
        ci->buildFn = Value::function(ch);
        if (s->isView) res_.views.push_back(s->name);
    }
    // static initialiser
    if (!staticStmts.empty()) {
        BlockP sb = std::make_shared<Block>();
        sb->stmts = staticStmts;
        sb->line = s->line;
        // remove statics already const-evaluated to avoid double work; keep assignments harmless
        std::shared_ptr<Chunk> ch = compileFunction(s->name + ".static", {}, sb, true, ci, {});
        ci->staticInitFn = Value::function(ch);
    }

    int k = addConst(Value::klass(ci));
    emit(OP_CLASS, s->line);
    emitU16((uint16_t)k, s->line);
    if (ci->staticInitFn.t != VT::Null) {
        emit(OP_DUP, s->line);
        emit(OP_INIT_CLASS, s->line);
    }
    if (isGlobalScope()) {
        emit(OP_DEF_GLOBAL, s->line);
        emitU16((uint16_t)addString(s->name), s->line);
    } else {
        int slot = declareLocal(s->name, false);
        emit(OP_INIT_LOCAL, s->line);
        emitByte((uint8_t)slot, s->line);
    }
    res_.classes.push_back(ci);
}

std::shared_ptr<Chunk> Compiler::compileFunction(const std::string& name, const std::vector<Param>& params,
                                                const BlockP& body, bool isMethod,
                                                const std::shared_ptr<ClassInfo>& cls,
                                                const std::vector<Annotation>& anns) {
    FuncState* f = pushState(name, false);
    f->cls = cls;
    f->chunk->fnName = name;
    f->chunk->isMethod = isMethod;
    if (cls) f->clsConstIdx = addConst(Value::klass(cls));

    if (isMethod) declareLocal("this", false);
    int fixed = 0;
    for (auto& p : params) {
        ParamInfo pi;
        pi.name = p.name;
        pi.hasDefault = (bool)p.def;
        pi.vararg = p.vararg;
        f->chunk->params.push_back(pi);
        int slot = declareLocal(p.name, false);
        if (p.vararg) {
            f->chunk->varargSlot = slot;
        } else {
            fixed++;
        }
    }
    f->chunk->fixedCount = fixed;

    if (f->chunk->varargSlot >= 0) {
        emit(OP_COLLECT_VARARGS, 0);
        emitByte((uint8_t)f->chunk->varargSlot, 0);
        emitByte((uint8_t)fixed, 0);
    }
    // optional parameter defaults
    for (size_t i = 0; i < params.size(); i++) {
        if (!params[i].def) continue;
        int slot = resolveLocal(f, params[i].name);
        emit(OP_JUMP_IF_PROVIDED, 0);
        emitByte((uint8_t)i, 0);
        int at = (int)f->chunk->code.size();
        emitByte(0xff, 0);
        emitByte(0xff, 0);
        expr(params[i].def);
        emit(OP_SET_LOCAL, 0);
        emitByte((uint8_t)slot, 0);
        int target = (int)f->chunk->code.size();
        int jump = target - (at + 2);
        f->chunk->code[at] = (uint8_t)((jump >> 8) & 0xff);
        f->chunk->code[at + 1] = (uint8_t)(jump & 0xff);
    }
    // contracts
    if (contracts_) {
        const char* names[] = {"require"};
        compileContracts(anns, names, 1);
        if (const Annotation* e = findAnn(anns, "ensure")) {
            f->hasResult = true;
            f->resultSlot = declareLocal("result", false);
            f->ensures.push_back(e->args.empty() ? nullptr : e->args[0].expr);
            f->ensureTexts.push_back(e->args.empty() ? "" : e->args[0].text);
        }
        for (auto& a : anns) {
            if (a.name != "ensure" || a.args.empty()) continue;
            if (f->ensures.size() == 1 && f->ensures[0] == a.args[0].expr) continue;
            f->ensures.push_back(a.args[0].expr);
            f->ensureTexts.push_back(a.args[0].text);
        }
    }

    int savedUi = uiSlot_;
    if (body && body->isUI) {
        int slot = declareLocal("__nodes", false);
        emit(OP_BUILD_LIST, 0);
        emitU16(0, 0);
        emit(OP_INIT_LOCAL, 0);
        emitByte((uint8_t)slot, 0);
        uiSlot_ = slot;
    }
    if (body) {
        if (body->exceptBody) {
            BlockP copy = std::make_shared<Block>(*body);
            copy->isUI = false;
            stmtBlock(copy);
        } else {
            for (auto& s : body->stmts) stmt(s);
        }
    }
    if (body && body->isUI) {
        emit(OP_GET_LOCAL, 0);
        emitByte((uint8_t)uiSlot_, 0);
        uiSlot_ = savedUi;
        emit(OP_RETURN, 0);
    } else {
        emit(OP_RETURN_NULL, 0);
    }
    std::shared_ptr<Chunk> ch = f->chunk;
    popState();
    return ch;
}

// ---------------------------------------------------------------- expressions
void Compiler::uiAppend(const ExprP& e) {
    expr(e);
    if (uiSlot_ >= 0) {
        emit(OP_UI_APPEND, e->line);
        emitByte((uint8_t)uiSlot_, e->line);
    } else {
        emit(OP_POP, e->line);
    }
}

int Compiler::compileUIBlock(const BlockP& b) {
    int savedUi = uiSlot_;
    beginScope();
    int slot = declareLocal("__ui", false);
    emit(OP_BUILD_LIST, b->line);
    emitU16(0, b->line);
    emit(OP_INIT_LOCAL, b->line);
    emitByte((uint8_t)slot, b->line);
    uiSlot_ = slot;
    for (auto& s : b->stmts) stmt(s);
    uiSlot_ = savedUi;
    emit(OP_GET_LOCAL, b->line);
    emitByte((uint8_t)slot, b->line);
    endScope();
    return slot;
}

void Compiler::compileArgs(const ExprP& e) {
    emit(OP_BUILD_LIST, e->line);
    emitU16(0, e->line);
    for (auto& a : e->args) {
        if (!a.name.empty()) continue;              // named arguments go into the map below
        expr(a.value);
        if (a.spread) emit(OP_LIST_EXTEND, e->line);
        else emit(OP_LIST_APPEND, e->line);
    }
    int n = 0;
    for (auto& a : e->args) if (!a.name.empty()) n++;
    if (n) {
        for (auto& a : e->args) {
            if (a.name.empty()) continue;
            emit(OP_CONST, e->line);
            emitU16((uint16_t)addString(a.name), e->line);
            expr(a.value);
        }
    }
    emit(OP_BUILD_NAMED, e->line);
    emitU16((uint16_t)n, e->line);
}

void Compiler::exprCall(const ExprP& e) {
    expr(e->a);
    compileArgs(e);
    if (e->children) {
        compileUIBlock(e->children);
        emit(OP_CALL_UI, e->line);
    } else {
        emit(OP_CALL, e->line);
    }
}

void Compiler::expr(const ExprP& e) {
    if (!e) { emit(OP_NULL, 0); return; }
    switch (e->kind) {
        case EK::Null: emit(OP_NULL, e->line); return;
        case EK::Bool: emit(e->bval ? OP_TRUE : OP_FALSE, e->line); return;
        case EK::Int:
            if (e->ival >= -128 && e->ival <= 127) {
                emit(OP_INT1, e->line);
                emitByte((uint8_t)(int8_t)e->ival, e->line);
            } else {
                emit(OP_CONST, e->line);
                emitU16((uint16_t)addConst(Value::integer(e->ival)), e->line);
            }
            return;
        case EK::Float:
            emit(OP_CONST, e->line);
            emitU16((uint16_t)addConst(Value::real(e->fval)), e->line);
            return;
        case EK::Str:
            emit(OP_CONST, e->line);
            emitU16((uint16_t)addConst(Value::str(e->sval)), e->line);
            return;
        case EK::Color:
            emit(OP_CONST, e->line);
            emitU16((uint16_t)addConst(Value::color(e->color)), e->line);
            return;
        case EK::Ident: {
            const std::string& nm = e->name;
            int slot = resolveLocal(fs_, nm);
            if (slot >= 0) { emit(OP_GET_LOCAL, e->line); emitByte((uint8_t)slot, e->line); return; }
            int up = resolveUpval(fs_, nm);
            if (up >= 0) { emit(OP_GET_UPVAL, e->line); emitByte((uint8_t)up, e->line); return; }
            // inside a method a bare name that names a field of the enclosing class is `this.name`
            if (isOwnField(nm)) {
                emit(OP_GET_LOCAL, e->line);
                emitByte(0, e->line);
                emit(OP_GET_FIELD, e->line);
                emitU16((uint16_t)addString(nm), e->line);
                return;
            }
            emit(OP_GET_GLOBAL, e->line);
            emitU16((uint16_t)addString(nm), e->line);
            return;
        }
        case EK::This: {
            // `this` is an ordinary name: slot 0 of a method, captured as an upvalue by lambdas
            int slot = resolveLocal(fs_, "this");
            if (slot >= 0) { emit(OP_GET_LOCAL, e->line); emitByte((uint8_t)slot, e->line); return; }
            int up = resolveUpval(fs_, "this");
            if (up >= 0) { emit(OP_GET_UPVAL, e->line); emitByte((uint8_t)up, e->line); return; }
            error("'this' outside a method", e->line);
            return;
        }
        case EK::Super: {
            error("'super' must be used as 'super.method(...)'", e->line);
            return;
        }
        case EK::Children: {
            int slot = resolveLocal(fs_, "this");
            if (slot >= 0) { emit(OP_GET_LOCAL, e->line); emitByte((uint8_t)slot, e->line); }
            else {
                int up = resolveUpval(fs_, "this");
                if (up < 0) error("'children()' outside a component", e->line);
                emit(OP_GET_UPVAL, e->line);
                emitByte((uint8_t)up, e->line);
            }
            emit(OP_GET_FIELD, e->line);
            emitU16((uint16_t)addString("__children"), e->line);
            return;
        }
        case EK::List: {
            for (auto& i : e->items) expr(i);
            emit(OP_BUILD_LIST, e->line);
            emitU16((uint16_t)e->items.size(), e->line);
            return;
        }
        case EK::Tuple: {
            for (auto& i : e->items) expr(i);
            emit(OP_BUILD_TUPLE, e->line);
            emitU16((uint16_t)e->items.size(), e->line);
            return;
        }
        case EK::Unary: {
            expr(e->a);
            switch (e->op) {
                case T::Minus: emit(OP_NEG, e->line); break;
                case T::Not:   emit(OP_NOT, e->line); break;
                case T::Tilde: emit(OP_BNOT, e->line); break;
                default: break;
            }
            return;
        }
        case EK::Binary: {
            expr(e->a);
            expr(e->b);
            switch (e->op) {
                case T::Plus: emit(OP_ADD, e->line); break;
                case T::Minus: emit(OP_SUB, e->line); break;
                case T::Star: emit(OP_MUL, e->line); break;
                case T::Slash: emit(OP_DIV, e->line); break;
                case T::Percent: emit(OP_MOD, e->line); break;
                case T::StarStar: emit(OP_POW, e->line); break;
                case T::Eq: emit(OP_EQ, e->line); break;
                case T::Ne: emit(OP_NE, e->line); break;
                case T::Lt: emit(OP_LT, e->line); break;
                case T::Gt: emit(OP_GT, e->line); break;
                case T::Le: emit(OP_LE, e->line); break;
                case T::Ge: emit(OP_GE, e->line); break;
                case T::Amp: emit(OP_BAND, e->line); break;
                case T::Pipe: emit(OP_BOR, e->line); break;
                case T::Caret: emit(OP_BXOR, e->line); break;
                case T::Shl: emit(OP_SHL, e->line); break;
                case T::Shr: emit(OP_SHR, e->line); break;
                default: break;
            }
            return;
        }
        case EK::Logical: {
            expr(e->a);
            if (e->op == T::AndAnd) {
                int j = emitJump(OP_JUMP_IF_FALSE_KEEP, e->line);
                emit(OP_POP, e->line);
                expr(e->b);
                patchJump(j);
            } else {
                int j = emitJump(OP_JUMP_IF_TRUE_KEEP, e->line);
                emit(OP_POP, e->line);
                expr(e->b);
                patchJump(j);
            }
            return;
        }
        case EK::Call: exprCall(e); return;
        case EK::Index: {
            expr(e->a);
            expr(e->b);
            emit(OP_GET_INDEX, e->line);
            return;
        }
        case EK::Field: {
            if (e->a->kind == EK::Super) {
                if (!fs_->cls) error("'super' outside a class", e->line);
                emit(OP_GET_SUPER, e->line);
                emitU16((uint16_t)addString(e->name), e->line);
                emitU16((uint16_t)fs_->clsConstIdx, e->line);
                return;
            }
            expr(e->a);
            emit(OP_GET_FIELD, e->line);
            emitU16((uint16_t)addString(e->name), e->line);
            return;
        }
        case EK::Piecewise: {
            std::vector<int> endJumps;
            for (auto& c : e->cases) {
                expr(c.first);
                int next = emitJump(OP_JUMP_IF_FALSE, e->line);
                expr(c.second);
                endJumps.push_back(emitJump(OP_JUMP, e->line));
                patchJump(next);
            }
            expr(e->elseVal);
            for (int j : endJumps) patchJump(j);
            return;
        }
        case EK::Lambda: {
            BlockP b = e->body;
            if (!b) {
                b = std::make_shared<Block>();
                StmtP r = std::make_shared<Stmt>();
                r->kind = SK::Return;
                r->line = e->line;
                r->expr = e->bodyExpr;
                b->stmts.push_back(r);
            }
            std::shared_ptr<Chunk> ch = compileFunction("<lambda>", e->params, b, false, fs_->cls, {});
            int k = addConst(Value::function(ch));
            emit(OP_CLOSURE, e->line);
            emitU16((uint16_t)k, e->line);
            return;
        }
        case EK::Iter: {
            expr(e->a);
            expr(e->b);
            if (e->bodyExpr) {
                expr(e->bodyExpr);
                emit(OP_ITER_RANGE_STEP, e->line);
            } else {
                emit(OP_ITER_RANGE, e->line);
            }
            return;
        }
    }
}

} // namespace annota
