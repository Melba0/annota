// Annota - ide_cmd.cpp : `analyze`, `analyze-suite` and the `ide` query commands.
#include "commands.hpp"
#include "vm.hpp"
#include "parser.hpp"
#include "lexer.hpp"
#include "compiler.hpp"
#include "builtins.hpp"
#include "ffi.hpp"
#include "analyzer.hpp"
#include <chrono>
#include "sys_api.hpp"
#include "json.hpp"
#include "common.hpp"
#include <filesystem>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace annota {

namespace fs = std::filesystem;

namespace {

std::string readAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// collect `-` args
struct Args {
    std::vector<std::string> positional;
    std::string option(const std::string& name, const std::string& def = "") const {
        for (auto& a : positional)
            if (a.compare(0, name.size() + 1, name + "=") == 0) return a.substr(name.size() + 1);
        return def;
    }
    bool flag(const std::string& name) const {
        for (auto& a : positional) if (a == name) return true;
        return false;
    }
    std::vector<std::string> values() const {
        std::vector<std::string> v;
        for (auto& a : positional) if (!a.empty() && a[0] == '-') v.push_back(a);
        return v;
    }
};

Args splitArgs(int argc, char** argv, int from) {
    Args a;
    for (int i = from; i < argc; i++) a.positional.push_back(argv[i]);
    return a;
}

std::vector<std::string> optionValues(const Args& a, const std::string& name) {
    std::vector<std::string> out;
    std::string p = name + "=";
    for (auto& s : a.positional)
        if (s.compare(0, p.size(), p) == 0) out.push_back(s.substr(p.size()));
    return out;
}

AnalysisOptions optionsFrom(const Args& a) {
    AnalysisOptions o;
    o.level = std::atoi(a.option("--level", "3").c_str());
    o.intBits = std::atoi(a.option("--int-bits", "32").c_str());
    o.includeModules = a.flag("--modules");
    o.budgetMs = std::atoll(a.option("--budget", "0").c_str());
    return o;
}

void printUsage() {
    std::printf(
        "usage:\n"
        "  annota analyze <file.ant> [--json] [--level=1|2|3] [--int-bits=32|64]\n"
        "                            [--min-severity=info|warning|error] [--budget=ms] [--modules]\n"
        "  annota analyze-suite <dir> [--budget=ms]     run the checker over *.ant fixtures\n"
        "  annota ide hover      <file> <line> <col>\n"
        "  annota ide definition <file> <line> <col>\n"
        "  annota ide rename     <file> <line> <col> <newName>\n"
        "  annota ide complete   [prefix] [--annotation]\n"
        "  annota ide inline     <file>\n"
        "  annota ide coverage   <file>\n"
        "  annota ide quickfix   <file>\n"
        "  annota ide suppressions <file>\n"
        "  annota ide report     <file>\n"
        "  annota lsp                                    JSON-RPC over stdio (LSP)\n"
        "  annota repl                                   interactive session\n");
}

const char* severityText(Severity s) { return severityName(s); }

} // namespace

