// Annota - analysis_flow.cpp : abstract domain, CFG, data-flow analysis and the checks.
#include "value.hpp"
#include "analyzer.hpp"
#include "analysis_internal.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

namespace annota {

// ---------------------------------------------------------------- tiny CAS
// Integer *linear* forms  sum(k_i * v_i) + c  are normalised exactly, which decides conditions
// such as `i + 1 > i`, `2 * x == x + x`, `y - y == 0` or `n - 1 < n` symbolically instead of
// giving up.  Equalities learned from `[[assume: x == ...]]` become substitutions, so
// `assert(x * 2 == 10)` is provable once `x == 5` is known.
struct Lin {
    std::map<std::string, long long> t;
    long long c = 0;
    bool ok = true;
};


// -1 / 0 / 1 when the form is a constant, 2 when it still has terms
static int linConstSign(const Lin& l) {
    if (!l.ok) return 2;
    if (!l.t.empty()) return 2;
    return l.c < 0 ? -1 : (l.c > 0 ? 1 : 0);
}

static Lin linConst(long long v) { Lin l; l.c = v; return l; }
static Lin linVar(const std::string& n) { Lin l; l.t[n] = 1; return l; }
static Lin linAdd(const Lin& a, const Lin& b, long long sign) {
    Lin r = a;
    if (!b.ok) r.ok = false;
    r.c += sign * b.c;
    for (auto& kv : b.t) {
        r.t[kv.first] += sign * kv.second;
        if (r.t[kv.first] == 0) r.t.erase(kv.first);
    }
    return r;
}
static Lin linScale(const Lin& a, long long k) {
    Lin r = a;
    r.c *= k;
    for (auto& kv : r.t) kv.second *= k;
    for (auto it = r.t.begin(); it != r.t.end();) it = it->second == 0 ? r.t.erase(it) : std::next(it);
    return r;
}

// ---------------------------------------------------------------- CAS context
// Everything the symbolic layer may consult: equalities learned from conditions, linear lower and
// upper bounds (so `i < n` really gives `i + 1 <= n`), and the numeric ranges the abstract domain
// already knows about.
struct CasCtx {
    const std::map<std::string, Lin>* subst = nullptr;
    const std::map<std::string, Lin>* lo = nullptr;
    const std::map<std::string, Lin>* hi = nullptr;
    std::function<bool(const std::string&, long long&, long long&)> range;
};
static bool linInterval(const Lin& l, const CasCtx& ctx, Lin& loOut, Lin& hiOut, int depth = 0) {
    if (!l.ok || depth > 8) return false;
    loOut = linConst(l.c);
    hiOut = linConst(l.c);
    for (auto& kv : l.t) {
        const std::string& v = kv.first;
        long long k = kv.second;
        Lin vlo, vhi;
        bool haveLo = false, haveHi = false;
        if (ctx.lo) {
            auto it = ctx.lo->find(v);
            if (it != ctx.lo->end()) { vlo = it->second; haveLo = true; }
        }
        if (ctx.hi) {
            auto it = ctx.hi->find(v);
            if (it != ctx.hi->end()) { vhi = it->second; haveHi = true; }
        }
        if ((!haveLo || !haveHi) && ctx.range) {
            long long rl = 0, rh = 0;
            if (ctx.range(v, rl, rh)) {
                if (!haveLo) { vlo = linConst(rl); haveLo = true; }
                if (!haveHi) { vhi = linConst(rh); haveHi = true; }
            }
        }
        // A missing endpoint is the variable itself: symbolic terms then cancel exactly when the
        // same variable appears on both sides, which is what makes `i < n` prove `i + 1 <= n`
        // and the loop exit prove `i >= n`, while anything else stays "unknown".
        if (!haveLo) vlo = linVar(v);
        if (!haveHi) vhi = linVar(v);
        if ((haveLo && !vlo.t.empty() && vlo.t.count(v)) ||
            (haveHi && !vhi.t.empty() && vhi.t.count(v)))
            return false;                 // a bound mentioning the variable would recurse
        if (k >= 0) {
            loOut = linAdd(loOut, linScale(vlo, k), 1);
            hiOut = linAdd(hiOut, linScale(vhi, k), 1);
        } else {
            loOut = linAdd(loOut, linScale(vhi, k), 1);
            hiOut = linAdd(hiOut, linScale(vlo, k), 1);
        }
    }
        // resolve whatever variables the bounds introduced (one round is enough for
    // `i <= n - 1` combined with `n <= 1000`)
    if (depth < 2) {
        Lin l2, h2;
        if (linInterval(loOut, ctx, l2, h2, depth + 1)) loOut = l2;
        if (linInterval(hiOut, ctx, l2, h2, depth + 1)) hiOut = h2;
    }
    return true;
}



namespace detail {
// numeric type families: declared widths and the boxed scalar names all count as numbers
static bool isBoxedNumName(const std::string& t) {
    return t == "longlong" || t == "ulonglong" || t == "longdouble";
}
static int numFamilyOf(const std::string& t) {
    if (t == "int" || t == "long" || t == "int8" || t == "int16" || t == "int32" ||
        t == "int64" || t == "longlong") return 1;
    if (t == "uint" || t == "ulong" || t == "uint8" || t == "uint16" || t == "uint32" ||
        t == "uint64" || t == "ulonglong") return 2;
    if (t == "float" || t == "double" || t == "longdouble") return 3;
    return 0;
}

namespace {

// ================================================================ helpers
const char* opNameOf(T op) {
    switch (op) {
        case T::Plus: return "+";
        case T::Minus: return "-";
        case T::Star: return "*";
        case T::Slash: return "/";
        case T::Percent: return "%";
        case T::StarStar: return "**";
        case T::Eq: return "==";
        case T::Ne: return "!=";
        case T::Lt: return "<";
        case T::Gt: return ">";
        case T::Le: return "<=";
        case T::Ge: return ">=";
        case T::Amp: return "&";
        case T::Pipe: return "|";
        case T::Caret: return "^";
        case T::Shl: return "<<";
        case T::Shr: return ">>";
        default: return "?";
    }
}

// ================================================================ abstract domain
struct Range {
    bool known = false;
    long long lo = 0, hi = 0;
};
Range rng(long long lo, long long hi) { Range r; r.known = true; r.lo = lo; r.hi = hi; return r; }

struct AbsVal {
    Range range;
    Range size;
    std::string sizeKey;                 // when the size equals a variable's value: "v:len"
    enum NullState { NullUnknown, IsNull, NonNull, MaybeNull } nullState = NullUnknown;
    enum InitState { InitUnknown, IsInit, NotInit } init = InitUnknown;
    bool tainted = false;
    std::string taintSource;
    std::string type = "unknown";
    std::string eltType;                  // element type of an Array value
    bool isIter = false;
    int declLine = 0;
    bool read = false;
    int assignLine = 0;
    bool readSinceAssign = false;
    bool isLocal = false;
};

std::string rangeText(const Range& r) {
    if (!r.known) return "unknown";
    if (r.lo == r.hi) return formatInt(r.lo);
    return "[" + formatInt(r.lo) + ", " + formatInt(r.hi) + "]";
}

std::string stateText(const AbsVal& v) {
    std::string s = v.type == "unknown" ? "value" : v.type;
    if (v.isIter) s = "Iter";
    if (v.range.known) s += " ∈ " + rangeText(v.range);
    if (v.size.known) s += " size ∈ " + rangeText(v.size);
    if (v.nullState == AbsVal::IsNull) s += " (null)";
    else if (v.nullState == AbsVal::MaybeNull) s += " (maybe-null)";
    else if (v.nullState == AbsVal::NonNull && (v.type == "List" || v.type == "Tuple" ||
                                                v.type == "String" || v.type == "unknown"))
        s += " (non-null)";
    if (v.init == AbsVal::NotInit) s += " [未初始化]";
    if (v.tainted) s += " [tainted" + (v.taintSource.empty() ? "" : ":" + v.taintSource) + "]";
    return s;
}

struct Scope {
    std::map<std::string, AbsVal> vars;
};
struct Env {
    std::vector<Scope> scopes;
    Env() { scopes.emplace_back(); }
    AbsVal* find(const std::string& n) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto v = it->vars.find(n);
            if (v != it->vars.end()) return &v->second;
        }
        return nullptr;
    }
    void declare(const std::string& n, const AbsVal& v) { scopes.back().vars[n] = v; }
    void push() { scopes.emplace_back(); }
    void pop() { if (scopes.size() > 1) scopes.pop_back(); }
};

AbsVal mergeVal(const AbsVal& a, const AbsVal& b) {
    AbsVal o;
    o.type = (a.type == b.type) ? a.type : "unknown";
    if (a.range.known && b.range.known)
        o.range = rng(std::min(a.range.lo, b.range.lo), std::max(a.range.hi, b.range.hi));
    if (a.size.known && b.size.known)
        o.size = rng(std::min(a.size.lo, b.size.lo), std::max(a.size.hi, b.size.hi));
    if (a.sizeKey == b.sizeKey) o.sizeKey = a.sizeKey;
    if (a.nullState == b.nullState) o.nullState = a.nullState;
    else if (a.nullState == AbsVal::NullUnknown || b.nullState == AbsVal::NullUnknown)
        o.nullState = AbsVal::NullUnknown;
    else o.nullState = AbsVal::MaybeNull;
    if (a.init == AbsVal::NotInit || b.init == AbsVal::NotInit) o.init = AbsVal::NotInit;
    else if (a.init == AbsVal::IsInit && b.init == AbsVal::IsInit) o.init = AbsVal::IsInit;
    else o.init = AbsVal::InitUnknown;
    o.tainted = a.tainted || b.tainted;
    o.taintSource = a.tainted ? a.taintSource : b.taintSource;
    o.isIter = a.isIter && b.isIter;
    o.declLine = a.declLine ? a.declLine : b.declLine;
    o.read = a.read || b.read;
    o.isLocal = a.isLocal || b.isLocal;
    return o;
}

void mergeEnv(Env& into, const Env& other) {
    size_t n = std::min(into.scopes.size(), other.scopes.size());
    for (size_t i = 0; i < n; i++) {
        Scope& s = into.scopes[i];
        for (auto& kv : other.scopes[i].vars) {
            auto it = s.vars.find(kv.first);
            if (it == s.vars.end()) s.vars[kv.first] = kv.second;
            else it->second = mergeVal(it->second, kv.second);
        }
    }
}

// ================================================================ wide arithmetic
using W = __int128;
const W WCLAMP = (W)1 << 100;
W clampW(W v) { return v > WCLAMP ? WCLAMP : (v < -WCLAMP ? -WCLAMP : v); }

// ================================================================ CFG (control flow graph)
struct CfgNode {
    const Stmt* s = nullptr;
    int line = 0;
    std::vector<int> succ;
};
struct Cfg {
    std::vector<CfgNode> nodes;
    int add(const Stmt* s, int line) {
        nodes.push_back({s, line, {}});
        return (int)nodes.size() - 1;
    }
};

int cfgStmt(const StmtP& s, int next, int breakT, int contT, Cfg& cfg);
int cfgList(const std::vector<StmtP>& list, size_t from, int next, int breakT, int contT, Cfg& cfg);

int cfgBlock(const BlockP& b, int next, int breakT, int contT, Cfg& cfg) {
    if (!b) return next;
    int entry = cfgList(b->stmts, 0, next, breakT, contT, cfg);
    if (b->exceptBody) {
        int handler = cfgList(b->exceptBody->stmts, 0, next, breakT, contT, cfg);
        if (entry >= 0) cfg.nodes[(size_t)entry].succ.push_back(handler);   // exception edge
        else entry = handler;
    }
    return entry;
}

int cfgList(const std::vector<StmtP>& list, size_t from, int next, int breakT, int contT, Cfg& cfg) {
    if (from >= list.size()) return next;
    int following = cfgList(list, from + 1, next, breakT, contT, cfg);
    return cfgStmt(list[from], following, breakT, contT, cfg);
}

int cfgStmt(const StmtP& s, int next, int breakT, int contT, Cfg& cfg) {
    if (!s) return next;
    switch (s->kind) {
        case SK::If: {
            int n = cfg.add(s.get(), s->line);
            int t = cfgBlock(s->body, next, breakT, contT, cfg);
            int e = s->elseBody ? cfgBlock(s->elseBody, next, breakT, contT, cfg) : next;
            if (t >= 0) cfg.nodes[(size_t)n].succ.push_back(t);
            if (e >= 0) cfg.nodes[(size_t)n].succ.push_back(e);
            return n;
        }
        case SK::While: case SK::For: {
            int n = cfg.add(s.get(), s->line);
            int body = cfgBlock(s->body, n, next, n, cfg);
            if (body >= 0) cfg.nodes[(size_t)n].succ.push_back(body);
            if (next >= 0) cfg.nodes[(size_t)n].succ.push_back(next);
            return n;
        }
        case SK::Block:
            return cfgBlock(s->body, next, breakT, contT, cfg);
        case SK::Break: {
            int n = cfg.add(s.get(), s->line);
            if (breakT >= 0) cfg.nodes[(size_t)n].succ.push_back(breakT);
            return n;
        }
        case SK::Continue: {
            int n = cfg.add(s.get(), s->line);
            if (contT >= 0) cfg.nodes[(size_t)n].succ.push_back(contT);
            return n;
        }
        case SK::Return: case SK::Throw: {
            return cfg.add(s.get(), s->line);           // control leaves the block
        }
        default: {
            int n = cfg.add(s.get(), s->line);
            if (next >= 0) cfg.nodes[(size_t)n].succ.push_back(next);
            return n;
        }
    }
}

// variable sets for liveness
void collectUse(const ExprP& e, std::set<std::string>& use);
void collectDef(const ExprP& e, std::set<std::string>& def);

void collectUse(const ExprP& e, std::set<std::string>& use) {
    if (!e) return;
    if (e->kind == EK::Ident) use.insert(e->name);
    collectUse(e->a, use);
    collectUse(e->b, use);
    for (auto& i : e->items) collectUse(i, use);
    for (auto& a : e->args) collectUse(a.value, use);
    for (auto& c : e->cases) { collectUse(c.first, use); collectUse(c.second, use); }
    collectUse(e->elseVal, use);
    collectUse(e->bodyExpr, use);
}

void collectDef(const ExprP& e, std::set<std::string>& def) {
    if (!e) return;
    if (e->kind == EK::Ident) { def.insert(e->name); return; }
    if (e->kind == EK::Field) { collectUse(e->a, def); return; }
    if (e->kind == EK::Index) { collectUse(e->a, def); collectUse(e->b, def); return; }
}

void stmtUseDef(const Stmt* s, std::set<std::string>& use, std::set<std::string>& def) {
    if (!s) return;
    switch (s->kind) {
        case SK::New: case SK::Const:
            for (auto& n : s->names) def.insert(n);
            collectUse(s->initExpr, use);
            return;
        case SK::State:
            def.insert(s->name);
            collectUse(s->initExpr, use);
            return;
        case SK::Assign:
            if (s->target && s->target->kind == EK::Ident) def.insert(s->target->name);
            else collectDef(s->target, def);
            collectUse(s->target && s->target->kind != EK::Ident ? s->target : nullptr, use);
            if (s->target && s->target->kind == EK::Ident) { /* plain store */ }
            collectUse(s->value, use);
            return;
        case SK::CompoundAssign:
            if (s->target && s->target->kind == EK::Ident) { def.insert(s->target->name); use.insert(s->target->name); }
            else { collectDef(s->target, def); collectUse(s->target, use); }
            collectUse(s->value, use);
            return;
        case SK::Expr: collectUse(s->expr, use); return;
        case SK::Throw: collectUse(s->expr, use); return;
        case SK::Return: collectUse(s->expr, use); return;
        case SK::Print: for (auto& a : s->args) collectUse(a, use); collectUse(s->sep, use); return;
        case SK::Input: if (s->target && s->target->kind == EK::Ident) def.insert(s->target->name);
                        collectDef(s->target, def); return;
        case SK::Del: for (auto& n : s->names) def.insert(n); return;
        case SK::If: collectUse(s->cond, use); break;
        case SK::While: collectUse(s->cond, use); break;
        case SK::For: collectUse(s->iterable, use); for (auto& v : s->loopVars) def.insert(v); break;
        default: break;
    }
    if (s->body) for (auto& x : s->body->stmts) stmtUseDef(x.get(), use, def);
    if (s->elseBody) for (auto& x : s->elseBody->stmts) stmtUseDef(x.get(), use, def);
    if (s->body && s->body->exceptBody) {
        if (!s->body->exceptName.empty()) def.insert(s->body->exceptName);
        for (auto& x : s->body->exceptBody->stmts) stmtUseDef(x.get(), use, def);
    }
}

// uses/defs of this statement alone: nested statements have their own CFG nodes, so their
// definitions must not kill liveness at the parent node
void stmtDirectUseDef(const Stmt* s, std::set<std::string>& use, std::set<std::string>& def) {
    if (!s) return;
    switch (s->kind) {
        case SK::New: case SK::Const:
            for (auto& n : s->names) def.insert(n);
            collectUse(s->initExpr, use);
            return;
        case SK::State:
            def.insert(s->name);
            collectUse(s->initExpr, use);
            return;
        case SK::Assign:
            if (s->target && s->target->kind == EK::Ident) def.insert(s->target->name);
            else { collectDef(s->target, def); collectUse(s->target, use); }
            collectUse(s->value, use);
            return;
        case SK::CompoundAssign:
            if (s->target && s->target->kind == EK::Ident) {
                def.insert(s->target->name);
                use.insert(s->target->name);
            } else {
                collectDef(s->target, def);
                collectUse(s->target, use);
            }
            collectUse(s->value, use);
            return;
        case SK::Expr: collectUse(s->expr, use); return;
        case SK::Throw: collectUse(s->expr, use); return;
        case SK::Return: collectUse(s->expr, use); return;
        case SK::Print:
            for (auto& a : s->args) collectUse(a, use);
            collectUse(s->sep, use);
            return;
        case SK::Input:
            if (s->target && s->target->kind == EK::Ident) def.insert(s->target->name);
            collectDef(s->target, def);
            return;
        case SK::Del: for (auto& n : s->names) def.insert(n); return;
        case SK::If: collectUse(s->cond, use); return;
        case SK::While: collectUse(s->cond, use); return;
        case SK::For:
            collectUse(s->iterable, use);
            for (auto& v : s->loopVars) def.insert(v);
            return;
        default:
            return;
    }
}

ExprP makeIdent(const std::string& name, int line) {
    ExprP e = std::make_shared<Expr>();
    e->kind = EK::Ident;
    e->name = name;
    e->line = line;
    return e;
}

// the names a nested body reads but does not declare itself (its closure)
void freeNames(const BlockP& body, std::set<std::string>& out) {
    if (!body) return;
    std::set<std::string> used, declared;
    for (auto& st : body->stmts) {
        std::set<std::string> u, d;
        stmtUseDef(st.get(), u, d);
        used.insert(u.begin(), u.end());
        declared.insert(d.begin(), d.end());
    }
    for (auto& n : used)
        if (!declared.count(n)) out.insert(n);
}

std::string discoverText(const ExprP& cond, Env& env) {
    std::set<std::string> names;
    collectUse(cond, names);
    std::string out;
    for (auto& n : names) {
        AbsVal* v = env.find(n);
        if (!v) continue;
        if (!out.empty()) out += "; ";
        out += n + " ∈ " + (v->range.known ? rangeText(v->range) : std::string("unknown"));
    }
    return out;
}

// ================================================================ function info
struct FuncInfo {
    std::string name, cls, qual;
    Stmt* def = nullptr;
    std::vector<Param> params;
    std::vector<Annotation> anns;
    bool pure = false, trusted = false, noreturn = false, unsafe = false;
    bool isMethod = false, nested = false;
    std::vector<ExprP> requireExprs, ensureExprs, decreaseExprs;
    std::vector<std::string> requireTexts, ensureTexts, modifies;
    int errors = 0, warnings = 0;
    bool analyzed = false;
    AbsVal returnVal;
    bool returnTainted = false;
};

// ================================================================ the analyzer
class Flow {
public:
    Flow(const Program& prog, const std::string& file, const AnalysisOptions& opts,
         AnalysisResult& res, Collector& col)
        : prog_(prog), file_(file), opts_(opts), res_(res), col_(col) {
        if (opts.intBits == 64) { intLo_ = INT64_MIN; intHi_ = INT64_MAX; }
    }
    void run();

private:
    const Program& prog_;
    std::string file_;
    AnalysisOptions opts_;
    AnalysisResult& res_;
    Collector& col_;
    long long intLo_ = -2147483648LL, intHi_ = 2147483647LL;

