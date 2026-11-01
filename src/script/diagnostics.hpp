#pragma once
// Source locations and errors of the script module.
#include <cstdint>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>

namespace gromada::script {

// Position in a source file. `file` is the display name of the file (include path as written,
// or the path given to the loader); it is shared by all tokens of a file.
struct SourceLocation {
    const std::string* file = nullptr;
    std::uint32_t line = 0;   // 1-based, 0 = unknown
    std::uint32_t column = 0; // 1-based

    std::string str() const {
        if (!file) return "<unknown>";
        return std::format("{}:{}:{}", *file, line, column);
    }
};

// Every error raised by the lexer, preprocessor, parser, linker or interpreter.
class ScriptError : public std::runtime_error {
public:
    enum class Phase { Load, Lex, Preprocess, Parse, Link, Runtime };

    ScriptError(Phase phase, const SourceLocation& loc, std::string message)
        : std::runtime_error(format(phase, loc, message)), phase_{phase}, location_{loc},
          message_{std::move(message)} {}

    Phase phase() const noexcept { return phase_; }
    const SourceLocation& location() const noexcept { return location_; }
    const std::string& message() const noexcept { return message_; }

    static constexpr std::string_view phaseName(Phase p) {
        switch (p) {
            case Phase::Load: return "load";
            case Phase::Lex: return "lex";
            case Phase::Preprocess: return "preprocess";
            case Phase::Parse: return "parse";
            case Phase::Link: return "link";
            case Phase::Runtime: return "runtime";
        }
        return "?";
    }

private:
    static std::string format(Phase phase, const SourceLocation& loc, const std::string& message) {
        return std::format("{}: {} error: {}", loc.str(), phaseName(phase), message);
    }

    Phase phase_;
    SourceLocation location_;
    std::string message_;
};

} // namespace gromada::script