// ---------------------------------------------------------------- analyze
int cmdAnalyze(int argc, char** argv) {
    Args a = splitArgs(argc, argv, 1);
    std::vector<std::string> files;
    for (auto& s : a.positional) if (!s.empty() && s[0] != '-') files.push_back(s);
    if (files.empty()) { printUsage(); return 2; }
    std::string file = files[0];
    std::string src = readAll(file);
    if (src.empty()) { std::fprintf(stderr, "annota: cannot read '%s'\n", file.c_str()); return 2; }

    AnalysisOptions opts = optionsFrom(a);
    AnalysisResult res = analyzeSource(src, file, opts);

    if (a.flag("--json")) {
        std::printf("%s\n", jsonDump(diagnosticsToJson(res), true).c_str());
        return res.errors > 0 ? 1 : 0;
    }
    Severity minSev = Severity::Info;
    std::string ms = a.option("--min-severity", "info");
    if (ms == "warning") minSev = Severity::Warning;
    else if (ms == "error") minSev = Severity::Error;

    std::printf("%s: level %d, int %d-bit, %.1f ms, %d lines\n", file.c_str(), opts.level,
                opts.intBits, (double)res.elapsedMs, res.lines);
    int shown = 0;
    for (auto& d : res.diagnostics) {
        if (d.suppressed) continue;
        if ((int)d.severity < (int)minSev) continue;
        shown++;
        // diagnostics inlined from a `use`d module carry that module's own file name
        std::string where = d.file.empty() || d.file == res.file ? std::string() : (d.file + ": ");
        std::printf("%4d:%-3d %-7s %-22s %s%s\n", d.line, d.col, severityText(d.severity), d.code.c_str(),
                    where.c_str(), d.message.c_str());
        if (!d.detail.empty()) std::printf("             %s\n", d.detail.c_str());
        if (!d.fix.empty()) std::printf("             修复: %s\n", d.fix.c_str());
    }
    std::printf("%d errors, %d warnings, %d infos (%d shown); contract coverage %d/%d\n",
                res.errors, res.warnings, res.infos, shown, res.coverage.withAny, res.coverage.functions);
    if (res.cancelled) std::printf("(分析被中断，结果为部分结果)\n");
    return res.errors > 0 ? 1 : 0;
}

// ---------------------------------------------------------------- analyze-suite
namespace {
// `-[ expect: code1 code2 ]-` in the fixture header declares the expected diagnostics
std::vector<std::string> expectedCodes(const std::string& src) {
    std::vector<std::string> out;
    std::istringstream in(src);
    std::string line;
    while (std::getline(in, line)) {
        size_t p = line.find("expect:");
        if (p == std::string::npos) continue;
        size_t end = line.find("]-", p);
        std::string list = line.substr(p + 7, end == std::string::npos ? std::string::npos : end - p - 7);
        std::string cur;
        for (char c : list) {
            if (c == ',' || c == ' ' || c == '\t') {
                if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            } else cur += c;
        }
        if (!cur.empty()) out.push_back(cur);
        break;
    }
    return out;
}
} // namespace

int cmdAnalyzeSuite(int argc, char** argv) {
    Args a = splitArgs(argc, argv, 1);
    std::vector<std::string> files;
    for (auto& s : a.positional) if (!s.empty() && s[0] != '-') files.push_back(s);
    if (files.empty()) { printUsage(); return 2; }
    std::string dir = files[0];
    AnalysisOptions opts = optionsFrom(a);
    opts.level = std::atoi(a.option("--level", "3").c_str());

    // gather fixtures (portable: no shell, no platform specific directory listing)
    std::vector<std::string> fixtures = optionValues(a, "--file");
    if (fixtures.empty()) {
        std::error_code ec;
        fs::recursive_directory_iterator it(fs::path(dir), ec), end;
        if (ec) {
            std::fprintf(stderr, "annota: cannot list '%s' (%s)\n", dir.c_str(), ec.message().c_str());
            return 2;
        }
        for (; it != end; it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            if (it->path().extension() != ".ant") continue;
            fixtures.push_back(it->path().string());
        }
    }
    if (fixtures.empty()) {
        std::fprintf(stderr, "annota: no .ant fixtures under '%s'\n", dir.c_str());
        return 2;
    }
    std::sort(fixtures.begin(), fixtures.end());

    int pass = 0, fail = 0;
    double worst = 0;
    for (auto& fx : fixtures) {
        std::string src = readAll(fx);
        if (src.empty()) continue;
        AnalysisResult res = analyzeSource(src, fx, opts);
        worst = std::max(worst, (double)res.elapsedMs);
        std::vector<std::string> want = expectedCodes(src);
        std::set<std::string> have;         // every reported code (including info level)
        std::set<std::string> haveSevere;   // only warning/error: those must be expected
        for (auto& d : res.diagnostics) {
            if (d.suppressed) continue;
            have.insert(d.code);
            if (d.severity != Severity::Info) haveSevere.insert(d.code);
        }
        std::vector<std::string> missing, unexpected;
        for (auto& w : want) {
            if (w == "none") continue;
            if (!have.count(w)) missing.push_back(w);
        }
        for (auto& h : haveSevere) {
            if (std::find(want.begin(), want.end(), h) == want.end()) unexpected.push_back(h);
        }
        bool wantNone = std::find(want.begin(), want.end(), "none") != want.end();
        if (wantNone) {
            // "none" means: no error and no warning (info level notes are advisory)
            for (auto& h : haveSevere) unexpected.push_back(h);
            have.clear();
        }
        std::string name = fx;
        size_t q = name.find_last_of("/\\");
        if (q != std::string::npos) name = name.substr(q + 1);
        if (missing.empty() && unexpected.empty()) {
            pass++;
            std::printf("PASS  %-28s %d diags, %.0f ms\n", name.c_str(), (int)have.size(), (double)res.elapsedMs);
        } else {
            fail++;
            std::printf("FAIL  %-28s", name.c_str());
            if (!missing.empty()) {
                std::printf(" missing:");
                for (auto& m : missing) std::printf(" %s", m.c_str());
            }
            if (!unexpected.empty()) {
                std::printf(" unexpected:");
                for (auto& u : unexpected) std::printf(" %s", u.c_str());
            }
            std::printf("\n");
            for (auto& d : res.diagnostics) {
                if (d.suppressed || d.severity == Severity::Info) continue;
                std::printf("        %4d %-24s %s\n", d.line, d.code.c_str(), d.message.c_str());
            }
        }
    }
    std::printf("\n%d passed, %d failed, slowest %.0f ms\n", pass, fail, worst);
    if (opts.budgetMs > 0 && worst > (double)opts.budgetMs)
        std::printf("BUDGET EXCEEDED: %.0f ms > %lld ms\n", worst, opts.budgetMs);
    return fail ? 1 : 0;
}