    std::vector<FuncInfo*> funcs_;
    std::map<std::string, FuncInfo*> byName_;
    std::map<std::string, std::map<std::string, std::string>> classFields_;
    std::set<std::string> globals_;
    std::set<std::string> builtinNames_;
    std::set<std::string> outOfScope_;
    std::vector<std::string> loopStack_;
    std::map<std::string, std::string> indexBound_;   // loop variable -> proven upper bound

    FuncInfo* cur_ = nullptr;
    Env* env_ = nullptr;
    Scope globalsEnv_;                  // abstract state of file level variables
    int loopDepth_ = 0;
    int suppressDead_ = 0;              // > 0 while re-analysing a loop body
    std::set<std::string> deadReported_;   // avoid reporting the same dead store twice
    std::set<int> unreachableReported_;
    std::map<int, std::vector<Annotation>> ignoreByLine_;   // [[ignore]] sites by line
    bool cancelled_ = false;
    int steps_ = 0;
    long long startClock_ = 0;
    int loopsSeen_ = 0, loopsWithInvariant_ = 0;
    StmtP mainHolder_;

    bool budgetExceeded();

    // mutes the collector for the lifetime of a statement that came from a `use`d module
    struct MuteGuard {
        Collector& c;
        bool on;
        MuteGuard(Collector& cc, bool o) : c(cc), on(o) {
            if (on) c.muteDepth++;
        }
        ~MuteGuard() {
            if (on) c.muteDepth--;
        }
    };

    bool skip(const StmtP& s) const {
        return !opts_.includeModules && !s->origin.empty() && s->origin != file_;
    }

    // expression evaluation
    AbsVal eval(const ExprP& e, Env& env, bool iterablePosition = false);
    AbsVal evalBinary(const ExprP& e, Env& env);
    AbsVal evalCall(const ExprP& e, Env& env);
    int triCondOf(const ExprP& e, Env& env);
    void narrow(Env& env, const ExprP& cond, bool truth);
    void narrowMap(std::map<std::string, AbsVal>& m, const ExprP& cond, bool truth);
    std::string describeCond(const ExprP& cond, Env& env);

    // statements
    void block(const BlockP& b, Env& env, bool newScope);
    void stmt(const StmtP& s, Env& env);
    void noteState(int line, Env& env);
    // reports emitted after the statement itself (unused variable, dead store) still honour
    // the [[ignore]] annotation that was written in front of the declaration
    void withSuppression(int line, const std::function<void()>& emit);
    void assignTo(const ExprP& target, const AbsVal& v, Env& env, int line, bool declare,
                  const std::string& declType);
    void checkDeclaredType(const std::string& declared, const AbsVal& v, int line, const std::string& what);
    void checkIndex(const ExprP& baseE, const ExprP& idxE, const AbsVal& base, const AbsVal& idx, int line);

