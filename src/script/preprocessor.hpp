#pragma once
// Preprocessor: `#include "file"`, object-like `#define NAME tokens...` and `#undef NAME`.
//
// Directives must start a line. A #define body runs to the end of its line (comments removed)
// and is expanded lazily, at the point of use, like in C; a name is not expanded inside its own
// expansion. Redefinition is allowed (the game's header defines V_M_FIGHTER twice) and the last
// definition wins. Function-like macros and conditional compilation do not occur in the game
// scripts and are rejected.
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "script/lexer.hpp"
#include "script/source.hpp"

namespace gromada::script {

struct MacroDefinition {
    std::vector<Token> body;
    SourceLocation location;
};

class Preprocessor {
public:
    Preprocessor(SourceLoader& loader, SourceFiles& files, std::vector<std::string>* warnings = nullptr)
        : loader_{loader}, files_{files}, warnings_{warnings} {}

    void define(std::string name, std::vector<Token> body = {}) { macros_[std::move(name)] = {std::move(body), {}}; }

    // Preprocesses the main file and everything it includes. The result ends with an End token.
    std::vector<Token> run(std::string_view mainPath) {
        std::vector<Token> out;
        processFile(mainPath, {}, SourceLocation{}, out);
        Token end;
        end.kind = TokenKind::End;
        end.loc = out.empty() ? SourceLocation{} : out.back().loc;
        out.push_back(std::move(end));
        return out;
    }

    const std::map<std::string, MacroDefinition>& macros() const { return macros_; }
    // Display names of every file read, in order (the main file first).
    const std::vector<std::string>& files() const { return fileList_; }

    static constexpr int kMaxIncludeDepth = 16;

private:
    [[noreturn]] static void fail(const SourceLocation& loc, std::string msg) {
        throw ScriptError(ScriptError::Phase::Preprocess, loc, std::move(msg));
    }

    void processFile(std::string_view path, std::string_view includedFrom, const SourceLocation& includeLoc,
                     std::vector<Token>& out) {
        auto source = loader_.load(path, includedFrom);
        if (!source) {
            if (includedFrom.empty()) throw ScriptError(ScriptError::Phase::Load, includeLoc, "cannot open '" + std::string{path} + "'");
            fail(includeLoc, "cannot open include file '" + std::string{path} + "'");
        }
        for (const auto& active : includeStack_)
            if (active == source->key) fail(includeLoc, "recursive #include of '" + std::string{path} + "'");
        if (includeStack_.size() >= kMaxIncludeDepth) fail(includeLoc, "#include nested too deeply");

        const std::string* name = files_.intern(source->name);
        fileList_.push_back(source->name);
        includeStack_.push_back(source->key);
        const std::vector<Token> tokens = Lexer{source->text, name}.tokenize();

        for (std::size_t i = 0; tokens[i].kind != TokenKind::End;) {
            const Token& t = tokens[i];
            if (t.is(Punct::Hash) && t.lineStart) {
                // Gather the directive line.
                std::size_t j = i + 1;
                std::vector<const Token*> line;
                while (tokens[j].kind != TokenKind::End && tokens[j].loc.line == t.loc.line) line.push_back(&tokens[j++]);
                directive(t, line, source->key, out);
                i = j;
                continue;
            }
            emit(t, out);
            ++i;
        }
        includeStack_.pop_back();
    }

    void directive(const Token& hash, const std::vector<const Token*>& line, const std::string& fileKey,
                   std::vector<Token>& out) {
        if (line.empty() || line[0]->kind != TokenKind::Identifier) fail(hash.loc, "expected a directive name after '#'");
        const std::string& kw = line[0]->text;
        if (kw == "include") {
            if (line.size() < 2 || line[1]->kind != TokenKind::String) fail(line[0]->loc, "#include expects a \"file name\"");
            if (line.size() > 2) fail(line[2]->loc, "unexpected tokens after #include file name");
            // Like a C header-name, the file name is taken verbatim: `"maps\header.ini"` keeps its
            // single backslash (ordinary string literals in the game do process escapes: "maps\\x").
            processFile(line[1]->raw, fileKey, line[1]->loc, out);
        } else if (kw == "define") {
            if (line.size() < 2 || line[1]->kind != TokenKind::Identifier) fail(line[0]->loc, "#define expects a name");
            const Token& nameTok = *line[1];
            if (line.size() > 2 && line[2]->is(Punct::LParen) &&
                line[2]->loc.column == nameTok.loc.column + nameTok.text.size())
                fail(nameTok.loc, "function-like macros are not supported");
            MacroDefinition def;
            def.location = nameTok.loc;
            for (std::size_t k = 2; k < line.size(); ++k) def.body.push_back(*line[k]);
            if (const auto it = macros_.find(nameTok.text); it != macros_.end() && warnings_ && !sameBody(it->second.body, def.body))
                warnings_->push_back(nameTok.loc.str() + ": warning: '" + nameTok.text + "' redefined with a different value");
            macros_[nameTok.text] = std::move(def);
        } else if (kw == "undef") {
            if (line.size() != 2 || line[1]->kind != TokenKind::Identifier) fail(line[0]->loc, "#undef expects a name");
            macros_.erase(line[1]->text);
        } else {
            fail(line[0]->loc, "unsupported preprocessor directive '#" + kw + "'");
        }
    }

    static bool sameBody(const std::vector<Token>& a, const std::vector<Token>& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (a[i].kind != b[i].kind || a[i].text != b[i].text || a[i].number != b[i].number) return false;
        return true;
    }

    void emit(const Token& t, std::vector<Token>& out) {
        if (t.kind == TokenKind::Identifier) {
            const auto it = macros_.find(t.text);
            if (it != macros_.end() && !expanding_.contains(t.text)) {
                expanding_.insert(t.text);
                // Copy: a nested expansion cannot modify the table, but keep the body stable anyway.
                const std::vector<Token> body = it->second.body;
                for (Token b : body) {
                    b.loc = t.loc; // report errors at the use site
                    b.lineStart = false;
                    emit(b, out);
                }
                expanding_.erase(t.text);
                return;
            }
        }
        Token copy = t;
        copy.lineStart = false;
        out.push_back(std::move(copy));
    }

    SourceLoader& loader_;
    SourceFiles& files_;
    std::vector<std::string>* warnings_;
    std::map<std::string, MacroDefinition> macros_;
    std::set<std::string> expanding_;
    std::vector<std::string> includeStack_;
    std::vector<std::string> fileList_;
};

} // namespace gromada::script