// ---------------------------------------------------------------- benchmark
namespace {
// Build a deterministic ~1000 line program that exercises the whole pipeline.
std::string genBenchSource(int funcs, int& lines) {
    std::string s = "-[ 分析器性能基准 ]-\n\nnew total = 0\n\n";
    for (int i = 0; i < funcs; i++) {
        std::string n = formatInt(i);
        s += "[[require: n > 0 && n < 1000]]\n";
        s += "[[ensure: result >= 0]]\n";
        s += "bench" + n + "(n, xs)(\n";
        s += "    new acc = 0\n";
        s += "    new seen = []\n";
        s += "    [[invariant: acc >= 0]]\n";
        s += "    [[decrease: n - i]]\n";
        s += "    for i in 0 to n(\n";
        s += "        if i % 2 == 0(\n";
        s += "            acc = acc + i\n";
        s += "        ) else (\n";
        s += "            acc = acc + 1\n";
        s += "        )\n";
        s += "        seen.push(acc)\n";
        s += "        if acc > n(\n";
        s += "            acc = acc - n\n";
        s += "        )\n";
        s += "    )\n";
        s += "    new picked = 0\n";
        s += "    for x in seen(\n";
        s += "        picked = picked + x\n";
        s += "    )\n";
        s += "    total = total + picked\n";
        s += "    =acc\n";
        s += ")\n\n";
    }
    lines = 1;
    for (char c : s) if (c == '\n') lines++;
    return s;
}
} // namespace