    // loops / functions
    void analyzeLoop(const StmtP& s, Env& env, bool isFor);
    void collectStmts(const std::vector<StmtP>& stmts, const std::string& cls, bool nested);
    void analyzeFunc(FuncInfo* f);
    void checkReachability(FuncInfo* f);
    void checkDeadStores(FuncInfo* f);
    std::map<std::string, Lin> subst_;   // CAS equalities
    std::map<std::string, Lin> loBnd_, hiBnd_;   // CAS linear lower/upper bounds
    // Fold the linear bounds that turn out to be constants back into the abstract ranges, so the
    // overflow/index checks see them too (`[[assume: n <= 1000 && i < n]]` gives i <= 999).
    void applyConstBounds(Env& env) {
        for (auto& kv : loBnd_) {
            auto hi = hiBnd_.find(kv.first);
            if (hi == hiBnd_.end()) continue;
            CasCtx c;
            c.lo = &loBnd_;
            c.hi = &hiBnd_;
            c.range = [&](const std::string& n, long long& l, long long& h) {
                AbsVal* v = env.find(n);
                if (!v || !v->range.known) return false;
                l = v->range.lo;
                h = v->range.hi;
                return true;
            };
            Lin lo, hiV, dummy;
            if (!linInterval(kv.second, c, lo, dummy)) continue;
            if (!linInterval(hi->second, c, dummy, hiV)) continue;
            bool haveLo = lo.t.empty(), haveHi = hiV.t.empty();
            if (!haveLo && !haveHi) continue;
            AbsVal* v = env.find(kv.first);
            if (!v) continue;
            long long nlo = v->range.known ? v->range.lo : (long long)(-(1LL << 62));
            long long nhi = v->range.known ? v->range.hi : (long long)(1LL << 62);
            if (haveLo) nlo = std::max<long long>(nlo, lo.c);
            if (haveHi) nhi = std::min<long long>(nhi, hiV.c);
            if (nlo > nhi) continue;
            v->range = rng(nlo, nhi);
        }
    }
    void macroChecks();
    void collectGlobals(const std::vector<StmtP>& stmts);
    void collectClassFields(const StmtP& cls);
};

bool Flow::budgetExceeded() {
    if (cancelled_) return true;
    if (++steps_ % 128 == 0) {
        if (opts_.isCancelled && opts_.isCancelled()) { cancelled_ = true; return true; }
        if (opts_.budgetMs > 0) {
            long long ms = (clock() - startClock_) * 1000LL / CLOCKS_PER_SEC;
            if (ms > opts_.budgetMs) { cancelled_ = true; return true; }
        }
    }
    return false;
}

void Flow::withSuppression(int line, const std::function<void()>& emit) {
    auto it = ignoreByLine_.find(line);
    size_t mark = col_.active.size();
    if (it != ignoreByLine_.end())
        for (auto& a : it->second) col_.pushIgnore(a, line, line);
    emit();
    col_.popTo(mark);
}

void Flow::noteState(int line, Env& env) {
    if (!opts_.collectIde) return;
    if (res_.lineState.size() > 20000) return;
    auto& m = res_.lineState[line];
    for (auto& sc : env.scopes)
        for (auto& kv : sc.vars) {
            if (kv.first == "this") continue;
            m[kv.first] = stateText(kv.second);
        }
}

// ---------------------------------------------------------------- conditions

// Compact source-like text of an expression, used to name branches and conditions in messages.
static std::string exprText(const ExprP& e, int depth = 0) {
    if (!e || depth > 8) return "...";
    switch (e->kind) {
        case EK::Int:    return e->wideLiteral ? e->sval : std::to_string(e->ival);
        case EK::Float:  { std::string t = std::to_string(e->fval);
                           while (t.size() > 1 && t.back() == '0') t.pop_back();
                           if (!t.empty() && t.back() == '.') t.pop_back();
                           return t; }
        case EK::Bool:   return e->bval ? "true" : "false";
        case EK::Null:   return "null";
        case EK::Str:    return "\"" + e->sval + "\"";
        case EK::Ident:  return e->name;
        case EK::Unary:  return std::string(e->op == T::Not ? "!" : "-") + exprText(e->a, depth + 1);
        case EK::Binary:
        case EK::Logical:
            return exprText(e->a, depth + 1) + " " + opNameOf(e->op) + " " +
                   exprText(e->b, depth + 1);
        case EK::Index:  return exprText(e->a, depth + 1) + "[" + exprText(e->b, depth + 1) + "]";
        case EK::Field:  return exprText(e->a, depth + 1) + "." + e->name;
        case EK::Call:   {
            std::string t = exprText(e->a, depth + 1) + "(";
            for (size_t i = 0; i < e->args.size(); i++) {
                if (i) t += ", ";
                t += exprText(e->args[i].value, depth + 1);
            }
            return t + ")";
        }
        default: return "...";
    }
}

// normalise to a linear form (exact, or ok == false)
static Lin linOfExpr(const ExprP& e, const std::map<std::string, Lin>& subst, int depth = 0) {
    Lin bad; bad.ok = false;
    if (!e || depth > 24) return bad;
    switch (e->kind) {
        case EK::Int: return linConst(e->ival);
        case EK::Ident: {
            auto it = subst.find(e->name);
            return it != subst.end() ? it->second : linVar(e->name);
        }
        case EK::Unary: {
            Lin a = linOfExpr(e->a, subst, depth + 1);
            if (!a.ok) return bad;
            if (e->op == T::Minus) return linScale(a, -1);
            if (e->op == T::Plus) return a;
            return bad;
        }
        case EK::Binary: {
            Lin a = linOfExpr(e->a, subst, depth + 1);
            Lin b = linOfExpr(e->b, subst, depth + 1);
            if (!a.ok || !b.ok) return bad;
            switch (e->op) {
                case T::Plus:  return linAdd(a, b, 1);
                case T::Minus: return linAdd(a, b, -1);
                case T::Star:
                    if (a.t.empty()) return linScale(b, a.c);
                    if (b.t.empty()) return linScale(a, b.c);
                    return bad;
                case T::Slash: {
                    if (!b.t.empty() || b.c == 0) return bad;
                    long long k = b.c;
                    Lin r = a;
                    if (r.c % k != 0) return bad;
                    r.c /= k;
                    for (auto& kv : r.t) {
                        if (kv.second % k != 0) return bad;
                        kv.second /= k;
                    }
                    for (auto it = r.t.begin(); it != r.t.end();)
                        it = it->second == 0 ? r.t.erase(it) : std::next(it);
                    return r;
                }
                default: return bad;
            }
        }
        default: return bad;
    }
}


// interval [loOut, hiOut] of a linear form; false when some variable is unbounded

// canonical keys of a comparison, used to recognise `A || !A` and `A && !A`.
// Every form is turned into a predicate on the *same* polynomial (rhs - lhs), so `k < n` and
// `!(k < n)` end up with one identical polynomial key and complementary codes.
static std::string linKey(const Lin& l) {
    if (!l.ok) return "?";
    std::string s = "@" + std::to_string(l.c);
    for (auto& kv : l.t) s += "|" + kv.first + "*" + std::to_string(kv.second);
    return s;
}
static std::string complementCode(const std::string& c) {
    if (c == "gt0") return "le0";
    if (c == "le0") return "gt0";
    if (c == "lt0") return "ge0";
    if (c == "ge0") return "lt0";
    if (c == "eq0") return "ne0";
    if (c == "ne0") return "eq0";
    return "";
}
static bool cmpCanon(const ExprP& e, std::string& code, std::string& poly) {
    if (!e) return false;
    if (e->kind == EK::Unary && e->op == T::Not) {
        std::string c2, p2;
        if (!cmpCanon(e->a, c2, p2)) return false;
        code = complementCode(c2);
        poly = p2;
        return !code.empty();
    }
    if (e->kind != EK::Binary) return false;
    T op = e->op;
    if (op != T::Lt && op != T::Le && op != T::Gt && op != T::Ge && op != T::Eq && op != T::Ne)
        return false;
    Lin a = linOfExpr(e->a, std::map<std::string, Lin>{});
    Lin b = linOfExpr(e->b, std::map<std::string, Lin>{});
    Lin d = linAdd(b, a, -1);                 // rhs - lhs
    if (!d.ok) return false;
    switch (op) {
        case T::Lt: code = "gt0"; break;
        case T::Le: code = "ge0"; break;
        case T::Gt: code = "lt0"; break;
        case T::Ge: code = "le0"; break;
        case T::Eq: code = "eq0"; break;
        default:    code = "ne0"; break;
    }
    poly = linKey(d);
    return true;
}
static T negatedOp(T op) {
    switch (op) {
        case T::Lt: return T::Ge;
        case T::Le: return T::Gt;
        case T::Gt: return T::Le;
        case T::Ge: return T::Lt;
        case T::Eq: return T::Ne;
        case T::Ne: return T::Eq;
        default: return op;
    }
}

static int casCompareIntervals(const ExprP& e, const CasCtx& ctx, const Lin& a,
                               const Lin& b, int depth);

static int casTriCtx(const ExprP& e, const CasCtx& ctx, int depth = 0) {
    if (!e || depth > 16) return -1;
    if (e->kind == EK::Logical && (e->op == T::AndAnd || e->op == T::OrOr)) {
        int a = casTriCtx(e->a, ctx, depth + 1), b = casTriCtx(e->b, ctx, depth + 1);
        // A && !A  /  A || !A  (boolean tautology and contradiction)
        std::string ca, pa, cb, pb;
        if (cmpCanon(e->a, ca, pa) && cmpCanon(e->b, cb, pb) && pa == pb &&
            !ca.empty() && !cb.empty() && complementCode(ca) == cb) {
            if (e->op == T::OrOr) return 1;
            if (e->op == T::AndAnd) return 0;
        }
        if (e->op == T::AndAnd) {
            if (a == 0 || b == 0) return 0;
            return (a == 1 && b == 1) ? 1 : -1;
        }
        if (a == 1 || b == 1) return 1;
        return (a == 0 && b == 0) ? 0 : -1;
    }
    if (e->kind == EK::Unary && e->op == T::Not) {
        int a = casTriCtx(e->a, ctx, depth + 1);
        return a < 0 ? -1 : 1 - a;
    }
    if (e->kind != EK::Binary) return -1;
    bool cmp = e->op == T::Eq || e->op == T::Ne || e->op == T::Lt || e->op == T::Gt ||
               e->op == T::Le || e->op == T::Ge;
    if (!cmp) return -1;
    Lin a = linOfExpr(e->a, ctx.subst ? *ctx.subst : std::map<std::string, Lin>{}, depth + 1);
    Lin b = linOfExpr(e->b, ctx.subst ? *ctx.subst : std::map<std::string, Lin>{}, depth + 1);
    if (!a.ok || !b.ok) return -1;
    Lin d = linAdd(a, b, -1);
    int sign = linConstSign(d);
    if (sign != 2) {                                  // decided without any interval work
        switch (e->op) {
            case T::Eq: return sign == 0 ? 1 : 0;
            case T::Ne: return sign != 0 ? 1 : 0;
            case T::Lt: return sign < 0 ? 1 : 0;
            case T::Gt: return sign > 0 ? 1 : 0;
            case T::Le: return sign <= 0 ? 1 : 0;
            case T::Ge: return sign >= 0 ? 1 : 0;
            default: return -1;
        }
    }
    // Interval reasoning in two passes: first with the symbolic bounds only (so identical
    // variables cancel, giving `i + 1 <= n` from `i <= n - 1`), then with the numeric ranges
    // mixed in (giving `i <= 999` once `n <= 1000` is known).
    CasCtx sym = ctx;
    sym.range = nullptr;                       // symbolic pass: identical variables cancel
    int r = casCompareIntervals(e, sym, a, b, depth);
    if (r >= 0) return r;
    if (ctx.range) {                           // numeric pass: constant bounds from the ranges
        r = casCompareIntervals(e, ctx, a, b, depth);
        if (r >= 0) return r;
    }
    return -1;
}

// the interval half of casTriCtx: -1 unknown, 0 false, 1 true
static int casCompareIntervals(const ExprP& e, const CasCtx& ctx, const Lin& a, const Lin& b,
                               int depth) {
    (void)depth;
    Lin aLo, aHi, bLo, bHi;
    if (!linInterval(a, ctx, aLo, aHi) || !linInterval(b, ctx, bLo, bHi)) return -1;
    int loGap = linConstSign(linAdd(aHi, bLo, -1));    // aHi - bLo, 2 when still symbolic
    int hiGap = linConstSign(linAdd(aLo, bHi, -1));    // aLo - bHi
    auto neg = [](int g) { return g != 2 && g < 0; };  // gap < 0
    auto zero = [](int g) { return g == 0; };
    auto pos = [](int g) { return g != 2 && g > 0; };  // gap > 0
    auto notPos = [](int g) { return g != 2 && g <= 0; };
    auto notNeg = [](int g) { return g != 2 && g >= 0; };
    switch (e->op) {
        case T::Lt:
            if (neg(loGap)) return 1;                  // aHi < bLo
            if (notNeg(hiGap)) return 0;               // aLo >= bHi
            return -1;
        case T::Le:
            if (notPos(loGap)) return 1;
            if (pos(hiGap)) return 0;
            return -1;
        case T::Gt:
            if (pos(hiGap)) return 1;
            if (notPos(loGap)) return 0;
            return -1;
        case T::Ge:
            if (notNeg(hiGap)) return 1;
            if (neg(loGap)) return 0;
            return -1;
        case T::Eq:
            if (zero(loGap) && zero(hiGap)) return 1;
            if (neg(loGap) || pos(hiGap)) return 0;
            return -1;
        case T::Ne:
            if (neg(loGap) || pos(hiGap)) return 1;
            if (zero(loGap) && zero(hiGap)) return 0;
            return -1;
        default: return -1;
    }
}

// Remember what a condition tells us.  `truth == false` learns the *negation*, which is what
// case analysis needs: `if i < 1 ( ... )` gives `i >= 1` on the else path.
static void learnFacts(const ExprP& e, bool truth,
                       std::map<std::string, Lin>& subst,
                       std::map<std::string, Lin>& loBnd,
                       std::map<std::string, Lin>& hiBnd, int depth = 0) {
    if (!e || depth > 12) return;
    if (e->kind == EK::Unary && e->op == T::Not) {
        learnFacts(e->a, !truth, subst, loBnd, hiBnd, depth + 1);
        return;
    }
    if (e->kind == EK::Logical && (e->op == T::AndAnd || e->op == T::OrOr)) {
        bool both = (e->op == T::AndAnd) == truth;      // De Morgan on the negated side
        if (both) {
            learnFacts(e->a, truth, subst, loBnd, hiBnd, depth + 1);
            learnFacts(e->b, truth, subst, loBnd, hiBnd, depth + 1);
        }
        return;
    }
    if (e->kind != EK::Binary) return;
    T op = e->op;
    if (!truth) op = negatedOp(op);
    const ExprP& lhs = e->a;
    const ExprP& rhs = e->b;
    if (!lhs || !rhs) return;
    Lin r = linOfExpr(rhs, subst);
    Lin l = linOfExpr(lhs, subst);
    if (!r.ok || !l.ok) return;
    auto setLo = [&](const std::string& v, const Lin& b) { loBnd[v] = b; };
    auto setHi = [&](const std::string& v, const Lin& b) { hiBnd[v] = b; };
    bool lhsVar = lhs->kind == EK::Ident && !subst.count(lhs->name);
    bool rhsVar = rhs->kind == EK::Ident && !subst.count(rhs->name);
    if (op == T::Eq && lhsVar) { subst[lhs->name] = r; return; }
    if (op == T::Eq && rhsVar) { subst[rhs->name] = l; return; }
    if (op == T::Ne) return;                        // no interval information
    // integer comparison: `x < b` is `x <= b - 1`
    auto minusOne = [](Lin v) { v.c -= 1; return v; };
    auto plusOne = [](Lin v) { v.c += 1; return v; };
    if (lhsVar && (op == T::Lt || op == T::Le || op == T::Gt || op == T::Ge)) {
        const std::string& v = lhs->name;
        if (op == T::Lt) setHi(v, minusOne(r));
        else if (op == T::Le) setHi(v, r);
        else if (op == T::Gt) setLo(v, plusOne(r));
        else setLo(v, r);
        return;
    }
    if (rhsVar && (op == T::Lt || op == T::Le || op == T::Gt || op == T::Ge)) {
        const std::string& v = rhs->name;
        if (op == T::Lt) setLo(v, plusOne(l));       // l < v  ==  v >= l + 1
        else if (op == T::Le) setLo(v, l);
        else if (op == T::Gt) setHi(v, minusOne(l));
        else setHi(v, l);
        return;
    }
}

// ------------------------------------------------ restored definitions
void Flow::assignTo(const ExprP& target, const AbsVal& v, Env& env, int line, bool declare,
                    const std::string& declType) {
    if (!target) return;
    if (target->kind == EK::Ident) {
        const std::string& name = target->name;
        if (declare) {
            AbsVal nv = v;
            nv.declLine = line;
            nv.assignLine = line;
            nv.read = false;
            nv.readSinceAssign = false;
            // at the top level of <main> a `new` declares a global, not a local
            nv.isLocal = !(cur_ && cur_->name == "<main>" && env.scopes.size() == 1);
            if (!declType.empty()) nv.type = declType;
            outOfScope_.erase(name);
            env.declare(name, nv);
            return;
        }
        AbsVal* found = env.find(name);
        if (!found) {
            if (globals_.count(name) || builtinNames_.count(name)) {
                if (cur_ && cur_->pure)
                    col_.error("purity", line, "[[pure]] 函数不得写全局变量 '" + name + "'",
                               "纯函数不允许有副作用", "去掉 [[pure]]，或改为返回值");
                globalsEnv_.vars[name] = v;      // keep the file level state up to date
                return;
            }
            col_.error("uninitialized", line, "给未声明的变量 '" + name + "' 赋值",
                       "文档要求先 new 再赋值", "改为 new " + name + " = ...");
            return;
        }
        if (found->init == AbsVal::NotInit)
            found->init = AbsVal::IsInit;
        if (!declType.empty()) checkDeclaredType(declType, v, line, "赋值给 '" + name + "'");
        *found = v;
        found->assignLine = line;
        found->readSinceAssign = false;
        if (found->declLine == 0) found->declLine = line;
        return;
    }
    if (target->kind == EK::Field) {
        AbsVal base = eval(target->a, env);
        if (base.nullState == AbsVal::IsNull)
            col_.error("null-dereference", line, "对 null 写成员 '" + target->name + "'", "", "先判空");
        else if (base.nullState == AbsVal::MaybeNull)
            col_.warn("null-dereference", line, "可能对 null 写成员 '" + target->name + "'", "", "先判空");
        if (cur_ && cur_->pure)
            col_.error("purity", line, "[[pure]] 函数不得修改字段 '" + target->name + "'",
                       "纯函数不允许有副作用", "去掉 [[pure]]，或改为返回值");
        return;
    }
    if (target->kind == EK::Index) {
        AbsVal base = eval(target->a, env);
        AbsVal idx = eval(target->b, env);
        checkIndex(target->a, target->b, base, idx, line);
        if (base.nullState == AbsVal::IsNull)
            col_.error("null-dereference", line, "对 null 写下标", "", "先判空");
    }
}

void Flow::checkDeclaredType(const std::string& declared, const AbsVal& v, int line, const std::string& what) {
    if (declared.empty() || v.type == "unknown" || v.type == "null") return;
    auto compatible = [&](const std::string& d, const std::string& a) {
        if (d == a) return true;
        if (d == "float" && a == "int") return true;
        if (d == "String" && a == "String") return true;
        if (d == "List" && (a == "List")) return true;
        if (d == "Tuple" && a == "Tuple") return true;
        if (d == "Bytes" && a == "Bytes") return true;
        if (d == "Fn" || d == "Any") return true;
        // a user class name accepts instances of the same class
        return false;
    };
    if (!compatible(declared, v.type))
        col_.error("type-mismatch", line, what + " 类型不匹配",
                   "声明为 " + declared + "，实际是 " + v.type, "改成匹配的类型或去掉类型标注");
}

void Flow::checkIndex(const ExprP& baseE, const ExprP& idxE, const AbsVal& base, const AbsVal& idx, int line) {
    // an index that is the loop variable of `for i in 0 to len(xs)` (or `0 to n` where the
    // container was allocated with n) is proven to be in range
    if (idxE && idxE->kind == EK::Ident) {
        auto it = indexBound_.find(idxE->name);
        if (it != indexBound_.end() && !it->second.empty()) {
            if (!base.sizeKey.empty() && it->second == base.sizeKey) return;
            if (baseE && baseE->kind == EK::Ident && it->second == "len:" + baseE->name) return;
        }
    }
    if (!base.size.known || !idx.range.known) return;
    long long slo = base.size.lo, shi = base.size.hi;
    long long lo = idx.range.lo, hi = idx.range.hi;
    long long minOk = -shi, maxOk = shi - 1;
    if (hi < minOk || lo > maxOk) {
        col_.error("out-of-bounds", line, "数组越界",
                   "索引 ∈ " + rangeText(idx.range) + "，长度 ∈ " + rangeText(base.size) +
                       "，合法范围 [" + formatInt(minOk) + ", " + formatInt(maxOk) + "]",
                   "加 [[assert: i >= 0 && i < xs.size()]]");
    } else if (lo < minOk || hi > maxOk) {
        col_.warn("out-of-bounds", line, "数组索引可能越界",
                  "索引 ∈ " + rangeText(idx.range) + "，长度 ∈ " + rangeText(base.size) +
                      "，合法范围 [" + formatInt(minOk) + ", " + formatInt(maxOk) + "]",
                  "加 [[assert: i >= 0 && i < xs.size()]]");
    }
    (void)slo;
}

std::string Flow::describeCond(const ExprP& cond, Env& env) {
    // print the free variables of the condition with their abstract values
    std::set<std::string> names;
    collectUse(cond, names);
    std::string out;
    for (auto& n : names) {
        AbsVal* v = env.find(n);
        if (!v) continue;
        if (!out.empty()) out += "; ";
        out += n + " ∈ " + (v->range.known ? rangeText(v->range) : std::string("unknown"));
    }
    return out;
}

AbsVal Flow::eval(const ExprP& e, Env& env, bool iterablePosition) {
    AbsVal v;
    if (!e) return v;
    if (budgetExceeded()) return v;
    switch (e->kind) {
        case EK::Int:
            // a literal that did not fit in 64 bits is a 128 bit value: no 32 bit range at all
            v.type = e->wideLiteral ? "longlong" : "int";
            if (!e->wideLiteral) v.range = rng(e->ival, e->ival);
            v.init = AbsVal::IsInit;
            v.nullState = AbsVal::NonNull;
            return v;
        case EK::Float:
            v.type = "float";
            v.init = AbsVal::IsInit;
            v.nullState = AbsVal::NonNull;
            return v;
        case EK::Str:
            v.type = "String";
            v.size = rng((long long)e->sval.size(), (long long)e->sval.size());
            v.init = AbsVal::IsInit;
            v.nullState = AbsVal::NonNull;
            return v;
        case EK::Bool:
            v.type = "bool";
            v.range = rng(e->bval ? 1 : 0, e->bval ? 1 : 0);
            v.init = AbsVal::IsInit;
            v.nullState = AbsVal::NonNull;
            return v;
        case EK::Color:
            v.type = "Color";
            v.init = AbsVal::IsInit;
            return v;
        case EK::Null:
            v.type = "null";
            v.nullState = AbsVal::IsNull;
            v.init = AbsVal::IsInit;
            return v;
        case EK::This:
            v.type = (cur_ && !cur_->cls.empty()) ? cur_->cls : "instance";
            v.nullState = AbsVal::NonNull;
            v.init = AbsVal::IsInit;
            return v;
        case EK::Super: case EK::Children:
            v.type = "List";
            v.nullState = AbsVal::NonNull;
            v.init = AbsVal::IsInit;
            return v;
        case EK::Ident: {
            AbsVal* found = env.find(e->name);
            if (found) {
                if (found->init == AbsVal::NotInit)
                    col_.error("uninitialized", e->line, "读取未初始化的变量 '" + e->name + "'",
                               "声明时没有给出初始值", "在声明处赋值，例如 new " + e->name + " = ...");
                found->read = true;
                found->readSinceAssign = true;
                return *found;
            }
            if (globals_.count(e->name) || builtinNames_.count(e->name)) {
                auto g = globalsEnv_.vars.find(e->name);
                if (g != globalsEnv_.vars.end()) {
                    g->second.read = true;
                    return g->second;
                }
                v.type = builtinNames_.count(e->name) ? "Fn" : "unknown";
                v.init = AbsVal::IsInit;
                v.nullState = AbsVal::NonNull;
                return v;
            }
            if (outOfScope_.count(e->name)) {
                col_.error("scope", e->line, "变量 '" + e->name + "' 已离开作用域",
                           "它在裸作用域 ( ... ) 内用 new 声明，出块后不再可见",
                           "在块外重新声明，或把结果赋给块外的变量");
            } else {
                col_.error("uninitialized", e->line, "使用了未声明的变量 '" + e->name + "'",
                           "既不是局部变量，也不是已定义的全局/函数/内置", "先用 new 声明");
            }
            v.init = AbsVal::IsInit;
            return v;
        }
        case EK::List: case EK::Tuple: {
            v.type = e->kind == EK::List ? "List" : "Tuple";
            v.size = rng((long long)e->items.size(), (long long)e->items.size());
            v.nullState = AbsVal::NonNull;
            v.init = AbsVal::IsInit;
            for (auto& i : e->items) {
                AbsVal iv = eval(i, env);
                if (iv.tainted) { v.tainted = true; v.taintSource = iv.taintSource; }
            }
            return v;
        }
        case EK::Unary: {
            AbsVal a = eval(e->a, env);
            v = a;
            if (e->op == T::Not) { v.type = "bool"; v.range = Range(); return v; }
            if (e->op == T::Minus) {
                if (a.range.known && a.range.lo != INT64_MIN) v.range = rng(-a.range.hi, -a.range.lo);
                else v.range = Range();
                if (v.range.known && (v.range.lo < intLo_ || v.range.hi > intHi_))
                    col_.warn("overflow", e->line, "取负可能溢出",
                              "操作数 ∈ " + rangeText(a.range), "加 [[assert: ...]] 限制范围");
            }
            if (e->op == T::Tilde && a.type != "int" && a.type != "bool" && a.type != "unknown")
                col_.error("type-mismatch", e->line, "按位取反要求整数，实际是 " + a.type, "", "改用整数");
            return v;
        }
        case EK::Binary: return evalBinary(e, env);
        case EK::Logical: {
            eval(e->a, env);
            eval(e->b, env);
            v.type = "bool";
            return v;
        }
        case EK::Call: return evalCall(e, env);
        case EK::Index: {
            AbsVal base = eval(e->a, env);
            AbsVal idx = eval(e->b, env);
            checkIndex(e->a, e->b, base, idx, e->line);
            v.tainted = base.tainted;
            v.taintSource = base.taintSource;
            if (base.type == "String") v.type = "String";
            return v;
        }
        case EK::Field: {
            AbsVal base = eval(e->a, env, false);
            if (base.nullState == AbsVal::IsNull)
                col_.error("null-dereference", e->line, "对 null 取成员 '" + e->name + "'",
                           "接收者的空值状态为 null", "先判空：if x != null( ... )");
            else if (base.nullState == AbsVal::MaybeNull)
                col_.warn("null-dereference", e->line, "可能对 null 取成员 '" + e->name + "'",
                          "接收者的空值状态为 maybe-null", "加 [[assert: x != null]] 或先判空");
            v.tainted = base.tainted;
            v.taintSource = base.taintSource;
            auto cf = classFields_.find(base.type);
            if (cf != classFields_.end()) {
                auto f = cf->second.find(e->name);
                if (f != cf->second.end() && !f->second.empty()) v.type = f->second;
            }
            return v;
        }
        case EK::Piecewise: {
            AbsVal acc;
            bool first = true;
            for (auto& c : e->cases) {
                eval(c.first, env);
                AbsVal cv = eval(c.second, env);
                acc = first ? cv : mergeVal(acc, cv);
                first = false;
            }
            if (e->elseVal) {
                AbsVal cv = eval(e->elseVal, env);
                acc = first ? cv : mergeVal(acc, cv);
            }
            return acc;
        }
        case EK::Lambda: {
            v.type = "Fn";
            v.nullState = AbsVal::NonNull;
            v.init = AbsVal::IsInit;
            // captured variables count as reads of the enclosing scope
            std::set<std::string> names;
            if (e->bodyExpr) collectUse(e->bodyExpr, names);
            if (e->body) for (auto& st : e->body->stmts) { std::set<std::string> u, d; stmtUseDef(st.get(), u, d); names.insert(u.begin(), u.end()); }
            for (auto& n : names) {
                AbsVal* f = env.find(n);
                if (f) { f->read = true; f->readSinceAssign = true; }
            }
            return v;
        }
        case EK::Iter: {
            AbsVal a = eval(e->a, env, true);
            AbsVal b = eval(e->b, env, true);
            AbsVal st;
            if (e->bodyExpr) st = eval(e->bodyExpr, env, true);
            else st.range = rng(1, 1);
            v.type = "Iter";
            v.isIter = true;
            v.nullState = AbsVal::NonNull;
            v.init = AbsVal::IsInit;
            if (a.range.known && b.range.known)
                v.range = rng(std::min(a.range.lo, b.range.lo), std::max(a.range.hi, b.range.hi));
            if (!iterablePosition)
                col_.error("iterator-misuse", e->line, "迭代器 (1 to 10) 不能当作普通值使用",
                           "迭代器只用于 for 遍历或作为可迭代对象传参",
                           "用 for 遍历，或先转成列表");
            return v;
        }
    }
    return v;
}

AbsVal Flow::evalBinary(const ExprP& e, Env& env) {
    AbsVal a = eval(e->a, env);
    AbsVal b = eval(e->b, env);
    AbsVal v;
    v.tainted = a.tainted || b.tainted;
    v.taintSource = a.tainted ? a.taintSource : b.taintSource;
    v.nullState = AbsVal::NonNull;
    v.init = AbsVal::IsInit;
    int line = e->line;
    auto num = [](const AbsVal& x) {
        return numFamilyOf(x.type) != 0 || x.type == "bool" || x.type == "unknown";
    };
    switch (e->op) {
        case T::Plus: case T::Minus: case T::Star: case T::Slash: case T::Percent: case T::StarStar: {
            // string / list concatenation is legal for '+'
            if (e->op == T::Plus && (a.type == "String" || b.type == "String")) {
                v.type = "String";
                if (a.size.known && b.size.known) v.size = rng(a.size.lo + b.size.lo, a.size.hi + b.size.hi);
                return v;
            }
            if (a.isIter || b.isIter) {
                col_.error("iterator-misuse", line, "迭代器不能参与算术运算", "", "改用 for 遍历");
                return v;
            }
            // unknown operands (un-analysable expressions, values from modules) are never reported
            bool unknownSide = !num(a) && a.type != "unknown" && !num(b) && b.type != "unknown";
            if (e->op == T::Plus && (a.type == "List" || b.type == "Tuple" || a.type == "unknown" || b.type == "unknown") &&
                (b.type == "List" || b.type == "Tuple" || b.type == "unknown" || a.type == "unknown")) {
                // list concatenation
                if (a.type == "List" || b.type == "List" || a.type == "Tuple" || b.type == "Tuple") {
                    v.type = a.type == "unknown" ? b.type : a.type;
                    if (a.size.known && b.size.known) v.size = rng(a.size.lo + b.size.lo, a.size.hi + b.size.hi);
                    return v;
                }
            }
            if (unknownSide && (a.type == "unknown" || b.type == "unknown")) return v;
            if (!num(a) || !num(b)) {
                col_.error("type-mismatch", line,
                           std::string("运算符 '") + opNameOf(e->op) + "' 不支持 " + a.type + " 与 " + b.type,
                           "两侧类型不可运算", "检查表达式或显式转换");
                return v;
            }
            if (e->op == T::Slash || e->op == T::Percent) {
                if (b.range.known) {
                    if (b.range.lo == 0 && b.range.hi == 0)
                        col_.error("division-by-zero", line, e->op == T::Slash ? "除零" : "对零取模",
                                   "除数恒为 0",
                                   "加 [[assert: b != 0]] 或 [[require: b != 0]]");
                    else if (b.range.lo <= 0 && b.range.hi >= 0)
                        col_.warn("division-by-zero", line,
                                  std::string(e->op == T::Slash ? "除数" : "模数") + "可能为 0",
                                  "除数 ∈ " + rangeText(b.range) + "，包含 0",
                                  "加 [[assert: b != 0]] 或 [[require: b != 0]]");
                }
            }
            if (e->op == T::StarStar) { v.type = "float"; return v; }
            if (a.type == "float" || b.type == "float") { v.type = "float"; return v; }
            v.type = "int";
            if (e->op == T::Slash) { v.type = "float"; return v; }
            if (e->op == T::Percent) {
                if (a.range.known && b.range.known) {
                    long long m = std::max(std::llabs(b.range.lo), std::llabs(b.range.hi));
                    v.range = rng(-(m - 1), m - 1);
                }
                return v;
            }
            if (!a.range.known || !b.range.known) return v;
            W al = a.range.lo, ah = a.range.hi, bl = b.range.lo, bh = b.range.hi;
            W lo = 0, hi = 0;
            if (e->op == T::Plus) { lo = clampW(al + bl); hi = clampW(ah + bh); }
            else if (e->op == T::Minus) { lo = clampW(al - bh); hi = clampW(ah - bl); }
            else {
                W c1 = clampW(al * bl), c2 = clampW(al * bh), c3 = clampW(ah * bl), c4 = clampW(ah * bh);
                lo = std::min(std::min(c1, c2), std::min(c3, c4));
                hi = std::max(std::max(c1, c2), std::max(c3, c4));
            }
            v.range = rng((long long)lo, (long long)hi);
            bool boxedOperand = isBoxedNumName(a.type) || isBoxedNumName(b.type);
            if (!boxedOperand && (lo < (W)intLo_ || hi > (W)intHi_)) {
                std::string detail = "左 " + rangeText(a.range) + " " + opNameOf(e->op) + " 右 " +
                                     rangeText(b.range) + " = " + rangeText(v.range) +
                                     "，int 范围 [" + formatInt(intLo_) + ", " + formatInt(intHi_) + "]";
                if (hi > (W)intHi_ && lo > (W)intHi_)
                    col_.error("overflow", line, "整数溢出（上溢）", detail, "改用 float 或加 [[assert: ...]]");
                else if (lo < (W)intLo_ && hi < (W)intLo_)
                    col_.error("overflow", line, "整数溢出（下溢）", detail, "改用 float 或加 [[assert: ...]]");
                else
                    col_.warn("overflow", line, "整数可能溢出", detail, "加 [[assert: ...]] 限制范围");
            }
            return v;
        }
        case T::Amp: case T::Pipe: case T::Caret: case T::Shl: case T::Shr: {
            v.type = "int";
            if ((a.type != "int" && a.type != "bool" && a.type != "unknown") ||
                (b.type != "int" && b.type != "bool" && b.type != "unknown"))
                col_.error("type-mismatch", line, std::string("位运算 '") + opNameOf(e->op) + "' 要求整数",
                           a.type + " 与 " + b.type, "改用整数");
            return v;
        }
        case T::Eq: case T::Ne: {
            v.type = "bool";
            bool aNull = a.type == "null" || a.nullState == AbsVal::IsNull;
            bool bNull = b.type == "null" || b.nullState == AbsVal::IsNull;
            if (aNull || bNull) {
                const AbsVal& x = aNull ? b : a;
                if (x.nullState == AbsVal::IsNull) return v;
                if (x.nullState == AbsVal::NonNull) return v;
            } else if (a.type != "unknown" && b.type != "unknown" && a.type != b.type &&
                       !(a.type == "int" && b.type == "float") && !(a.type == "float" && b.type == "int"))
                col_.warn("type-mismatch", line, "比较不同类型的值：" + a.type + " 与 " + b.type,
                          "该比较恒为 " + std::string(e->op == T::Eq ? "false" : "true"), "确认是有意为之");
            return v;
        }
        case T::Lt: case T::Gt: case T::Le: case T::Ge: {
            v.type = "bool";
            if (a.type != "unknown" && b.type != "unknown" && !(num(a) && num(b)) &&
                !(a.type == "String" && b.type == "String"))
                col_.warn("type-mismatch", line, "比较运算不支持 " + a.type + " 与 " + b.type, "", "");
            return v;
        }
        default:
            return v;
    }
}

AbsVal Flow::evalCall(const ExprP& e, Env& env) {
    std::vector<AbsVal> args;
    for (auto& a : e->args) args.push_back(eval(a.value, env));
    if (e->children) block(e->children, env, true);
    AbsVal v;
    v.nullState = AbsVal::NonNull;
    v.init = AbsVal::IsInit;
    for (auto& a : args) if (a.tainted) { v.tainted = true; v.taintSource = a.taintSource; }
    if (e->a && e->a->kind == EK::Field) {
        const std::string& m = e->a->name;
        // mutating container methods invalidate the size information of the receiver
        static const std::set<std::string> mutators = {
            "push", "append", "add", "pop", "remove", "clear", "set", "sort", "insert",
            "extend", "reverse", "setdefault", "delete", "update", "fill"};
        AbsVal* recvPtr = nullptr;
        if (e->a->a && e->a->a->kind == EK::Ident) {
            recvPtr = env.find(e->a->a->name);
            // taking the receiver's slot directly must still count as a read, otherwise
            // `x.method()` looks like x is never used
            if (recvPtr) {
                if (recvPtr->init == AbsVal::NotInit)
                    col_.error("uninitialized", e->line,
                               "读取未初始化的变量 '" + e->a->a->name + "'", "声明时没有给出初始值",
                               "在声明处赋值，例如 new " + e->a->a->name + " = ...");
                recvPtr->read = true;
                recvPtr->readSinceAssign = true;
            }
        }
        AbsVal recv = recvPtr ? *recvPtr : eval(e->a->a, env);
        if (recv.nullState == AbsVal::IsNull)
            col_.error("null-dereference", e->line, "对 null 调用方法 '" + m + "'", "接收者为 null", "先判空");
        else if (recv.nullState == AbsVal::MaybeNull)
            col_.warn("null-dereference", e->line, "可能对 null 调用方法 '" + m + "'",
                      "接收者可能是 null", "加 [[assert: x != null]]");
        if (recv.tainted) { v.tainted = true; v.taintSource = recv.taintSource; }
        if (mutators.count(m)) {
            if (recvPtr) {
                recvPtr->size = Range();
                recvPtr->sizeKey.clear();
                recvPtr->read = true;
            }
            if (m == "pop" || m == "remove") {
                v.type = "unknown";
                return v;
            }
        }
        if (m == "size" || m == "len" || m == "count") {
            v.type = "int";
            if (recv.size.known) v.range = recv.size;
            return v;
        }
        if ((m == "get" || m == "at") && !args.empty() && !e->args.empty()) {
            checkIndex(e->args[0].value, e->args[0].value, recv, args[0], e->line);
            if (recv.type == "String") v.type = "String";
            return v;
        }
        return v;
    }
    std::string callee;
    if (e->a && e->a->kind == EK::Ident) {
        callee = e->a->name;
        // `f(x)` where f is a parameter or a local (a lambda / function value) is a *read* of f:
        // without this, every higher order function looked like it never used its callback
        if (AbsVal* fn = env.find(callee)) {
            if (fn->init == AbsVal::NotInit)
                col_.error("uninitialized", e->line, "调用了未初始化的可调用值 '" + callee + "'",
                           "声明时没有给出初始值", "先给它赋一个函数值");
            fn->read = true;
            fn->readSinceAssign = true;
        }
    }
    else { if (e->a) eval(e->a, env); return v; }

    auto isBuiltin = [&](const char* n) { return callee == n; };
    if (isBuiltin("len")) {
        v.type = "int";
        if (!args.empty() && args[0].size.known) {
            // a length is an int, so it can never exceed the target integer width
            v.range = rng(std::max(0LL, std::min(args[0].size.lo, intHi_)),
                          std::max(0LL, std::min(args[0].size.hi, intHi_)));
        }
        return v;
    }
    if (isBuiltin("int")) { v.type = "int"; return v; }
    if (isBuiltin("float")) { v.type = "float"; return v; }
    if (isBuiltin("bool")) { v.type = "bool"; return v; }
    if (isBuiltin("str") || isBuiltin("String")) { v.type = "String"; return v; }
    if (isBuiltin("Bytes")) { v.type = "Bytes"; return v; }
    if (isBuiltin("List")) {
        v.type = "List";
        if (!args.empty()) { if (args[0].size.known) v.size = args[0].size; else if (args[0].range.known) v.size = args[0].range; }
        return v;
    }
    if (isBuiltin("Tuple")) { v.type = "Tuple"; return v; }
    if (isBuiltin("alloc")) {
        v.type = "List";
        if (!args.empty() && args[0].range.known) {
            v.size = args[0].range;
            if (args[0].range.lo < 0)
                col_.error("out-of-bounds", e->line, "alloc 的长度为负",
                           "长度 ∈ " + rangeText(args[0].range), "保证长度 >= 0");
        }
        if (!e->args.empty() && e->args[0].value && e->args[0].value->kind == EK::Ident)
            v.sizeKey = "v:" + e->args[0].value->name;
        return v;
    }
    if (isBuiltin("sorted")) {
        v.type = "List";
        if (!args.empty()) v.size = args[0].size;
        return v;
    }
    if (isBuiltin("sum")) { v.type = "int"; return v; }
    if (isBuiltin("zip") || isBuiltin("enumerate") || isBuiltin("pairs")) {
        v.type = "List";
        if (!args.empty()) v.size = args[0].size;
        return v;
    }
    if (isBuiltin("ok") || isBuiltin("Ok") || isBuiltin("Err")) {
        if (!args.empty()) return args[0];
        return v;
    }
    if (isBuiltin("abs") && !args.empty() && args[0].range.known) {
        v.type = args[0].type == "unknown" ? "int" : args[0].type;
        long long lo = args[0].range.lo, hi = args[0].range.hi;
        long long alo = std::min(std::llabs(lo), std::llabs(hi));
        long long ahi = std::max(std::llabs(lo), std::llabs(hi));
        if (lo <= 0 && hi >= 0) alo = 0;
        v.range = rng(alo, ahi);
        return v;
    }
    if ((isBuiltin("min") || isBuiltin("max")) && args.size() >= 2) {
        bool mn = callee == "min";
        v.type = args[0].type;
        bool allKnown = true;
        for (auto& a : args) if (!a.range.known) allKnown = false;
        if (allKnown) {
            long long best = mn ? args[0].range.lo : args[0].range.hi;
            for (auto& a : args) best = mn ? std::min(best, a.range.lo) : std::max(best, a.range.hi);
            v.range = rng(best, best);
        }
        return v;
    }
    if (isBuiltin("range")) {
        v.type = "Iter";
        v.isIter = true;
        if (args.size() >= 2 && args[0].range.known && args[1].range.known)
            v.range = rng(std::min(args[0].range.lo, args[1].range.lo),
                          std::max(args[0].range.hi, args[1].range.hi));
        return v;
    }
    if (isBuiltin("ord")) { v.type = "int"; return v; }
    if (isBuiltin("chr")) { v.type = "String"; return v; }
    if (isBuiltin("is_null")) { v.type = "bool"; return v; }
    if (isBuiltin("typeof")) { v.type = "String"; return v; }
    if (isBuiltin("print")) { v.type = "null"; v.nullState = AbsVal::IsNull; return v; }
    if (isBuiltin("raw_copy")) { if (!args.empty()) return args[0]; return v; }
    if (callee == "json") return v;
    // low level file primitives: model the useful return shapes
    if (callee == "_file_read") { v.type = "String"; v.nullState = AbsVal::NonNull; return v; }
    if (callee == "_file_read_bytes") { v.type = "Bytes"; v.nullState = AbsVal::NonNull; return v; }
    if (callee == "_file_size" || callee == "_file_mtime") { v.type = "int"; return v; }
    if (callee == "_file_exists" || callee == "_file_is_dir" || callee == "_file_remove" ||
        callee == "_file_rename" || callee == "_file_copy" || callee == "_chdir" ||
        callee == "_dir_make" || callee == "_dir_remove") { v.type = "bool"; return v; }
    if (callee == "_file_write" || callee == "_file_append" || callee == "_file_write_bytes") {
        v.type = "int";
        return v;
    }
    if (callee == "_dir_list" || callee == "_dir_walk") {
        v.type = "List";
        v.size = rng(0, 1000000);
        v.nullState = AbsVal::NonNull;
        return v;
    }
    if (callee.rfind("_path_", 0) == 0 || callee == "_cwd") {
        v.type = "String";
        v.nullState = AbsVal::NonNull;
        return v;
    }

    auto it = byName_.find(callee);
    if (it != byName_.end()) {
        FuncInfo* f = it->second;
        // contract check at the call site
        if (!f->trusted) {
            std::map<std::string, AbsVal> binds;
            for (size_t i = 0; i < f->params.size(); i++) {
                AbsVal pv;
                pv.type = f->params[i].type.empty() ? "unknown" : f->params[i].type;
                pv.nullState = AbsVal::NonNull;
                pv.init = AbsVal::IsInit;
                if (i < args.size()) {
                    pv = args[i];
                    if (!f->params[i].type.empty()) pv.type = f->params[i].type;
                }
                binds[f->params[i].name] = pv;
            }
            for (size_t i = 0; i < f->requireExprs.size(); i++) {
                env.push();
                for (auto& kv : binds) env.declare(kv.first, kv.second);
                int tri = triCondOf(f->requireExprs[i], env);
                std::string detail = describeCond(f->requireExprs[i], env);
                env.pop();
                std::string text = i < f->requireTexts.size() ? f->requireTexts[i] : "";
                if (tri == 1) continue;
                if (tri == 0) {
                    f->errors++;
                    col_.error("contract-violation", e->line,
                               "契约违反：" + f->name + " 要求 " + text + "，但实参可证伪",
                               detail, "调用前加 [[assert: " + text + "]]");
                } else {
                    f->warnings++;
                    col_.warn("contract-possible", e->line,
                              "契约可能违反：" + f->name + " 要求 " + text + "，无法判定",
                              detail, "加 [[assert: " + text + "]] 缩小实参范围");
                }
            }
        }
        // return value: assume the ensures constrain it (conservatively unknown)
        // purity: a pure function may only call other pure functions
        if (cur_ && cur_->pure && !f->pure && !f->trusted && f != cur_)
            col_.error("purity", e->line, "[[pure]] 函数调用了非纯函数 '" + f->name + "'",
                       "被调函数没有 [[pure]] 标注", "给 " + f->name + " 加 [[pure]]，或去掉调用者的 [[pure]]");
        // taint: tainted data crossing an [[unsafe]] boundary
        if (f->unsafe) {
            for (size_t i = 0; i < args.size() && i < f->params.size(); i++) {
                if (!args[i].tainted) continue;
                col_.error("taint", e->line, "污点数据流入 [[unsafe]] 函数 '" + f->name + "'",
                           "实参 '" + f->params[i].name + "' 来自 [[taint: " + args[i].taintSource + "]]",
                           "在边界前校验或净化数据");
                break;
            }
        }
        return v;
    }
    // unknown function (maybe from a module): still propagate taint
    return v;
}

void Flow::narrow(Env& env, const ExprP& cond, bool truth) {
    if (!cond) return;
    // build a flat map of the visible variables, narrow, then write back
    std::map<std::string, AbsVal> flat;
    for (auto& sc : env.scopes)
        for (auto& kv : sc.vars) flat[kv.first] = kv.second;
    narrowMap(flat, cond, truth);
    for (auto& sc : env.scopes) {
        for (auto& kv : sc.vars) {
            auto it = flat.find(kv.first);
            if (it != flat.end()) kv.second = it->second;
        }
    }
}

void Flow::narrowMap(std::map<std::string, AbsVal>& m, const ExprP& cond, bool truth) {
    if (!cond) return;
    if (cond->kind == EK::Unary && cond->op == T::Not) { narrowMap(m, cond->a, !truth); return; }
    if (cond->kind == EK::Logical && cond->op == T::AndAnd) {
        if (truth) { narrowMap(m, cond->a, true); narrowMap(m, cond->b, true); }
        else return;
        return;
    }
    if (cond->kind == EK::Logical && cond->op == T::OrOr) {
        if (!truth) { narrowMap(m, cond->a, false); narrowMap(m, cond->b, false); }
        return;
    }
    if (cond->kind == EK::Call && cond->a && cond->a->kind == EK::Ident) {
        const std::string& fn = cond->a->name;
        if (fn == "is_null" && !cond->args.empty()) {
            const ExprP& arg = cond->args[0].value;
            if (arg && arg->kind == EK::Ident) {
                auto it = m.find(arg->name);
                if (it != m.end()) it->second.nullState = truth ? AbsVal::IsNull : AbsVal::NonNull;
            }
            return;
        }
    }
    if (cond->kind != EK::Binary) return;
    const ExprP& L = cond->a;
    const ExprP& R = cond->b;
    if (!L || !R) return;
    // len(x) <op> constant  ->  narrow x's size
    {
        auto isLenOf = [](const ExprP& e, const Expr*& arg) {
            if (e && e->kind == EK::Call && e->a && e->a->kind == EK::Ident && e->a->name == "len" &&
                e->args.size() == 1) {
                arg = e->args[0].value.get();
                return true;
            }
            return false;
        };
        const Expr* argA = nullptr;
        const Expr* argB = nullptr;
        long long c = 0;
        T op = cond->op;
        bool ok = false;
        if (isLenOf(L, argA) && R->kind == EK::Int) { c = R->ival; ok = true; }
        else if (isLenOf(R, argB) && L->kind == EK::Int) {
            c = L->ival;
            ok = true;
            switch (op) {
                case T::Lt: op = T::Gt; break;
                case T::Gt: op = T::Lt; break;
                case T::Le: op = T::Ge; break;
                case T::Ge: op = T::Le; break;
                default: break;
            }
        }
        if (ok && argA && argA->kind == EK::Ident) {
            auto it = m.find(argA->name);
            if (it != m.end()) {
                T eff = truth ? op : [&] {
                    switch (op) {
                        case T::Lt: return T::Ge;
                        case T::Le: return T::Gt;
                        case T::Gt: return T::Le;
                        case T::Ge: return T::Lt;
                        case T::Eq: return T::Ne;
                        case T::Ne: return T::Eq;
                        default: return op;
                    }
                }();
                Range& s = it->second.size;
                long long lo = s.known ? s.lo : 0;
                long long hi = s.known ? s.hi : intHi_;      // lengths are ints
                switch (eff) {
                    case T::Lt: s = rng(lo, std::min(hi, c - 1)); break;
                    case T::Le: s = rng(lo, std::min(hi, c)); break;
                    case T::Gt: s = rng(std::max(lo, c + 1), hi); break;
                    case T::Ge: s = rng(std::max(lo, c), hi); break;
                    case T::Eq: s = rng(c, c); break;
                    default: break;
                }
                s.lo = std::max(0LL, s.lo);
            }
            return;
        }
    }
    // null comparisons
    if (L->kind == EK::Null || R->kind == EK::Null) {
        const ExprP& other = (L->kind == EK::Null) ? R : L;
        if (other->kind == EK::Ident) {
            auto it = m.find(other->name);
            if (it != m.end()) {
                bool isEq = cond->op == T::Eq;
                bool nullNow = truth ? isEq : !isEq;
                it->second.nullState = nullNow ? AbsVal::IsNull : AbsVal::NonNull;
            }
        }
        return;
    }
    // Ident <op> literal
    auto litOf = [](const ExprP& e, long long& out) {
        if (e && e->kind == EK::Int) { out = e->ival; return true; }
        return false;
    };
    const Expr* id = nullptr;
    long long c = 0;
    T op = cond->op;
    if (L->kind == EK::Ident && litOf(R, c)) id = L.get();
    else if (R->kind == EK::Ident && litOf(L, c)) {
        id = R.get();
        switch (op) {
            case T::Lt: op = T::Gt; break;
            case T::Gt: op = T::Lt; break;
            case T::Le: op = T::Ge; break;
            case T::Ge: op = T::Le; break;
            default: break;
        }
    }
    if (!id) return;
    auto it = m.find(id->name);
    if (it == m.end()) return;
    AbsVal& v = it->second;
    T eff = truth ? op : [&] {
        switch (op) {
            case T::Lt: return T::Ge;
            case T::Le: return T::Gt;
            case T::Gt: return T::Le;
            case T::Ge: return T::Lt;
            case T::Eq: return T::Ne;
            case T::Ne: return T::Eq;
            default: return op;
        }
    }();
    long long lo = v.range.known ? v.range.lo : intLo_;
    long long hi = v.range.known ? v.range.hi : intHi_;
    switch (eff) {
        case T::Lt: v.range = rng(lo, std::min(hi, c - 1)); break;
        case T::Le: v.range = rng(lo, std::min(hi, c)); break;
        case T::Gt: v.range = rng(std::max(lo, c + 1), hi); break;
        case T::Ge: v.range = rng(std::max(lo, c), hi); break;
        case T::Eq: v.range = rng(c, c); break;
        case T::Ne:
            if (!v.range.known) break;
            if (v.range.lo == c && v.range.hi == c) v.range = Range();    // contradictory
            else if (v.range.lo == c) v.range = rng(c + 1, v.range.hi);
            else if (v.range.hi == c) v.range = rng(v.range.lo, c - 1);
            break;
        default: break;
    }
    // `int` is a bounded type, so a narrowed range never exceeds the target width
    if (v.range.known && (v.type == "int" || v.type == "unknown" || v.type == "bool")) {
        v.range.lo = std::max(v.range.lo, intLo_);
        v.range.hi = std::min(v.range.hi, intHi_);
    }
}

int Flow::triCondOf(const ExprP& e, Env& env) {
    CasCtx ctx;
    ctx.subst = &subst_;
    ctx.lo = &loBnd_;
    ctx.hi = &hiBnd_;
    ctx.range = [&](const std::string& name, long long& lo, long long& hi) {
        AbsVal* v = env.find(name);
        if (!v || !v->range.known) return false;
        lo = v->range.lo;
        hi = v->range.hi;
        return true;
    };
    int sym = casTriCtx(e, ctx);
    if (sym >= 0) return sym;                      // the CAS decided it exactly
    if (!e) return -1;
    if (e->kind == EK::Bool) return e->bval ? 1 : 0;
    if (e->kind == EK::Int) return e->ival != 0 ? 1 : 0;
    if (e->kind == EK::Null) return 0;
    if (e->kind == EK::Unary && e->op == T::Not) {
        int t = triCondOf(e->a, env);
        return t < 0 ? -1 : 1 - t;
    }
    if (e->kind == EK::Logical) {
        int a = triCondOf(e->a, env);
        int b = triCondOf(e->b, env);
        if (e->op == T::AndAnd) {
            if (a == 0 || b == 0) return 0;
            if (a == 1 && b == 1) return 1;
            return -1;
        }
        if (a == 1 || b == 1) return 1;
        if (a == 0 && b == 0) return 0;
        return -1;
    }
    if (e->kind == EK::Binary) {
        switch (e->op) {
            case T::Eq: case T::Ne: case T::Lt: case T::Gt: case T::Le: case T::Ge: {
                // null comparisons first
                const ExprP& L = e->a;
                const ExprP& R = e->b;
                if ((L && L->kind == EK::Null) || (R && R->kind == EK::Null)) {
                    const ExprP& other = (L && L->kind == EK::Null) ? R : L;
                    AbsVal ov = eval(other, env);
                    if (ov.nullState == AbsVal::NullUnknown) return -1;
                    bool isNull = ov.nullState == AbsVal::IsNull;
                    if (ov.nullState == AbsVal::MaybeNull) return -1;
                    bool eq = (e->op == T::Eq);
                    return (eq == isNull) ? 1 : 0;
                }
                AbsVal a = eval(L, env);
                AbsVal b = eval(R, env);
                bool aNullish = a.type == "null" || (a.nullState == AbsVal::IsNull);
                bool bNullish = b.type == "null" || (b.nullState == AbsVal::IsNull);
                bool numA = a.type == "int" || a.type == "float" || a.type == "bool";
                bool numB = b.type == "int" || b.type == "float" || b.type == "bool";
                if (numA && numB) {
                    if (e->op == T::Eq || e->op == T::Ne) {
                        bool bothSingle = a.range.known && b.range.known && a.range.lo == a.range.hi && b.range.lo == b.range.hi;
                        if (bothSingle) { bool eq = a.range.lo == b.range.lo; return (e->op == T::Eq) == eq ? 1 : 0; }
                        if (a.range.known && b.range.known &&
                            (a.range.hi < b.range.lo || b.range.hi < a.range.lo))
                            return e->op == T::Ne ? 1 : 0;
                        return -1;
                    }
                    if (!a.range.known || !b.range.known) return -1;
                    switch (e->op) {
                        case T::Lt: if (a.range.hi < b.range.lo) return 1; if (a.range.lo >= b.range.hi) return 0; return -1;
                        case T::Gt: if (a.range.lo > b.range.hi) return 1; if (a.range.hi <= b.range.lo) return 0; return -1;
                        case T::Le: if (a.range.hi <= b.range.lo) return 1; if (a.range.lo > b.range.hi) return 0; return -1;
                        case T::Ge: if (a.range.lo >= b.range.hi) return 1; if (a.range.hi < b.range.lo) return 0; return -1;
                        default: return -1;
                    }
                }
                if (aNullish && bNullish && (e->op == T::Eq || e->op == T::Ne))
                    return e->op == T::Eq ? 1 : 0;
                return -1;
            }
            default: break;
        }
    }
    if (e->kind == EK::Call && e->a && e->a->kind == EK::Ident) {
        const std::string& fn = e->a->name;
        if (fn == "is_null" && !e->args.empty()) {
            AbsVal a = eval(e->args[0].value, env);
            if (a.nullState == AbsVal::IsNull) return 1;
            if (a.nullState == AbsVal::NonNull) return 0;
            return -1;
        }
        if (fn == "len" && !e->args.empty()) {
            AbsVal a = eval(e->args[0].value, env);
            if (a.size.known) return a.size.hi > 0 ? (a.size.lo > 0 ? 1 : -1) : 0;
            return -1;
        }
    }
    AbsVal v = eval(e, env);
    if (v.range.known) {
        if (v.range.lo == 0 && v.range.hi == 0) return 0;
        if (v.range.lo > 0 || v.range.hi < 0) return 1;
    }
    if (v.type == "null") return 0;
    return -1;
}

void Flow::block(const BlockP& b, Env& env, bool newScope) {
    if (!b) return;
    if (newScope) env.push();
    bool dead = false;
    int deadLine = 0;
    bool sawUnreachableAnn = false;
    // dead store within a straight line block: an assignment that the very next statement
    // overwrites without reading it (a sound, conservative form of the classic check)
    std::map<std::string, int> pendingStore;
    for (size_t i = 0; i < b->stmts.size(); i++) {
        const StmtP& s = b->stmts[i];
        if (!s) continue;
        if (!skip(s) && s->kind != SK::Annot) {
            std::set<std::string> u, d;
            stmtDirectUseDef(s.get(), u, d);
            for (auto& x : u) pendingStore.erase(x);
            std::set<std::string> stored;
            if (s->kind == SK::Assign && s->target && s->target->kind == EK::Ident) stored.insert(s->target->name);
            else if (s->kind == SK::CompoundAssign && s->target && s->target->kind == EK::Ident) stored.insert(s->target->name);
            else if ((s->kind == SK::New || s->kind == SK::Const) && s->initExpr)
                for (auto& x : s->names) stored.insert(x);
            for (auto& x : stored) {
                auto it = pendingStore.find(x);
                if (it != pendingStore.end() && !deadReported_.count(x)) {
                    int dl = it->second;
                    std::string nm = x;
                    withSuppression(dl, [&] {
                        col_.info("dead-store", dl, "死存储：'" + nm + "' 的新值从未被读取",
                                  "下一条语句直接覆盖了这个值，中间没有任何读取", "删除这次赋值，或先使用该值");
                    });
                                        deadReported_.insert(x);
                }
                pendingStore[x] = s->line;
            }
        }
        if (!dead && i > 0) {
            const StmtP& prev = b->stmts[i - 1];
            if (prev && (prev->kind == SK::Return || prev->kind == SK::Throw || prev->kind == SK::Break ||
                         prev->kind == SK::Continue)) {
                if (!(prev->kind == SK::Throw && b->exceptBody)) dead = true;
            }
            if (prev && prev->kind == SK::If && prev->elseBody) {
                auto endsControl = [](const BlockP& blk) {
                    if (!blk || blk->stmts.empty()) return false;
                    SK k = blk->stmts.back()->kind;
                    return k == SK::Return || k == SK::Throw || k == SK::Break || k == SK::Continue;
                };
                if (endsControl(prev->body) && endsControl(prev->elseBody)) dead = true;
            }
        }
        if (dead) {
            if (s->kind == SK::Annot) continue;
            if (hasAnnotation(s->annotations, "unreachable")) { sawUnreachableAnn = true; continue; }
            if (deadLine == 0) {
                deadLine = s->line;
                unreachableReported_.insert(s->line);
                col_.warn("unreachable", s->line, "不可达代码",
                          "前面的语句一定会转移控制流（return / throw / break / continue）",
                          "删除，或加 [[unreachable]] 说明这是有意保留");
            }
            continue;
        }
        stmt(s, env);
        noteState(s->line, env);
    }
    if (newScope) {
        // locals of this scope are gone; remember their names so a later read is reported
        for (auto& kv : env.scopes.back().vars) {
            const AbsVal& v = kv.second;
            if (v.declLine != 0) outOfScope_.insert(kv.first);
            if (!v.isLocal || v.declLine == 0 || v.read) continue;
            if (!kv.first.empty() && kv.first[0] == '_') continue;
            std::string nm = kv.first;
            int dl = v.declLine;
            withSuppression(dl, [&] {
                col_.info("unused-variable", dl, "变量 '" + nm + "' 声明后从未被读取", "",
                          "删除该变量，或使用它");
            });
        }
        env.pop();
    }
    (void)sawUnreachableAnn;
}

void Flow::stmt(const StmtP& s, Env& env) {
    if (!s || budgetExceeded()) return;
    // A statement inlined from a `use`d module used to be skipped entirely, which meant the
    // module's own globals were never declared and every later use looked "undefined".  Analyse
    // it, and mute the diagnostics that belong to the module file.
    MuteGuard mute(col_, skip(s));
    // statements inlined from a `use`d module keep their own line numbers, so report them
    // against their origin file (only visible with --modules)
    std::string savedFile = col_.file;
    if (!s->origin.empty()) col_.file = s->origin;
    size_t mark = col_.active.size();
    for (auto& a : s->annotations) {
        if (a.name == "ignore") {
            col_.pushIgnore(a, s->line, std::max(s->line, stmtLastLine(s)));
            ignoreByLine_[s->line].push_back(a);
        }
    }

    // assume / require narrow the state before the statement
    for (auto& a : s->annotations) {
        if ((a.name == "assume") && !a.args.empty() && a.args[0].expr) {
            int tri = triCondOf(a.args[0].expr, env);
            if (tri == 0)
                col_.warn("annotation-argument", a.line, "[[assume]] 的条件在当前状态恒为假",
                          "假设与已有事实矛盾，后续分析可能不可靠", "");
            narrow(env, a.args[0].expr, true);
            learnFacts(a.args[0].expr, true, subst_, loBnd_, hiBnd_);   // [[assume]] facts
            applyConstBounds(env);
        }
        if (a.name == "assert" && !a.args.empty() && a.args[0].expr) {
            int tri = triCondOf(a.args[0].expr, env);
            std::string text = annotationPrimaryText(a);
            if (tri == 0)
                col_.error("assert-violation", a.line, "断言可证伪：" + text,
                           describeCond(a.args[0].expr, env), "修正条件，或用 [[assume]]/[[ignore]] 说明");
            else if (tri < 0)
                col_.info("assert-unproven", a.line, "无法证明断言：" + text,
                          describeCond(a.args[0].expr, env), "补充 [[assume]] 或更精确的约束");
            else
                narrow(env, a.args[0].expr, true);
        }
    }

    // the CAS bounds learned above (`i < n` with `n <= 1000`) also narrow the numeric ranges,
    // so the overflow and index checks see them
    applyConstBounds(env);

    bool taintMark = false;
    bool unsafeMark = false;
    std::string taintSource;
    for (auto& a : s->annotations) {
        if (a.name == "taint") {
            taintMark = true;
            taintSource = annotationPrimaryText(a);
        }
        if (a.name == "unsafe") unsafeMark = true;
    }

    switch (s->kind) {
        case SK::Annot:
            break;
        case SK::Expr: {
            eval(s->expr, env);
            break;
        }
        case SK::New: case SK::Const: {
            AbsVal v;
            if (!s->typeDims.empty()) {
                // `T[n]` / `T[]` / `T[m][n]`: an Array of the element type, with a statically
                // known outermost length whenever that dimension is a literal
                v.type = "Array";
                v.eltType = s->type;
                v.init = AbsVal::IsInit;
                v.nullState = AbsVal::NonNull;
                if (s->typeDims[0] && s->typeDims[0]->kind == EK::Int) {
                    long long n = (long long)s->typeDims[0]->ival;
                    if (n < 0) n = 0;
                    v.size = rng(n, n);
                }
            } else if (s->initExpr) v = eval(s->initExpr, env);
            else { v.init = AbsVal::NotInit; v.type = s->type.empty() ? "unknown" : s->type; }
            if (s->names.size() == 1) {
                if (s->typeDims.empty())
                    checkDeclaredType(s->type, v, s->line, "声明 '" + s->names[0] + "'");
                assignTo(makeIdent(s->names[0], s->line), v, env, s->line, true,
                         s->typeDims.empty() ? s->type : std::string());
            } else {
                if (v.size.known && v.size.lo != (long long)s->names.size())
                    col_.warn("out-of-bounds", s->line, "解构的元素个数与右侧长度不一致",
                              "右侧长度 ∈ " + rangeText(v.size) + "，左侧有 " +
                                  formatInt((long long)s->names.size()) + " 个名字", "");
                for (auto& n : s->names) {
                    AbsVal e = v;
                    e.type = "unknown";
                    e.range = Range();
                    e.size = Range();
                    assignTo(makeIdent(n, s->line), e, env, s->line, true, "");
                }
            }
            break;
        }
        case SK::State: {
            AbsVal v = s->initExpr ? eval(s->initExpr, env) : AbsVal();
            globals_.insert(s->name);
            (void)v;
            break;
        }
        case SK::Assign: {
            AbsVal v = eval(s->value, env);
            assignTo(s->target, v, env, s->line, false, "");
            break;
        }
        case SK::CompoundAssign: {
            AbsVal base = eval(s->target, env);
            AbsVal rhs = eval(s->value, env);
            AbsVal r = base;
            if (base.range.known && rhs.range.known && base.type == "int" && rhs.type == "int") {
                long long lo = 0, hi = 0;
                bool known = true;
                switch (s->op) {
                    case T::PlusA: lo = base.range.lo + rhs.range.lo; hi = base.range.hi + rhs.range.hi; break;
                    case T::MinusA: lo = base.range.lo - rhs.range.hi; hi = base.range.hi - rhs.range.lo; break;
                    case T::StarA: {
                        long long c1 = base.range.lo * rhs.range.lo, c2 = base.range.lo * rhs.range.hi;
                        long long c3 = base.range.hi * rhs.range.lo, c4 = base.range.hi * rhs.range.hi;
                        lo = std::min(std::min(c1, c2), std::min(c3, c4));
                        hi = std::max(std::max(c1, c2), std::max(c3, c4));
                        break;
                    }
                    default: known = false; break;
                }
                if (known) {
                    r.range = rng(lo, hi);
                    if (lo < intLo_ || hi > intHi_)
                        col_.warn("overflow", s->line, "复合赋值可能溢出",
                                  "结果 ∈ " + rangeText(r.range), "加 [[assert: ...]] 限制范围");
                }
            } else {
                r.range = Range();
            }
            assignTo(s->target, r, env, s->line, false, "");
            break;
        }
        case SK::Del: {
            for (auto& n : s->names) {
                AbsVal* v = env.find(n);
                if (v) v->init = AbsVal::NotInit;
            }
            break;
        }
        case SK::If: {
            eval(s->cond, env);
            if (s->cond) {
                int tri = triCondOf(s->cond, env);
                if (tri == 1)
                    col_.info("redundant-condition", s->line, "条件恒为真",
                              "在当前抽象状态下条件已成立", "可以去掉这个分支");
                else if (tri == 0 && !s->elseBody)
                    col_.info("redundant-condition", s->line, "条件恒为假，分支永远不会执行", "", "");
            }
            // Case analysis: the then path assumes the condition, the else path assumes its
            // negation; both learn symbolic facts (equalities and linear bounds) and every
            // diagnostic inside says which path it came from.
            auto savedSubst = subst_, savedLo = loBnd_, savedHi = hiBnd_;
            std::string savedNote = col_.branchNote;
            std::string condText = exprText(s->cond);
            std::string negText = condText;
            if (s->cond && s->cond->kind == EK::Binary) {
                ExprP flipped = std::make_shared<Expr>(*s->cond);
                flipped->op = negatedOp(s->cond->op);
                negText = exprText(flipped);
            } else {
                negText = "!" + condText;
            }
            Env thenEnv = env;
            narrow(thenEnv, s->cond, true);
            learnFacts(s->cond, true, subst_, loBnd_, hiBnd_);
            applyConstBounds(thenEnv);
            col_.branchNote = savedNote + (savedNote.empty() ? "" : " 且 ") +
                              "条件成立（" + condText + "）";
            block(s->body, thenEnv, true);
            subst_ = savedSubst; loBnd_ = savedLo; hiBnd_ = savedHi;
            Env elseEnv = env;
            if (s->elseBody) {
                narrow(elseEnv, s->cond, false);
                learnFacts(s->cond, false, subst_, loBnd_, hiBnd_);
                applyConstBounds(elseEnv);
                col_.branchNote = savedNote + (savedNote.empty() ? "" : " 且 ") +
                                  "条件不成立（" + negText + "）";
                block(s->elseBody, elseEnv, true);
                subst_ = savedSubst; loBnd_ = savedLo; hiBnd_ = savedHi;
            }
            col_.branchNote = savedNote;
            // The state after the branch is the *join of the two paths that reach it* - joining
            // the pre-if state as well would let "unknown" wipe out what both paths established,
            // which is exactly the case-analysis payoff (`if i < 1 ( i = 1 )` gives `i >= 1`:
            // one path has [1, 1], the other is narrowed to [1, max]).
            auto endsControl = [](const BlockP& blk) {
                if (!blk || blk->stmts.empty()) return false;
                SK k = blk->stmts.back()->kind;
                return k == SK::Return || k == SK::Throw || k == SK::Break || k == SK::Continue;
            };
            bool thenExits = endsControl(s->body);
            Env joined = thenEnv;
            if (s->elseBody) {
                if (thenExits) joined = elseEnv;             // only the else path reaches the join
                else mergeEnv(joined, elseEnv);
            } else {
                Env falsePath = env;
                narrow(falsePath, s->cond, false);
                // `if n < 2( =n )` never falls through, so what follows only sees `n >= 2`
                if (thenExits) joined = falsePath;
                else mergeEnv(joined, falsePath);
            }
            env = joined;
            break;
        }
        case SK::While: {
            analyzeLoop(s, env, false);
            break;
        }
        case SK::For: {
            analyzeLoop(s, env, true);
            break;
        }
        case SK::Break: case SK::Continue:
            break;
        case SK::Throw: {
            eval(s->expr, env);
            if (cur_ && cur_->pure)
                col_.info("purity", s->line, "[[pure]] 函数抛出异常", "抛出不是副作用，这里只是提示", "");
            break;
        }
        case SK::Print: {
            for (auto& a : s->args) eval(a, env);
            if (s->sep) eval(s->sep, env);
            if (cur_ && cur_->pure)
                col_.error("purity", s->line, "[[pure]] 函数不得执行 print",
                           "输出是副作用", "去掉 [[pure]]，或把值返回给调用者");
            break;
        }
        case SK::Input: {
            AbsVal v;
            v.type = "String";
            v.init = AbsVal::IsInit;
            v.tainted = true;
            v.taintSource = "input";
            if (s->target && s->target->kind == EK::Ident) {
                AbsVal* found = env.find(s->target->name);
                if (found) *found = v;
                else assignTo(s->target, v, env, s->line, true, "String");
            }
            break;
        }
        case SK::Return: {
            AbsVal v = s->expr ? eval(s->expr, env) : AbsVal();
            if (cur_) {
                cur_->returnVal = v;
                if (v.tainted) cur_->returnTainted = true;
                for (size_t i = 0; i < cur_->ensureExprs.size(); i++) {
                    env.push();
                    AbsVal res = v;
                    res.type = v.type;
                    env.declare("result", res);
                    int tri = triCondOf(cur_->ensureExprs[i], env);
                    std::string detail = describeCond(cur_->ensureExprs[i], env);
                    env.pop();
                    if (tri == 0) {
                        cur_->errors++;
                        col_.error("ensure-violation", s->line,
                                   "后置条件可证伪：" + (i < cur_->ensureTexts.size() ? cur_->ensureTexts[i] : ""),
                                   detail, "修正实现，或调整 [[ensure]]");
                    }
                }
            }
            break;
        }
        case SK::Block: {
            block(s->body, env, true);
            break;
        }
        case SK::FuncDef: {
            // mark the captures of a nested function as reads of this scope
            if (s->body) {
                std::set<std::string> f;
                freeNames(s->body, f);
                for (auto& n : f) {
                    AbsVal* v = env.find(n);
                    if (v) { v->read = true; v->readSinceAssign = true; }
                }
            }
            break;
        }
        case SK::ClassDef:
            break;      // analyzed in their own pass
        case SK::Use:
            if (s->body) for (auto& x : s->body->stmts) stmt(x, env);
            break;
        case SK::FieldDecl: case SK::ParentInit:
            break;
        case SK::MacroDef:
            break;
        default:
            break;
    }

    // taint marking applies to whatever the statement produced
    if (taintMark) {
        std::set<std::string> defs, uses;
        stmtUseDef(s.get(), uses, defs);
        for (auto& d : defs) {
            AbsVal* v = env.find(d);
            if (!v) {
                auto g = globalsEnv_.vars.find(d);
                if (g != globalsEnv_.vars.end()) v = &g->second;
            }
            if (v) {
                v->tainted = true;
                v->taintSource = taintSource;
            }
        }
    }
    // tainted data reaching an [[unsafe]] site
    if (unsafeMark) {
        std::set<std::string> defs, uses;
        stmtUseDef(s.get(), uses, defs);
        for (auto& u : uses) {
            AbsVal* v = env.find(u);
            if (!v) {
                auto g = globalsEnv_.vars.find(u);
                if (g != globalsEnv_.vars.end()) v = &g->second;
            }
            if (v && v->tainted) {
                col_.error("taint", s->line, "污点数据流入 [[unsafe]] 区域",
                           "'" + u + "' 来自 [[taint: " + v->taintSource + "]]",
                           "在边界前做校验或净化，或为该处加 [[ignore: taint]]");
                break;
            }
        }
    }
    col_.popTo(mark);
    col_.file = savedFile;
}

// ---------------------------------------------------------------- loops
void Flow::analyzeLoop(const StmtP& s, Env& env, bool isFor) {
    loopsSeen_++;
    bool hasInvariant = hasAnnotation(s->annotations, "invariant");
    if (hasInvariant) loopsWithInvariant_++;
    loopDepth_++;
    std::string loopVar;
    AbsVal iterVal;

    Env before = env;
    env.push();
    if (isFor) {
        AbsVal it = eval(s->iterable, env, true);
        iterVal = it;
        // the upper bound of the loop variable, used to prove that `xs[i]` is in range
        std::string boundKey;
        if (s->iterable && s->iterable->kind == EK::Iter && s->iterable->b) {
            const Expr* bound = s->iterable->b.get();
            std::string lenOf;
            std::function<void(const Expr*)> scan = [&](const Expr* x) {
                if (!x || !lenOf.empty()) return;
                if (x->kind == EK::Call && x->a && x->a->kind == EK::Ident && x->a->name == "len" &&
                    x->args.size() == 1 && x->args[0].value && x->args[0].value->kind == EK::Ident) {
                    lenOf = x->args[0].value->name;
                    return;
                }
                if (x->kind == EK::Call && x->a && x->a->kind == EK::Field && x->a->name == "size" &&
                    x->a->a && x->a->a->kind == EK::Ident) {
                    lenOf = x->a->a->name;
                    return;
                }
                scan(x->a.get());
                scan(x->b.get());
            };
            scan(bound);
            if (!lenOf.empty()) boundKey = "len:" + lenOf;
            else {
                const Expr* leftmost = bound;
                while (leftmost && leftmost->kind == EK::Binary) leftmost = leftmost->a.get();
                if (leftmost && leftmost->kind == EK::Ident) boundKey = "v:" + leftmost->name;
            }
        }
        for (auto& v : s->loopVars) {
            AbsVal lv;
            // `for i in 0 to n` binds an int; iterating a list/string binds an element of
            // unknown type (only strings are known to yield strings)
            bool rangeLoop = s->iterable && s->iterable->kind == EK::Iter;
            lv.type = rangeLoop ? "int" : (it.type == "String" ? "String" : "unknown");
            lv.init = AbsVal::IsInit;
            lv.isLocal = true;
            lv.declLine = s->line;
            if (it.range.known && rangeLoop) lv.range = it.range;
            env.declare(v, lv);
            if (!boundKey.empty()) indexBound_[v] = boundKey;
        }
        if (!it.isIter && it.type != "unknown" && it.type != "List" && it.type != "Tuple" &&
            it.type != "String" && it.type != "Array" && s->iterable &&
            s->iterable->kind != EK::Call)
            col_.error("type-mismatch", s->iterable ? s->iterable->line : s->line,
                       "for 遍历的对象不可迭代：" + it.type, "",
                       "使用列表 / 元组 / 字符串 / 数组 / 1 to 10 迭代器");
    } else {
        eval(s->cond, env);
    }

    // the entry invariant is checked with the loop variables bound (文档 5.8)
    for (auto& a : s->annotations) {
        if (a.name == "invariant" && !a.args.empty() && a.args[0].expr) {
            int tri = triCondOf(a.args[0].expr, env);
            if (tri == 0)
                col_.error("invariant-violation", a.line, "循环不变量在入口处不成立",
                           discoverText(a.args[0].expr, env), "修正不变量或循环前的状态");
        }
    }

    // fixed point: two passes, then widen the variables the body modifies so that the
    // analysis cannot conclude "the condition is still true" from a single iteration
    Env bodyExit;                       // state at the end of one iteration (for invariant/decrease)
    auto savedSubst = subst_, savedLo = loBnd_, savedHi = hiBnd_;
    std::string savedNote = col_.branchNote;
    std::string condText = s->cond ? exprText(s->cond) : std::string("条件");
    learnFacts(s->cond, true, subst_, loBnd_, hiBnd_);
    col_.branchNote = "分支: 循环体（" + condText + "）";
    for (int pass = 0; pass < 2; pass++) {
        Env bodyEnv = env;
        narrow(bodyEnv, s->cond, true);
        if (pass > 0) suppressDead_++;
        block(s->body, bodyEnv, true);
        if (pass > 0) suppressDead_--;
        bodyExit = bodyEnv;
        mergeEnv(env, bodyEnv);
        if (budgetExceeded()) break;
    }
    col_.branchNote = savedNote;
    subst_ = savedSubst; loBnd_ = savedLo; hiBnd_ = savedHi;
    // the loop is only left when the condition is false: that is a fact for what follows
    learnFacts(s->cond, false, subst_, loBnd_, hiBnd_);

    // ---- the checks below run before widening
    if (bodyExit.scopes.empty()) bodyExit = env;
    for (auto& a : s->annotations) {
        if (a.name != "invariant" || a.args.empty() || !a.args[0].expr) continue;
        int tri = triCondOf(a.args[0].expr, bodyExit);
        if (tri == 0)
            col_.warn("invariant-violation", a.line, "循环不变量在迭代后不再成立",
                      discoverText(a.args[0].expr, bodyExit), "加强条件或修正循环体");
    }

    // termination
    if (!isFor) {
        for (auto& a : s->annotations) {
            if (a.name != "decrease" || a.args.empty() || !a.args[0].expr) continue;
            AbsVal head = eval(a.args[0].expr, before);
            AbsVal tail = eval(a.args[0].expr, env);
            if (head.range.known && tail.range.known) {
                if (head.range.lo <= 0)
                    col_.error("termination", a.line, "终止度量在迭代前不为正",
                               "[[decrease: " + annotationPrimaryText(a) + "]] 入口值 ∈ " + rangeText(head.range),
                               "保证度量在循环入口为正");
                else if (tail.range.lo >= head.range.lo)
                    col_.error("termination", a.line, "终止度量没有严格递减",
                               "入口 ∈ " + rangeText(head.range) + "，迭代后 ∈ " + rangeText(tail.range),
                               "确保每次迭代都减小该表达式");
            } else if (head.range.known && head.range.lo == head.range.hi) {
                col_.warn("termination", a.line, "终止度量是常量，无法证明循环终止",
                          "[[decrease: " + annotationPrimaryText(a) + "]] 恒为 " + formatInt(head.range.lo),
                          "改用随迭代变化的表达式");
            }
        }
    }

    // infinite loop: the condition is the literal true and the body has no exit (文档 5.13)
    if (!isFor && s->cond) {
        bool literalTrue = (s->cond->kind == EK::Bool && s->cond->bval) ||
                           (s->cond->kind == EK::Int && s->cond->ival != 0);
        if (literalTrue) {
            std::function<bool(const BlockP&)> hasExit = [&](const BlockP& b) -> bool {
                if (!b) return false;
                for (auto& st : b->stmts) {
                    if (!st) continue;
                    if (st->kind == SK::Break || st->kind == SK::Return || st->kind == SK::Throw) return true;
                    if (st->body && hasExit(st->body)) return true;
                    if (st->elseBody && hasExit(st->elseBody)) return true;
                    if (st->body && st->body->exceptBody && hasExit(st->body->exceptBody)) return true;
                }
                return false;
            };
            if (!hasExit(s->body))
                col_.warn("infinite-loop", s->line, "无限循环：条件恒为真且循环体内没有退出路径",
                          "没有 break / return / throw", "加退出条件，或标注 [[ignore: infinite-loop]]");
        }
    }
    // widen the variables the loop body modifies: a single iteration cannot bound them
    for (size_t i = 0; i < env.scopes.size() && i < before.scopes.size(); i++) {
        for (auto& kv : env.scopes[i].vars) {
            auto it = before.scopes[i].vars.find(kv.first);
            if (it == before.scopes[i].vars.end()) continue;
            if (kv.second.range.known && it->second.range.known &&
                (kv.second.range.lo != it->second.range.lo || kv.second.range.hi != it->second.range.hi))
                kv.second.range = Range();
            if (kv.second.size.known && it->second.size.known &&
                (kv.second.size.lo != it->second.size.lo || kv.second.size.hi != it->second.size.hi))
                kv.second.size = Range();
        }
    }
    env.pop();
    loopDepth_--;
    // union of "the loop never ran" and "the loop ran": keeps reads/initialisation observed
    // in the body while remaining sound for zero-iteration loops
    Env after = before;
    mergeEnv(after, env);
    env = after;
    if (isFor) {
        for (auto& v : s->loopVars) outOfScope_.insert(v);
    }
}

// ---------------------------------------------------------------- reachability / dead stores
void Flow::checkReachability(FuncInfo* f) {
    if (!f->def || !f->def->body) return;
    Cfg cfg;
    int entry = cfgBlock(f->def->body, -1, -1, -1, cfg);
    if (cfg.nodes.empty()) return;
    std::vector<char> seen(cfg.nodes.size(), 0);
    std::vector<int> work;
    if (entry >= 0) { work.push_back(entry); seen[(size_t)entry] = 1; }
    while (!work.empty()) {
        int n = work.back();
        work.pop_back();
        for (int s : cfg.nodes[(size_t)n].succ)
            if (s >= 0 && !seen[(size_t)s]) { seen[(size_t)s] = 1; work.push_back(s); }
    }
    for (size_t i = 0; i < cfg.nodes.size(); i++) {
        if (seen[i]) continue;
        const Stmt* st = cfg.nodes[i].s;
        if (!st) continue;
        if (st->kind == SK::Annot) continue;
        if (hasAnnotation(st->annotations, "unreachable")) continue;
        if (unreachableReported_.count(st->line)) continue;
        col_.info("unreachable", st->line, "CFG 中不可达的语句",
                  "从函数入口出发没有任何路径到达这里", "删除该死代码");
        unreachableReported_.insert(st->line);
    }
}

void Flow::checkDeadStores(FuncInfo* f) {
    if (!f->def || !f->def->body) return;
    Cfg cfg;
    int entry = cfgBlock(f->def->body, -1, -1, -1, cfg);
    if (entry < 0) return;
    size_t n = cfg.nodes.size();
    std::vector<std::set<std::string>> use(n), def(n), liveIn(n), liveOut(n);
    for (size_t i = 0; i < n; i++) stmtDirectUseDef(cfg.nodes[i].s, use[i], def[i]);
    // locals only
    std::set<std::string> locals;
    for (auto& p : f->params) locals.insert(p.name);
    std::function<void(const StmtP&)> decls = [&](const StmtP& s) {
        if (!s) return;
        if (s->kind == SK::New || s->kind == SK::Const) for (auto& x : s->names) locals.insert(x);
        if (s->kind == SK::For) for (auto& v : s->loopVars) locals.insert(v);
        if (s->kind == SK::FuncDef) return;
        if (s->body) { for (auto& x : s->body->stmts) decls(x); }
        if (s->elseBody) for (auto& x : s->elseBody->stmts) decls(x);
        if (s->body && s->body->exceptBody) for (auto& x : s->body->exceptBody->stmts) decls(x);
    };
    for (auto& s : f->def->body->stmts) decls(s);
    for (size_t i = 0; i < n; i++) {
        std::set<std::string> u2, d2;
        for (auto& x : use[i]) if (locals.count(x)) u2.insert(x);
        for (auto& x : def[i]) if (locals.count(x)) d2.insert(x);
        use[i] = u2;
        def[i] = d2;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = n; i-- > 0;) {
            std::set<std::string> out;
            for (int s : cfg.nodes[i].succ) if (s >= 0) out.insert(liveIn[(size_t)s].begin(), liveIn[(size_t)s].end());
            std::set<std::string> in = use[i];
            for (auto& x : out) if (!def[i].count(x)) in.insert(x);
            if (out != liveOut[i] || in != liveIn[i]) {
                liveOut[i] = out;
                liveIn[i] = in;
                changed = true;
            }
            if (budgetExceeded()) return;
        }
    }
    for (size_t i = 0; i < n; i++) {
        const Stmt* st = cfg.nodes[i].s;
        if (!st) continue;
        std::set<std::string> stored;
        switch (st->kind) {
            case SK::Assign: if (st->target && st->target->kind == EK::Ident) stored.insert(st->target->name); break;
            case SK::CompoundAssign: if (st->target && st->target->kind == EK::Ident) stored.insert(st->target->name); break;
            case SK::New: case SK::Const:
                for (auto& x : st->names) if (st->initExpr) stored.insert(x);
                break;
            default: break;
        }
        for (auto& x : stored) {
            if (!locals.count(x)) continue;
            if (liveOut[i].count(x)) continue;
            // a variable that is never read at all is reported as unused instead
            bool usedAnywhere = false;
            for (size_t k = 0; k < n && !usedAnywhere; k++)
                if (use[k].count(x)) usedAnywhere = true;
            if (!usedAnywhere) continue;
            if (deadReported_.count(x)) continue;
            if (x.empty() || x[0] == '_') continue;
            col_.info("dead-store", st->line, "死存储：'" + x + "' 的值之后再未被读取",
                      "该赋值的结果不会影响任何后续计算", "删除该赋值，或检查是否漏用了这个变量");
            deadReported_.insert(x);
        }
    }
}

