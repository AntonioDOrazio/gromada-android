#pragma once
// Lexer of the Gromada script language (a small C dialect).
//
// Input is raw bytes: string/char literals and comments may contain any byte (the game files
// mix CP866, CP1251 and CP1252), while code outside literals must be ASCII. CR, LF, tabs and
// form feeds are blanks; byte 0x1A (DOS end-of-file) ends the input.
#include <array>
#include <cctype>
#include <cstdint>
#include <format>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

#include "script/diagnostics.hpp"

namespace gromada::script {

enum class TokenKind : std::uint8_t { End, Identifier, Number, String, Char, Punct };

enum class Punct : std::uint8_t {
    None,
    LParen, RParen, LBrace, RBrace, LBracket, RBracket, Comma, Semicolon, Hash, Question, Colon,
    Plus, Minus, Star, Slash, Percent, Amp, Pipe, Caret, Tilde, Bang, Less, Greater, Assign,
    PlusPlus, MinusMinus, Shl, Shr, LessEq, GreaterEq, EqEq, NotEq, AndAnd, OrOr,
    PlusAssign, MinusAssign, StarAssign, SlashAssign, PercentAssign,
    AmpAssign, PipeAssign, CaretAssign, ShlAssign, ShrAssign,
};

constexpr std::string_view punctSpelling(Punct p) {
    constexpr std::array<std::string_view, 46> names{
        "",   "(",  ")",  "{",  "}",  "[",  "]",  ",",  ";",  "#",  "?",  ":",
        "+",  "-",  "*",  "/",  "%",  "&",  "|",  "^",  "~",  "!",  "<",  ">",  "=",
        "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
        "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="};
    return names[static_cast<std::size_t>(p)];
}

struct Token {
    TokenKind kind = TokenKind::End;
    Punct punct = Punct::None;
    bool lineStart = false;   // first token on its source line (used for # directives)
    SourceLocation loc;
    std::string text;         // identifier name, decoded string literal bytes, or punctuator spelling
    std::string raw;          // String: the bytes between the quotes, escapes not processed
    std::int32_t number = 0;  // value of Number and Char tokens

    bool is(Punct p) const { return kind == TokenKind::Punct && punct == p; }
    bool isIdent(std::string_view name) const { return kind == TokenKind::Identifier && text == name; }

    std::string describe() const {
        switch (kind) {
            case TokenKind::End: return "end of file";
            case TokenKind::Identifier: return "'" + text + "'";
            case TokenKind::Number: return "number " + std::to_string(number);
            case TokenKind::String: return "string literal";
            case TokenKind::Char: return "character literal";
            case TokenKind::Punct: return "'" + text + "'";
        }
        return "?";
    }
};

class Lexer {
public:
    Lexer(std::string_view source, const std::string* fileName) : src_{source}, file_{fileName} {}

    // Tokenizes the whole input; the last token is always TokenKind::End.
    std::vector<Token> tokenize() {
        std::vector<Token> out;
        for (;;) {
            out.push_back(next());
            if (out.back().kind == TokenKind::End) return out;
        }
    }

    Token next() {
        skipBlanksAndComments();
        Token t;
        t.loc = here();
        t.lineStart = atLineStart_;
        atLineStart_ = false;
        if (atEnd()) { t.kind = TokenKind::End; return t; }
        const char c = src_[pos_];
        if (isIdentStart(c)) {
            const std::size_t b = pos_;
            while (!atEnd() && isIdentChar(src_[pos_])) advance();
            t.kind = TokenKind::Identifier;
            t.text = std::string{src_.substr(b, pos_ - b)};
            return t;
        }
        if (c >= '0' && c <= '9') { lexNumber(t); return t; }
        if (c == '"') { lexString(t); return t; }
        if (c == '\'') { lexChar(t); return t; }
        lexPunct(t);
        return t;
    }

private:
    static bool isIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
    static bool isIdentChar(char c) { return isIdentStart(c) || (c >= '0' && c <= '9'); }
    static bool isBlank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