int cmdBench(int argc, char** argv) {
    Args a = splitArgs(argc, argv, 1);
    int funcs = std::atoi(a.option("--funcs", "80").c_str());
    int repeat = std::atoi(a.option("--repeat", "1").c_str());
    int lines = 0;
    std::string src = genBenchSource(funcs, lines);
    std::printf("annota bench: %d functions, %d lines, %d bytes\n", funcs, lines, (int)src.size());

    struct Budget {
        const char* name;
        long long budget;
    };
    Budget budgets[] = {{"level 1 keystroke ", 50}, {"level 2 save      ", 500}, {"level 3 background", 5000}};
    int failed = 0;
    for (int level = 1; level <= 3; level++) {
        long long best = -1;
        AnalysisResult res;
        for (int r = 0; r < std::max(1, repeat); r++) {
            AnalysisOptions opts;
            opts.level = level;
            res = analyzeSource(src, "<bench>", opts);
            if (best < 0 || res.elapsedMs < best) best = res.elapsedMs;
        }
        bool ok = best <= budgets[level - 1].budget;
        if (!ok) failed++;
        std::printf("%s: %6lld ms  (budget %5lld ms)  %s   diagnostics=%d\n", budgets[level - 1].name,
                    best, budgets[level - 1].budget, ok ? "PASS" : "FAIL", (int)res.diagnostics.size());
    }

    // ---- VM throughput: the number to watch when the interpreter gets faster.  Each case is a
    // self contained script; `ops` is the count of inner iterations so the result is comparable
    // across machines.
    struct VmCase {
        const char* name;
        long long ops;
        const char* src;
    };
    static const VmCase vmCases[] = {
        {"int loop      ", 1000000,
         "new n = 1000000\nnew s = 0\nnew i = 0\nwhile i < n( s = s + i * 2 - 1, i = i + 1 )\nprint s\n"},
        {"call loop     ", 300000,
         "add(a, b) = a + b\nnew n = 300000\nnew s = 0\nnew i = 0\nwhile i < n( s = add(s, i), i = i + 1 )\nprint s\n"},
        {"list loop     ", 200000,
         "new xs = []\nnew i = 0\nwhile i < 100(\n    xs.push(i)\n    i = i + 1\n)\nnew n = 2000\nnew t = 0\nnew k = 0\nwhile k < n(\n    for x in xs( t = t + x )\n    k = k + 1\n)\nprint t\n"},
        {"jit off loop  ", 1000000,
         "sum_to(n)(\n    new s = 0\n    new i = 0\n    while i < n(\n        s = s + i\n        i = i + 1\n    )\n    =s\n)\nprint sum_to(1000000)\n"},
        {"jit on loop   ", 1000000,
         "[[jit]]\nsum_to(n)(\n    new s = 0\n    new i = 0\n    while i < n(\n        s = s + i\n        i = i + 1\n    )\n    =s\n)\nprint sum_to(1000000)\n"},
        {"float loop    ", 500000,
         "new n = 500000\nnew f = 0.5\nnew i = 0\nwhile i < n( f = f * 1.000001 + 0.25, i = i + 1 )\nprint f\n"},
    };
    for (auto& c : vmCases) {
        long long bestNs = -1;
        bool okRun = true;
        for (int r = 0; r < std::max(1, repeat); r++) {
            auto t0 = std::chrono::steady_clock::now();
            try {
                VM vm;
                registerBuiltins(vm);
                vm.capture = true;
                MacroRegistry registry;
                std::vector<Token> toks = lex(c.src, "<bench>");
                Parser parser(std::move(toks), "<bench>", nullptr, &registry);
                Program prog = parser.parse();
                Compiler comp(prog, "<bench>", false);
                CompileResult cr = comp.compile();
                vm.mainChunk = cr.main;
                vm.run();
            } catch (CompileError& e) {
                std::printf("  %s FAILED to compile: %s\n", c.name, e.message.c_str());
                okRun = false;
                break;
            } catch (VMError& e) {
                std::printf("  %s FAILED at run time: %s\n", c.name, e.message.c_str());
                okRun = false;
                break;
            } catch (std::exception& e) {
                std::printf("  %s FAILED: %s\n", c.name, e.what());
                okRun = false;
                break;
            }
            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - t0).count();
            if (bestNs < 0 || ns < bestNs) bestNs = ns;
        }
        if (!okRun) { failed++; continue; }
        double perOp = (double)bestNs / (double)c.ops;
        std::printf("vm %s: %6.1f ms  %7.1f ns/iter  %6.1f Mops/s\n", c.name,
                    (double)bestNs / 1e6, perOp, perOp > 0 ? 1000.0 / perOp : 0.0);
    }
    return failed ? 1 : 0;
}

