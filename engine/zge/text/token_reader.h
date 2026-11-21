#pragma once

// The token reader (ztoken.c, initTokenReader-searchTreeVisitPostOrder; zge/text/token_reader.cpp): the
// text files' tokenizer the scene database (Scene.Txt), the park editor's
// projects and piece files and the parsers after them read through. A script
// is read a line at a time into the current file's line buffer; a token is a
// word up to a space, tab, comma, carriage return or line feed, or a "quoted"
// string. ";" and "//" start a comment to the line's end, "/*" .. "*/" one
// over tokens (the markers are tokens of their own), and with hash comments
// on, a "#" token skips its line.
//
// Directives (with the directives on, the default): "#INCLUDE name" opens
// INCLUDE_PATH + name as a nested file (three deep at most), "#INCLUDE_PATH
// path" sets that prefix, "#DEFINE name value" defines a symbol; "#ifndef
// name" is passed over with its name and "#endif" alone (no conditionals in
// this build). A token starting "$" is a symbol: replaced by its value (an
// unknown one is skipped), "$+ a b", "$- a b", "$* a b" and "$/ a b" are the
// numbers' sum, difference, product and quotient as a decimal token, "$$~"
// forgets every symbol, "$$name value" defines one, "$$$name" includes a file.
// With capitalize on (the default) tokens come back upper-cased.
//
// The other builds (GameCube, Xbox) print what this one compiles out: "***
// Opened File %s ***" and "*** Closed File ***" (with trace on), "Replacing
// symbol:%s", "Out of symbol space", "Symbol not found: '%s'", "**ERROR**
// Maximum include nesting reached!", "Can't open include file: %s", "ZTOKEN
// assert failed : found '%s', expecting '%s'" and the include chain's "File:%s,
// Line:%d" (reportTokenPosition).

#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/file.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/text/token_reader.h.h"

namespace zge
{
    constexpr uint32_t kTokenFileNameBytes = 0x100;
    constexpr uint32_t kTokenLineBytes = game::readable::perGame(0x400u, 0x400u, 0x800u); // (BMX XXX's lines are twice as long)
    constexpr uint32_t kTokenLineLimit = kTokenLineBytes - 1; // readFileLine's size: the line and its 0
    constexpr uint32_t kTokenIncludePathBytes = 0x100;
    constexpr int32_t kMaxIncludeDepth = 3;
    constexpr uint32_t kTokenSymbolBytes = 0x800;
    constexpr uint32_t kTokenBytes = 0x80; // the token buffers the reader's own reads use

    // One open script or include: its file and the line being read.
    struct TokenFile
    {
        ee::Ptr<ZFile> file;                        // +0x000
        char name[kTokenFileNameBytes];             // +0x004 the path it was opened by ("Unknown": a file handed over)
        char line[kTokenLineBytes];                 // +0x104 the line being read
        uint16_t cursor;                            // +0x504 (BMX XXX: +0x904) the next character's index in line
        uint8_t _506[2];
        int32_t lineNumber;                         // +0x508 the lines read (-1 once closed)
        char includePath[kTokenIncludePathBytes];   // +0x50C #INCLUDE_PATH: the prefix of the names #INCLUDE opens
    };
    static_assert(offsetof(TokenFile, line) == 0x104 && offsetof(TokenFile, cursor) == 0x104 + kTokenLineBytes &&
                  offsetof(TokenFile, lineNumber) == 0x108 + kTokenLineBytes &&
                  offsetof(TokenFile, includePath) == 0x10C + kTokenLineBytes &&
                  sizeof(TokenFile) == game::readable::perGame(0x60Cu, 0x60Cu, 0xA0Cu));

    // The reader's whole state (tokenReaderAddress; saveTokenReader copies it).
    struct TokenReader
    {
        TokenFile files[kMaxIncludeDepth + 1];      // +0x0000 the script, then its includes
        ee::Ptr<TokenFile> current;                 // +0x1830 the one read from: files[includeDepth]
        int32_t includeDepth;                       // +0x1834
        int32_t trace;                              // +0x1838 (the other builds: print the opens and closes)
        int32_t capitalize;                         // +0x183C tokens upper-cased (1 after a reset)
        int32_t hashComments;                       // +0x1840 a "#" token skips its line (0 after a reset)
        int32_t directives;                         // +0x1844 #INCLUDE, #INCLUDE_PATH and #DEFINE obeyed (1 after a reset)
        // The symbols, "$NAME@VALUE" one after another (no terminator).
        uint8_t symbols[kTokenSymbolBytes];         // +0x1848
        uint32_t symbolBytes;                       // +0x2048 the bytes of symbols in use
    };
    // (BMX XXX: each 0x1000 later, its four files' lines being 0x400 bytes longer)
    static_assert(offsetof(TokenReader, current) == game::readable::perGame(0x1830u, 0x1830u, 0x2830u) &&
                  offsetof(TokenReader, directives) == offsetof(TokenReader, current) + 0x14 &&
                  offsetof(TokenReader, symbols) == offsetof(TokenReader, current) + 0x18 &&
                  offsetof(TokenReader, symbolBytes) == offsetof(TokenReader, current) + 0x818 &&
                  sizeof(TokenReader) == offsetof(TokenReader, current) + 0x81C);

    constexpr uint32_t kTokenReaderAddress = sym::tokenReaderAddress;

    // The scripts' calls (the readable ones; see token_reader.cpp).
    int32_t beginScript(ee::Address path);
    void endScript();
    uint32_t nextToken(ee::Address token, uint32_t bytes); // 0 at the end or on an error
    int32_t readNumberToken(ee::Address value);          // (int32_t *)
    int32_t readFloatToken(ee::Address value);           // (float *)
    int32_t readStringToken(ee::Address token, uint32_t bytes);
    int32_t expectToken(ee::Address text);
    ee::Ptr<char> restOfTokenLine(); // what is left of the current line
    void skipTokenLine();            // ZGE4 only: the rest of the current line passed over
    void reportTokenPosition();

    // The reader's start and end (1), and whether tokens are read upper-cased.
    uint32_t initTokenReader();
    uint32_t shutDownTokenReader();
    void setTokenCapitalize(uint32_t on);
    // Whether a '#' starts a comment.
    void setTokenHashComments(uint32_t on);
}
