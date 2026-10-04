// Annota - analyzer.cpp : annotation registry, layer 1 checks, driver, IDE queries, reporting.
#include "analyzer.hpp"
#include "analysis_internal.hpp"
#include "parser.hpp"
#include "lexer.hpp"
#include "common.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

namespace annota {

// ---------------------------------------------------------------- small helpers
const char* severityName(Severity s) {
    switch (s) {
        case Severity::Error: return "error";
        case Severity::Warning: return "warning";
        default: return "info";
    }
}
Severity severityFromName(const std::string& s) {
    if (s == "low") return Severity::Info;
    if (s == "medium") return Severity::Warning;
    if (s == "high" || s == "critical") return Severity::Error;
    return Severity::Warning;
}

bool hasAnnotation(const std::vector<Annotation>& anns, const char* name) {
    for (auto& a : anns) if (a.name == name) return true;
    return false;
}
const Annotation* findAnnotation(const std::vector<Annotation>& anns, const char* name) {
    for (auto& a : anns) if (a.name == name) return &a;
    return nullptr;
}
std::string annotationPrimaryText(const Annotation& a) {
    for (auto& g : a.args) if (!g.hasKey) return g.text;
    return "";
}
std::string annotationKeyText(const Annotation& a, const char* key, const std::string& def) {
    for (auto& g : a.args) if (g.hasKey && g.key == key) return g.text;
    return def;
}

int stmtLastLine(const StmtP& s) {
    if (!s) return 0;
    int last = s->line;
    auto upd = [&](const ExprP& e) {
        std::function<void(const ExprP&)> walk = [&](const ExprP& x) {
            if (!x) return;
            last = std::max(last, x->line);
            walk(x->a); walk(x->b);
            for (auto& i : x->items) walk(i);
            for (auto& a : x->args) walk(a.value);
            for (auto& c : x->cases) { walk(c.first); walk(c.second); }
            walk(x->elseVal); walk(x->bodyExpr);
        };
        walk(e);
    };
    upd(s->expr); upd(s->target); upd(s->value); upd(s->cond); upd(s->iterable); upd(s->initExpr);
    for (auto& a : s->args) upd(a);
    if (s->body) for (auto& x : s->body->stmts) last = std::max(last, stmtLastLine(x));
    if (s->body && s->body->exceptBody) for (auto& x : s->body->exceptBody->stmts) last = std::max(last, stmtLastLine(x));
    if (s->elseBody) for (auto& x : s->elseBody->stmts) last = std::max(last, stmtLastLine(x));
    for (auto& m : s->members) last = std::max(last, stmtLastLine(m));
    return last;
}
int stmtFirstLine(const StmtP& s) { return s ? s->line : 0; }

// ---------------------------------------------------------------- annotation registry
static AnnotationSpec mk(const char* name, const char* cat, const char* sum, const char* det,
                         int minArgs, int maxArgs, std::vector<std::string> scopes, bool kwargs = false) {
    AnnotationSpec s;
    s.name = name;
    s.category = cat;
    s.summary = sum;
    s.detail = det;
    s.minArgs = minArgs;
    s.maxArgs = maxArgs;
    s.scopes = std::move(scopes);
    s.takesKwargs = kwargs;
    return s;
}

const std::vector<AnnotationSpec>& annotationRegistry() {
    static const std::vector<AnnotationSpec> reg = {
        // ---- 分析控制
        mk("ignore", "分析控制", "忽略下一单元的分析", "不带参数忽略全部分析；带检查名只忽略该检查；可再跟原因与 severity。",
           0, 16, {"any"}, true),
        mk("assume", "分析控制", "分析时假设条件成立", "把条件加入当前抽象状态（例如缩小 range），不会生成运行期代码。",
           1, 1, {"stmt", "func", "loop", "block", "any"}),
        mk("assert", "分析控制", "分析时断言条件成立", "在当前位置检查条件；可证明为假时报错，无法判定时给出 warning。",
           1, 1, {"stmt", "func", "loop", "block", "any"}),
        mk("unreachable", "分析控制", "标记不可达代码", "声明该位置本就不可达，从而关闭死代码检查。",
           0, 1, {"stmt", "any"}),
        mk("propagate", "分析控制", "把本标注传播到函数", "[[propagate: f]] 表示把当前单元的标注语义传播给函数 f。",
           1, 1, {"stmt", "func", "any"}),
        // ---- 安全与信任
        mk("unsafe", "安全与信任", "标记不安全代码", "常用于 FFI 边界；带原因的 unsafe 会降低相关诊断的严重度。",
           0, 1, {"func", "stmt", "any"}, true),
        mk("trusted", "安全与信任", "信任此函数并跳过函数体分析", "整段跳过函数体的数据流分析，require 只作为文档保留。",
           0, 1, {"func", "any"}),
        mk("pure", "安全与信任", "无副作用", "禁止写全局、写字段、调用非纯函数。",
           0, 0, {"func", "any"}),
        mk("noreturn", "安全与信任", "函数不会正常返回", "通常内部 throw 或无限循环；与 ensure: result... 冲突。",
           0, 0, {"func", "any"}),
        // ---- 契约
        mk("require", "契约", "前置条件", "函数入口假定成立；在每个调用点检查实参是否满足。",
           1, 1, {"func"}),
        mk("ensure", "契约", "后置条件", "函数出口检查；可用 result 引用返回值。",
           1, 1, {"func"}),
        mk("invariant", "契约", "循环不变量", "在循环入口、每次迭代末尾检查；只作用于循环。",
           1, 1, {"loop"}),
        mk("decrease", "契约", "终止度量", "要求表达式在每次迭代严格递减且迭代前为正。",
           1, 1, {"loop"}),
        mk("modifies", "契约", "允许修改的对象", "声明副作用范围；被调函数修改范围之外的对象会被报告。",
           1, 1, {"func"}),
        // ---- 元信息
        mk("module", "元信息", "模块名", "文件级元信息。", 1, 1, {"file", "any"}),
        mk("version", "元信息", "版本号", "文件级元信息。", 1, 1, {"file", "any"}),
        mk("author", "元信息", "作者", "文件级元信息。", 1, 1, {"file", "any"}),
        mk("macro_depth", "元信息", "宏展开深度上限", "解析阶段生效，超出即报 Macro expansion depth exceeded。",
           1, 1, {"file", "any"}),
        mk("doc", "元信息", "文档字符串", "悬停时显示。", 1, 1, {"any"}),
        mk("deprecated", "元信息", "弃用提示", "替代方案写在参数里。", 1, 1, {"any"}),
        mk("todo", "元信息", "待办", "不参与分析，仅登记。", 1, 1, {"any"}),
        // ---- 分析器与规则
        mk("analyzer", "分析器", "指定分析器", "例如 polyspace；本实现接受并记录。", 1, 1, {"file", "any"}),
        mk("rule", "分析器", "开关规则", "[[rule: MISRA-C 8.1, on]] / off。", 2, 3, {"file", "any", "func"}),
        mk("complexity", "分析器", "循环复杂度上限", "超过上限时给出 info。", 1, 1, {"func", "file", "any"}),
        mk("max_memory", "分析器", "内存上限", "记录用；本实现不做内存建模。", 1, 1, {"func", "file", "any"}),
        mk("max_time", "分析器", "时间上限", "记录用。", 1, 1, {"func", "file", "any"}),
        mk("taint", "分析器", "污点源", "该处产生的值被标记为污染，传播到 [[unsafe]] 处报告。",
           1, 1, {"stmt", "func", "any"}),
        // ---- 可见性
        mk("expose", "可见性", "暴露给 GUI", "列出暴露给界面层的名字。", 1, 16, {"class", "file", "any"}),
        mk("private", "可见性", "私有成员", "类成员默认私有。", 0, 0, {"field", "func", "class"}),
        mk("public", "可见性", "公有成员", "顶层默认公有。", 0, 0, {"field", "func", "class"}),
        mk("static", "可见性", "静态成员", "字段或方法属于类本身。", 0, 0, {"field", "func"}),
        // ---- 后端
        mk("backend", "后端", "生成目标", "dafny / lean / c / webgl，可叠加。", 1, 1, {"file", "func", "any"}),
    };
    return reg;
}

const AnnotationSpec* findAnnotationSpec(const std::string& name) {
    for (auto& s : annotationRegistry()) if (s.name == name) return &s;
    return nullptr;
}

const char* checkCodeSummary(const std::string& code) {
    static const std::map<std::string, const char*> table = {
        {"syntax-error", "语法错误"},
        {"bracket-mismatch", "括号 / 块没有正确闭合"},
        {"unknown-annotation", "标注不在注册表中，也不是模块用 macro 注册的"},
        {"annotation-scope", "标注用在了不允许的位置（如把 [[invariant]] 写在函数上）"},
        {"annotation-conflict", "两个标注语义冲突（如 [[pure]] 与 [[modifies]]）"},
        {"annotation-argument", "参数个数或取值非法（如 severity 不是 low/medium/high/critical）"},
        {"unknown-check", "[[ignore]] 里写了不存在的检查名"},
        {"duplicate-annotation", "同一个标注重复出现"},
        {"uninitialized", "读取未初始化或未声明的变量"},
        {"unused-variable", "变量 / 参数声明后从未被读取"},
        {"unreachable", "控制流不可达的代码"},
        {"division-by-zero", "除数为 0，或可能为 0"},
        {"overflow", "整数运算超出目标宽度（默认按 32 位判定）"},
        {"out-of-bounds", "索引超出序列长度范围"},
        {"null-dereference", "对 null（或可能为 null）的值取成员 / 下标 / 调方法"},
        {"contract-violation", "调用点可以证伪被调函数的 [[require]]"},
        {"contract-possible", "调用点无法判定被调函数的 [[require]]"},
        {"ensure-violation", "函数出口可以证伪 [[ensure]]"},
        {"invariant-violation", "循环不变量在入口不成立，或迭代后不再成立"},
        {"termination", "[[decrease]] 度量不是正数或没有严格递减"},
        {"purity", "[[pure]] 函数里出现副作用（写全局 / 写字段 / print / 调用非纯函数）"},
        {"taint", "被 [[taint]] 标记的数据流入 [[unsafe]] 区域"},
        {"dead-store", "赋值之后从未被读取就被覆盖"},
        {"infinite-loop", "条件恒为真且循环体没有退出路径"},
        {"type-mismatch", "声明类型、运算符、比较或 for 的可迭代性不匹配"},
        {"iterator-misuse", "把 (1 to 10) 迭代器当作普通值使用"},
        {"macro-side-effect", "宏体重复展开含函数调用的实参，可能重复副作用"},
        {"redundant-condition", "条件在当前抽象状态下恒真或恒假"},
        {"scope", "读取已经结束的裸作用域里的变量"},
        {"except-placement", "except 没有跟在块的最后"},
        {"assert-violation", "[[assert]] 可以证伪"},
        {"assert-unproven", "[[assert]] 无法证明（分析器精度不足）"},
    };
    auto it = table.find(code);
    return it == table.end() ? "未知检查" : it->second;
}

const std::vector<std::string>& checkCodeRegistry() {
    static const std::vector<std::string> codes = {
        "syntax-error", "bracket-mismatch", "unknown-annotation", "annotation-scope",
        "annotation-conflict", "annotation-argument", "unknown-check", "duplicate-annotation",
        "uninitialized", "unused-variable", "unreachable", "division-by-zero", "overflow",
        "out-of-bounds", "null-dereference", "contract-violation", "contract-possible",
        "ensure-violation", "invariant-violation", "termination", "purity", "taint",
        "dead-store", "infinite-loop", "type-mismatch", "iterator-misuse", "macro-side-effect",
        "redundant-condition", "scope", "except-placement",
        "assert-violation", "assert-unproven",
    };
    return codes;
}

static bool isKnownCheckCode(const std::string& c) {
    for (auto& s : checkCodeRegistry()) if (s == c) return true;
    return c == "all";
}

// ---------------------------------------------------------------- layer 1 walk
namespace {

const char* scopeOfStmt(SK k) {
    switch (k) {
        case SK::FuncDef: return "func";
        case SK::ClassDef: return "class";
        case SK::FieldDecl: return "field";
        case SK::For: case SK::While: return "loop";
        case SK::Block: return "block";
        case SK::Annot: return "file";
        default: return "stmt";
    }
}

bool scopeAllowed(const AnnotationSpec& spec, const std::string& scope) {
    for (auto& s : spec.scopes) {
        if (s == "any") return true;
        if (s == scope) return true;
        // every unit (function, class, loop, ...) is also a statement, so an annotation that
        // accepts plain statements accepts those positions too
        if (s == "stmt") return true;
    }
    return false;
}

void collectMacroAnnotations(const Program& prog, std::set<std::string>& out) {
    for (auto& m : prog.macros) {
        if (m.pattern.size() >= 3 && m.pattern[0].type == T::LBracket && m.pattern[1].type == T::LBracket &&
            m.pattern[2].type == T::Ident)
            out.insert(m.pattern[2].text);
    }
}

struct Layer1 {
    detail::Collector& col;
    const std::set<std::string>& macroAnnotations;
    int intBits = 32;

