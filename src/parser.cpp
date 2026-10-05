// Annota - parser.cpp : recursive descent parser with a compile-time macro engine.
#include "value.hpp"
#include "parser.hpp"
#include "builtins.hpp"
#include <algorithm>
#include <cctype>
#include <map>

namespace annota {

// ---------------------------------------------------------------- helpers
static bool isOpenTok(T t)  { return t == T::LParen || t == T::LBracket || t == T::LBrace; }
static bool isCloseTok(T t) { return t == T::RParen || t == T::RBracket || t == T::RBrace; }
static T closerOf(T t) {
    if (t == T::LParen) return T::RParen;
    if (t == T::LBracket) return T::RBracket;
    return T::RBrace;
}

Parser::Parser(std::vector<Token> toks, std::string file, ModuleLoader* loader, MacroRegistry* reg)
    : t_(std::move(toks)), file_(std::move(file)), loader_(loader), reg_(reg) {
    if (t_.empty() || t_.back().type != T::End) {
        Token e; e.type = T::End; e.line = t_.empty() ? 1 : t_.back().line;
        t_.push_back(e);
    }
    depth_.assign(t_.size(), 0);
    knownTypes_ = {"bool", "String", "List", "Tuple", "Map", "Bytes", "Color"};
    for (auto& t : builtinTypes()) knownTypes_.insert(t.name);
}

void Parser::error(const std::string& msg) { errorAt(p_, msg); }

void Parser::errorAt(size_t idx, const std::string& msg) {
    const Token& tk = at(idx);
    throw CompileError(file_ + ":" + formatInt(tk.line) + ": " + msg, tk.line);
}

Token Parser::expect(T t, const char* what) {
    if (!check(t)) {
        std::string want = what ? what : tokenName(t);
        error("expected " + want + " but found " + std::string(tokenName(cur().type)));
    }
    Token tk = cur();
    p_++;
    return tk;
}

void Parser::expectStmtEnd() {
    if (check(T::Newline)) { skipNL(); return; }
    if (check(T::RParen) || check(T::RBrace) || check(T::End) || check(T::RBracket)) return;
    error("expected end of statement but found " + std::string(tokenName(cur().type)));
}

size_t Parser::matchBracket(size_t open) const {
    if (open >= t_.size() || !isOpenTok(t_[open].type)) return (size_t)-1;
    T close = closerOf(t_[open].type);
    int depth = 0;
    for (size_t i = open; i < t_.size(); i++) {
        T ty = t_[i].type;
        if (ty == t_[open].type) depth++;
        else if (ty == close) { depth--; if (depth == 0) return i; }
        else if (ty == T::End) break;
    }
    return (size_t)-1;
}

size_t Parser::skipBalanced(size_t i) const {
    if (i >= t_.size()) return i;
    if (isOpenTok(t_[i].type)) {
        size_t cl = matchBracket(i);
        return cl == (size_t)-1 ? i : cl + 1;
    }
    return i + 1;
}

size_t Parser::findBlockParen(size_t from) const {
    int depth = 0;
    for (size_t i = from; i < t_.size(); i++) {
        T ty = t_[i].type;
        if (ty == T::End) return (size_t)-1;
        if (ty == T::Newline && depth == 0) return (size_t)-1;
        if (ty == T::LParen && depth == 0) {
            size_t cl = matchBracket(i);
            if (cl == (size_t)-1) return (size_t)-1;
            T after = at(cl + 1).type;
            if (after == T::Newline || after == T::End || after == T::RParen ||
                after == T::RBrace || after == T::Kw_else || after == T::Kw_elif)
                return i;
            i = cl;
            continue;
        }
        if (isOpenTok(ty)) { depth++; continue; }
        if (isCloseTok(ty)) { depth--; continue; }
    }
    return (size_t)-1;
}

bool Parser::isTypeName(const std::string& s) const { return knownTypes_.count(s) > 0; }

// Mark the '(' that opens a statement body. The mark lives on the token itself so that macro
// expansion (which shifts token indices) cannot invalidate it.
void Parser::markBlockParen(size_t idx) {
    if (idx < t_.size()) t_[idx].blockOpen = true;
    blockParen_ = idx;
}

bool Parser::startsOperand(T t, bool allowSign) const {
    switch (t) {
        case T::Ident: case T::Int: case T::Float: case T::Str: case T::Char:
        case T::Color: case T::LParen: case T::LBracket: case T::Kw_true:
        case T::Kw_false: case T::Kw_null: case T::Kw_this: case T::Kw_super:
            return true;
        case T::Minus: case T::Not: case T::Tilde:
            return allowSign;
        default: return false;
    }
}

std::string Parser::joinTokens(const std::vector<Token>& toks, size_t a, size_t b) {
    std::string out;
    for (size_t i = a; i < b; i++) {
        const Token& tk = toks[i];
        std::string s;
        switch (tk.type) {
            case T::Str: s = "\"" + tk.text + "\""; break;
            case T::Char: s = "'" + tk.text + "'"; break;
            case T::Int: s = formatInt(tk.ival); break;
            case T::Float: s = formatDouble(tk.fval); break;
            case T::Color: {
                char buf[16]; std::snprintf(buf, sizeof(buf), "#%06x", (unsigned)(tk.ival & 0xFFFFFF));
                s = buf; break;
            }
            case T::MacroParam: s = "$" + tk.text; break;
            default: {
                s = tk.text;
                if (s.empty()) {
                    std::string n = tokenName(tk.type);
                    if (n.size() >= 2 && n.front() == '\'' && n.back() == '\'') s = n.substr(1, n.size() - 2);
                    else s = n;
                }
            }
        }
        if (!out.empty()) {
            // reproduce the spacing of the original source: annotation arguments are shown to
            // the user (and to tooling) verbatim, so `MISRA-C 8.1` must stay intact
            const Token& pv = toks[i - 1];
            size_t prevLen = 1;
            if (pv.type == T::MacroParam) prevLen = pv.text.size() + 1;
            else if (pv.type == T::Str || pv.type == T::Char) prevLen = pv.text.size() + 2;
            else if (pv.type == T::Int) prevLen = formatInt(pv.ival).size();
            else if (pv.type == T::Float) prevLen = formatDouble(pv.fval).size();
            else if (pv.type == T::Color) prevLen = 7;
            else if (!pv.text.empty()) prevLen = pv.text.size();
            else {
                std::string n = tokenName(pv.type);
                prevLen = (n.size() >= 2 && n.front() == '\'' && n.back() == '\'') ? n.size() - 2 : n.size();
            }
            bool adjacent = (pv.line == tk.line) && (size_t)tk.col == (size_t)pv.col + prevLen;
            if (!adjacent) out += " ";
        }
        out += s;
    }
    return out;
}

// ---------------------------------------------------------------- macros
bool Parser::literalMatches(const Token& pat, const Token& tok) const {
    if (pat.type != tok.type) return false;
    switch (pat.type) {
        case T::Ident: case T::MacroParam: return pat.text == tok.text;
        case T::Int:   return pat.ival == tok.ival;
        case T::Float: return pat.fval == tok.fval;
        case T::Str: case T::Char: return pat.text == tok.text;
        case T::Color: return pat.ival == tok.ival;
        default: return true;
    }
}

bool Parser::matchMacroAt(const MacroDef& m, size_t start,
                          std::vector<std::pair<std::string, std::pair<size_t, size_t>>>& binds,
                          size_t& end) const {
    size_t pos = start;
    binds.clear();
    for (size_t pi = 0; pi < m.pattern.size(); pi++) {
        const Token& pt = m.pattern[pi];
        if (pt.type == T::MacroParam) {
            size_t nextLit = pi + 1;
            while (nextLit < m.pattern.size() && m.pattern[nextLit].type == T::MacroParam) nextLit++;
            if (nextLit < m.pattern.size()) {
                const Token& lit = m.pattern[nextLit];
                size_t q = pos;
                bool found = false;
                while (q < t_.size()) {
                    if (t_[q].type == T::End || t_[q].type == T::Newline) break;
                    if (t_[q].type == T::Assign) break;      // never let a pattern span an assignment
                    if (t_[q].blockOpen) break;              // nor a statement body
                    if (literalMatches(lit, t_[q])) { found = true; break; }
                    if (t_[q].type == T::Comma) break;       // nor an argument boundary
                    if (isOpenTok(t_[q].type)) {
                        size_t cl = matchBracket(q);
                        if (cl == (size_t)-1) break;
                        q = cl + 1;
                        continue;
                    }
                    if (isCloseTok(t_[q].type)) break;
                    q++;
                }
                if (!found || q == pos) return false;
                binds.push_back({pt.text, {pos, q}});
                pos = q;
            } else {
                size_t q = skipBalanced(pos);
                if (q == pos) return false;
                binds.push_back({pt.text, {pos, q}});
                pos = q;
            }
        } else {
            if (pos >= t_.size() || !literalMatches(pt, t_[pos])) return false;
            pos++;
        }
    }
    end = pos;
    return true;
}

void Parser::spliceMacro(size_t start, size_t end, const std::vector<Token>& body,
                         const std::vector<std::pair<std::string, std::pair<size_t, size_t>>>& binds,
                         const std::vector<int>& bodyDepth) {
    std::vector<Token> nt;
    std::vector<int> nd;
    auto depthOf = [&](const std::string& name) -> int {
        int d = 0;
        for (auto& b : binds) if (b.first == name) {
            for (size_t k = b.second.first; k < b.second.second; k++) d = std::max(d, depth_[k]);
            break;
        }
        return d;
    };
    for (size_t i = 0; i < body.size(); i++) {
        const Token& bt = body[i];
        int bd = i < bodyDepth.size() ? bodyDepth[i] : 0;
        if (bt.type == T::MacroParam) {
            bool replaced = false;
            for (auto& b : binds) {
                if (b.first != bt.text) continue;
                for (size_t k = b.second.first; k < b.second.second; k++) {
                    nt.push_back(t_[k]);
                    nd.push_back(std::max(depth_[k], bd));
                }
                replaced = true;
                break;
            }
            if (!replaced) { nt.push_back(bt); nd.push_back(bd); }
        } else {
            nt.push_back(bt);
            nd.push_back(bd);
        }
    }
    (void)depthOf;
    t_.erase(t_.begin() + (long)start, t_.begin() + (long)end);
    depth_.erase(depth_.begin() + (long)start, depth_.begin() + (long)end);
    t_.insert(t_.begin() + (long)start, nt.begin(), nt.end());
    depth_.insert(depth_.begin() + (long)start, nd.begin(), nd.end());
}

bool Parser::tryMacro() {
    if (noMacro_ || !reg_ || reg_->macros.empty()) return false;
    if (p_ >= t_.size()) return false;
    bool stmtPosition = (p_ == stmtStart_);
    for (const auto& m : reg_->macros) {
        if (m.pattern.empty()) continue;
        // at the start of a statement only patterns beginning with a literal token may fire,
        // otherwise `$x[$a:$b]` would happily swallow `new s = arr[1:4]`
        if (stmtPosition && m.pattern[0].type == T::MacroParam) continue;
        if (m.pattern[0].type != T::MacroParam && !literalMatches(m.pattern[0], cur())) continue;
        std::vector<std::pair<std::string, std::pair<size_t, size_t>>> binds;
        size_t end = 0;
        size_t save = p_;
        if (!matchMacroAt(m, p_, binds, end)) continue;
        int d = 0;
        for (size_t k = save; k < end && k < depth_.size(); k++) d = std::max(d, depth_[k]);
        if (d + 1 > macroLimit_)
            error("Macro expansion depth exceeded (limit " + formatInt(macroLimit_) + ")");
        // record expansions whose body repeats an argument that contains a call
        {
            std::map<std::string, int> counts;
            for (auto& bt : m.body) if (bt.type == T::MacroParam) counts[bt.text]++;
            for (auto& kv : counts) {
                if (kv.second < 2) continue;
                for (auto& b : binds) {
                    if (b.first != kv.first) continue;
                    bool hasCall = false;
                    for (size_t k = b.second.first; k + 1 < b.second.second; k++)
                        if (t_[k].type == T::Ident && t_[k + 1].type == T::LParen) hasCall = true;
                    if (hasCall && expansions_) {
                        MacroExpansion ex;
                        ex.macro = m.name;
                        ex.line = cur().line;
                        ex.argument = joinTokens(t_, b.second.first, b.second.second);
                        ex.duplicatedArgWithCall = true;
                        expansions_->push_back(ex);
                    }
                }
            }
        }
        std::vector<int> bodyDepth(m.body.size(), d + 1);
        spliceMacro(save, end, m.body, binds, bodyDepth);
        if (++macroBurst_ > 200000) error("macro expansion did not terminate");
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- lookahead
bool Parser::looksLikeParamList(size_t a, size_t b) const {
    if (b <= a) return true;
    size_t i = a;
    while (i < b) {
        if (at(i).type == T::Ellipsis || at(i).type == T::Star || at(i).type == T::StarStar) {
            i++;
            if (i >= b || at(i).type != T::Ident) return false;
            i++;
        }
        else if (at(i).type == T::Ident) { i++; }
        else return false;
        if (i < b && at(i).type == T::Colon) {
            i++;
            if (i >= b || (at(i).type != T::Ident && at(i).type != T::Kw_this)) return false;
            i++;
        }
        if (i < b && at(i).type == T::Assign) {
            i++;
            int depth = 0;
            while (i < b) {
                T ty = at(i).type;
                if (depth == 0 && (ty == T::Comma)) break;
                if (ty == T::End || ty == T::Newline) return false;
                if (isOpenTok(ty)) depth++;
                else if (isCloseTok(ty)) depth--;
                i++;
            }
        }
        if (i < b) {
            if (at(i).type != T::Comma) return false;
            i++;
        }
    }
    return true;
}

bool Parser::looksLikeClassBody(size_t openIdx) const {
    if (at(openIdx).type != T::LParen) return false;
    size_t cl = matchBracket(openIdx);
    if (cl == (size_t)-1) return false;
    if (cl == openIdx + 1) return true;                 // empty body -> empty class
    size_t i = openIdx + 1;
    while (i < cl) {
        while (i < cl && at(i).type == T::Newline) i++;
        if (i >= cl) break;
        // segment start
        if (at(i).type == T::Colon) return true;                       // :parent(...)
        if (at(i).type == T::LBracket && at(i + 1).type == T::LBracket) return true;  // [[annot]]
        if (at(i).type == T::Ident) {
            if (at(i + 1).type == T::Colon) return true;               // typed field
            if (at(i + 1).type == T::LParen) {
                size_t pcl = matchBracket(i + 1);
                if (pcl != (size_t)-1 && (at(pcl + 1).type == T::Assign || at(pcl + 1).type == T::LParen))
                    return true;                                        // method
            }
        }
        // skip this segment
        while (i < cl && at(i).type != T::Newline) {
            if (isOpenTok(at(i).type)) { size_t c2 = matchBracket(i); i = (c2 == (size_t)-1) ? cl : c2 + 1; }
            else i++;
        }
    }
    return false;
}

bool Parser::looksLikeStyleBody(size_t openIdx) const {
    if (at(openIdx).type != T::LParen) return false;
    size_t cl = matchBracket(openIdx);
    if (cl == (size_t)-1) return false;
    if (cl == openIdx + 1) return false;
    size_t i = openIdx + 1;
    bool sawLine = false;
    while (i < cl) {
        while (i < cl && at(i).type == T::Newline) i++;
        if (i >= cl) break;
        if (at(i).type != T::Ident) return false;
        if (at(i + 1).type != T::Assign && at(i + 1).type != T::Colon) return false;
        sawLine = true;
        while (i < cl && at(i).type != T::Newline) {
            if (isOpenTok(at(i).type)) { size_t c2 = matchBracket(i); i = (c2 == (size_t)-1) ? cl : c2 + 1; }
            else i++;
        }
    }
    return sawLine;
}

bool Parser::looksLikeLambda(size_t openIdx, size_t closeIdx) const {
    if (at(openIdx).type != T::LParen) return false;
    if (at(closeIdx + 1).type != T::LParen) return false;
    if (at(closeIdx + 1).line != at(closeIdx).line) return false;
    return looksLikeParamList(openIdx + 1, closeIdx);
}

// ---------------------------------------------------------------- program
Program Parser::parse() {
    Program prog;
    prog.file = file_;

    // pre-scan [[macro_depth: N]]
    for (size_t i = 0; i + 6 < t_.size(); i++) {
        if (t_[i].type == T::LBracket && t_[i + 1].type == T::LBracket &&
            t_[i + 2].type == T::Ident && t_[i + 2].text == "macro_depth" &&
            t_[i + 3].type == T::Colon && t_[i + 4].type == T::Int &&
            t_[i + 5].type == T::RBracket && t_[i + 6].type == T::RBracket) {
            prog.macroDepth = (int)t_[i + 4].ival;
            macroLimit_ = prog.macroDepth;
        }
    }
    annIndex_ = &prog.annotationIndex;
    expansions_ = &prog.expansions;
    Ctx ctx;
    stmtList(ctx, prog.stmts, T::End);
    if (!atEnd()) error("unexpected " + std::string(tokenName(cur().type)));
    if (reg_) prog.macros = reg_->macros;
    return prog;
}

void Parser::stmtList(Ctx ctx, std::vector<StmtP>& out, T terminator) {
    skipNL();
    while (!check(terminator) && !atEnd()) {
        std::vector<StmtP> ss = statement(ctx);
        for (auto& s : ss) out.push_back(s);
        if (check(terminator) || atEnd()) break;
        if (check(T::Comma)) { p_++; skipNL(); continue; }   // `a = 1, b = 2` on one line
        expectStmtEnd();
        skipNL();
    }
}

// ---------------------------------------------------------------- annotations
std::vector<Annotation> Parser::parseAnnotations(bool& any) {
    std::vector<Annotation> out;
    any = false;
    while (check(T::LBracket) && type(1) == T::LBracket) {
        Annotation a = parseAnnotation();
        int aline = a.line;
        out.push_back(a);
        any = true;
        // a blank line closes the current annotation block (file level metadata)
        skipNL();
        if (cur().line > aline + 1) break;
    }
    return out;
}

Annotation Parser::parseAnnotation() {
    int line = cur().line;
    expect(T::LBracket);
    expect(T::LBracket);
    Annotation a;
    a.line = line;
    if (check(T::Ident)) { a.name = cur().text; p_++; }
    else if (check(T::Kw_new) || check(T::Kw_use)) { a.name = cur().text; p_++; }
    else error("expected an annotation name");

    if (accept(T::Colon)) {
        // collect everything up to the closing ]] and split on top-level commas
        std::vector<std::pair<size_t, size_t>> groups;
        size_t gs = p_;
        int depth = 0;
        while (true) {
            if (atEnd()) error("unterminated annotation");
            T ty = cur().type;
            if (depth == 0 && ty == T::RBracket && type(1) == T::RBracket) break;
            if (depth == 0 && ty == T::Newline) break;
            if (depth == 0 && ty == T::Comma) { groups.push_back({gs, p_}); p_++; gs = p_; continue; }
            if (isOpenTok(ty)) { depth++; p_++; continue; }
            if (isCloseTok(ty)) { depth--; p_++; continue; }
            p_++;
        }
        groups.push_back({gs, p_});
        for (auto& g : groups) {
            size_t s = g.first, e = g.second;
            while (s < e && at(s).type == T::Newline) s++;
            while (e > s && at(e - 1).type == T::Newline) e--;
            if (s >= e) continue;
            AnnotArg arg;
            // named argument?  Ident '=' value
            if (at(s).type == T::Ident && at(s + 1).type == T::Assign && s + 2 <= e) {
                arg.hasKey = true;
                arg.key = at(s).text;
                s += 1;                       // keep '=' out of the text
                size_t vs = s + 1;
                arg.text = joinTokens(t_, vs, e);
                std::vector<Token> sub(t_.begin() + (long)vs, t_.begin() + (long)e);
                arg.expr = exprFromTokens(sub, file_);
            } else {
                arg.text = joinTokens(t_, s, e);
                std::vector<Token> sub(t_.begin() + (long)s, t_.begin() + (long)e);
                arg.expr = exprFromTokens(sub, file_);
            }
            a.args.push_back(std::move(arg));
        }
    }
    expect(T::RBracket, "']' closing the annotation");
    expect(T::RBracket, "']' closing the annotation");
    return a;
}

std::string Parser::describeStmt(const StmtP& s) const {
    if (!s) return "statement";
    switch (s->kind) {
        case SK::FuncDef:  return "function '" + s->name + "'";
        case SK::ClassDef: return (s->isView ? "view '" : "class '") + s->name + "'";
        case SK::FieldDecl:return "field '" + s->name + "'";
        case SK::New:      return s->names.empty() ? "declaration" : "variable '" + s->names[0] + "'";
        case SK::State:    return "state '" + s->name + "'";
        case SK::For:      return "loop";
        case SK::While:    return "loop";
        case SK::If:       return "branch";
        case SK::Block:    return "scope";
        case SK::MacroDef: return "macro";
        case SK::Use:      return "module '" + s->module + "'";
        default:           return "statement";
    }
}

void Parser::describeAndRecord(const std::vector<Annotation>& anns, const StmtP& s) {
    if (!annIndex_) return;
    std::string target = describeStmt(s);
    for (auto& a : anns) annIndex_->push_back({a, target, s ? s->line : 0});
}

// ---------------------------------------------------------------- statements
std::vector<StmtP> Parser::statement(Ctx ctx) {
    std::vector<StmtP> out;
    skipNL();
    stmtStart_ = p_;
    if (tryMacro()) return statement(ctx);

    bool any = false;
    std::vector<Annotation> anns = parseAnnotations(any);
    if (any) {
        skipNL();
        if (check(T::LBracket) && type(1) == T::LBracket) {
            // the block above was separated by a blank line: pure metadata, no target
            for (auto& a : anns) {
                StmtP s = std::make_shared<Stmt>();
                s->kind = SK::Annot;
                s->ann = a;
                s->line = a.line;
                if (annIndex_) annIndex_->push_back({a, "(file)", 0});
                out.push_back(s);
            }
            std::vector<StmtP> rest = statement(ctx);
            for (auto& r : rest) out.push_back(r);
            return out;
        }
        if (check(T::RParen) || check(T::RBrace) || atEnd()) {
            for (auto& a : anns) {
                StmtP s = std::make_shared<Stmt>();
                s->kind = SK::Annot;
                s->ann = a;
                s->line = a.line;
                if (annIndex_) annIndex_->push_back({a, "(none)", 0});
                out.push_back(s);
            }
            return out;
        }
    }
    StmtP s = statementCore(ctx);
    if (s) {
        if (!anns.empty()) {
            s->annotations = anns;
            describeAndRecord(anns, s);
        }
        out.push_back(s);
        // statements produced by one source line, e.g. `new a = 1, b = 2`
        for (auto& extra : pendingStmts_) out.push_back(extra);
        pendingStmts_.clear();
    }
    return out;
}

StmtP Parser::statementCore(Ctx ctx) {
    int line = cur().line;
    switch (cur().type) {
        case T::Kw_new:    return parseNew();
        case T::Kw_const:  return parseSimpleDecl(SK::Const, true);
        case T::Kw_state:  return parseState();
        case T::Kw_use:    return parseUse();
        case T::Kw_macro:  return parseMacro();
        case T::Kw_view:   return parseView();
        case T::Kw_if:     return parseIf(ctx);
        case T::Kw_elif:
            error("'elif' must follow an if/elif block (write `) elif cond( ... )`)");
            return nullptr;
        case T::Kw_while:  return parseWhile(ctx);
        case T::Kw_for:    return parseFor(ctx);
        case T::Kw_break: {
            p_++;
            StmtP s = std::make_shared<Stmt>(); s->kind = SK::Break; s->line = line; return s;
        }
        case T::Kw_continue: {
            p_++;
            StmtP s = std::make_shared<Stmt>(); s->kind = SK::Continue; s->line = line; return s;
        }
        case T::Kw_del: {
            p_++;
            StmtP s = std::make_shared<Stmt>(); s->kind = SK::Del; s->line = line;
            s->names.push_back(expect(T::Ident, "a variable name").text);
            while (check(T::Comma)) {
                p_++;
                skipNL();
                s->names.push_back(expect(T::Ident, "a variable name").text);
            }
            return s;
        }
        case T::Kw_throw: {
            p_++;
            StmtP s = std::make_shared<Stmt>(); s->kind = SK::Throw; s->line = line;
            s->expr = parseExpr();
            return s;
        }
        case T::Kw_print:  return parsePrint();
        case T::Kw_input: {
            p_++;
            StmtP first;
            for (;;) {                           // `input a, b` reads one line each
                StmtP s = std::make_shared<Stmt>(); s->kind = SK::Input; s->line = line;
                s->target = parseExpr();
                if (!first) first = s;
                else pendingStmts_.push_back(s);
                if (!accept(T::Comma)) break;
                skipNL();
            }
            return first;
        }
        case T::Assign: {                       // `= expr` -> return
            p_++;
            StmtP s = std::make_shared<Stmt>(); s->kind = SK::Return; s->line = line;
            if (!check(T::Newline) && !check(T::RParen) && !atEnd()) s->expr = parseExpr();
            return s;
        }
        case T::LParen: {                       // bare scope
            Ctx bc = ctx; bc.classBody = false;
            StmtP s = std::make_shared<Stmt>(); s->kind = SK::Block; s->line = line;
            s->body = parseBlock(bc, false);
            return s;
        }
        case T::Colon: {                        // :parent(args) inside a class body
            if (!ctx.classBody) error("inheritance clause is only allowed directly in a class body");
            p_++;
            StmtP s = std::make_shared<Stmt>(); s->kind = SK::ParentInit; s->line = line;
            s->parentName = expect(T::Ident, "a base class name").text;
            expect(T::LParen, "'(' after the base class name");
            skipNL();
            while (!check(T::RParen)) {
                s->parentArgs.push_back(parseExpr());
                skipNL();
                if (!accept(T::Comma)) break;
                skipNL();
            }
            expect(T::RParen, "')' closing the base class arguments");
            return s;
        }
        case T::Ident: break;
        default: break;
    }
    if (check(T::Ident)) {
        // class-body member declarations
        if (ctx.classBody) {
            if (type(1) == T::Colon) {
                StmtP s = std::make_shared<Stmt>(); s->kind = SK::FieldDecl; s->line = line;
                s->name = cur().text; p_++;
                p_++;                                   // ':'
                s->type = expect(T::Ident, "a type name").text;
                s->typeDims = parseDims();
                if (accept(T::Assign)) s->initExpr = parseExpr();
                return s;
            }
            if (type(1) == T::Assign && type(2) != T::LParen) {
                StmtP s = std::make_shared<Stmt>(); s->kind = SK::FieldDecl; s->line = line;
                s->name = cur().text; p_++;
                p_++;
                s->initExpr = parseExpr();
                return s;
            }
            if (type(1) == T::Newline || type(1) == T::RParen || type(1) == T::End) {
                StmtP s = std::make_shared<Stmt>(); s->kind = SK::FieldDecl; s->line = line;
                s->name = cur().text; p_++;
                return s;
            }
        }
        // definitions
        std::string nm = cur().text;
        size_t q = (type(1) == T::LParen) ? matchBracket(p_ + 1) : (size_t)-1;
        if (type(1) == T::Assign && type(2) == T::LParen && looksLikeStyleBody(p_ + 2)) {
            p_ += 2;
            auto saved = knownTypes_; (void)saved;
            StmtP s = parseClassDef(nm, {}, false, true);
            return s;
        }
        if (q != (size_t)-1) {
            size_t after = q + 1;
            bool paramsOk = looksLikeParamList(p_ + 2, q);
            if (at(after).type == T::Assign && at(after + 1).type == T::LParen &&
                looksLikeClassBody(after + 1)) {
                p_++;                                   // the class name
                std::vector<Param> ps = parseParams();
                p_++;                                    // '='
                return parseClassDef(nm, ps, false, false);
            }
            if (at(after).type == T::Assign) {
                bool isDef = paramsOk;
                if (ctx.classBody && isupper((unsigned char)nm[0])) isDef = false;
                if (isDef) {
                    p_++;                               // the function name
                    std::vector<Param> ps = parseParams();
                    return parseFuncDef(ctx, nm, ps, ctx.classBody && !ctx.ui);
                }
            }
            if (at(after).type == T::LParen && at(after).line == at(q).line) {
                size_t bcl = matchBracket(after);
                bool bodyEmpty = (bcl == (size_t)(after + 1));
                bool paramsEmpty = (q == p_ + 2);
                bool isDef = paramsOk && !(paramsEmpty && bodyEmpty);
                // inside a view body / children list `Name(args)( children )` is always a component call
                if (ctx.ui && !ctx.classBody) isDef = false;
                // component names are PascalCase: inside a class body such a call is not a method
                if (ctx.classBody && isupper((unsigned char)nm[0])) isDef = false;
                if (isDef) {
                    p_++;                               // the method name
                    std::vector<Param> ps = parseParams();
                    return parseFuncDef(ctx, nm, ps, ctx.classBody && !ctx.ui);
                }
            }
        }
    }
    // expression statement / assignment
    ExprP e = parseExpr();
    if (check(T::Assign)) {
        p_++;
        StmtP s = std::make_shared<Stmt>(); s->kind = SK::Assign; s->line = line;
        s->target = e;
        s->value = parseExpr();
        return s;
    }
    if (check(T::PlusA) || check(T::MinusA) || check(T::StarA) || check(T::SlashA) ||
        check(T::PercentA) || check(T::StarStarA) || check(T::AmpA) || check(T::PipeA) ||
        check(T::CaretA) || check(T::ShlA) || check(T::ShrA)) {
        T op = cur().type; p_++;
        StmtP s = std::make_shared<Stmt>(); s->kind = SK::CompoundAssign; s->line = line;
        s->target = e; s->op = op; s->value = parseExpr();
        return s;
    }
    StmtP s = std::make_shared<Stmt>(); s->kind = SK::Expr; s->line = line;
    s->expr = e;
    return s;
}

StmtP Parser::parseIf(Ctx ctx) {
    int line = cur().line;
    expect(T::Kw_if);
    size_t bp = findBlockParen(p_);
    markBlockParen(bp);
    StmtP s = std::make_shared<Stmt>(); s->kind = SK::If; s->line = line;
    s->cond = parseExpr();
    blockParen_ = (size_t)-1;
    Ctx bc = ctx; bc.classBody = false;
    s->body = parseBlock(bc, ctx.ui);
    // `elif c( ... )` is exactly `else if c( ... )`; the token is rewritten so both spellings
    // take the same path (and `elif` chains nest like the documentation describes)
    bool elifHere = check(T::Kw_elif);
    if (elifHere) t_[p_].type = T::Kw_if;
    if (elifHere || accept(T::Kw_else)) {
        if (check(T::Kw_if)) {
            BlockP b = std::make_shared<Block>();
            b->line = cur().line;
            b->stmts.push_back(parseIf(ctx));
            s->elseBody = b;
        } else if (elifHere) {
            error("'elif' needs a condition: elif <条件>( ... )");
        } else {
            s->elseBody = parseBlock(bc, ctx.ui);
        }
    }
    return s;
}

StmtP Parser::parseWhile(Ctx ctx) {
    int line = cur().line;
    expect(T::Kw_while);
    size_t bp = findBlockParen(p_);
    markBlockParen(bp);
    StmtP s = std::make_shared<Stmt>(); s->kind = SK::While; s->line = line;
    s->cond = parseExpr();
    blockParen_ = (size_t)-1;
    Ctx bc = ctx; bc.classBody = false;
    s->body = parseBlock(bc, ctx.ui);
    return s;
}

StmtP Parser::parseFor(Ctx ctx) {
    int line = cur().line;
    expect(T::Kw_for);
    StmtP s = std::make_shared<Stmt>(); s->kind = SK::For; s->line = line;
    if (accept(T::LParen)) {
        s->loopVars.push_back(expect(T::Ident, "a loop variable").text);
        expect(T::Comma, "',' between loop variables");
        s->loopVars.push_back(expect(T::Ident, "a loop variable").text);
        expect(T::RParen, "')' after the loop variables");
    } else {
        s->loopVars.push_back(expect(T::Ident, "a loop variable").text);
    }
    expect(T::Kw_in, "'in'");
    size_t bp = findBlockParen(p_);
    markBlockParen(bp);
    s->iterable = parseExpr();
    blockParen_ = (size_t)-1;
    Ctx bc = ctx; bc.classBody = false;
    s->body = parseBlock(bc, ctx.ui);
    return s;
}

StmtP Parser::parsePrint() {
    int line = cur().line;
    expect(T::Kw_print);
    StmtP s = std::make_shared<Stmt>(); s->kind = SK::Print; s->line = line;
    while (true) {
        if (check(T::Newline) || check(T::RParen) || check(T::RBrace) || atEnd()) break;
        ExprP e = parseExpr();
        if (check(T::Assign)) {
            p_++;
            if (e->kind == EK::Ident && e->name == "sep") s->sep = parseExpr();
            else error("only 'sep=' is allowed after print arguments");
        } else {
            s->args.push_back(e);
        }
        if (!accept(T::Comma)) break;
    }
    if (s->args.empty() && !s->sep) error("print expects at least one argument");
    return s;
}

StmtP Parser::parseState() {
    expect(T::Kw_state);
    StmtP first;
    for (;;) {                                  // `state a = 0, b = 1`
        StmtP s = std::make_shared<Stmt>(); s->kind = SK::State; s->line = cur().line;
        s->name = expect(T::Ident, "a state name").text;
        if (accept(T::Colon)) { s->type = expect(T::Ident, "a type name").text; s->typeDims = parseDims(); }
        expect(T::Assign, "'=' in a state declaration");
        s->initExpr = parseExpr();
        if (!first) first = s;
        else pendingStmts_.push_back(s);
        if (!accept(T::Comma)) break;
        skipNL();
    }
    return first;
}

StmtP Parser::parseNew() {
    int line = cur().line;
    expect(T::Kw_new);
    StmtP first;
    if (accept(T::LParen)) {
        StmtP s = std::make_shared<Stmt>(); s->kind = SK::New; s->line = line;
        s->names.push_back(expect(T::Ident, "a variable name").text);
        while (accept(T::Comma)) s->names.push_back(expect(T::Ident, "a variable name").text);
        expect(T::RParen, "')' after the destructuring pattern");
        if (accept(T::Colon)) { s->type = expect(T::Ident, "a type name").text; s->typeDims = parseDims(); }
        if (accept(T::Assign)) s->initExpr = parseExpr();
        return s;
    }
    // `new a, b` / `new a = 1, b = 2` : one statement per name
    for (;;) {
        StmtP s = std::make_shared<Stmt>(); s->kind = SK::New; s->line = cur().line;
        Token nm = expect(T::Ident, "a variable name");
        s->names.push_back(nm.text);
        if (accept(T::Colon)) {
            // either a plain type or a leftover of `(T)` casts
            if (check(T::Ident)) s->type = cur().text, p_++;
            else if (check(T::LParen)) { p_++; s->type = expect(T::Ident, "a type name").text; expect(T::RParen); }
            s->typeDims = parseDims();
        }
        if (accept(T::Assign)) s->initExpr = parseExpr();
        if (!first) first = s;
        else pendingStmts_.push_back(s);
        if (!accept(T::Comma)) break;     // only a comma joins the next declaration
        skipNL();
    }
    return first;
}

StmtP Parser::parseSimpleDecl(SK kind, bool requireInit) {
    p_++;                                    // 'const'
    StmtP first;
    for (;;) {                               // `const a = 1, b = 2`
        StmtP s = std::make_shared<Stmt>(); s->kind = kind; s->line = cur().line;
        s->isConst = true;
        s->names.push_back(expect(T::Ident, "a constant name").text);
        if (accept(T::Colon)) { s->type = expect(T::Ident, "a type name").text; s->typeDims = parseDims(); }
        if (accept(T::Assign)) s->initExpr = parseExpr();
        else if (requireInit) error("a constant requires an initial value");
        if (!first) first = s;
        else pendingStmts_.push_back(s);
        if (!accept(T::Comma)) break;
        skipNL();
    }
    return first;
}

// A `use`d module contributes its statements to the current program; remember where they came
// from so that tooling can tell user code and library code apart.
static void tagOrigin(const StmtP& s, const std::string& origin) {
    if (!s) return;
    s->origin = origin;
    if (s->body) for (auto& x : s->body->stmts) tagOrigin(x, origin);
    if (s->elseBody) for (auto& x : s->elseBody->stmts) tagOrigin(x, origin);
    for (auto& m : s->members) tagOrigin(m, origin);
}

StmtP Parser::parseUse() {
    int line = cur().line;
    expect(T::Kw_use);
    std::string spec;
    if (check(T::Str)) { spec = cur().text; p_++; }
    else {
        // `use name`, `use a.b.c`, `use sub/dir/name`, `use ./rel/name.mod` - a module spec is a
        // path made of identifiers separated by `.` or `/`, with an optional `.mod` suffix
        if (check(T::Dot)) { spec = "."; p_++; }                 // leading `./`
        spec += expect(T::Ident, "a module name").text;
        while (check(T::Dot) || check(T::Slash)) {
            bool dot = check(T::Dot);
            size_t save = p_;
            p_++;
            if (!check(T::Ident)) { p_ = save; break; }          // not part of the path
            if (dot && cur().text == "mod") { spec += ".mod"; p_++; break; }
            spec += (dot ? "." : "/") + cur().text;
            p_++;
        }
    }
    StmtP s = std::make_shared<Stmt>(); s->kind = SK::Use; s->line = line; s->module = spec;
    if (loader_ && reg_ && !reg_->loaded.count(spec)) {
        std::vector<Token> mtoks;
        std::string mfile;
        if (loader_->loadModule(spec, mtoks, mfile)) {
            reg_->loaded.insert(spec);
            Parser sub(std::move(mtoks), mfile, loader_, reg_);
            Program mp = sub.parse();
            for (auto& st : mp.stmts) tagOrigin(st, mfile);
            BlockP b = std::make_shared<Block>();
            b->stmts = mp.stmts;
            b->line = line;
            s->body = b;
        } else if (!isBuiltinModule(spec)) {
            // a missing module used to be a silent no-op, which turned into confusing
            // "undefined variable" errors much later - say it right here instead
            error("cannot find module '" + spec + "'" + (loader_ ? loader_->searchHint(spec) : ""));
        }
    }
    return s;
}

StmtP Parser::parseMacro() {
    int line = cur().line;
    expect(T::Kw_macro);
    size_t bodyOpen = findBlockParen(p_);
    if (bodyOpen == (size_t)-1) error("a macro requires a body in parentheses: macro pattern( body )");
    size_t bodyClose = matchBracket(bodyOpen);
    if (bodyClose == (size_t)-1) error("unterminated macro body");
    auto md = std::make_shared<MacroDef>();
    md->line = line;
    md->file = file_;
    md->pattern.assign(t_.begin() + (long)p_, t_.begin() + (long)bodyOpen);
    md->body.assign(t_.begin() + (long)(bodyOpen + 1), t_.begin() + (long)bodyClose);
    // a macro body/pattern spread over several lines keeps its inner newlines but not the
    // ones that merely surround the parentheses
    while (!md->pattern.empty() && md->pattern.front().type == T::Newline) md->pattern.erase(md->pattern.begin());
    while (!md->pattern.empty() && md->pattern.back().type == T::Newline) md->pattern.pop_back();
    while (!md->body.empty() && md->body.front().type == T::Newline) md->body.erase(md->body.begin());
    while (!md->body.empty() && md->body.back().type == T::Newline) md->body.pop_back();
    if (!md->pattern.empty() && md->pattern[0].type == T::Ident) md->name = md->pattern[0].text;
    else md->name = "<pattern>";
    StmtP s = std::make_shared<Stmt>(); s->kind = SK::MacroDef; s->line = line; s->macro = md;
    if (reg_) reg_->macros.push_back(*md);
    p_ = bodyClose + 1;
    return s;
}

StmtP Parser::parseView() {
    int line = cur().line;
    (void)line;
    expect(T::Kw_view);
    std::string nm = expect(T::Ident, "a view name").text;
    std::vector<Param> ps = parseParams();
    return parseClassDef(nm, ps, true, false);
}

// `T[10]`, `T[]`, `T[10][10]` after a type name; a null entry means an open dimension
std::vector<ExprP> Parser::parseDims() {
    std::vector<ExprP> dims;
    while (check(T::LBracket)) {
        p_++;
        skipNL();
        if (check(T::RBracket)) { dims.push_back(nullptr); p_++; continue; }
        dims.push_back(parseExpr());
        skipNL();
        expect(T::RBracket, "']' after an array dimension");
    }
    return dims;
}

std::vector<Param> Parser::parseParams() {
    std::vector<Param> ps;
    expect(T::LParen, "'(' starting a parameter list");
    skipNL();
    while (!check(T::RParen)) {
        Param p;
        // `*args` (Python style) or `...args` marks the variadic tail
        if (check(T::StarStar))
            error("'**kwargs' is not supported; pass a Dict as a normal parameter instead");
        if (accept(T::Star) || accept(T::Ellipsis)) p.vararg = true;
        p.name = expect(T::Ident, "a parameter name").text;
        if (accept(T::Colon)) {
            if (check(T::Ident)) p.type = cur().text, p_++;
            else if (check(T::LParen)) { p_++; p.type = expect(T::Ident, "a type name").text; expect(T::RParen); }
            p.typeDims = parseDims();
        }
        if (accept(T::Assign)) p.def = parseExpr();
        if (p.vararg && p.def) error("a variadic parameter cannot have a default value");
        ps.push_back(p);
        skipNL();
        if (!accept(T::Comma)) break;
        skipNL();
    }
    expect(T::RParen, "')' closing the parameter list");
    return ps;
}

StmtP Parser::parseFuncDef(Ctx ctx, const std::string& name, const std::vector<Param>& params, bool isMethod) {
    (void)ctx;
    int line = at(p_ - 1).line;
    StmtP s = std::make_shared<Stmt>(); s->kind = SK::FuncDef; s->line = line;
    s->name = name; s->params = params; s->isMethod = isMethod;
    Ctx fc; fc.func = true; fc.method = isMethod;
    if (check(T::Assign)) {
        p_++;
        BlockP b = std::make_shared<Block>();
        b->line = line;
        StmtP r = std::make_shared<Stmt>(); r->kind = SK::Return; r->line = line;
        r->expr = parseExpr();
        b->stmts.push_back(r);
        s->body = b;
    } else {
        s->body = parseBlock(fc, false);
    }
    return s;
}

StmtP Parser::parseClassDef(const std::string& name, const std::vector<Param>& params, bool isView, bool isStyle) {
    int line = at(p_ - 1).line;
    if (!isView && !isStyle) {
        // `=` already consumed by the caller
    }
    StmtP s = std::make_shared<Stmt>(); s->kind = SK::ClassDef; s->line = line;
    s->name = name; s->params = params; s->isView = isView; s->isStyle = isStyle;
    Ctx cc; cc.classBody = true; cc.func = true; cc.method = true; cc.ui = isView;
    s->body = parseBlock(cc, isView);
    knownTypes_.insert(name);
    // pull out members
    for (auto& st : s->body->stmts) s->members.push_back(st);
    return s;
}

BlockP Parser::parseBlock(Ctx ctx, bool ui) {
    BlockP b = std::make_shared<Block>();
    b->line = cur().line;
    b->isUI = ui || ctx.ui;
    ctx.ui = b->isUI;
    expect(T::LParen, "'(' starting a block");
    skipNL();
    while (!check(T::RParen) && !atEnd()) {
        if (check(T::Kw_except)) {
            b->exceptLine = cur().line;
            p_++;
            b->exceptName = expect(T::Ident, "an error variable name").text;
            b->exceptBody = parseBlock(ctx, false);
            skipNL();
            break;
        }
        std::vector<StmtP> ss = statement(ctx);
        for (auto& s : ss) b->stmts.push_back(s);
        if (check(T::RParen) || atEnd()) break;
        if (check(T::Comma)) { p_++; skipNL(); continue; }   // `a = 1, b = 2` inside a block
        expectStmtEnd();
        skipNL();
    }
    expect(T::RParen, "')' closing the block");
    return b;
}

// ---------------------------------------------------------------- expressions
ExprP Parser::mkExpr(EK k, int line) {
    ExprP e = std::make_shared<Expr>();
    e->kind = k;
    e->line = line;
    return e;
}

// `to` and `step` are soft keywords: they only start a range when they sit between two
// operands, so they remain usable as ordinary names (e.g. `copy(from, to)`).
static bool isSoftWord(const Token& t, const char* w) {
    return t.type == T::Ident && t.text == w;
}

ExprP Parser::parseExpr() {
    ExprP e = parseOr();
    if (isSoftWord(cur(), "to")) {
        p_++;
        ExprP stop = parseOr();
        ExprP step;
        if (isSoftWord(cur(), "step")) { p_++; step = parseOr(); }
        ExprP it = mkExpr(EK::Iter, e->line);
        it->a = e; it->b = stop; it->bodyExpr = step;
        return it;
    }
    return e;
}

ExprP Parser::parseOr() {
    ExprP e = parseAnd();
    while (check(T::OrOr)) {
        int line = cur().line; p_++;
        ExprP r = parseAnd();
        ExprP n = mkExpr(EK::Logical, line);
        n->op = T::OrOr; n->a = e; n->b = r;
        e = n;
    }
    return e;
}
ExprP Parser::parseAnd() {
    ExprP e = parseBitOr();
    while (check(T::AndAnd)) {
        int line = cur().line; p_++;
        ExprP r = parseBitOr();
        ExprP n = mkExpr(EK::Logical, line);
        n->op = T::AndAnd; n->a = e; n->b = r;
        e = n;
    }
    return e;
}
ExprP Parser::parseBitOr() {
    ExprP e = parseBitXor();
    while (check(T::Pipe)) {
        int line = cur().line; p_++;
        ExprP r = parseBitXor();
        ExprP n = mkExpr(EK::Binary, line); n->op = T::Pipe; n->a = e; n->b = r; e = n;
    }
    return e;
}
ExprP Parser::parseBitXor() {
    ExprP e = parseBitAnd();
    while (check(T::Caret)) {
        int line = cur().line; p_++;
        ExprP r = parseBitAnd();
        ExprP n = mkExpr(EK::Binary, line); n->op = T::Caret; n->a = e; n->b = r; e = n;
    }
    return e;
}
ExprP Parser::parseBitAnd() {
    ExprP e = parseEquality();
    while (check(T::Amp)) {
        int line = cur().line; p_++;
        ExprP r = parseEquality();
        ExprP n = mkExpr(EK::Binary, line); n->op = T::Amp; n->a = e; n->b = r; e = n;
    }
    return e;
}
ExprP Parser::parseEquality() {
    ExprP e = parseComparison();
    while (check(T::Eq) || check(T::Ne)) {
        T op = cur().type; int line = cur().line; p_++;
        ExprP r = parseComparison();
        ExprP n = mkExpr(EK::Binary, line); n->op = op; n->a = e; n->b = r; e = n;
    }
    return e;
}
ExprP Parser::parseComparison() {
    ExprP e = parseShift();
    while (check(T::Lt) || check(T::Gt) || check(T::Le) || check(T::Ge)) {
        T op = cur().type; int line = cur().line; p_++;
        ExprP r = parseShift();
        ExprP n = mkExpr(EK::Binary, line); n->op = op; n->a = e; n->b = r; e = n;
    }
    return e;
}
ExprP Parser::parseShift() {
    ExprP e = parseAdditive();
    while (check(T::Shl) || check(T::Shr)) {
        T op = cur().type; int line = cur().line; p_++;
        ExprP r = parseAdditive();
        ExprP n = mkExpr(EK::Binary, line); n->op = op; n->a = e; n->b = r; e = n;
    }
    return e;
}
ExprP Parser::parseAdditive() {
    ExprP e = parseMultiplicative();
    while (check(T::Plus) || check(T::Minus)) {
        T op = cur().type; int line = cur().line; p_++;
        ExprP r = parseMultiplicative();
        ExprP n = mkExpr(EK::Binary, line); n->op = op; n->a = e; n->b = r; e = n;
    }
    return e;
}
ExprP Parser::parseMultiplicative() {
    ExprP e = parseUnary();
    while (check(T::Star) || check(T::Slash) || check(T::Percent)) {
        T op = cur().type; int line = cur().line; p_++;
        ExprP r = parseUnary();
        ExprP n = mkExpr(EK::Binary, line); n->op = op; n->a = e; n->b = r; e = n;
    }
    return e;
}
ExprP Parser::parseUnary() {
    if (check(T::Minus) || check(T::Not) || check(T::Tilde) || check(T::Plus)) {
        T op = cur().type; int line = cur().line; p_++;
        ExprP r = parseUnary();
        if (op == T::Plus) return r;
        ExprP n = mkExpr(EK::Unary, line); n->op = op; n->a = r;
        return n;
    }
    return parsePower();
}
ExprP Parser::parsePower() {
    ExprP e = parsePostfix();
    if (check(T::StarStar)) {
        int line = cur().line; p_++;
        ExprP r = parseUnary();
        ExprP n = mkExpr(EK::Binary, line); n->op = T::StarStar; n->a = e; n->b = r;
        return n;
    }
    return e;
}

// Components are PascalCase (documentation 15.3), so `Name(args)( children )` is a component
// call only when the callee starts with an upper case letter; every other `f(a)(b)` is a call
// of the value returned by `f(a)`.
static bool looksLikeComponentCallee(const ExprP& e) {
    if (!e) return false;
    if (e->kind == EK::Ident) return !e->name.empty() && isupper((unsigned char)e->name[0]);
    if (e->kind == EK::Field)
        return (!e->name.empty() && isupper((unsigned char)e->name[0])) || looksLikeComponentCallee(e->a);
    if (e->kind == EK::Call || e->kind == EK::Index) return looksLikeComponentCallee(e->a);
    return false;
}

ExprP Parser::parsePostfix() {
    ExprP e = parsePrimary();
    while (true) {
        if (check(T::Dot)) {
            int line = cur().line; p_++;
            std::string nm = expect(T::Ident, "a member name").text;
            ExprP n = mkExpr(EK::Field, line);
            n->a = e; n->name = nm;
            e = n;
            continue;
        }
        if (check(T::LBracket)) {
            int line = cur().line; p_++;
            skipNL();
            ExprP idx = parseExpr();
            skipNL();
            expect(T::RBracket, "']' closing an index");
            ExprP n = mkExpr(EK::Index, line);
            n->a = e; n->b = idx;
            e = n;
            continue;
        }
        if (check(T::LParen) && !cur().blockOpen && at(p_).line == at(p_ - 1).line) {
            if (e->kind == EK::Call && !e->children && looksLikeComponentCallee(e->a)) {
                Ctx cc; cc.ui = true;
                e->children = parseBlock(cc, true);
                continue;
            }
            if (e->kind == EK::Ident || e->kind == EK::Field || e->kind == EK::Index) {
                e = parseCallOrChildren(e);
                continue;
            }
            if (e->kind == EK::Call) {
                e = parseCallOrChildren(e);       // call the value returned by `f(a)`
                continue;
            }
            break;
        }
        break;
    }
    return e;
}

ExprP Parser::parseCallOrChildren(ExprP callee) {
    ExprP e = mkExpr(EK::Call, callee->line);
    e->a = callee;
    e->args = parseCallArgs(T::RParen);
    if (check(T::LParen) && !cur().blockOpen && at(p_).line == at(p_ - 1).line &&
        looksLikeComponentCallee(callee)) {
        Ctx cc; cc.ui = true;
        e->children = parseBlock(cc, true);
    }
    return e;
}

std::vector<Arg> Parser::parseCallArgs(T closer) {
    std::vector<Arg> out;
    expect(T::LParen, "'('");
    skipNL();
    while (!check(closer)) {
        skipNL();
        if (check(closer)) break;
        Arg a;
        if (accept(T::Ellipsis)) {
            a.spread = true;
            a.value = parseExpr();
        } else if (check(T::Ident) && type(1) == T::Assign) {
            a.name = cur().text;
            p_ += 2;
            a.value = parseExpr();
        } else {
            a.value = parseExpr();
        }
        out.push_back(a);
        skipNL();
        if (!accept(T::Comma)) break;
        skipNL();
    }
    skipNL();
    expect(closer, "')' closing an argument list");
    return out;
}

ExprP Parser::parseListLiteral() {
    int line = cur().line;
    expect(T::LBracket, "'['");
    skipNL();
    ExprP e = mkExpr(EK::List, line);
    while (!check(T::RBracket)) {
        skipNL();
        if (check(T::RBracket)) break;
        e->items.push_back(parseExpr());
        skipNL();
        if (!accept(T::Comma)) break;
        skipNL();
    }
    expect(T::RBracket, "']' closing a list literal");
    return e;
}

ExprP Parser::parsePiecewise() {
    int line = cur().line;
    expect(T::LBrace, "'{'");
    skipNL();
    ExprP e = mkExpr(EK::Piecewise, line);
    while (!check(T::RBrace) && !atEnd()) {
        if (accept(T::Kw_else)) {
            expect(T::Colon, "':' after 'else'");
            e->elseVal = parseExpr();
            if (check(T::RBrace)) break;
            expectStmtEnd();
            skipNL();
            continue;
        }
        ExprP cond = parseExpr();
        expect(T::Colon, "':' after a piecewise condition");
        ExprP val = parseExpr();
        e->cases.push_back({cond, val});
        if (check(T::RBrace)) break;
        expectStmtEnd();
        skipNL();
    }
    expect(T::RBrace, "'}' closing a piecewise expression");
    if (!e->elseVal) errorAt(line - 0, "a piecewise expression requires an 'else' branch");
    return e;
}

ExprP Parser::parseLambdaOrCastOrGroup() {
    int line = cur().line;
    size_t open = p_;
    size_t close = matchBracket(open);
    if (close == (size_t)-1) error("unbalanced '('");

    // ( Type ) expr   -> cast
    if (close == open + 2 && at(open + 1).type == T::Ident) {
        std::string nm = at(open + 1).text;
        T nt = at(close + 1).type;
        bool known = isTypeName(nm);
        bool nextIsCallParen = (nt == T::LParen);
        if (known ? startsOperand(nt, true)
                  : (startsOperand(nt, false) || nt == T::LParen)) {
            if (!(nextIsCallParen && !known && looksLikeLambda(open, close))) {
                p_ = close + 1;
                ExprP callee = mkExpr(EK::Ident, line);
                callee->name = nm;
                ExprP call = mkExpr(EK::Call, line);
                call->a = callee;
                Arg a; a.value = parseUnary();
                call->args.push_back(a);
                return call;
            }
        }
    }
    // ( params ) ( body )  -> lambda
    if (looksLikeLambda(open, close)) {
        std::vector<Param> ps = parseParams();
        Ctx fc; fc.func = true;
        ExprP e = mkExpr(EK::Lambda, line);
        e->params = ps;
        e->body = parseBlock(fc, false);
        return e;
    }
    // plain grouping / tuple
    p_++;                                   // '('
    skipNL();
    if (check(T::RParen)) {                 // empty tuple
        p_++;
        return mkExpr(EK::Tuple, line);
    }
    ExprP first = parseExpr();
    skipNL();
    if (check(T::Comma)) {
        ExprP e = mkExpr(EK::Tuple, line);
        e->items.push_back(first);
        while (accept(T::Comma)) {
            skipNL();
            if (check(T::RParen)) break;
            e->items.push_back(parseExpr());
            skipNL();
        }
        expect(T::RParen, "')' closing a tuple");
        return e;
    }
    expect(T::RParen, "')' closing a group");
    return first;
}

ExprP Parser::parsePrimary() {
    int line = cur().line;
    if (tryMacro()) return parsePostfix();

    switch (cur().type) {
        case T::Int:   {
            int64_t v = cur().ival;
            std::string big = cur().text;          // set by the lexer when the literal needs 128 bits
            p_++;
            ExprP e = mkExpr(EK::Int, line);
            e->ival = v;
            if (!big.empty()) { e->wideLiteral = true; e->sval = big; }
            return e;
        }
        case T::Float: { double v = cur().fval; p_++; ExprP e = mkExpr(EK::Float, line); e->fval = v; return e; }
        case T::Str:   { std::string v = cur().text; p_++; ExprP e = mkExpr(EK::Str, line); e->sval = v; return e; }
        case T::Char:  { std::string v = cur().text; p_++; ExprP e = mkExpr(EK::Str, line); e->sval = v; return e; }
        case T::Color: { uint32_t v = (uint32_t)cur().ival; p_++; ExprP e = mkExpr(EK::Color, line); e->color = v; return e; }
        case T::Kw_true:  { p_++; ExprP e = mkExpr(EK::Bool, line); e->bval = true; return e; }
        case T::Kw_false: { p_++; ExprP e = mkExpr(EK::Bool, line); e->bval = false; return e; }
        case T::Kw_null:  { p_++; return mkExpr(EK::Null, line); }
        case T::Kw_this:  { p_++; return mkExpr(EK::This, line); }
        case T::Kw_super: { p_++; return mkExpr(EK::Super, line); }
        case T::LBracket: return parseListLiteral();
        case T::LBrace:   return parsePiecewise();
        case T::LParen:   return parseLambdaOrCastOrGroup();
        case T::Ident: {
            std::string nm = cur().text;
            p_++;
            if (nm == "children" && check(T::LParen) && type(1) == T::RParen) {
                p_ += 2;
                return mkExpr(EK::Children, line);
            }
            ExprP e = mkExpr(EK::Ident, line);
            e->name = nm;
            return e;
        }
        default: break;
    }
    error("expected an expression but found " + std::string(tokenName(cur().type)));
}

// ---------------------------------------------------------------- entry points
ExprP Parser::exprFromTokens(std::vector<Token> toks, const std::string& file) {
    if (toks.empty()) return nullptr;
    if (toks.back().type != T::End) {
        Token e; e.type = T::End; e.line = toks.back().line;
        toks.push_back(e);
    }
    MacroRegistry reg;
    Parser p(std::move(toks), file, nullptr, &reg);
    p.noMacro_ = true;
    try {
        ExprP e = p.parseExpr();
        if (p.cur().type != T::End) return nullptr;
        return e;
    } catch (CompileError&) {
        return nullptr;
    }
}

} // namespace annota