void Flow::macroChecks() {
    for (auto& ex : prog_.expansions) {
        if (!ex.duplicatedArgWithCall) continue;
        col_.warn("macro-side-effect", ex.line,
                  "宏 " + ex.macro + " 会多次展开带副作用的实参",
                  "实参 `" + ex.argument + "` 在宏体中出现多次且包含函数调用，展开后会被求值多次",
                  "把实参先赋给变量再传入，或改写宏避免重复使用占位符");
    }
}

// ---------------------------------------------------------------- collection + driver
void Flow::collectGlobals(const std::vector<StmtP>& stmts) {
    for (auto& s : stmts) {
        if (!s) continue;
        switch (s->kind) {
            case SK::New: case SK::Const:
                // a `new`/`const` at the top level of a module is a real global: the runtime
                // inlines module statements, so the analyzer has to know it too
                for (auto& n : s->names) globals_.insert(n);
                break;
            case SK::State: globals_.insert(s->name); break;
            case SK::FuncDef: globals_.insert(s->name); break;
            case SK::ClassDef: globals_.insert(s->name); break;
            case SK::Use:
                if (s->body) collectGlobals(s->body->stmts);
                break;
            default: break;
        }
    }
}

static void extractFuncAnnotations(FuncInfo* f) {
    for (auto& a : f->anns) {
        if (a.name == "pure") f->pure = true;
        else if (a.name == "trusted") f->trusted = true;
        else if (a.name == "noreturn") f->noreturn = true;
        else if (a.name == "unsafe") f->unsafe = true;
        else if (a.name == "require" && !a.args.empty()) {
            f->requireExprs.push_back(a.args[0].expr);
            f->requireTexts.push_back(a.args[0].text);
        } else if (a.name == "ensure" && !a.args.empty()) {
            f->ensureExprs.push_back(a.args[0].expr);
            f->ensureTexts.push_back(a.args[0].text);
        } else if (a.name == "decrease" && !a.args.empty()) {
            f->decreaseExprs.push_back(a.args[0].expr);
        } else if (a.name == "modifies") {
            for (auto& g : a.args) if (!g.hasKey) f->modifies.push_back(g.text);
        }
    }
}