// ---------------------------------------------------------------- docs generator
namespace {
// The reference manual is generated from the analyzer's own registries, so the documentation
// can never drift away from what the checker actually implements.
std::string markdownDocs() {
    std::string out;
    out += "# Annota reference\n\n";
    out += "> Generated by `annota ide docs`. Do not edit by hand: change the registries in\n";
    out += "> `src/analyzer.cpp` (annotations / checks), `src/builtins.cpp` (file primitives) and\n";
    out += "> `src/sys_api.cpp` (system primitives).\n>\n";
    out += "> The descriptions below are the strings the tool itself shows, so they are in Chinese;\n";
    out += "> identifiers (annotation names, diagnostic codes, primitives) are stable ASCII.\n\n";

    out += "## 1. Annotations\n\n";
    out += "Annotations are the specification language: the analyzer assumes `[[require]]` at the\n";
    out += "function entry, checks it at every call site, and so on. Write them on the unit they\n";
    out += "describe (a statement, a function, a loop, or the file itself).\n\n";
    out += "| Annotation | Category | Scopes | Arguments | Meaning |\n";
    out += "|---|---|---|---|---|\n";
    for (auto& a : annotationRegistry()) {
        std::string scopes;
        for (auto& s : a.scopes) scopes += (scopes.empty() ? "" : " \\| ") + s;
        std::string args = a.minArgs == a.maxArgs ? formatInt(a.minArgs)
                                                  : (formatInt(a.minArgs) + ".." + formatInt(a.maxArgs));
        if (a.takesKwargs) args += " + kwargs";
        out += "| `[[" + a.name + "]]` | " + a.category + " | " + scopes + " | " + args + " | " +
               a.summary + " |\n";
    }
    out += "\nDetails:\n\n";
    for (auto& a : annotationRegistry())
        out += "* `[[" + a.name + "]]` - " + a.summary + " " + a.detail + "\n";

    out += "\n## 2. Diagnostic codes\n\n";
    out += "Every message the analyzer emits carries a stable code. Use it in\n";
    out += "`[[ignore: <code>]]` to suppress one check, and in tooling to map diagnostics.\n";
    out += "The human readable messages are Chinese; the codes and severities are English.\n\n";
    out += "| Code | Meaning |\n|---|---|\n";
    for (auto& c : checkCodeRegistry()) out += "| `" + c + "` | " + checkCodeSummary(c) + " |\n";

    out += "\n## 3. File and path primitives\n\n";
    out += "Low level, `_`-prefixed, raise on error. `lib/file.mod` wraps them into the\n";
    out += "`File` / `Dir` / `Path` / `Text` / `Result` API used by normal programs.\n\n";
    out += "| Primitive | Returns | Notes |\n|---|---|---|\n";
    struct Row {
        const char* name;
        const char* ret;
        const char* note;
    };
    static const Row rows[] = {
        {"_file_read(path)", "String", "raises when the file cannot be opened"},
        {"_file_read_bytes(path)", "Bytes", ""},
        {"_file_write(path, text)", "int", "truncates, returns the number of bytes written"},
        {"_file_append(path, text)", "int", "appends"},
        {"_file_write_bytes(path, bytes)", "int", ""},
        {"_file_exists(path)", "bool", ""},
        {"_file_is_dir(path)", "bool", ""},
        {"_file_size(path)", "int", "-1 when missing"},
        {"_file_mtime(path)", "int", "unix seconds, -1 when missing"},
        {"_file_remove(path)", "bool", ""},
        {"_file_rename(from, to)", "bool", ""},
        {"_file_copy(from, to, overwrite = true)", "bool", ""},
        {"_dir_list(path)", "List", "names, sorted"},
        {"_dir_make(path)", "bool", "creates parents"},
        {"_dir_remove(path, recursive = false)", "bool", ""},
        {"_dir_walk(path)", "List", "relative paths, sorted, recursive"},
        {"_path_join(a, b, ...)", "String", "platform separator"},
        {"_path_dirname(p)", "String", ""},
        {"_path_basename(p)", "String", ""},
        {"_path_ext(p)", "String", "with the leading dot, or empty"},
        {"_path_abs(p)", "String", "absolute + normalised"},
        {"_path_normalize(p)", "String", ""},
        {"_cwd()", "String", ""},
        {"_chdir(path)", "bool", ""},
        {"_stdin_line()", "String", "null at end of input"},
        {"_stdin_all()", "String", ""},
    };
    for (auto& r : rows) out += std::string("| `") + r.name + "` | " + r.ret + " | " + r.note + " |\n";

    out += "\n";
    out += sysPrimitivesMarkdown();
    // linked C++ modules: read from the FFI registry, so this section can never go stale
    {
        std::vector<std::string> nativeFns = ffiFunctionNames();
        std::vector<std::string> nativeMods = ffiModuleNames();
        if (!nativeFns.empty() || !nativeMods.empty()) {
            out += "## 5. Linked C++ modules (FFI)\n\n";
            out += "Contributed by C++ linked into the binary (`native/*.cpp`) or loaded as a plugin.\n";
            out += "Registering them is all it takes for `use <module>` to resolve and for members to be\n";
            out += "callable; the language core is not involved.  See `docs/ffi.md`.\n\n";
            if (!nativeFns.empty()) {
                out += "| Global native function |\n|---|\n";
                for (auto& f : nativeFns) out += "| `" + f + "` |\n";
                out += "\n";
            }
            for (auto& m : nativeMods) {
                out += "Module `" + m + "`:\n\n| Member |\n|---|\n";
                for (auto& mem : ffiModuleMembers(m)) out += "| `" + m + "." + mem + "` |\n";
                out += "\n";
            }
        }
    }
    out += "## 6. Commands\n\n";
    out += "```\n";
    out += "annota                       REPL (also the default with no arguments)\n";
    out += "annota studio [file]         graphical IDE (Qt build)\n";
    out += "annota run|check <file>      execute / parse only\n";
    out += "annota analyze <file>        static analysis (--json, --level, --int-bits, --modules)\n";
    out += "annota analyze-suite <dir>   run the checker over fixtures\n";
    out += "annota ide <query> ...       hover, definition, rename, complete, inline,\n";
    out += "                             coverage, quickfix, suppressions, report,\n";
    out += "                             checks, annotations, docs\n";
    out += "annota bench                 analysis performance benchmark\n";
    out += "annota lsp                   language server (stdio, JSON-RPC)\n";
    out += "```\n";
    return out;
}
} // namespace