    void checkAnnotations(const std::vector<Annotation>& anns, const StmtP& stmt, const char* scope,
                          int lineFrom, int lineTo) {
        std::set<std::string> seen;
        bool hasPure = false, hasModifies = false, hasNoreturn = false, hasEnsure = false;
        bool hasTrusted = false, hasRequire = false, hasPrivate = false, hasPublic = false, hasUnsafe = false;
        for (auto& a : anns) {
            const AnnotationSpec* spec = findAnnotationSpec(a.name);
            bool isMacro = macroAnnotations.count(a.name) > 0;
            if (!spec && !isMacro) {
                col.warn("unknown-annotation", a.line,
                         "未知标注 [[" + a.name + "]]",
                         "标注必须是语言内置的，或由模块用 `macro [[名字: $x]]( ... )` 注册。",
                         "检查拼写，或 `use` 提供该标注的模块");
                continue;
            }
            if (spec) {
                if (!scopeAllowed(*spec, scope))
                    col.error("annotation-scope", a.line,
                              "[[" + a.name + "]] 不能用于此位置",
                              std::string("当前作用于 ") + scope + "，该标注只允许用于: " +
                                  [&] { std::string s; for (auto& x : spec->scopes) { if (!s.empty()) s += "/"; s += x; } return s; }(),
                              "把它移到允许的位置，例如函数声明之前");
                int npos = 0;
                for (auto& g : a.args) if (!g.hasKey) npos++;
                if (npos < spec->minArgs)
                    col.error("annotation-argument", a.line, "[[" + a.name + "]] 缺少参数",
                              "至少需要 " + formatInt(spec->minArgs) + " 个位置参数", "");
                if (npos > spec->maxArgs)
                    col.error("annotation-argument", a.line, "[[" + a.name + "]] 参数过多",
                              "最多 " + formatInt(spec->maxArgs) + " 个位置参数，实际 " + formatInt(npos), "");
                for (auto& g : a.args) {
                    if (g.hasKey && !spec->takesKwargs)
                        col.warn("annotation-argument", a.line,
                                 "[[" + a.name + "]] 不支持键值参数 " + g.key, "", "");
                    if (g.expr == nullptr && !g.text.empty() && g.text != "on" && g.text != "off") {
                        // only contracts and assume/assert need real expressions
                        if (a.name == "require" || a.name == "ensure" || a.name == "invariant" ||
                            a.name == "assert" || a.name == "assume" || a.name == "decrease")
                            col.warn("annotation-argument", a.line,
                                     "[[" + a.name + "]] 的参数不是合法表达式: " + g.text, "", "");
                    }
                }
                if (a.name == "ignore") {
                    for (auto& g : a.args) {
                        if (g.hasKey) continue;
                        if (!g.text.empty() && g.text[0] == '"') continue;      // a reason string
                        if (!isKnownCheckCode(g.text))
                            col.warn("unknown-check", a.line, "[[ignore: " + g.text + "]] 引用了未知检查",
                                     "可用检查: 见 [[ignore]] 的悬停文档", "改为已有检查名，或直接 [[ignore]]");
                    }
                    std::string sev = annotationKeyText(a, "severity");
                    if (!sev.empty() && sev != "low" && sev != "medium" && sev != "high" && sev != "critical")
                        col.warn("annotation-argument", a.line, "severity 取值非法: " + sev,
                                 "允许 low / medium / high / critical", "改为 medium");
                }
                if (a.name == "unsafe") {
                    std::string sev = annotationKeyText(a, "severity");
                    if (!sev.empty() && sev != "low" && sev != "medium" && sev != "high" && sev != "critical")
                        col.warn("annotation-argument", a.line, "severity 取值非法: " + sev, "", "");
                }
            }
            if (!seen.insert(a.name).second) {
                if (a.name != "require" && a.name != "ensure" && a.name != "backend" && a.name != "rule")
                    col.info("duplicate-annotation", a.line, "重复的 [[" + a.name + "]]", "", "");
            }
            hasPure |= a.name == "pure";
            hasModifies |= a.name == "modifies";
            hasNoreturn |= a.name == "noreturn";
            hasEnsure |= a.name == "ensure";
            hasTrusted |= a.name == "trusted";
            hasRequire |= a.name == "require";
            hasPrivate |= a.name == "private";
            hasPublic |= a.name == "public";
            hasUnsafe |= a.name == "unsafe";
        }
        int line = stmt ? stmt->line : lineFrom;
        if (hasPure && hasModifies)
            col.error("annotation-conflict", line, "[[pure]] 与 [[modifies]] 冲突",
                      "纯函数不允许声明副作用范围", "删除其中一个");
        if (hasNoreturn && hasEnsure)
            col.error("annotation-conflict", line, "[[noreturn]] 与 [[ensure: result...]] 冲突",
                      "不会返回的函数没有 result", "删除 ensure 或 noreturn");
        if (hasPrivate && hasPublic)
            col.error("annotation-conflict", line, "[[private]] 与 [[public]] 冲突", "", "只保留一个");
        if (hasUnsafe && hasTrusted)
            col.error("annotation-conflict", line, "[[unsafe]] 与 [[trusted]] 冲突",
                      "既标记不安全又完全信任同一段代码", "只保留一个");
        if (hasTrusted && hasRequire)
            col.warn("annotation-conflict", line, "[[trusted]] 覆盖 [[require]]",
                      "trusted 会跳过函数体分析，调用点也不再检查 require", "如仍需检查请去掉 trusted");
    }