    bool atEnd() const { return pos_ >= src_.size() || src_[pos_] == '\x1A'; }
    char peek(std::size_t ahead = 0) const {
        const std::size_t p = pos_ + ahead;
        return p < src_.size() ? src_[p] : '\0';
    }
    void advance() {
        if (src_[pos_] == '\n') { ++line_; col_ = 1; atLineStart_ = true; }
        else ++col_;
        ++pos_;
    }
    SourceLocation here() const { return SourceLocation{file_, line_, col_}; }

    [[noreturn]] void fail(const SourceLocation& loc, std::string msg) const {
        throw ScriptError(ScriptError::Phase::Lex, loc, std::move(msg));
    }

    void skipBlanksAndComments() {
        for (;;) {
            while (!atEnd() && isBlank(src_[pos_])) advance();
            if (atEnd()) return;
            if (peek() == '/' && peek(1) == '/') {
                while (!atEnd() && src_[pos_] != '\n') advance();
            } else if (peek() == '/' && peek(1) == '*') {
                // Like in C a comment is a single blank: line breaks inside it do not make the
                // next token start a line (so it cannot start a # directive).
                const bool lineStartBefore = atLineStart_;
                const auto start = here();
                advance(); advance();
                for (;;) {
                    if (pos_ >= src_.size()) fail(start, "unterminated /* comment");
                    if (src_[pos_] == '*' && peek(1) == '/') { advance(); advance(); break; }
                    advance();
                }
                atLineStart_ = lineStartBefore;
            } else {
                return;
            }
        }
    }

    void lexNumber(Token& t) {
        t.kind = TokenKind::Number;
        const std::size_t b = pos_;
        std::uint64_t v = 0;
        int base = 10;
        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
            base = 16;
            advance(); advance();
            if (!std::isxdigit(static_cast<unsigned char>(peek()))) fail(t.loc, "hexadecimal literal without digits");
        } else if (peek() == '0') {
            base = 8; // C rules: a leading 0 means octal (the game scripts have no such literal)
        }
        while (!atEnd() && isIdentChar(src_[pos_])) {
            const char c = src_[pos_];
            int d = -1;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            if (d < 0 || d >= base) fail(t.loc, "invalid numeric literal '" + std::string{src_.substr(b, pos_ - b + 1)} + "'");
            v = v * std::uint64_t(base) + std::uint64_t(d);
            if (v > 0xFFFFFFFFull) fail(t.loc, "numeric literal does not fit in 32 bits");
            advance();
        }
        t.number = static_cast<std::int32_t>(static_cast<std::uint32_t>(v));
        t.text = std::string{src_.substr(b, pos_ - b)};
    }

