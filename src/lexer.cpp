// Annota - lexer.cpp
#include <cctype>
#include <cstring>
#include "lexer.hpp"
#include <unordered_map>

namespace annota {

const char* tokenName(T t) {
    switch (t) {
        case T::End: return "end of file";
        case T::Newline: return "newline";
        case T::Int: return "int";
        case T::Float: return "float";
        case T::Str: return "string";
        case T::Char: return "char";
        case T::Color: return "color";
        case T::Ident: return "identifier";
        case T::MacroParam: return "macro parameter";
        case T::Kw_new: return "'new'";
        case T::Kw_lend: return "'lend'";
        case T::Kw_del: return "'del'";
        case T::Kw_const: return "'const'";
        case T::Kw_macro: return "'macro'";
        case T::Kw_use: return "'use'";
        case T::Kw_view: return "'view'";
        case T::Kw_state: return "'state'";
        case T::Kw_if: return "'if'";
        case T::Kw_else: return "'else'";
        case T::Kw_elif: return "'elif'";
        case T::Kw_for: return "'for'";
        case T::Kw_while: return "'while'";
        case T::Kw_in: return "'in'";
        case T::Kw_to: return "'to'";
        case T::Kw_step: return "'step'";
        case T::Kw_break: return "'break'";
        case T::Kw_continue: return "'continue'";
        case T::Kw_throw: return "'throw'";
        case T::Kw_except: return "'except'";
        case T::Kw_print: return "'print'";
        case T::Kw_input: return "'input'";
        case T::Kw_true: return "'true'";
        case T::Kw_false: return "'false'";
        case T::Kw_null: return "'null'";
        case T::Kw_this: return "'this'";
        case T::Kw_super: return "'super'";
        case T::Kw_sep: return "'sep'";
        case T::LParen: return "'('";
        case T::RParen: return "')'";
        case T::LBracket: return "'['";
        case T::RBracket: return "']'";
        case T::LBrace: return "'{'";
        case T::RBrace: return "'}'";
        case T::Comma: return "','";
        case T::Colon: return "':'";
        case T::Dot: return "'.'";
        case T::Ellipsis: return "'...'";
        case T::Semi: return "';'";
        case T::Assign: return "'='";
        case T::PlusA: return "'+='";
        case T::MinusA: return "'-='";
        case T::StarA: return "'*='";
        case T::SlashA: return "'/='";
        case T::PercentA: return "'%='";
        case T::StarStarA: return "'**='";
        case T::AmpA: return "'&='";
        case T::PipeA: return "'|='";
        case T::CaretA: return "'^='";
        case T::ShlA: return "'<<='";
        case T::ShrA: return "'>>='";
        case T::Plus: return "'+'";
        case T::Minus: return "'-'";
        case T::Star: return "'*'";
        case T::Slash: return "'/'";
        case T::Percent: return "'%'";
        case T::StarStar: return "'**'";
        case T::Eq: return "'=='";
        case T::Ne: return "'!='";
        case T::Lt: return "'<'";
        case T::Gt: return "'>'";
        case T::Le: return "'<='";
        case T::Ge: return "'>='";
        case T::AndAnd: return "'&&'";
        case T::OrOr: return "'||'";
        case T::Not: return "'!'";
        case T::Amp: return "'&'";
        case T::Pipe: return "'|'";
        case T::Caret: return "'^'";
        case T::Tilde: return "'~'";
        case T::Shl: return "'<<'";
        case T::Shr: return "'>>'";
    }
    return "?";
}

namespace {
const std::unordered_map<std::string, T>& keywords() {
    static const std::unordered_map<std::string, T> kw = {
        {"new", T::Kw_new}, {"del", T::Kw_del}, {"const", T::Kw_const},
        {"lend", T::Kw_lend},
        {"macro", T::Kw_macro}, {"use", T::Kw_use}, {"view", T::Kw_view},
        {"state", T::Kw_state}, {"if", T::Kw_if}, {"else", T::Kw_else}, {"elif", T::Kw_elif},
        {"for", T::Kw_for}, {"while", T::Kw_while}, {"in", T::Kw_in},
        {"break", T::Kw_break},
        {"continue", T::Kw_continue}, {"throw", T::Kw_throw}, {"except", T::Kw_except},
        {"print", T::Kw_print}, {"input", T::Kw_input}, {"true", T::Kw_true},
        {"false", T::Kw_false}, {"null", T::Kw_null}, {"this", T::Kw_this},
        {"super", T::Kw_super},
    };
    return kw;
}
inline bool isIdStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
inline bool isIdChar(char c) { return isIdStart(c) || (c >= '0' && c <= '9'); }
inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
inline bool isHex(char c) { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
inline int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}
} // namespace

std::vector<Token> lex(const std::string& src, const std::string& file) {
    std::vector<Token> out;
    size_t p = 0, n = src.size();
    int line = 1, col = 1;
    bool atLineStart = true;

    auto push = [&](T t, int l, int c) {
        Token tk; tk.type = t; tk.line = l; tk.col = c;
        out.push_back(std::move(tk));
        return out.size() - 1;
    };
    auto err = [&](const std::string& m) -> void {
        throw CompileError(file + ":" + formatInt(line) + ": " + m, line);
    };
    // A line that ends with a binary operator (or `=` / `.`) continues on the next line, so
    //     a + b +
    //     c + d
    // is one expression.  Ends with a comma are NOT implicit, because `new a, b` is a
    // statement list.
    auto continuesLine = [](T t) {
        switch (t) {
            case T::Plus: case T::Minus: case T::Star: case T::Slash: case T::Percent:
            case T::StarStar: case T::Amp: case T::Pipe: case T::Caret: case T::Shl: case T::Shr:
            case T::Eq: case T::Ne: case T::Lt: case T::Gt: case T::Le: case T::Ge:
            case T::AndAnd: case T::OrOr: case T::Assign: case T::Dot:
            case T::PlusA: case T::MinusA: case T::StarA: case T::SlashA: case T::PercentA:
            case T::StarStarA: case T::AmpA: case T::PipeA: case T::CaretA: case T::ShlA:
            case T::ShrA: case T::Kw_to: case T::Kw_step:
                return true;
            default:
                return false;
        }
    };
    auto newline = [&]() {
        if (!out.empty() && out.back().type == T::Newline) return;   // collapse runs
        if (!out.empty() && continuesLine(out.back().type)) return;  // implicit continuation
        // `) else` / `) elif` may start the next line: an if/elif chain reads better with one
        // branch per line, and the block above already ended with its `)`
        if (!out.empty() && out.back().type == T::RParen && p < n) {
            size_t q = p;
            while (q < n && (src[q] == '\n' || src[q] == '\r' || src[q] == ' ' || src[q] == '\t')) q++;
            auto wordAt = [&](size_t at, const char* w) {
                size_t len = std::strlen(w);
                if (at + len > n || src.compare(at, len, w) != 0) return false;
                char after = at + len < n ? src[at + len] : ' ';
                return !(std::isalnum((unsigned char)after) || after == '_');
            };
            if (wordAt(q, "else") || wordAt(q, "elif")) return;
        }
        push(T::Newline, line, col);
    };

    while (p < n) {
        char c = src[p];
        // ---- skip a UTF-8 BOM
        if (p == 0 && n >= 3 && (unsigned char)src[0] == 0xEF &&
            (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF) {
            p = 3; col = 1; continue;
        }
        // ---- line ending
        if (c == '\n') { p++; line++; col = 1; atLineStart = true; newline(); continue; }
        if (c == '\r') { p++; col++; continue; }
        if (c == ' ' || c == '\t') { p++; col++; continue; }

        // ---- explicit line continuation: a trailing `\` joins the next line
        if (c == '\\') {
            size_t q = p + 1;
            while (q < n && (src[q] == ' ' || src[q] == '\t' || src[q] == '\r')) q++;
            if (q < n && src[q] == '\n') { p = q + 1; line++; col = 1; atLineStart = false; continue; }
            err(std::string("unexpected character '\\'"));
        }

        int tl = line, tc = col;

        // ---- section marker  —分段 名称   (em dash U+2014)
        if (atLineStart && (unsigned char)c == 0xE2 && p + 2 < n &&
            (unsigned char)src[p + 1] == 0x80 && (unsigned char)src[p + 2] == 0x94) {
            while (p < n && src[p] != '\n') { p++; col++; }
            continue;
        }
        // ---- comments
        if (c == '-' && p + 1 < n && src[p + 1] == '-') {
            while (p < n && src[p] != '\n') { p++; col++; }
            continue;
        }
        if (c == '-' && p + 1 < n && src[p + 1] == '[') {
            p += 2; col += 2;
            bool closed = false;
            while (p + 1 < n) {
                if (src[p] == ']' && src[p + 1] == '-') { p += 2; col += 2; closed = true; break; }
                if (src[p] == '\n') { line++; col = 1; p++; continue; }
                p++; col++;
            }
            if (!closed) err("unterminated multi-line comment");
            continue;
        }
        atLineStart = false;

        // ---- identifiers / keywords
        if (isIdStart(c)) {
            size_t s = p;
            while (p < n && isIdChar(src[p])) { p++; col++; }
            std::string word = src.substr(s, p - s);
            auto it = keywords().find(word);
            size_t idx = push(it == keywords().end() ? T::Ident : it->second, tl, tc);
            out[idx].text = word;
            continue;
        }
        // ---- macro placeholder $name
        if (c == '$') {
            p++; col++;
            size_t s = p;
            while (p < n && isIdChar(src[p])) { p++; col++; }
            if (s == p) err("expected a name after '$'");
            size_t idx = push(T::MacroParam, tl, tc);
            out[idx].text = src.substr(s, p - s);
            continue;
        }
        // ---- numbers
        if (isDigit(c)) {
            size_t s = p;
            if (c == '0' && p + 1 < n && (src[p + 1] == 'x' || src[p + 1] == 'X')) {
                p += 2; col += 2;
                size_t d = p;
                while (p < n && (isHex(src[p]) || src[p] == '_')) { p++; col++; }
                if (d == p) err("malformed hexadecimal literal");
                size_t idx = push(T::Int, tl, tc);
                int64_t v = 0;
                unsigned __int128 acc = 0;
                bool big = false;
                for (size_t k = d; k < p; k++) {
                    if (src[k] == '_') continue;
                    v = v * 16 + hexVal(src[k]);
                    acc = acc * 16 + (unsigned)hexVal(src[k]);
                    if (acc > (unsigned __int128)9223372036854775807ULL) big = true;
                }
                out[idx].ival = v;
                if (big) {
                    std::string digits = "0x";
                    for (size_t k = d; k < p; k++) if (src[k] != '_' ) digits += src[k];
                    out[idx].text = digits;
                }
                continue;
            }
            if (c == '0' && p + 1 < n && (src[p + 1] == 'b' || src[p + 1] == 'B')) {
                p += 2; col += 2;
                size_t d = p;
                int64_t v = 0;
                while (p < n && (src[p] == '0' || src[p] == '1' || src[p] == '_')) {
                    if (src[p] != '_') v = v * 2 + (src[p] - '0');
                    p++; col++;
                }
                if (d == p) err("malformed binary literal");
                size_t idx = push(T::Int, tl, tc);
                out[idx].ival = v;
                continue;
            }
            if (c == '0' && p + 1 < n && (src[p + 1] == 'o' || src[p + 1] == 'O')) {
                p += 2; col += 2;
                size_t d = p;
                int64_t v = 0;
                while (p < n && src[p] >= '0' && src[p] <= '7') { v = v * 8 + (src[p] - '0'); p++; col++; }
                if (d == p) err("malformed octal literal");
                size_t idx = push(T::Int, tl, tc); out[idx].ival = v;
                if (v < 0) {   // overflowed the signed range while accumulating in base 8
                    std::string digits = "0o";
                    for (size_t k = d; k < p; k++) if (src[k] != '_') digits += src[k];
                    out[idx].text = digits;
                }
                continue;
            }
            bool isFloat = false;
            while (p < n && (isDigit(src[p]) || src[p] == '_')) { p++; col++; }
            if (p < n && src[p] == '.' && p + 1 < n && isDigit(src[p + 1])) {
                isFloat = true; p++; col++;
                while (p < n && (isDigit(src[p]) || src[p] == '_')) { p++; col++; }
            } else if (p < n && src[p] == '.' && !(p + 1 < n && src[p + 1] == '.')) {
                isFloat = true; p++; col++;
            }
            if (p < n && (src[p] == 'e' || src[p] == 'E')) {
                size_t q = p + 1;
                if (q < n && (src[q] == '+' || src[q] == '-')) q++;
                if (q < n && isDigit(src[q])) {
                    isFloat = true; p = q;
                    while (p < n && isDigit(src[p])) { p++; col++; }
                }
            }
            std::string num = src.substr(s, p - s);
            std::string clean;
            for (char ch : num) if (ch != '_') clean += ch;
            size_t idx = push(isFloat ? T::Float : T::Int, tl, tc);
            if (isFloat) out[idx].fval = std::strtod(clean.c_str(), nullptr);
            else {
                out[idx].ival = (int64_t)std::strtoll(clean.c_str(), nullptr, 10);
                // longer than the 64 bit range: keep the digits, the parser makes it a 128 bit
                // constant (`longlong`)
                if (clean.size() > 19 ||
                    (clean.size() == 19 && clean > std::string("9223372036854775807")))
                    out[idx].text = clean;
            }
            continue;
        }
        // ---- strings
        if (c == '"') {
            if (p + 2 < n && src[p + 1] == '"' && src[p + 2] == '"') {
                p += 3; col += 3;
                std::string val;
                bool closed = false;
                while (p + 2 < n) {
                    if (src[p] == '"' && src[p + 1] == '"' && src[p + 2] == '"') {
                        p += 3; col += 3; closed = true; break;
                    }
                    if (src[p] == '\n') { line++; col = 1; }
                    val += src[p]; p++; col++;
                }
                if (!closed) err("unterminated multi-line string");
                // strip a single leading newline
                if (!val.empty() && val[0] == '\n') val.erase(0, 1);
                size_t idx = push(T::Str, tl, tc); out[idx].text = val;
                continue;
            }
            p++; col++;
            std::string val;
            bool closed = false;
            while (p < n) {
                char ch = src[p];
                if (ch == '"') { p++; col++; closed = true; break; }
                if (ch == '\n') err("unterminated string literal");
                if (ch == '\\' && p + 1 < n) {
                    char e = src[p + 1];
                    p += 2; col += 2;
                    switch (e) {
                        case 'n': val += '\n'; break;
                        case 't': val += '\t'; break;
                        case 'r': val += '\r'; break;
                        case '0': val += '\0'; break;
                        case '\\': val += '\\'; break;
                        case '"': val += '"'; break;
                        case '\'': val += '\''; break;
                        default: val += '\\'; val += e; break;
                    }
                    continue;
                }
                val += ch; p++; col++;
            }
            if (!closed) err("unterminated string literal");
            size_t idx = push(T::Str, tl, tc); out[idx].text = val;
            continue;
        }
        if (c == '\'') {
            p++; col++;
            std::string val;
            if (p < n && src[p] == '\\' && p + 1 < n) {
                char e = src[p + 1]; p += 2; col += 2;
                switch (e) {
                    case 'n': val += '\n'; break;
                    case 't': val += '\t'; break;
                    case '\\': val += '\\'; break;
                    case '\'': val += '\''; break;
                    case '"': val += '"'; break;
                    default: val += e; break;
                }
            } else if (p < n) {
                val += src[p]; p++; col++;
            }
            if (p >= n || src[p] != '\'') err("unterminated character literal");
            p++; col++;
            size_t idx = push(T::Char, tl, tc); out[idx].text = val;
            continue;
        }
        // ---- colour literal #RRGGBB / #RGB
        if (c == '#') {
            size_t q = p + 1;
            while (q < n && isHex(src[q]) && q - p <= 8) q++;
            size_t len = q - p - 1;
            if (len == 3 || len == 6) {
                uint32_t v;
                if (len == 3) {
                    int r = hexVal(src[p + 1]), g = hexVal(src[p + 2]), b2 = hexVal(src[p + 3]);
                    v = (uint32_t)((r * 17) << 16 | (g * 17) << 8 | (b2 * 17));
                } else {
                    v = 0;
                    for (size_t k = p + 1; k < q; k++) v = v * 16 + hexVal(src[k]);
                }
                p = q; col += len + 1;
                size_t idx = push(T::Color, tl, tc); out[idx].ival = (int64_t)v;
                continue;
            }
            err("malformed colour literal, expected #RGB or #RRGGBB");
        }
        // ---- operators & punctuation (longest match first)
        struct OpDef { const char* s; T t; };
        static const OpDef ops[] = {
            {"**=", T::StarStarA}, {"<<=", T::ShlA}, {">>=", T::ShrA},
            {"+=", T::PlusA}, {"-=", T::MinusA}, {"*=", T::StarA}, {"/=", T::SlashA},
            {"%=", T::PercentA}, {"&=", T::AmpA}, {"|=", T::PipeA}, {"^=", T::CaretA},
            {"==", T::Eq}, {"!=", T::Ne}, {"<=", T::Le}, {">=", T::Ge},
            {"&&", T::AndAnd}, {"||", T::OrOr}, {"<<", T::Shl}, {">>", T::Shr},
            {"**", T::StarStar}, {"...", T::Ellipsis},
            {"(", T::LParen}, {")", T::RParen}, {"[", T::LBracket}, {"]", T::RBracket},
            {"{", T::LBrace}, {"}", T::RBrace}, {",", T::Comma}, {":", T::Colon},
            {".", T::Dot}, {";", T::Semi}, {"=", T::Assign},
            {"+", T::Plus}, {"-", T::Minus}, {"*", T::Star}, {"/", T::Slash},
            {"%", T::Percent}, {"<", T::Lt}, {">", T::Gt}, {"!", T::Not},
            {"&", T::Amp}, {"|", T::Pipe}, {"^", T::Caret}, {"~", T::Tilde},
        };
        bool matched = false;
        for (const auto& op : ops) {
            size_t len = std::strlen(op.s);
            if (src.compare(p, len, op.s) == 0) {
                p += len; col += (int)len;
                // greedy: make sure "..." is preferred over "." and "**=" over "**"
                push(op.t, tl, tc);
                matched = true;
                break;
            }
        }
        if (!matched) err(std::string("unexpected character '") + c + "'");
    }
    push(T::Newline, line, col);
    push(T::End, line, col);
    return out;
}

} // namespace annota