void Flow::collectStmts(const std::vector<StmtP>& stmts, const std::string& cls, bool nested) {
    for (auto& s : stmts) {
        if (!s) continue;
        if (s->kind == SK::ClassDef) {
            // every constructor parameter and field becomes visible inside the methods,
            // including the untyped ones (`value = null`)
            for (auto& p : s->params) classFields_[s->name][p.name] = p.type;
            for (auto& m : s->members)
                if (m->kind == SK::FieldDecl) classFields_[s->name][m->name] = m->type;
            for (auto& m : s->members) {
                if (m->kind != SK::FuncDef) continue;
                auto* f = new FuncInfo();
                f->def = m.get();
                f->name = m->name;
                f->cls = s->name;
                f->qual = s->name + "." + m->name;
                f->params = m->params;
                f->anns = m->annotations;
                f->isMethod = true;
                f->nested = nested;
                extractFuncAnnotations(f);
                funcs_.push_back(f);
                if (m->body) collectStmts(m->body->stmts, s->name, true);
            }
            continue;
        }
        if (s->kind == SK::FuncDef) {
            auto* f = new FuncInfo();
            f->def = s.get();
            f->name = s->name;
            f->cls = cls;
            f->qual = cls.empty() ? s->name : cls + "." + s->name;
            f->params = s->params;
            f->anns = s->annotations;
            f->nested = nested;
            extractFuncAnnotations(f);
            funcs_.push_back(f);
            if (s->body) collectStmts(s->body->stmts, cls, true);
            continue;
        }
        if (s->body) collectStmts(s->body->stmts, cls, nested);
        if (s->elseBody) collectStmts(s->elseBody->stmts, cls, nested);
        if (s->body && s->body->exceptBody) collectStmts(s->body->exceptBody->stmts, cls, nested);
        for (auto& m : s->members) if (m->kind != SK::FuncDef) { std::vector<StmtP> one{m}; collectStmts(one, cls, nested); }
    }
}

