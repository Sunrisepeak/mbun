// error.cppm — common parser diagnostics.
//
// Ref: bun-ref/src/parsers/json5.rs ParseError/Error and the TOML/JSON
// lexer diagnostics. Concrete parsers can add richer categories later while
// retaining the stable source position and message shape here.
export module mbun.parsers.error;

import std;
import mbun.parsers.input;

namespace mbun::parsers {

export enum class ErrorKind {
    UnexpectedEof,
    UnexpectedToken,
    InvalidSyntax,
    InvalidNumber,
    InvalidString,
    DuplicateKey,
    DepthLimit,
    Unsupported,
};

export struct ParseError {
    ErrorKind kind { ErrorKind::InvalidSyntax };
    Position position {};
    std::string message {};

    static ParseError at(ErrorKind kind, const Input& input, std::size_t offset,
                         std::string message) {
        return ParseError { kind, input.position(offset), std::move(message) };
    }
};

}  // namespace mbun::parsers
