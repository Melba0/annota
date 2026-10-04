// Annota - analyzer.hpp : the IDE analysis layer.
//
// The analyzer reuses the existing AST (it never re-parses) and turns annotations into
// specifications that are checked against a lightweight abstract interpretation.
//
//   layer 1 (keystroke, < 50 ms)  : syntax + annotation registry + annotation scope/conflicts
//   layer 2 (save,     < 500 ms)  : intra-procedural data flow, contracts, types, CFG checks
//   layer 3 (background, < 5 s)   : invariants, termination, taint, purity, macro effects
#pragma once
#include "ast.hpp"
#include "json.hpp"
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace annota {

enum class Severity { Info, Warning, Error };
const char* severityName(Severity s);
Severity severityFromName(const std::string& s);

struct Diagnostic {
    int line = 0, col = 1;
    int endLine = 0, endCol = 1;
    Severity severity = Severity::Warning;
    std::string code;        // check id, e.g. "division-by-zero"
    std::string message;     // one line summary
    std::string detail;      // abstract values / reasoning
    std::string fix;         // what to write instead
    std::string symbol;      // related symbol, when any
    std::string file;        // reporting file
    bool suppressed = false;
    std::string suppressedBy;
};

struct Suppression {
    int line = 0;
    std::vector<std::string> codes;      // empty == all checks
    std::string reason;
    std::string severity;
    std::string target;
    bool used = false;
    int usedCount = 0;
};

struct FunctionStatus {
    std::string name;
    std::string className;
    int line = 0;
    bool hasRequire = false, hasEnsure = false, hasModifies = false;
    bool hasInvariant = false, hasDecrease = false;
    bool pure = false, trusted = false, unsafe = false, noreturn = false;
    int errors = 0, warnings = 0;
    char status = '?';                   // 'v' verified, '?' unknown, 'x' violated
};

struct Coverage {
    int functions = 0;
    int withRequire = 0, withEnsure = 0, withModifies = 0, withAny = 0;
    int loops = 0, loopsWithInvariant = 0;
    std::vector<std::string> uncovered;
};

struct QuickFix {
    int line = 0, col = 1;
    std::string title;
    std::string insertText;
    std::string code;
};

struct AnalysisOptions {
    int level = 3;                       // 1 = syntax only, 2 = +data flow, 3 = +background
    int intBits = 32;                    // integer width used by the overflow check
    bool includeModules = false;         // also report inside `use`d modules
    bool collectIde = true;              // record per-line variable state for hover
    std::function<bool()> isCancelled;   // cooperative interruption
    long long budgetMs = 0;              // 0 = no budget
};

struct AnalysisResult {
    std::string file;
    std::vector<Diagnostic> diagnostics;
    std::vector<Suppression> suppressions;
    std::vector<FunctionStatus> functions;
    std::vector<QuickFix> fixes;
    Coverage coverage;
    bool cancelled = false;
    long long elapsedMs = 0;
    int lines = 0;
    int errors = 0, warnings = 0, infos = 0;
    // hover support: line -> "var: state"
    std::map<int, std::map<std::string, std::string>> lineState;
};

// ---------------------------------------------------------------- annotation registry
struct AnnotationSpec {
    std::string name;
    std::string category;
    std::string summary;                 // one line, shown by hover/completion
    std::string detail;                  // longer explanation
    int minArgs = 0, maxArgs = 0;
    std::vector<std::string> scopes;     // file | stmt | func | class | field | loop | block | any
    bool userRegistered = false;         // declared by a macro pattern (documentation 2.4)
    bool takesKwargs = false;
};

const std::vector<AnnotationSpec>& annotationRegistry();
const AnnotationSpec* findAnnotationSpec(const std::string& name);
// every diagnostic code the analyzer can produce (used by `[[ignore: code]]` validation)
const std::vector<std::string>& checkCodeRegistry();
// one line description of a diagnostic code, shown by hover and completion
const char* checkCodeSummary(const std::string& code);

// ---------------------------------------------------------------- entry points
AnalysisResult analyzeSource(const std::string& source, const std::string& file, const AnalysisOptions& opts);
AnalysisResult analyzeProgram(const Program& prog, const std::string& file, const AnalysisOptions& opts);
void printDiagnosticsText(const AnalysisResult& res, bool verbose);
Json diagnosticsToJson(const AnalysisResult& res);
Json analysisSummaryJson(const AnalysisResult& res);

// ---------------------------------------------------------------- IDE queries
struct HoverInfo {
    bool valid = false;
    std::string kind;                    // annotation | variable | function | class | keyword | builtin
    std::string name;
    std::string title;
    std::string body;
    int line = 0;
};
HoverInfo hoverAt(const std::string& source, const std::string& file, int line, int col,
                  const AnalysisOptions& opts);

struct Location {
    bool valid = false;
    std::string file;
    int line = 0, col = 1, endCol = 1;
};
Location definitionAt(const std::string& source, const std::string& file, int line, int col,
                      const AnalysisOptions& opts);

struct TextEdit {
    int line = 1, col = 1, endCol = 1;
    std::string newText;
};
struct RenameResult {
    bool ok = false;
    std::string message;
    std::vector<TextEdit> edits;
};
RenameResult renameAt(const std::string& source, const std::string& file, int line, int col,
                      const std::string& newName, const AnalysisOptions& opts);

struct CompletionItem {
    std::string label;
    std::string kind;                    // annotation | keyword | builtin
    std::string detail;
    std::string insertText;
};
std::vector<CompletionItem> completionsFor(const std::string& prefix, bool afterDoubleBracket);

Json inlineStatusJson(const AnalysisResult& res);
Json coverageJson(const AnalysisResult& res);
Json suppressionsJson(const AnalysisResult& res);
Json quickFixesJson(const AnalysisResult& res);

// ---------------------------------------------------------------- shared helpers
bool hasAnnotation(const std::vector<Annotation>& anns, const char* name);
const Annotation* findAnnotation(const std::vector<Annotation>& anns, const char* name);
std::string annotationPrimaryText(const Annotation& a);
std::string annotationKeyText(const Annotation& a, const char* key, const std::string& def = "");
int stmtLastLine(const StmtP& s);
int stmtFirstLine(const StmtP& s);

namespace detail {
// implemented in analysis_flow.cpp
void runFlowAnalysis(const Program& prog, const std::string& file, const AnalysisOptions& opts,
                     AnalysisResult& out);
void tagStmtOrigin(const StmtP& s, const std::string& origin);
} // namespace detail

} // namespace annota