    // One (possibly escaped) byte of a string or char literal.
    unsigned char lexLiteralByte(const SourceLocation& start) {
        if (atEnd() || src_[pos_] == '\n') fail(start, "unterminated literal");
        char c = src_[pos_];
        advance();
        if (c != '\\') return static_cast<unsigned char>(c);
        if (atEnd()) fail(start, "unterminated literal");
        c = src_[pos_];
        advance();
        switch (c) {
            case 'n': return '\n';
            case 't': return '\t';
            case 'r': return '\r';
            case 'a': return '\a';
            case 'b': return '\b';
            case 'f': return '\f';
            case 'v': return '\v';
            case 'x': {
                unsigned v = 0;
                int n = 0;
                while (n < 2 && std::isxdigit(static_cast<unsigned char>(peek()))) {
                    const char h = src_[pos_];
                    v = v * 16 + unsigned(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
                    advance();
                    ++n;
                }
                if (n == 0) fail(start, "\\x escape without digits");
                return static_cast<unsigned char>(v);
            }
            default:
                if (c >= '0' && c <= '7') {
                    unsigned v = unsigned(c - '0');
                    for (int n = 1; n < 3 && peek() >= '0' && peek() <= '7'; ++n) {
                        v = v * 8 + unsigned(src_[pos_] - '0');
                        advance();
                    }
                    return static_cast<unsigned char>(v);
                }
                return static_cast<unsigned char>(c); // \\ \" \' and unknown escapes: the char itself
        }
    }

    void lexString(Token& t) {
        t.kind = TokenKind::String;
        advance(); // opening quote
        const std::size_t b = pos_;
        while (!atEnd() && src_[pos_] != '"') t.text.push_back(static_cast<char>(lexLiteralByte(t.loc)));
        if (atEnd()) fail(t.loc, "unterminated string literal");
        t.raw = std::string{src_.substr(b, pos_ - b)};
        advance();
    }

    void lexChar(Token& t) {
        t.kind = TokenKind::Char;
        advance(); // opening quote
        if (peek() == '\'') fail(t.loc, "empty character literal");
        t.number = lexLiteralByte(t.loc); // unsigned: 'x' is 0..255
        if (atEnd() || src_[pos_] != '\'') fail(t.loc, "character literal with more than one character");
        advance();
        t.text = std::string(1, static_cast<char>(t.number));
    }

    void lexPunct(Token& t) {
        // Longest match first.
        static constexpr std::array<std::pair<std::string_view, Punct>, 45> table{{
            {"<<=", Punct::ShlAssign}, {">>=", Punct::ShrAssign},
            {"++", Punct::PlusPlus}, {"--", Punct::MinusMinus}, {"<<", Punct::Shl}, {">>", Punct::Shr},
            {"<=", Punct::LessEq}, {">=", Punct::GreaterEq}, {"==", Punct::EqEq}, {"!=", Punct::NotEq},
            {"&&", Punct::AndAnd}, {"||", Punct::OrOr}, {"+=", Punct::PlusAssign}, {"-=", Punct::MinusAssign},
            {"*=", Punct::StarAssign}, {"/=", Punct::SlashAssign}, {"%=", Punct::PercentAssign},
            {"&=", Punct::AmpAssign}, {"|=", Punct::PipeAssign}, {"^=", Punct::CaretAssign},
            {"(", Punct::LParen}, {")", Punct::RParen}, {"{", Punct::LBrace}, {"}", Punct::RBrace},
            {"[", Punct::LBracket}, {"]", Punct::RBracket}, {",", Punct::Comma}, {";", Punct::Semicolon},
            {"#", Punct::Hash}, {"?", Punct::Question}, {":", Punct::Colon}, {"+", Punct::Plus},
            {"-", Punct::Minus}, {"*", Punct::Star}, {"/", Punct::Slash}, {"%", Punct::Percent},
            {"&", Punct::Amp}, {"|", Punct::Pipe}, {"^", Punct::Caret}, {"~", Punct::Tilde},
            {"!", Punct::Bang}, {"<", Punct::Less}, {">", Punct::Greater}, {"=", Punct::Assign},
            {"", Punct::None},
        }};
        const std::string_view rest = src_.substr(pos_);
        for (const auto& [spelling, p] : table) {
            if (spelling.empty()) break;
            if (rest.starts_with(spelling)) {
                t.kind = TokenKind::Punct;
                t.punct = p;
                t.text = std::string{spelling};
                for (std::size_t i = 0; i < spelling.size(); ++i) advance();
                return;
            }
        }
        const auto byte = static_cast<unsigned char>(src_[pos_]);
        fail(t.loc, byte >= 0x20 && byte < 0x7F ? std::string{"unexpected character '"} + char(byte) + "'"
                                                 : std::format("unexpected byte 0x{:02X} outside of a literal or comment", byte));
    }

    std::string_view src_;
    const std::string* file_;
    std::size_t pos_ = 0;
    std::uint32_t line_ = 1, col_ = 1;
    bool atLineStart_ = true;
};

} // namespace gromada::script