void Flow::analyzeFunc(FuncInfo* f) {
    if (!f || !f->def || !f->def->body) return;
    subst_.clear();
    loBnd_.clear();
    hiBnd_.clear();
    if (!opts_.includeModules && !f->def->origin.empty() && f->def->origin != file_) return;
    FunctionStatus st;
    st.name = f->qual;
    st.className = f->cls;
    st.line = f->def->line;
    st.hasRequire = !f->requireExprs.empty();
    st.hasEnsure = !f->ensureExprs.empty();
    st.hasModifies = !f->modifies.empty();
    st.pure = f->pure;
    st.trusted = f->trusted;
    st.unsafe = f->unsafe;
    st.noreturn = f->noreturn;
    for (auto& a : f->anns) if (a.name == "invariant") st.hasInvariant = true;

    if (!f->trusted) {
        cur_ = f;
        Env env;
        for (auto& p : f->params) {
            AbsVal v;
            v.type = p.type.empty() ? "unknown" : p.type;
            v.init = AbsVal::IsInit;
            v.nullState = AbsVal::NullUnknown;   // a parameter may be passed null
            env.declare(p.name, v);
        }
        if (f->isMethod) {
            AbsVal t;
            t.type = f->cls;
            t.nullState = AbsVal::NonNull;
            t.init = AbsVal::IsInit;
            env.declare("this", t);
            // constructor parameters and fields of the enclosing class are visible as fields
            auto cf = classFields_.find(f->cls);
            if (cf != classFields_.end())
                for (auto& kv : cf->second) {
                    if (env.scopes.front().vars.count(kv.first)) continue;
                    AbsVal fv;
                    fv.type = kv.second.empty() ? "unknown" : kv.second;
                    fv.init = AbsVal::IsInit;
                    fv.nullState = AbsVal::NullUnknown;
                    env.declare(kv.first, fv);
                }
        }
        if (f->nested && f->def->body) {
            // a nested function closes over the enclosing scope: declare the free names it
            // uses so that reads resolve instead of being reported as undeclared
            std::set<std::string> used;
            freeNames(f->def->body, used);
            for (auto& n : used) {
                if (env.find(n) || globals_.count(n) || builtinNames_.count(n) || byName_.count(n)) continue;
                AbsVal cv;
                cv.init = AbsVal::IsInit;
                cv.nullState = AbsVal::NullUnknown;
                env.declare(n, cv);
            }
        }
        outOfScope_.clear();
        for (auto& r : f->requireExprs) if (r) narrow(env, r, true);   // entry assumptions
        size_t errBefore = res_.diagnostics.size();
        block(f->def->body, env, false);
        // unused parameters and function level locals
        // (`__init__` has no parameters of its own: its real inputs are the class constructor
        //  parameters, which live in the constructor chunk and are checked there)
        const bool skipParamUse = f->def->name == "__init__";
        for (auto& sc : env.scopes) {
            for (auto& kv : sc.vars) {
                if (kv.second.read || kv.first == "this" || kv.first.empty() || kv.first[0] == '_') continue;
                std::string nm = kv.first;
                int dl = kv.second.declLine;
                bool param = dl == 0;
                if (param) dl = f->def->line;
                else if (!kv.second.isLocal) continue;
                // `__init__` has no parameters of its own; the class parameters are seeded into
                // every method, and they are used by the constructor body or by `__init__`, so a
                // method that does not touch them must not be reported.
                if (param) {
                    if (skipParamUse) continue;
                    auto cf = classFields_.find(f->cls);
                    if (!f->cls.empty() && cf != classFields_.end() && cf->second.count(nm)) continue;
                }
                withSuppression(dl, [&] {
                    col_.info("unused-variable", dl,
                              param ? ("参数 '" + nm + "' 从未被使用")
                                    : ("变量 '" + nm + "' 声明后从未被读取"),
                              "", param ? "" : "删除该变量，或使用它");
                });
            }
        }
        checkReachability(f);
        checkDeadStores(f);
        for (size_t i = errBefore; i < res_.diagnostics.size(); i++) {
            if (res_.diagnostics[i].suppressed) continue;
            if (res_.diagnostics[i].severity == Severity::Error) st.errors++;
            else if (res_.diagnostics[i].severity == Severity::Warning) st.warnings++;
        }
        f->errors = st.errors;
        f->warnings = st.warnings;
    }
    st.status = f->trusted ? 'v'
                           : (st.errors > 0 ? 'x'
                                            : ((st.hasRequire || st.hasEnsure) ? (st.warnings > 0 ? '?' : 'v') : '?'));
    res_.functions.push_back(st);
    cur_ = nullptr;
}