// ---------------------------------------------------------------- ide queries
int cmdIde(int argc, char** argv) {
    Args a = splitArgs(argc, argv, 1);
    std::vector<std::string> rest;
    for (auto& s : a.positional) if (!s.empty() && s[0] != '-') rest.push_back(s);
    if (rest.empty()) { printUsage(); return 2; }
    std::string query = rest[0];
    AnalysisOptions opts = optionsFrom(a);
    auto needFile = [&](size_t idx) -> std::string {
        if (idx >= rest.size()) { std::fprintf(stderr, "annota: missing file argument\n"); return ""; }
        return rest[idx];
    };

    if (query == "complete") {
        std::string prefix = rest.size() > 1 ? rest[1] : "";
        bool ann = a.flag("--annotation");
        Json arr = Json::array();
        for (auto& c : completionsFor(prefix, ann)) {
            Json j = Json::object();
            j.set("label", Json::string(c.label));
            j.set("kind", Json::string(c.kind));
            j.set("detail", Json::string(c.detail));
            j.set("insertText", Json::string(c.insertText));
            arr.push(j);
        }
        std::printf("%s\n", jsonDump(arr, true).c_str());
        return 0;
    }

    if (query == "docs") {
        std::string md = markdownDocs();
        std::string outPath = a.option("--out");
        if (outPath.empty()) {
            std::printf("%s", md.c_str());
            return 0;
        }
        std::ofstream out(outPath, std::ios::binary);
        if (!out) { std::fprintf(stderr, "annota: cannot write '%s'\n", outPath.c_str()); return 2; }
        out << md;
        std::printf("wrote %s (%d bytes)\n", outPath.c_str(), (int)md.size());
        return 0;
    }

    std::string file = needFile(1);
    if (file.empty()) return 2;
    std::string src = readAll(file);
    if (src.empty()) { std::fprintf(stderr, "annota: cannot read '%s'\n", file.c_str()); return 2; }

    if (query == "hover" || query == "definition" || query == "rename") {
        if (rest.size() < 4) { std::fprintf(stderr, "annota: %s needs <file> <line> <col>\n", query.c_str()); return 2; }
        int line = std::atoi(rest[2].c_str());
        int col = std::atoi(rest[3].c_str());
        if (query == "hover") {
            HoverInfo h = hoverAt(src, file, line, col, opts);
            Json j = Json::object();
            j.set("valid", Json::boolean(h.valid));
            j.set("kind", Json::string(h.kind));
            j.set("name", Json::string(h.name));
            j.set("title", Json::string(h.title));
            j.set("body", Json::string(h.body));
            j.set("line", Json::integer(h.line));
            std::printf("%s\n", jsonDump(j, true).c_str());
            return 0;
        }
        if (query == "definition") {
            Location l = definitionAt(src, file, line, col, opts);
            Json j = Json::object();
            j.set("valid", Json::boolean(l.valid));
            j.set("file", Json::string(l.file));
            j.set("line", Json::integer(l.line));
            j.set("column", Json::integer(l.col));
            j.set("endColumn", Json::integer(l.endCol));
            std::printf("%s\n", jsonDump(j, true).c_str());
            return 0;
        }
        if (rest.size() < 5) { std::fprintf(stderr, "annota: rename needs <file> <line> <col> <newName>\n"); return 2; }
        RenameResult r = renameAt(src, file, line, col, rest[4], opts);
        Json j = Json::object();
        j.set("ok", Json::boolean(r.ok));
        j.set("message", Json::string(r.message));
        Json edits = Json::array();
        for (auto& e : r.edits) {
            Json je = Json::object();
            je.set("line", Json::integer(e.line));
            je.set("column", Json::integer(e.col));
            je.set("endColumn", Json::integer(e.endCol));
            je.set("newText", Json::string(e.newText));
            edits.push(je);
        }
        j.set("edits", edits);
        std::printf("%s\n", jsonDump(j, true).c_str());
        return r.ok ? 0 : 1;
    }

    AnalysisResult res = analyzeSource(src, file, opts);
    if (query == "inline") {
        std::printf("%s\n", jsonDump(inlineStatusJson(res), true).c_str());
        return 0;
    }
    if (query == "coverage") {
        std::printf("%s\n", jsonDump(coverageJson(res), true).c_str());
        return 0;
    }
    if (query == "quickfix") {
        std::printf("%s\n", jsonDump(quickFixesJson(res), true).c_str());
        return 0;
    }
    if (query == "suppressions") {
        std::printf("%s\n", jsonDump(suppressionsJson(res), true).c_str());
        return 0;
    }
    if (query == "report") {
        std::printf("%s\n", jsonDump(diagnosticsToJson(res), true).c_str());
        return res.errors > 0 ? 1 : 0;
    }
    if (query == "checks") {
        Json arr = Json::array();
        for (auto& c : checkCodeRegistry()) arr.push(Json::string(c));
        std::printf("%s\n", jsonDump(arr, true).c_str());
        return 0;
    }
    if (query == "annotations") {
        Json arr = Json::array();
        for (auto& s : annotationRegistry()) {
            Json j = Json::object();
            j.set("name", Json::string(s.name));
            j.set("category", Json::string(s.category));
            j.set("summary", Json::string(s.summary));
            Json sc = Json::array();
            for (auto& x : s.scopes) sc.push(Json::string(x));
            j.set("scopes", sc);
            j.set("minArgs", Json::integer(s.minArgs));
            j.set("maxArgs", Json::integer(s.maxArgs));
            arr.push(j);
        }
        std::printf("%s\n", jsonDump(arr, true).c_str());
        return 0;
    }
    std::fprintf(stderr, "annota: unknown ide query '%s'\n", query.c_str());
    printUsage();
    return 2;
}

} // namespace annota
