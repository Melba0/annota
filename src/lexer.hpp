// Annota - lexer.hpp
#pragma once
#include "common.hpp"

namespace annota {

enum class T : uint8_t {
    End, Newline,
    Int, Float, Str, Char, Color, Ident, MacroParam,
    // keywords
    Kw_new, Kw_del, Kw_const, Kw_macro, Kw_use, Kw_view, Kw_state,
    Kw_if, Kw_else, Kw_for, Kw_while, Kw_in, Kw_to, Kw_step,
    Kw_break, Kw_continue, Kw_throw, Kw_except, Kw_print, Kw_input,
    Kw_true, Kw_false, Kw_null, Kw_this, Kw_super, Kw_sep,
    // punctuation
    LParen, RParen, LBracket, RBracket, LBrace, RBrace,
    Comma, Colon, Dot, Ellipsis, Semi,
    Assign, PlusA, MinusA, StarA, SlashA, PercentA, StarStarA,
    AmpA, PipeA, CaretA, ShlA, ShrA,
    Plus, Minus, Star, Slash, Percent, StarStar,
    Eq, Ne, Lt, Gt, Le, Ge, AndAnd, OrOr, Not,
    Amp, Pipe, Caret, Tilde, Shl, Shr,
};

struct Token {
    T type = T::End;
    std::string text;      // identifier name / string content / char content
    int64_t ival = 0;
    double fval = 0.0;
    int line = 1, col = 1;
    bool blockOpen = false;   // marked on the '(' that opens an if/while/for body
};

const char* tokenName(T t);

// Tokenize a source buffer. Throws CompileError on lexical errors.
std::vector<Token> lex(const std::string& src, const std::string& file);

} // namespace annota
