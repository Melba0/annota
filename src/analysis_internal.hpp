// Annota - analysis_internal.hpp : state shared by the analyzer passes.
#pragma once
#include "analyzer.hpp"
#include <vector>

namespace annota {
namespace detail {

// Diagnostic sink with the `[[ignore]]` suppression stack.
struct Collector {
    AnalysisResult* res = nullptr;
    struct Active {
        int from = 0, to = 0;
        const Annotation* ann = nullptr;
    };
    std::vector<Active> active;
    std::string file;
    // when set, every diagnostic is annotated with the branch being analysed, so a
    // report says *which* path may be wrong (case analysis of if/while/for)
    std::string branchNote;
    // > 0 while analysing statements that came from a `use`d module: they must still be
    // *analysed* (their globals have to be declared), but their own diagnostics stay hidden
    int muteDepth = 0;

    void pushIgnore(const Annotation& a, int from, int to) { active.push_back({from, to, &a}); }
    void popTo(size_t n) { active.resize(n); }

    bool suppressedBy(const std::string& code, int line, Suppression** which) {
        for (auto it = active.rbegin(); it != active.rend(); ++it) {
            if (line < it->from || line > it->to) continue;
            bool all = true, match = false;
            for (auto& g : it->ann->args) {
                if (g.hasKey) continue;
                all = false;
                std::string t = g.text;
                if (!t.empty() && t[0] == '"') continue;      // a reason string, not a code
                if (t == code) match = true;
            }
            if (!all && !match) continue;
            *which = nullptr;
            for (auto& s : res->suppressions) {
                if (s.line == it->ann->line) { *which = &s; break; }
            }
            return true;
        }
        return false;
    }

    void add(const Diagnostic& d) {
        if (muteDepth > 0) return;
        Diagnostic diag = d;
        diag.file = file.empty() ? res->file : file;
        if (!branchNote.empty()) {
            diag.detail = diag.detail.empty() ? branchNote : diag.detail + "\n" + branchNote;
        }
        Suppression* which = nullptr;
        if (suppressedBy(diag.code, diag.line, &which)) {
            diag.suppressed = true;
            if (which) {
                which->used = true;
                which->usedCount++;
                diag.suppressedBy = which->reason.empty() ? "[[ignore]]" : which->reason;
            }
        } else {
            switch (diag.severity) {
                case Severity::Error: res->errors++; break;
                case Severity::Warning: res->warnings++; break;
                default: res->infos++; break;
            }
            if (!diag.fix.empty()) {
                // the actionable part of the advice is the annotation it suggests
                std::string ins;
                size_t p = diag.fix.find("[[");
                size_t q = p == std::string::npos ? std::string::npos : diag.fix.find("]]", p);
                if (p != std::string::npos && q != std::string::npos) ins = diag.fix.substr(p, q - p + 2);
                res->fixes.push_back({diag.line, diag.col, diag.fix, ins, diag.code});
            }
        }
        res->diagnostics.push_back(std::move(diag));
    }

    void error(const std::string& code, int line, const std::string& msg, const std::string& detail = "",
               const std::string& fix = "", int col = 1) {
        add({line, col, line, col + 1, Severity::Error, code, msg, detail, fix, "", "", false, ""});
    }
    void warn(const std::string& code, int line, const std::string& msg, const std::string& detail = "",
              const std::string& fix = "", int col = 1) {
        add({line, col, line, col + 1, Severity::Warning, code, msg, detail, fix, "", "", false, ""});
    }
    void info(const std::string& code, int line, const std::string& msg, const std::string& detail = "",
              const std::string& fix = "", int col = 1) {
        add({line, col, line, col + 1, Severity::Info, code, msg, detail, fix, "", "", false, ""});
    }
};

} // namespace detail
} // namespace annota