void Flow::run() {
    startClock_ = clock();
    for (const char* b : {"print", "len", "str", "String", "int", "float", "bool", "Bytes", "List",
                          "Tuple", "sorted", "sum", "zip", "enumerate", "ord", "chr", "min", "max",
                          "abs", "range", "pairs", "alloc", "raw_copy", "Ok", "Err", "is_null",
                          "typeof",
                          // low level file / path primitives (lib/file.mod wraps them)
                          "_file_read", "_file_read_bytes", "_file_write", "_file_append",
                          "_file_exists", "_file_is_dir", "_file_size", "_file_mtime",
                          "_file_remove", "_file_rename", "_file_copy", "_file_write_bytes",
                          "_dir_list", "_dir_make", "_dir_remove", "_dir_walk",
                          "_path_join", "_path_dirname", "_path_basename", "_path_ext",
                          "_path_abs", "_path_normalize", "_cwd", "_chdir",
                          "_stdin_line", "_stdin_all",
                          // low level system primitives (lib/time.mod, lib/os.mod,
                          // lib/thread.mod and lib/net.mod wrap them; docs/reference.md lists them)
                          "_sys_clock", "_sys_time", "_sys_sleep", "_sys_local_time",
                          "_sys_make_time", "_sys_info", "_sys_env", "_sys_env_set",
                          "_sys_env_all", "_sys_exec", "_sys_spawn", "_sys_join",
                          "_sys_task_done", "_sys_socket",
                          // built-in type names (also usable as conversion functions)
                          "int", "long", "longlong", "uint", "ulong", "ulonglong",
                          "int8", "int16", "int32", "int64",
                          "uint8", "uint16", "uint32", "uint64",
                          "float", "double", "longdouble", "float32", "float64",
                          // builtin module namespaces registered by the runtime
                          "math", "io", "json", "time", "net", "os", "thread", "system", "slice"})
        builtinNames_.insert(b);

    collectGlobals(prog_.stmts);
    collectStmts(prog_.stmts, "", false);
    for (auto* f : funcs_)
        if (f->cls.empty() && !f->nested) byName_[f->name] = f;

    // pre-pass: give file level variables their initial abstract state (including taint marks),
    // so that functions analyzed before <main> can see it
    for (auto& s : prog_.stmts) {
        if (!s) continue;
        if (s->kind != SK::New && s->kind != SK::Const && s->kind != SK::State) continue;
        if (s->names.size() > 1 || (!s->name.empty() && s->names.empty())) continue;
        std::string name = s->names.empty() ? s->name : s->names[0];
        if (name.empty()) continue;
        AbsVal v;
        v.init = AbsVal::IsInit;
        v.nullState = AbsVal::NonNull;
        v.type = s->type.empty() ? "unknown" : s->type;
        const ExprP& init = s->initExpr;
        if (init) {
            switch (init->kind) {
                case EK::Int:
                    v.type = init->wideLiteral ? "longlong" : "int";
                    if (!init->wideLiteral) v.range = rng(init->ival, init->ival);
                    break;
                case EK::Float: v.type = "float"; break;
                case EK::Str: v.type = "String"; v.size = rng((long long)init->sval.size(), (long long)init->sval.size()); break;
                case EK::Bool: v.type = "bool"; break;
                case EK::Null: v.type = "null"; v.nullState = AbsVal::IsNull; break;
                case EK::List: v.type = "List"; v.size = rng((long long)init->items.size(), (long long)init->items.size()); break;
                case EK::Tuple: v.type = "Tuple"; v.size = rng((long long)init->items.size(), (long long)init->items.size()); break;
                default: break;
            }
        }
        if (hasAnnotation(s->annotations, "taint")) {
            v.tainted = true;
            const Annotation* a = findAnnotation(s->annotations, "taint");
            v.taintSource = a ? annotationPrimaryText(*a) : "?";
        }
        globalsEnv_.vars[name] = v;
    }

    for (auto* f : funcs_) {
        if (budgetExceeded()) break;
        analyzeFunc(f);
    }
    // top level statements form the implicit `<main>` unit
    if (!budgetExceeded() && !prog_.stmts.empty()) {
        auto* f = new FuncInfo();
        auto holder = std::make_shared<Stmt>();
        holder->kind = SK::Block;
        holder->line = 1;
        holder->body = std::make_shared<Block>();
        for (auto& s : prog_.stmts) holder->body->stmts.push_back(s);
        f->name = "<main>";
        f->qual = "<main>";
        f->def = holder.get();
        mainHolder_ = holder;
        funcs_.push_back(f);
        analyzeFunc(f);
    }

    if (opts_.level >= 3) macroChecks();

    Coverage& cov = res_.coverage;
    cov.functions = (int)res_.functions.size();
    for (auto& f : res_.functions) {
        if (f.hasRequire) cov.withRequire++;
        if (f.hasEnsure) cov.withEnsure++;
        if (f.hasModifies) cov.withModifies++;
        if (f.hasRequire || f.hasEnsure || f.hasModifies) cov.withAny++;
        else cov.uncovered.push_back(f.name.empty() ? ("line " + formatInt(f.line)) : f.name);
    }
    cov.loops = loopsSeen_;
    cov.loopsWithInvariant = loopsWithInvariant_;
    res_.cancelled = cancelled_;
    res_.elapsedMs = (clock() - startClock_) * 1000LL / CLOCKS_PER_SEC;
}

} // namespace

// ---------------------------------------------------------------- driver
void runFlowAnalysis(const Program& prog, const std::string& file, const AnalysisOptions& opts,
                     AnalysisResult& out) {
    Collector col;
    col.res = &out;
    Flow f(prog, file, opts, out, col);
    f.run();
}

void tagStmtOrigin(const StmtP& s, const std::string& origin) {
    if (!s) return;
    s->origin = origin;
    if (s->body) for (auto& x : s->body->stmts) tagStmtOrigin(x, origin);
    if (s->elseBody) for (auto& x : s->elseBody->stmts) tagStmtOrigin(x, origin);
    for (auto& m : s->members) tagStmtOrigin(m, origin);
}

} // namespace detail
} // namespace annota