    void walkStmt(const StmtP& s, const char* scope) {
        if (!s) return;
        const char* sc = scope ? scope : scopeOfStmt(s->kind);
        int from = s->line, to = stmtLastLine(s);
        size_t mark = col.active.size();
        for (auto& a : s->annotations) {
            if (a.name == "ignore") col.pushIgnore(a, from, to);
        }
        checkAnnotations(s->annotations, s, sc, from, to);
        // nested units are checked by the flow walker as well; here we only recurse to reach
        // annotations that belong to statements without a function body
        switch (s->kind) {
            case SK::FuncDef:
                if (s->body) for (auto& x : s->body->stmts) walkStmt(x, nullptr);
                break;
            case SK::ClassDef:
                for (auto& m : s->members) walkStmt(m, nullptr);
                break;
            case SK::If:
                if (s->body) for (auto& x : s->body->stmts) walkStmt(x, nullptr);
                if (s->elseBody) for (auto& x : s->elseBody->stmts) walkStmt(x, nullptr);
                break;
            case SK::For: case SK::While:
                if (s->body) for (auto& x : s->body->stmts) walkStmt(x, nullptr);
                break;
            case SK::Block:
                if (s->body) for (auto& x : s->body->stmts) walkStmt(x, nullptr);
                break;
            case SK::Use:
                if (s->body) for (auto& x : s->body->stmts) walkStmt(x, nullptr);
                break;
            default: break;
        }
        col.popTo(mark);
    }
};
} // namespace

// ---------------------------------------------------------------- driver
namespace {
struct FileLoader : ModuleLoader {
    std::string baseDir;
    std::vector<std::string> dirs;
    explicit FileLoader(std::string dir) : baseDir(std::move(dir)) {
        dirs = {baseDir, baseDir + "/lib", "lib", "."};
    }
    static bool exists(const std::string& p) {
        std::FILE* f = std::fopen(p.c_str(), "rb");
        if (!f) return false;
        std::fclose(f);
        return true;
    }
    bool loadModule(const std::string& spec, std::vector<Token>& toks, std::string& file) override {
        std::vector<std::string> cands;
        bool pathLike = spec.find('/') != std::string::npos || spec.find('\\') != std::string::npos ||
                        spec.find(".mod") != std::string::npos;
        if (pathLike) {
            cands.push_back(spec);
            for (auto& d : dirs) cands.push_back(d + "/" + spec);
        } else {
            for (auto& d : dirs) cands.push_back(d + "/" + spec + ".mod");
            cands.push_back(spec + ".mod");
        }
        for (auto& c : cands) {
            if (!exists(c)) continue;
            std::ifstream in(c, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            file = c;
            toks = lex(ss.str(), c);
            return true;
        }
        return false;
    }
};
} // namespace

static std::string dirOfPath(const std::string& path) {
    size_t q = path.find_last_of("/\\");
    return q == std::string::npos ? std::string(".") : path.substr(0, q);
}

AnalysisResult analyzeSource(const std::string& source, const std::string& file, const AnalysisOptions& opts) {
    long long t0 = clock();
    AnalysisResult res;
    res.file = file;
    for (char c : source) if (c == '\n') res.lines++;
    if (!source.empty() && source.back() != '\n') res.lines++;
    int lines = res.lines;
    std::vector<Token> toks;
    try {
        toks = lex(source, file);
    } catch (CompileError& e) {
        detail::Collector col;
        col.res = &res;
        std::string code = e.message.find("unterminated") != std::string::npos ? "bracket-mismatch" : "syntax-error";
        col.error(code, e.line, e.message, "", "");
        res.elapsedMs = (clock() - t0) * 1000LL / CLOCKS_PER_SEC;
        return res;
    }
    FileLoader loader(dirOfPath(file));
    MacroRegistry registry;
    Program program;
    try {
        Parser parser(toks, file, &loader, &registry);
        program = parser.parse();
    } catch (CompileError& e) {
        detail::Collector col;
        col.res = &res;
        std::string code = "syntax-error";
        std::string msg = e.message;
        if (msg.find("closing the block") != std::string::npos || msg.find("unbalanced") != std::string::npos)
            code = "bracket-mismatch";
        if (msg.find("'except'") != std::string::npos || msg.find("unexpected 'except'") != std::string::npos)
            code = "except-placement";
        col.error(code, e.line, msg, "", "");
        res.elapsedMs = (clock() - t0) * 1000LL / CLOCKS_PER_SEC;
        return res;
    }
    AnalysisResult out = analyzeProgram(program, file, opts);
    out.lines = lines;
    out.elapsedMs = (clock() - t0) * 1000LL / CLOCKS_PER_SEC;   // lex + parse + analysis
    return out;
}

AnalysisResult analyzeProgram(const Program& prog, const std::string& file, const AnalysisOptions& opts) {
    AnalysisResult res;
    res.file = file;
    detail::Collector col;
    col.res = &res;

    std::set<std::string> macroAnnotations;
    collectMacroAnnotations(prog, macroAnnotations);

    // suppression bookkeeping (all [[ignore]] sites, used or not)
    std::function<void(const StmtP&)> collectSuppressions = [&](const StmtP& s) {
        if (!s) return;
        for (auto& a : s->annotations) {
            if (a.name != "ignore") continue;
            Suppression sup;
            sup.line = a.line;
            for (auto& g : a.args) {
                if (g.hasKey) continue;
                std::string t = g.text;
                if (!t.empty() && t[0] == '"') { if (sup.reason.empty()) sup.reason = t; continue; }
                if (isKnownCheckCode(t)) sup.codes.push_back(t);
                else if (sup.reason.empty()) sup.reason = t;
            }
            sup.severity = annotationKeyText(a, "severity", "medium");
            sup.target = "(next unit)";
            res.suppressions.push_back(sup);
        }
        if (s->body) for (auto& x : s->body->stmts) collectSuppressions(x);
        if (s->elseBody) for (auto& x : s->elseBody->stmts) collectSuppressions(x);
        if (s->body && s->body->exceptBody) for (auto& x : s->body->exceptBody->stmts) collectSuppressions(x);
        for (auto& m : s->members) collectSuppressions(m);
    };
    for (auto& s : prog.stmts) collectSuppressions(s);

    // ---- layer 1
    Layer1 l1{col, macroAnnotations, opts.intBits};
    for (auto& s : prog.stmts) l1.walkStmt(s, nullptr);

    // ---- layer 2 / 3
    if (opts.level >= 2) detail::runFlowAnalysis(prog, file, opts, res);

    std::stable_sort(res.diagnostics.begin(), res.diagnostics.end(),
                     [](const Diagnostic& a, const Diagnostic& b) {
                         if (a.line != b.line) return a.line < b.line;
                         return (int)a.severity > (int)b.severity;
                     });
    return res;
}

// ---------------------------------------------------------------- rendering
static std::string sevMark(Severity s) {
    switch (s) {
        case Severity::Error: return "✗";
        case Severity::Warning: return "?";
        default: return "i";
    }
}

void printDiagnosticsText(const AnalysisResult& res, bool verbose) {
    std::printf("%s: %d errors, %d warnings, %d infos (%.1f ms, %d lines)\n",
                res.file.c_str(), res.errors, res.warnings, res.infos, (double)res.elapsedMs, res.lines);
    for (auto& d : res.diagnostics) {
        if (d.suppressed) continue;
        // diagnostics coming from an inlined module carry that module's own file name
        std::string where = d.file.empty() || d.file == res.file ? std::string() : (d.file + ": ");
        std::printf("%4d:%-3d %s %-22s %s%s\n", d.line, d.col, sevMark(d.severity).c_str(), d.code.c_str(),
                    where.c_str(), d.message.c_str());
        if (verbose) {
            if (!d.detail.empty()) std::printf("             %s\n", d.detail.c_str());
            if (!d.fix.empty()) std::printf("             修复: %s\n", d.fix.c_str());
        }
    }
    if (!res.suppressions.empty()) {
        int used = 0;
        for (auto& s : res.suppressions) if (s.used) used++;
        std::printf("suppressions: %d (%d used)\n", (int)res.suppressions.size(), used);
    }
}

Json diagnosticsToJson(const AnalysisResult& res) {
    Json root = Json::object();
    root.set("file", Json::string(res.file));
    root.set("elapsedMs", Json::integer(res.elapsedMs));
    root.set("lines", Json::integer(res.lines));
    root.set("cancelled", Json::boolean(res.cancelled));
    root.set("errors", Json::integer(res.errors));
    root.set("warnings", Json::integer(res.warnings));
    root.set("infos", Json::integer(res.infos));
    Json diags = Json::array();
    for (auto& d : res.diagnostics) {
        Json j = Json::object();
        j.set("line", Json::integer(d.line));
        j.set("column", Json::integer(d.col));
        j.set("endLine", Json::integer(d.endLine ? d.endLine : d.line));
        j.set("endColumn", Json::integer(d.endCol));
        j.set("severity", Json::string(severityName(d.severity)));
        j.set("code", Json::string(d.code));
        j.set("message", Json::string(d.message));
        j.set("detail", Json::string(d.detail));
        j.set("fix", Json::string(d.fix));
        // a diagnostic from an inlined module keeps that module's own file name
        if (!d.file.empty() && d.file != res.file) j.set("file", Json::string(d.file));
        if (!d.symbol.empty()) j.set("symbol", Json::string(d.symbol));
        if (d.suppressed) {
            j.set("suppressed", Json::boolean(true));
            j.set("suppressedBy", Json::string(d.suppressedBy));
        }
        diags.push(j);
    }
    root.set("diagnostics", diags);
    root.set("suppressions", suppressionsJson(res));
    root.set("functions", inlineStatusJson(res));
    root.set("coverage", coverageJson(res));
    root.set("quickFixes", quickFixesJson(res));
    return root;
}

Json analysisSummaryJson(const AnalysisResult& res) {
    Json j = Json::object();
    j.set("file", Json::string(res.file));
    j.set("errors", Json::integer(res.errors));
    j.set("warnings", Json::integer(res.warnings));
    j.set("infos", Json::integer(res.infos));
    j.set("elapsedMs", Json::integer(res.elapsedMs));
    j.set("functions", Json::integer((long long)res.functions.size()));
    j.set("contractCoverage", Json::number(res.coverage.functions
        ? (double)res.coverage.withAny / (double)res.coverage.functions : 0.0));
    j.set("loopInvariantCoverage", Json::number(res.coverage.loops
        ? (double)res.coverage.loopsWithInvariant / (double)res.coverage.loops : 0.0));
    return j;
}

Json inlineStatusJson(const AnalysisResult& res) {
    Json arr = Json::array();
    for (auto& f : res.functions) {
        Json j = Json::object();
        j.set("name", Json::string(f.name));
        if (!f.className.empty()) j.set("class", Json::string(f.className));
        j.set("line", Json::integer(f.line));
        j.set("status", Json::string(std::string(1, f.status)));
        j.set("errors", Json::integer(f.errors));
        j.set("warnings", Json::integer(f.warnings));
        j.set("require", Json::boolean(f.hasRequire));
        j.set("ensure", Json::boolean(f.hasEnsure));
        j.set("modifies", Json::boolean(f.hasModifies));
        j.set("invariant", Json::boolean(f.hasInvariant));
        j.set("decrease", Json::boolean(f.hasDecrease));
        j.set("pure", Json::boolean(f.pure));
        j.set("trusted", Json::boolean(f.trusted));
        arr.push(j);
    }
    return arr;
}

Json coverageJson(const AnalysisResult& res) {
    Json j = Json::object();
    const Coverage& c = res.coverage;
    j.set("functions", Json::integer(c.functions));
    j.set("withRequire", Json::integer(c.withRequire));
    j.set("withEnsure", Json::integer(c.withEnsure));
    j.set("withModifies", Json::integer(c.withModifies));
    j.set("withAnyContract", Json::integer(c.withAny));
    j.set("loops", Json::integer(c.loops));
    j.set("loopsWithInvariant", Json::integer(c.loopsWithInvariant));
    Json un = Json::array();
    for (auto& n : c.uncovered) un.push(Json::string(n));
    j.set("uncovered", un);
    if (c.functions)
        j.set("contractPercent", Json::integer((long long)(100.0 * c.withAny / c.functions)));
    return j;
}

Json suppressionsJson(const AnalysisResult& res) {
    Json arr = Json::array();
    for (auto& s : res.suppressions) {
        Json j = Json::object();
        j.set("line", Json::integer(s.line));
        Json codes = Json::array();
        for (auto& c : s.codes) codes.push(Json::string(c));
        j.set("codes", codes);
        j.set("reason", Json::string(s.reason));
        j.set("severity", Json::string(s.severity));
        j.set("used", Json::boolean(s.used));
        j.set("usedCount", Json::integer(s.usedCount));
        arr.push(j);
    }
    return arr;
}

Json quickFixesJson(const AnalysisResult& res) {
    Json arr = Json::array();
    for (auto& f : res.fixes) {
        Json j = Json::object();
        j.set("line", Json::integer(f.line));
        j.set("column", Json::integer(f.col));
        j.set("title", Json::string(f.title));
        j.set("insertText", Json::string(f.insertText));
        j.set("code", Json::string(f.code));
        arr.push(j);
    }
    return arr;
}

// ---------------------------------------------------------------- IDE: token lookup
namespace {
// operator tokens carry no text, so rebuild their spelling when joining an annotation argument
static std::string tokenSpelling(const Token& t) {
    if (!t.text.empty()) return t.text;
    switch (t.type) {
        case T::Minus: return "-";
        case T::Plus: return "+";
        case T::Star: return "*";
        case T::Slash: return "/";
        case T::Percent: return "%";
        case T::Eq: return "=";
        case T::Dot: return ".";
        case T::Colon: return ":";
        case T::Comma: return ",";
        default: return "";
    }
}

struct TokenHit {
    bool valid = false;
    Token tok;
    bool inAnnotation = false;
    bool inAnnotationArg = false;
    std::string annotationArg;           // the whole `[[name: arg]]` argument the cursor is in
    std::string annotationName;
    int annotationLine = 0;
};
// find the token covering (line, col) and say whether it sits inside [[ ... ]]
TokenHit tokenAt(const std::string& source, const std::string& file, int line, int col) {
    TokenHit hit;
    std::vector<Token> toks;
    try {
        toks = lex(source, file);
    } catch (CompileError&) {
        return hit;
    }
    for (size_t i = 0; i < toks.size(); i++) {
        if (toks[i].line != line) continue;
        if (toks[i].type == T::End || toks[i].type == T::Newline) continue;
        size_t len = toks[i].text.empty() ? 1 : toks[i].text.size();
        if (toks[i].type == T::Str) len += 2;
        if ((int)toks[i].col <= col && col <= (int)toks[i].col + (int)len - 1) {
            hit.valid = true;
            hit.tok = toks[i];
            // is it between '[[' and ']]' on this line?
            for (size_t j = i; j-- > 0;) {
                if (toks[j].line != line) break;
                if (toks[j].type == T::RBracket && j + 1 < toks.size() && toks[j + 1].type == T::RBracket) break;
                if (toks[j].type == T::LBracket && j + 1 < toks.size() && toks[j + 1].type == T::LBracket) {
                    hit.inAnnotation = true;
                    hit.annotationLine = toks[j].line;
                    size_t end = toks.size();
                    for (size_t k = j + 2; k < toks.size(); k++) {
                        if (toks[k].line != line) { end = k; break; }
                        if (toks[k].type == T::RBracket && k + 1 < toks.size() &&
                            toks[k + 1].type == T::RBracket) { end = k; break; }
                    }
                    for (size_t k = j + 2; k < end; k++) {
                        if (toks[k].type == T::Ident) { hit.annotationName = toks[k].text; break; }
                        if (toks[k].type == T::Colon) break;
                    }
                    // the argument the cursor sits in (check codes like `division-by-zero`
                    // arrive as several tokens, so join them back together)
                    size_t argStart = j + 2;
                    if (argStart + 1 < end && toks[argStart].type == T::Ident &&
                        toks[argStart + 1].type == T::Colon)
                        argStart += 2;                       // skip `name:`
                    size_t segStart = argStart;
                    for (size_t k = argStart; k <= i && k < end; k++)
                        if (toks[k].type == T::Comma) segStart = k + 1;
                    std::string seg;
                    for (size_t k = segStart; k < end; k++) {
                        if (toks[k].type == T::Comma) break;
                        seg += tokenSpelling(toks[k]);
                    }
                    hit.annotationArg = seg;
                    hit.inAnnotationArg = !seg.empty();
                    break;
                }
            }
            return hit;
        }
    }
    return hit;
}
} // namespace

HoverInfo hoverAt(const std::string& source, const std::string& file, int line, int col,
                  const AnalysisOptions& opts) {
    HoverInfo h;
    TokenHit hit = tokenAt(source, file, line, col);
    if (!hit.valid) return h;
    h.line = line;
    if (hit.inAnnotation && !hit.annotationName.empty()) {
        // hovering a check code inside [[ignore: code]] documents the check itself
        if (hit.inAnnotationArg && isKnownCheckCode(hit.annotationArg)) {
            h.kind = "check";
            h.name = hit.annotationArg;
            h.title = hit.annotationArg + "  (检查)";
            h.body = std::string(checkCodeSummary(hit.annotationArg)) +
                     "\n\n在 [[ignore]] 里引用它即可只抑制这一条检查。";
            h.valid = true;
            return h;
        }
        const AnnotationSpec* spec = findAnnotationSpec(hit.annotationName);
        h.kind = "annotation";
        h.name = hit.annotationName;
        if (spec) {
            h.title = "[[" + spec->name + "]]  (" + spec->category + ")";
            h.body = spec->summary + "\n\n" + spec->detail;
            std::string scopes;
            for (auto& s : spec->scopes) { if (!scopes.empty()) scopes += " | "; scopes += s; }
            h.body += "\n作用域: " + scopes;
            h.body += "\n参数: " + formatInt(spec->minArgs) + ".." + formatInt(spec->maxArgs);
            if (spec->takesKwargs) h.body += " (+键值参数)";
            h.valid = true;
        } else {
            h.title = "[[" + hit.annotationName + "]]";
            h.body = "该标注由模块通过 macro 注册。";
            h.valid = true;
        }
        return h;
    }
    // variable / function resolution: re-parse and look the name up
    AnalysisResult res = analyzeSource(source, file, opts);
    std::string name = hit.tok.text;
    auto it = res.lineState.find(line);
    if (it != res.lineState.end()) {
        auto v = it->second.find(name);
        if (v != it->second.end()) {
            h.kind = "variable";
            h.name = name;
            h.title = name + " : " + v->second;
            h.body = "分析器推断的抽象状态（第 " + formatInt(line) + " 行）。";
            h.valid = true;
            return h;
        }
    }
    for (auto& f : res.functions) {
        if (f.name == name) {
            h.kind = "function";
            h.name = name;
            h.title = "函数 " + name + "  " + std::string(1, f.status);
            h.body = "契约: ";
            h.body += f.hasRequire ? "require " : "";
            h.body += f.hasEnsure ? "ensure " : "";
            h.body += f.hasModifies ? "modifies " : "";
            if (!f.hasRequire && !f.hasEnsure && !f.hasModifies) h.body += "(无)";
            h.valid = true;
            return h;
        }
    }
    h.kind = "token";
    h.name = name;
    h.title = name;
    using namespace std::string_literals;
    static const std::vector<std::string> builtins = {
        "print", "len", "str", "String", "int", "float", "bool", "Bytes", "List", "Tuple",
        "sorted", "sum", "zip", "enumerate", "ord", "chr", "min", "max", "abs", "range", "pairs",
        "alloc", "raw_copy", "Ok", "Err", "is_null", "typeof"};
    for (auto& b : builtins) if (b == name) {
        h.kind = "builtin";
        h.title = b + "  (内置)";
        h.body = "语言内置函数。";
        h.valid = true;
        return h;
    }
    return h;
}

Location definitionAt(const std::string& source, const std::string& file, int line, int col,
                      const AnalysisOptions& opts) {
    Location loc;
    TokenHit hit = tokenAt(source, file, line, col);
    if (!hit.valid) return loc;
    if (hit.inAnnotation && !hit.annotationName.empty()) {
        // `[[ignore: overflow]]` jumps to the *check's* definition, other annotations jump to
        // the annotation registry entry
        if (hit.inAnnotationArg && isKnownCheckCode(hit.annotationArg)) {
            loc.valid = true;
            loc.file = "annota://checks/" + hit.annotationArg;
            loc.line = 1;
            loc.col = 1;
            loc.endCol = (int)hit.annotationArg.size() + 1;
            return loc;
        }
        loc.valid = true;
        loc.file = "annota://annotations/" + hit.annotationName;
        loc.line = 1;
        loc.col = 1;
        loc.endCol = 2;
        return loc;
    }
    std::string name = hit.tok.text;
    AnalysisResult res = analyzeSource(source, file, opts);
    for (auto& f : res.functions) {
        if (f.name == name) {
            loc.valid = true;
            loc.file = file;
            loc.line = f.line;
            loc.col = 1;
            loc.endCol = (int)name.size() + 1;
            return loc;
        }
    }
    // search declarations
    struct Finder {
        const std::string& name;
        std::string file;
        Location& out;
        void walk(const StmtP& s) {
            if (!s || out.valid) return;
            if ((s->kind == SK::New || s->kind == SK::Const || s->kind == SK::State) ) {
                for (auto& n : s->names) if (n == name) { set(s->line, n); return; }
                if (s->name == name) { set(s->line, name); return; }
            }
            if (s->kind == SK::FuncDef && s->name == name) { set(s->line, name); return; }
            if (s->kind == SK::ClassDef && s->name == name) { set(s->line, name); return; }
            if (s->kind == SK::For) for (auto& v : s->loopVars) if (v == name) { set(s->line, v); return; }
            for (auto& p : s->params) if (p.name == name) { set(s->line, p.name); return; }
            if (s->body) for (auto& x : s->body->stmts) walk(x);
            if (s->elseBody) for (auto& x : s->elseBody->stmts) walk(x);
            if (s->body && s->body->exceptBody) {
                if (s->body->exceptName == name) { set(s->body->exceptLine, name); return; }
                for (auto& x : s->body->exceptBody->stmts) walk(x);
            }
            for (auto& m : s->members) walk(m);
        }
        void set(int line, const std::string& n) {
            out.valid = true;
            out.file = file;
            out.line = line;
            out.col = 1;
            out.endCol = (int)n.size() + 1;
        }
    };
    Finder finder{name, file, loc};
    FileLoader loader(dirOfPath(file));
    MacroRegistry registry;
    try {
        std::vector<Token> toks = lex(source, file);
        Parser parser(toks, file, &loader, &registry);
        Program prog = parser.parse();
        for (auto& s : prog.stmts) finder.walk(s);
    } catch (CompileError&) {
    }
    return loc;
}

RenameResult renameAt(const std::string& source, const std::string& file, int line, int col,
                      const std::string& newName, const AnalysisOptions& opts) {
    RenameResult r;
    (void)opts;
    TokenHit hit = tokenAt(source, file, line, col);
    if (!hit.valid) {
        r.message = "该位置没有可重命名的标识符";
        return r;
    }
    // reject keywords
    {
        std::vector<Token> probe;
        try { probe = lex(newName + "\n", "<rename>"); } catch (CompileError&) {}
        if (!probe.empty() && probe[0].type != T::Ident) {
            r.message = "'" + newName + "' 不是合法的标识符（可能是关键字）";
            return r;
        }
    }
    std::string target = hit.inAnnotation && !hit.annotationName.empty() ? hit.annotationName : hit.tok.text;
    std::vector<Token> toks;
    try {
        toks = lex(source, file);
    } catch (CompileError& e) {
        r.message = e.message;
        return r;
    }
    for (size_t i = 0; i < toks.size(); i++) {
        if (toks[i].line != line && toks[i].text != target) continue;
        bool isAnnotationName = false;
        if (toks[i].type == T::Ident && i >= 2 && toks[i - 1].type == T::LBracket && toks[i - 2].type == T::LBracket)
            isAnnotationName = true;
        if (toks[i].type != T::Ident) continue;
        if (toks[i].text != target) continue;
        if (hit.inAnnotation != isAnnotationName && !hit.inAnnotation) {
            // renaming a variable must not touch an annotation with the same name and vice versa
            if (isAnnotationName) continue;
        }
        if (isAnnotationName && !hit.inAnnotation) continue;
        TextEdit e;
        e.line = toks[i].line;
        e.col = toks[i].col;
        e.endCol = toks[i].col + (int)target.size();
        e.newText = newName;
        r.edits.push_back(e);
    }
    if (r.edits.empty()) {
        r.message = "没有找到可重命名的位置";
        return r;
    }
    r.ok = true;
    r.message = "将被替换 " + formatInt((long long)r.edits.size()) + " 处";
    return r;
}

std::vector<CompletionItem> completionsFor(const std::string& prefix, bool afterDoubleBracket) {
    std::vector<CompletionItem> out;
    if (afterDoubleBracket || prefix.rfind("[[", 0) == 0) {
        std::string p = afterDoubleBracket ? prefix : prefix.substr(2);
        for (auto& s : annotationRegistry()) {
            if (!p.empty() && s.name.compare(0, p.size(), p) != 0) continue;
            CompletionItem c;
            c.label = s.name;
            c.kind = "annotation";
            c.detail = s.category + " - " + s.summary;
            c.insertText = s.name;
            if (s.minArgs > 0) c.insertText += ": ";
            out.push_back(c);
        }
        return out;
    }
    static const std::vector<std::pair<std::string, std::string>> kw = {
        {"new", "声明变量"}, {"del", "删除变量"}, {"const", "常量"}, {"if", "条件"},
        {"else", "否则"}, {"for", "循环"}, {"while", "当循环"}, {"in", "成员/遍历"},
        {"to", "区间"}, {"step", "步长"}, {"throw", "抛出"}, {"except", "捕获"},
        {"print", "输出"}, {"input", "输入"}, {"use", "载入模块"}, {"macro", "宏"},
        {"view", "视图"}, {"state", "响应式状态"}, {"return", "用 = 表达式"},
    };
    for (auto& k : kw) {
        if (!prefix.empty() && k.first.compare(0, prefix.size(), prefix) != 0) continue;
        out.push_back({k.first, "keyword", k.second, k.first});
    }
    return out;
}

} // namespace annota
