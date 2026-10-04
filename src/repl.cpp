// Annota - repl.cpp : an interactive session on top of the bytecode interpreter.
//
// Every input line (or balanced multi-line chunk) is parsed with the *persistent* macro registry
// and compiled into the *persistent* VM, so globals, classes and macros accumulate across inputs.
#include "repl.hpp"
#include "parser.hpp"
#include "compiler.hpp"
#include "vm.hpp"
#include "builtins.hpp"
#include "analyzer.hpp"
#include "common.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>

namespace annota {
namespace {

// Same search order as the file runner.
struct ReplLoader : ModuleLoader {
    std::vector<std::string> dirs;
    ReplLoader() { dirs = {".", "lib"}; }
    void remember(const std::string& file) {
        std::string d = dirOf(file);
        for (auto& s : dirs) if (s == d) return;
        dirs.insert(dirs.begin(), d);
    }
    static std::string dirOf(const std::string& path) {
        size_t q = path.find_last_of("/\\");
        return q == std::string::npos ? std::string(".") : path.substr(0, q);
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
            remember(c);
            return true;
        }
        return false;
    }
};

bool g_showBytecode = false;
bool g_showTokens = false;

std::string readFileText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Is the input a complete chunk?  Uses the lexer: unbalanced brackets or an unterminated
// string / comment mean "keep reading".
bool needsMoreInput(const std::string& src) {
    std::vector<Token> toks;
    try {
        toks = lex(src, "<repl>");
    } catch (CompileError& e) {
        const std::string& m = e.message;
        if (m.find("unterminated") != std::string::npos) return true;
        return false;   // a real lexical error - report it now
    }
    int depth = 0;
    for (auto& t : toks) {
        switch (t.type) {
            case T::LParen: case T::LBracket: case T::LBrace: depth++; break;
            case T::RParen: case T::RBracket: case T::RBrace: depth--; break;
            default: break;
        }
    }
    return depth > 0;
}

void printSourceError(const std::string& src, const CompileError& e) {
    std::fprintf(stderr, "error: %s\n", e.message.c_str());
    std::istringstream in(src);
    std::string line;
    int n = 1;
    while (std::getline(in, line) && n < e.line) n++;
    if (n == e.line && !line.empty()) std::fprintf(stderr, "  %s\n", line.c_str());
}

std::string trimCopy(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

void printHelp() {
    std::printf(
        "REPL commands (everything else is Annota code):\n"
        "  :help              this text\n"
        "  :quit / :q         leave the session\n"
        "  :globals           list defined globals\n"
        "  :macros            list registered macros (including module ones)\n"
        "  :bc <code>         compile <code> and disassemble it\n"
        "  :tokens <code>     show the token stream (after macro expansion)\n"
        "  :load <file>       execute a file inside this session\n"
        "  :analyze           run the static analyzer over everything typed so far\n"
        "  :reset             forget all globals, classes and macros\n"
        "  :state             show the variable state the analyzer infers for the last input\n"
        "Input may span several lines: the prompt '...>' continues until brackets balance.\n"
        "A bare expression is printed automatically.\n");
}

// `:bc code` / `:analyze` reuse the normal front end.
void disassembleChunk(const std::shared_ptr<Chunk>& ch, int depth);

void disassembleChunk(const std::shared_ptr<Chunk>& ch, int depth) {
    std::string pad((size_t)depth * 2, ' ');
    std::printf("%s== %s (locals=%d params=%d) ==\n", pad.c_str(), ch->fnName.c_str(),
                ch->numLocals, (int)ch->params.size());
    size_t i = 0;
    while (i < ch->code.size()) {
        size_t at = i;
        uint8_t op = ch->code[i++];
        std::string extra;
        auto u16 = [&]() { uint16_t v = (uint16_t)((ch->code[i] << 8) | ch->code[i + 1]); i += 2; return v; };
        auto s16 = [&]() { int16_t v = (int16_t)((ch->code[i] << 8) | ch->code[i + 1]); i += 2; return v; };
        auto u8 = [&]() { return ch->code[i++]; };
        switch (op) {
            case OP_CONST: { uint16_t k = u16(); const Value& v = ch->consts[k]; extra = formatInt(k) + " (" + (v.t == VT::Str ? "\"" + v.o->str + "\"" : std::string(v.typeName())) + ")"; break; }
            case OP_GET_GLOBAL: case OP_SET_GLOBAL: case OP_DEF_GLOBAL: case OP_DEL_GLOBAL:
            case OP_GET_FIELD: case OP_SET_FIELD: { uint16_t k = u16(); extra = ch->consts[k].o ? ch->consts[k].o->str : "?"; break; }
            case OP_JUMP: case OP_JUMP_IF_FALSE: case OP_JUMP_IF_FALSE_KEEP: case OP_JUMP_IF_TRUE_KEEP:
            case OP_LOOP: case OP_TRY: case OP_ITER_NEXT: { int16_t j = s16(); extra = "-> " + formatInt((int64_t)i + j); break; }
            case OP_BUILD_LIST: case OP_BUILD_TUPLE: case OP_BUILD_NAMED: { uint16_t n = u16(); extra = formatInt(n); break; }
            case OP_GET_LOCAL: case OP_SET_LOCAL: case OP_INIT_LOCAL: case OP_DEL_LOCAL:
            case OP_GET_UPVAL: case OP_SET_UPVAL: case OP_UI_APPEND: { uint8_t s = u8(); extra = formatInt(s); break; }
            case OP_CLASS: case OP_CLOSURE: case OP_INIT_CLASS: { uint16_t k = u16(); extra = "#" + formatInt(k); break; }
            case OP_INT1: { int8_t v = (int8_t)u8(); extra = formatInt(v); break; }
            case OP_PRINT: { uint8_t n = u8(); uint8_t s = u8(); extra = formatInt(n) + (s ? " sep" : ""); break; }
            default: break;
        }
        std::printf("%s%5d  %-18s %s\n", pad.c_str(), (int)at, opName(op), extra.c_str());
    }
    for (auto& c : ch->consts)
        if (c.t == VT::Function && c.o->chunk.get() != ch.get()) disassembleChunk(c.o->chunk, depth + 1);
}

} // namespace

int runRepl(const std::vector<std::string>& preload) {
    VM vm;
    registerBuiltins(vm);
    MacroRegistry registry;
    ReplLoader loader;
    std::string history;          // everything typed so far (used by :analyze)

    auto runSource = [&](const std::string& src, const std::string& name, bool autoPrint) -> bool {
        Program program;
        std::vector<Token> toks;
        try {
            toks = lex(src, name);
        } catch (CompileError& e) {
            printSourceError(src, e);
            return false;
        }
        if (g_showTokens) {
            for (size_t i = 0; i < toks.size(); i++)
                std::printf("%4d %-16s '%s' line %d\n", (int)i, tokenName(toks[i].type), toks[i].text.c_str(), toks[i].line);
        }
        try {
            Parser parser(toks, name, &loader, &registry);
            program = parser.parse();
        } catch (CompileError& e) {
            printSourceError(src, e);
            return false;
        }
        // a lone expression is echoed
        if (autoPrint && program.stmts.size() == 1 && program.stmts[0]->kind == SK::Expr) {
            std::string wrapped = "print (" + src + ")";
            try {
                std::vector<Token> t2 = lex(wrapped, name);
                Parser p2(std::move(t2), name, &loader, &registry);
                Program p2prog = p2.parse();
                program = p2prog;
            } catch (CompileError&) {
                // keep the original program
            }
        }
        CompileResult compiled;
        try {
            Compiler compiler(program, name, false);
            compiled = compiler.compile();
        } catch (CompileError& e) {
            printSourceError(src, e);
            return false;
        }
        if (g_showBytecode) disassembleChunk(compiled.main, 0);
        for (auto& s : compiled.states) vm.stateNames.insert(s);
        vm.mainChunk = compiled.main;
        try {
            vm.run();
        } catch (CompileError& e) {
            printSourceError(src, e);
            vm.frames.clear(); vm.stack.clear(); vm.tryFrames.clear();
            return false;
        } catch (VMError& e) {
            std::fprintf(stderr, "runtime error: %s\n", e.message.c_str());
            std::string trace = vm.stackTrace();
            if (!trace.empty()) std::fprintf(stderr, "%s", trace.c_str());
            vm.frames.clear(); vm.stack.clear(); vm.tryFrames.clear();
            return false;
        }
        return true;
    };

    std::printf("Annota %s - type :help for commands, :quit to leave.\n", "1.0");
    for (auto& f : preload) {
        std::string src = readFileText(f);
        if (src.empty()) { std::fprintf(stderr, "annota: cannot read '%s'\n", f.c_str()); continue; }
        loader.remember(f);
        std::printf("[loaded %s]\n", f.c_str());
        runSource(src, f, false);
    }

    bool interactive = true;
    std::string pending;
    while (interactive) {
        std::printf("%s", pending.empty() ? "annota> " : "   ...> ");
        std::fflush(stdout);
        std::string line;
        if (!std::getline(std::cin, line)) { std::printf("\n"); break; }
        if (pending.empty()) {
            std::string t = trimCopy(line);
            if (t.empty()) continue;
            if (t == ":quit" || t == ":q" || t == ":exit") break;
            if (t == ":help" || t == ":h" || t == ":?") { printHelp(); continue; }
            if (t == ":globals" || t == ":g") {
                std::vector<std::string> names;
                for (auto& kv : vm.globals) names.push_back(kv.first);
                std::sort(names.begin(), names.end());
                if (names.empty()) std::printf("(no globals)\n");
                for (auto& n : names) {
                    Value v;
                    if (vm.getGlobal(n, v)) {
                        Value copy = v;
                        std::string repr;
                        try { repr = vm.repr(copy); } catch (VMError&) { repr = "<...>"; }
                        if (repr.size() > 70) repr = repr.substr(0, 67) + "...";
                        std::printf("  %-16s %s\n", n.c_str(), repr.c_str());
                    }
                }
                continue;
            }
            if (t == ":macros") {
                if (registry.macros.empty()) std::printf("(no macros)\n");
                for (auto& m : registry.macros)
                    std::printf("  %-20s from %s:%d\n", m.name.c_str(), m.file.c_str(), m.line);
                continue;
            }
            if (t == ":reset") {
                vm = VM();
                registerBuiltins(vm);
                registry = MacroRegistry();
                history.clear();
                std::printf("[session reset]\n");
                continue;
            }
            if (t.rfind(":load ", 0) == 0 || t == ":load") {
                std::string path = trimCopy(t.size() > 6 ? t.substr(6) : "");
                if (path.empty()) { std::printf("usage: :load <file>\n"); continue; }
                std::string src = readFileText(path);
                if (src.empty()) { std::fprintf(stderr, "annota: cannot read '%s'\n", path.c_str()); continue; }
                loader.remember(path);
                runSource(src, path, false);
                history += "\n" + src;
                continue;
            }
            if (t.rfind(":analyze", 0) == 0) {
                // `:analyze` analyses the session history; `:analyze <code>` analyses that snippet
                std::string snippet = t.size() > 8 ? trim(t.substr(8)) : "";
                std::string source = snippet.empty() ? history : snippet;
                if (source.empty()) { std::printf("(nothing to analyze)\n"); continue; }
                AnalysisOptions opts;
                opts.level = 3;
                AnalysisResult res = analyzeSource(source, snippet.empty() ? "<repl>" : "<repl:snippet>", opts);
                printDiagnosticsText(res, true);
                continue;
            }
            if (t == ":state") {
                std::printf("(use :analyze for a full report)\n");
                continue;
            }
            if (t.rfind(":bc ", 0) == 0) {
                std::string code = t.substr(4);
                try {
                    std::vector<Token> tk = lex(code, "<repl>");
                    Parser p(tk, "<repl>", &loader, &registry);
                    Program prog = p.parse();
                    Compiler c(prog, "<repl>", false);
                    CompileResult cr = c.compile();
                    disassembleChunk(cr.main, 0);
                } catch (CompileError& e) {
                    std::fprintf(stderr, "error: %s\n", e.message.c_str());
                }
                continue;
            }
            if (t.rfind(":tokens ", 0) == 0) {
                std::string code = t.substr(8);
                try {
                    std::vector<Token> tk = lex(code, "<repl>");
                    for (size_t i = 0; i < tk.size(); i++)
                        std::printf("%4d %-16s '%s' line %d\n", (int)i, tokenName(tk[i].type), tk[i].text.c_str(), tk[i].line);
                } catch (CompileError& e) {
                    std::fprintf(stderr, "error: %s\n", e.message.c_str());
                }
                continue;
            }
            if (!t.empty() && t[0] == ':') {
                std::printf("unknown command '%s' (try :help)\n", t.c_str());
                continue;
            }
        }
        pending += line + "\n";
        if (needsMoreInput(pending)) continue;
        std::string src = pending;
        pending.clear();
        if (trimCopy(src).empty()) continue;
        history += "\n" + src;
        runSource(src, "<repl>", true);
    }
    return 0;
}

} // namespace annota
