// The token reader (ztoken.c; zge/text/token_reader.h lays its state out and
// says what a script may hold): the reader's set-up, its files and includes,
// the symbols, the tokenizer and the typed reads on it, the settings.
//
// Original quirks kept: a word is read up to a space, tab, comma or line end
// but not up to a 0, so a last word with no line feed after it runs on past
// the line (until the token is full: 0); "$$$name" gives back 0 with an
// empty token when the include opened, and goes on reading when it did not;
// a symbol whose old value was empty keeps its successor's "$" under the new
// value (the tail is moved from the value's second byte on); with hash
// comments on, a "#" line at the end of an include gives back 0 without
// going back to the including file (a ";" or "//" line goes back).

#include "token_reader.h"

#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/assets/file.h"
#include "sce/libc.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/text/token_reader.cpp.h"

namespace zge
{
    int32_t openTokenFile(ee::Address path);
    uint32_t closeTokenFile();
    uint32_t pushIncludeFile(ee::Address name);
    void popIncludeFile();
    void defineTokenSymbol(ee::Address name, ee::Address value);
    uint32_t lookUpTokenSymbol(ee::Address token);
    void resetTokenReader();
    uint32_t tokenDirectivesOn();

    namespace
    {
        inline ee::Global<TokenReader, kTokenReaderAddress> g_tokenReader;
        // 1 from initTokenReader to shutDownTokenReader.
        inline ee::Global<uint32_t, sym::g_tokenReaderReady> g_tokenReaderReady;
        // Bit 0: the last token was a quoted string (nothing reads it in this build).
        inline ee::Global<uint32_t, sym::g_tokenFlags> g_tokenFlags;
        constexpr uint32_t kTokenQuoted = 1u << 0;

        constexpr uint32_t kHeapFree = sym::heapFree;     // (block)

        constexpr uint32_t kIncludeNameFormat = sym::includeNameFormat; // "%s%s": the include path, the name
        constexpr uint32_t kUnknownFileName = sym::unknownFileName;   // "Unknown"
        constexpr uint32_t kLineComment = sym::lineComment;       // "////" (its first two compared)
        constexpr uint32_t kCommentOpen = sym::commentOpen;       // "/*"
        constexpr uint32_t kCommentClose = sym::commentClose;      // "*/"
        constexpr uint32_t kDirectiveInclude = sym::directiveInclude;  // "#INCLUDE"
        constexpr uint32_t kDirectiveIncludePath = sym::directiveIncludePath; // "#INCLUDE_PATH"
        constexpr uint32_t kDirectiveIfndef = sym::directiveIfndef;   // "#ifndef"
        constexpr uint32_t kDirectiveEndif = sym::directiveEndif;    // "#endif"
        constexpr uint32_t kDirectiveDefine = sym::directiveDefine;   // "#DEFINE"
        constexpr uint32_t kDecimalFormat = sym::decimalFormat;     // "%d"
        constexpr uint32_t kHexPrefixUpper = sym::hexPrefixUpper;    // "0X"
        constexpr uint32_t kHexPrefix = sym::hexPrefix;         // "0x"
        constexpr uint32_t kHexFormat = sym::hexFormat;         // "%X"
        constexpr uint32_t kHexMark = sym::hexMark;           // "$"
        constexpr uint32_t kFloatFormat = sym::floatFormat;       // "%f"

        constexpr uint8_t kSymbolMark = '$';
        constexpr uint8_t kValueMark = '@';

        TokenFile &currentFile() { return *g_tokenReader->current; }
        uint8_t *symbolTable() { return ee::at<uint8_t>(kTokenReaderAddress + offsetof(TokenReader, symbols)); }
        uint8_t currentChar()
        {
            const TokenFile &file = currentFile();
            return static_cast<uint8_t>(file.line[file.cursor]);
        }
        ee::Ptr<char> currentLine() { return currentFile().line; }

        bool isLowerCase(uint8_t c) { return static_cast<uint32_t>(c - 'a') < 26u; }

        // The token upper-cased in place.
        void upperCase(ee::Ptr<uint8_t> text)
        {
            uint32_t i = 0;
            do
            {
                if (isLowerCase(text[i]))
                    text[i] = static_cast<uint8_t>(text[i] - ('a' - 'A'));
                ++i;
            } while (text[i] != 0);
        }
    }

    // ---- set-up ------------------------------------------------------------

    // initTokenReader (the engine's start-up). 1.
    uint32_t initTokenReader()
    {
        *g_tokenReaderReady = 1;
        return 1;
    }
    READABLE_FUNCTION(initTokenReader, Verified);

    // shutDownTokenReader (the engine's shutdown). 1.
    uint32_t shutDownTokenReader()
    {
        if (*g_tokenReaderReady == 0)
            return 1;
        *g_tokenReaderReady = 0;
        return 1;
    }
    READABLE_FUNCTION(shutDownTokenReader, Verified);

    // ---- files ---------------------------------------------------------------

    // openTokenFile: a script opened as the current file, its line and line
    // count cleared and its path kept as its name. 0 if it would not open.
    int32_t openTokenFile(ee::Address path)
    {
        const ee::Ptr<ZFile> file = openFile(path, kFileModeRead, 0).file();
        currentFile().file = file;
        if (!file)
            return 0;
        currentFile().cursor = 0;
        currentFile().line[0] = 0;
        currentFile().lineNumber = 0;
        stringCopy(ee::Ptr<char>(currentFile().name), path);
        return 1;
    }
    READABLE_FUNCTION_WAITS(openTokenFile, Verified);

    // closeTokenFile: the current file closed (its line number -1). 1.
    uint32_t closeTokenFile()
    {
        currentFile().lineNumber = -1;
        closeFile(currentFile().file, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(closeTokenFile, Verified);

    // pushIncludeFile: an include opened one level down: the current file's
    // record copied to the next (its include path with it), the include path
    // and the name joined and opened there. 0 at the deepest level or if it
    // would not open (the level given back).
    uint32_t pushIncludeFile(ee::Address name)
    {
        ee::Local<uint8_t[0x130]> path;
        TokenReader &reader = *g_tokenReader;
        if (reader.includeDepth == kMaxIncludeDepth)
            return 0;
        const ee::Ptr<TokenFile> outer = reader.current;
        reader.includeDepth = reader.includeDepth + 1;
        reader.current = outer + 1;
        memoryCopy(reader.current, outer, static_cast<uint32_t>(sizeof(TokenFile)));
        formatString(path, kIncludeNameFormat, ee::Ptr<char>(reader.current->includePath), name);
        if (openTokenFile(path) == 0)
        {
            reader.includeDepth = reader.includeDepth - 1;
            reader.current = reader.current - 1;
            return 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(pushIncludeFile, Verified);

    // popIncludeFile: the current include closed and its including file made
    // current again (nothing at the script's own level).
    void popIncludeFile()
    {
        TokenReader &reader = *g_tokenReader;
        if (reader.includeDepth != 0)
        {
            closeTokenFile();
            reader.includeDepth = reader.includeDepth - 1;
            reader.current = reader.current - 1;
        }
    }
    READABLE_FUNCTION_WAITS(popIncludeFile, Verified);

    // ---- symbols ---------------------------------------------------------------

    // defineTokenSymbol: a symbol defined ("$NAME", its value): an existing one's
    // value replaced in place (the symbols after it moved up or down by the
    // difference), else "$NAME@VALUE" appended; a new one is not kept if it
    // would not fit or has a "$" or "@" in its name or value ("Out of symbol
    // space").
    void defineTokenSymbol(ee::Address name, ee::Address value)
    {
        ee::Local<uint8_t> key;
        TokenReader &reader = *g_tokenReader;
        uint8_t *const symbols = symbolTable(); // (a longer value's move reads one byte past the end)
        const ee::Ptr<uint8_t> nameText(name);
        const ee::Ptr<uint8_t> valueChars(value);
        uint32_t keyLength = 0;
        if (nameText[0] != 0)
        {
            for (;;)
            {
                (&*key)[keyLength] = nameText[keyLength];
                ++keyLength;
                if (nameText[keyLength] == 0)
                    break;
            }
        }
        (&*key)[keyLength] = 0;
        uint32_t oldLength = 0;
        uint32_t symbolIndex = lookUpTokenSymbol(key);
        if (symbolIndex == 0)
        {
            // A new symbol: "$", the name after its own "$", "@", the value.
            uint32_t end = reader.symbolBytes;
            if (end >= kTokenSymbolBytes)
                return;
            symbols[end] = kSymbolMark;
            ++end;
            // Each byte of `text` from `from` on stored at end (a "$" or "@"
            // makes end too big); whether there is room after it. The
            // loop's blocks: its head, the bad byte's, the step's, the end's
            // (none: the text was empty) and the room check's.
            const auto append = [&](ee::Ptr<uint8_t> text, uint32_t from) {
                if (text[from] == 0)
                    return end < kTokenSymbolBytes;
                if (end >= kTokenSymbolBytes)
                    return false;
                uint32_t i = from;
                for (;;)
                {
                    symbols[end] = text[i];
                    if (text[i] == kSymbolMark || text[i] == kValueMark)
                        end = kTokenSymbolBytes + 1;
                    ++i;
                    ++end;
                    if (text[i] == 0)
                        return end < kTokenSymbolBytes;
                    if (end >= kTokenSymbolBytes)
                        return false;
                }
            };
            if (append(nameText, 1))
            {
                symbols[end] = kValueMark;
                ++end;
                if (append(valueChars, 0))
                {
                    reader.symbolBytes = end;
                    return;
                }
            }
            return;
        }
        // An existing one (at: its name, after the "$"): its value starts
        // after the "@" and runs to the next "$" or the end.
        const uint8_t valueFirst = valueChars[0];
        if (symbols[symbolIndex] != kValueMark)
        {
            for (;;)
            {
                ++symbolIndex;
                if (symbols[symbolIndex] == kValueMark)
                    break;
            }
        }
        const uint32_t start = symbolIndex + 1;
        const uint32_t end = reader.symbolBytes;
        uint32_t i = start;
        if (start < end && symbols[start] != kSymbolMark)
        {
            for (;;)
            {
                ++i;
                oldLength = i - start;
                if (i >= end || symbols[i] == kSymbolMark)
                    break;
            }
        }
        uint32_t newLength = 0;
        if (valueFirst != 0)
        {
            for (;;)
            {
                ++newLength;
                if (valueChars[newLength] == 0)
                    break;
            }
        }
        if (oldLength < newLength)
        {
            // Longer: the bytes after the value's first moved up, from the end down.
            const uint32_t grow = newLength - oldLength;
            uint32_t from = reader.symbolBytes;
            if (start < from)
            {
                for (;;)
                {
                    symbols[from + grow] = symbols[from];
                    --from;
                    if (start >= from)
                        break;
                }
            }
            reader.symbolBytes = reader.symbolBytes + grow;
        }
        else
        {
            if (newLength < oldLength)
            {
                // Shorter: the bytes after the old value moved down to the new one's end.
                const uint32_t shrink = oldLength - newLength;
                uint32_t to = start;
                if (start < reader.symbolBytes)
                {
                    for (;;)
                    {
                        symbols[to] = symbols[to + shrink];
                        ++to;
                        if (to >= reader.symbolBytes)
                            break;
                    }
                }
                reader.symbolBytes = reader.symbolBytes - shrink;
            }
        }
        // The new value over the old.
        if (valueChars[0] != 0)
        {
            uint32_t k = 0;
            for (;;)
            {
                symbols[start + k] = valueChars[k];
                ++k;
                if (valueChars[k] == 0)
                    break;
            }
        }
    }
    READABLE_FUNCTION(defineTokenSymbol, Verified);

    // lookUpTokenSymbol: a symbol's token ("$NAME") replaced by its value; the
    // symbol's place (its name's, after the "$"), or 0 for none.
    uint32_t lookUpTokenSymbol(ee::Address token)
    {
        TokenReader &reader = *g_tokenReader;
        const ee::Ptr<uint8_t> text(token);
        uint32_t symbolIndex = 0;
        if (reader.symbolBytes != 0)
        {
            for (;;)
            {
                if (reader.symbols[symbolIndex] == kSymbolMark)
                {
                    // The token against the symbol's name, up to the first
                    // byte that differs (or the symbols' end).
                    uint32_t matched = 0;
                    uint8_t next = 0;
                    if (symbolIndex >= reader.symbolBytes)
                        next = text[0];
                    else if (text[0] != kSymbolMark)
                        next = text[0];
                    else
                    {
                        const uint32_t end = reader.symbolBytes;
                        matched = 1;
                        for (;;)
                        {
                            if (symbolIndex + matched >= end)
                            {
                                next = text[matched];
                                break;
                            }
                            if (reader.symbols[symbolIndex + matched] != text[matched])
                            {
                                next = text[matched];
                                break;
                            }
                            ++matched;
                        }
                    }
                    if (next == 0 && reader.symbols[symbolIndex + matched] == kValueMark)
                    {
                        // Found: its value (to the next "$" or the end) copied over the token.
                        uint32_t from = symbolIndex + matched + 1;
                        uint32_t length = 0;
                        if (from < reader.symbolBytes && reader.symbols[from] != kSymbolMark)
                        {
                            for (;;)
                            {
                                text[length] = reader.symbols[from];
                                ++from;
                                ++length;
                                if (from >= reader.symbolBytes)
                                    break;
                                if (reader.symbols[from] == kSymbolMark)
                                    break;
                            }
                        }
                        text[length] = 0;
                        return symbolIndex + 1;
                    }
                }
                ++symbolIndex;
                if (symbolIndex >= reader.symbolBytes)
                    break;
            }
        }
        return 0;
    }
    READABLE_FUNCTION(lookUpTokenSymbol, Verified);

    // ---- scripts ---------------------------------------------------------------

    namespace
    {
        // The reset itself: resetTokenReader's body, and ZGE2's beginScript's
        // (ZGE2 has no resetTokenReader: its beginScript resets the reader inline).
        inline void resetTokenReaderState()
        {
            TokenReader &reader = *g_tokenReader;
            reader.directives = 1;
            reader.symbolBytes = 0;
            reader.includeDepth = 0;
            reader.current = ee::Ptr<TokenFile>(kTokenReaderAddress);
            reader.trace = 0;
            reader.capitalize = 1;
            reader.hashComments = 0;
            reader.files[0].includePath[0] = 0;
        }
    }

    // resetTokenReader: the reader set to its first file, no symbols, the
    // settings at their defaults (directives and capitalize on, trace and
    // hash comments off), no include path.
    void resetTokenReader()
    {
        resetTokenReaderState();
    }
    READABLE_FUNCTION(resetTokenReader, Verified);

    // beginScriptFromFile: a script read from a file already open ("Unknown" its
    // name). 0 for no file.
    uint32_t beginScriptFromFile(uint32_t file)
    {
        if (file == 0)
            return 0;
        resetTokenReader();
        currentFile().cursor = 0;
        currentFile().line[0] = 0;
        currentFile().lineNumber = 0;
        currentFile().file = ee::Ptr<ZFile>(file);
        stringCopy(ee::Ptr<char>(currentFile().name), kUnknownFileName);
        return 1;
    }
    READABLE_FUNCTION(beginScriptFromFile, Verified);

    // beginScript: the reader reset and the script at path opened; whether it opened.
    int32_t beginScript(ee::Address path)
    {
        if constexpr (game::readable::kIsDm2)
            resetTokenReaderState();
        else
            resetTokenReader();
        return openTokenFile(path) != 0 ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(beginScript, Verified);

    // endScript: the current file closed.
    void endScript()
    {
        closeTokenFile();
    }
    READABLE_FUNCTION_WAITS(endScript, Verified);

    // ---- the tokenizer -----------------------------------------------------------

    // nextToken: the next token into token (bytes long, its 0 included),
    // past comments, with the directives obeyed and the symbols expanded
    // (token_reader.h). 1, or 0 at the script's end, for a token that does
    // not fit, an unterminated string or an include that would not open.
    uint32_t nextToken(ee::Address token, uint32_t bytes)
    {
        ee::Local<char[0x80]> defineValue;
        ee::Local<int32_t> firstOperand;  // "$+"'s, and its like
        ee::Local<int32_t> secondOperand;
        TokenReader &reader = *g_tokenReader;
        const ee::Ptr<uint8_t> text(token);
        int32_t commentDepth = 0;   // "/*" .. "*/" nesting
        int32_t includePending = 0; // an #INCLUDE read: its name is the next token
        bool quoted = false;
        // The current file's next line read (its count first); whether there was one.
        const auto readLine = [&]() {
            return readFileLine(currentFile().file, ee::Ptr<char>(currentFile().line), kTokenLineLimit) != 0;
        };
        for (;;)
        {
            uint32_t length = 0;
            quoted = false;
            // Spaces, tabs, line ends and commas passed over.
            uint8_t c = currentChar();
            while (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',')
            {
                currentFile().cursor = static_cast<uint16_t>(currentFile().cursor + 1);
                c = currentChar();
            }
            c = currentChar();
            bool popInclude = false;
            if (c == 0)
            {
                // The line is used up: the next one, else back to the
                // including file, else the end.
                currentFile().lineNumber = currentFile().lineNumber + 1;
                currentFile().line[0] = 0;
                currentFile().cursor = 0;
                if (readLine())
                    continue;
                if (reader.includeDepth == 0)
                    return 0;
                popInclude = true;
            }
            else
            {
                if (c == '"')
                {
                    // A quoted string, up to its closing quote.
                    *g_tokenFlags = *g_tokenFlags | kTokenQuoted;
                    currentFile().cursor = static_cast<uint16_t>(currentFile().cursor + 1);
                    c = currentChar();
                    if (c != '"')
                    {
                        if (c != 0)
                        {
                            for (;;)
                            {
                                ++length;
                                const uint16_t cursor = currentFile().cursor;
                                currentFile().cursor = static_cast<uint16_t>(cursor + 1);
                                text[length - 1] = static_cast<uint8_t>(currentFile().line[cursor]);
                                if (static_cast<int32_t>(length) >= static_cast<int32_t>(bytes - 1))
                                    return 0;
                                c = currentChar();
                                if (c == '"' || c == 0)
                                    break;
                            }
                        }
                        if (c != '"')
                        {
                            // The line ended inside the string.
                            if (currentChar() != '"')
                                return 0;
                        }
                    }
                    quoted = true;
                    currentFile().cursor = static_cast<uint16_t>(currentFile().cursor + 1);
                }
                else
                {
                    // A word, up to a space, tab, comma or line end.
                    *g_tokenFlags = *g_tokenFlags & ~kTokenQuoted;
                    c = currentChar();
                    if (c != '\t' && c != ' ')
                    {
                        for (;;)
                        {
                            c = currentChar();
                            if (c == '\r' || c == '\n' || c == ',')
                                break;
                            currentFile().cursor = static_cast<uint16_t>(currentFile().cursor + 1);
                            text[length] = c;
                            ++length;
                            if (length == bytes - 1)
                                return 0;
                            c = currentChar();
                            if (c == ' ' || c == '\t')
                                break;
                        }
                    }
                }
                text[length] = 0;
                // A ";" or "//" comment: the rest of the line passed over.
                bool lineComment = false;
                if (!quoted)
                {
                    if (text[0] == ';')
                        lineComment = true;
                    else
                        lineComment = compareStringsN(token, kLineComment, 2) == 0;
                }
                if (lineComment)
                {
                    currentFile().lineNumber = currentFile().lineNumber + 1;
                    currentFile().line[0] = 0;
                    if (readLine())
                    {
                        currentFile().cursor = 0;
                        continue;
                    }
                    if (reader.includeDepth == 0)
                        return 0;
                    popInclude = true;
                }
            }
            if (popInclude)
            {
                popIncludeFile();
                continue;
            }
            // Hash comments: a "#" token's line passed over.
            int32_t depthBefore = commentDepth;
            bool commentChecked = false;
            if (reader.hashComments != 0)
            {
                if (quoted)
                    commentChecked = true;
                else if (text[0] == '#')
                {
                    currentFile().lineNumber = currentFile().lineNumber + 1;
                    currentFile().line[0] = 0;
                    if (!readLine())
                        return 0;
                    currentFile().cursor = 0;
                    continue;
                }
            }
            if (!commentChecked)
            {
                // "/*" .. "*/": the comment's tokens and its markers passed over.
                if (!quoted)
                {
                    if (compareStrings(token, kCommentOpen) == 0)
                        commentDepth = depthBefore + 1;
                    else
                    {
                        commentDepth = depthBefore - 1;
                        if (compareStrings(token, kCommentClose) != 0)
                            commentDepth = depthBefore;
                    }
                }
            }
            if constexpr (game::readable::kIsDm2)
            {
                // ZGE2: a "*/" with no comment open ends the tokens (0).
                if (commentDepth > 0 || (commentDepth == 0 && depthBefore == 1))
                    continue;
                if (commentDepth < 0)
                    return 0;
            }
            else if (commentDepth != 0 || depthBefore == 1)
                continue;
            if (reader.capitalize != 0 && text[0] != 0)
                upperCase(text);
            // The directives.
            if (stringCompareNoCase(token, kDirectiveInclude) == 0)
            {
                if (tokenDirectivesOn() != 0)
                {
                    includePending = 1;
                    continue;
                }
            }
            if (includePending == 1)
            {
                // The token is the include's name.
                const uint32_t opened = pushIncludeFile(token);
                includePending = 0;
                if (opened != 0)
                    continue;
                text[0] = 0;
                reportTokenPosition();
                return 0;
            }
            if (stringCompareNoCase(token, kDirectiveIncludePath) == 0)
            {
                const uint32_t on = tokenDirectivesOn();
                if (on == 0)
                    continue;
                if (nextToken(ee::Ptr<char>(currentFile().includePath), kTokenIncludePathBytes) == 0)
                    currentFile().includePath[0] = 0;
                continue;
            }
            if (stringCompareNoCase(token, kDirectiveIfndef) == 0)
            {
                // Its name passed over.
                if (nextToken(token + 1, kTokenBytes) != 0)
                    continue;
                return 0;
            }
            if (stringCompareNoCase(token, kDirectiveEndif) == 0)
                continue;
            uint32_t defineName = 0;
            bool define = false;
            if (stringCompareNoCase(token, kDirectiveDefine) == 0)
            {
                // "#DEFINE name value": the name read after a "$", the value.
                if (nextToken(token + 1, kTokenBytes) == 0)
                    return 0;
                text[0] = kSymbolMark;
                if (nextToken(defineValue, kTokenBytes) == 0)
                    return 0;
                if (tokenDirectivesOn() == 0)
                    continue;
                defineName = token;
                define = true;
            }
            else
            {
                // A "$" token: by its second byte (the table).
                if (text[0] != kSymbolMark)
                    return 1;
                const uint32_t operation = static_cast<uint32_t>(text[1]) - kSymbolMark;
                const uint8_t symbolKind = operation < 12 ? text[1] : 0;
                if (symbolKind == '$')
                {
                    // "$$~": no symbols; "$$$name": an include; "$$name value": a symbol.
                    const uint8_t kind = text[2];
                    if (kind == '~')
                    {
                        reader.symbolBytes = 0;
                        continue;
                    }
                    if (kind == '$')
                    {
                        if (pushIncludeFile(token + 3) == 0)
                            continue;
                        text[0] = 0;
                        return 0;
                    }
                    nextToken(defineValue, kTokenBytes);
                    defineName = token + 1;
                    define = true;
                }
                else if (symbolKind != '+' && symbolKind != '-' && symbolKind != '*' && symbolKind != '/')
                {
                    // A symbol: its value, or (none) the token after it.
                    if (lookUpTokenSymbol(token) == 0)
                        nextToken(token, bytes);
                    return 1;
                }
                else
                {
                    // "$+ a b", "$- a b", "$* a b", "$/ a b": the result as a decimal token.
                    readNumberToken(firstOperand);
                    readNumberToken(secondOperand);
                    const uint32_t first = static_cast<uint32_t>(*firstOperand), second = static_cast<uint32_t>(*secondOperand);
                    int32_t result = 0;
                    if (symbolKind == '+')
                        result = static_cast<int32_t>(first + second);
                    else if (symbolKind == '-')
                        result = static_cast<int32_t>(first - second);
                    else if (symbolKind == '*')
                        result = static_cast<int32_t>(first * second);
                    else
                    {
                        // (the original traps on a zero divisor)
                        const int32_t divisor = *secondOperand;
                        result = divisor == 0 ? 0
                               : divisor == -1 ? static_cast<int32_t>(0u - first)
                                               : *firstOperand / divisor;
                    }
                    *firstOperand = result;
                    formatString(token, kDecimalFormat, static_cast<uint64_t>(static_cast<int64_t>(result)));
                    return 1;
                }
            }
            if (define)
            {
                defineTokenSymbol(defineName, defineValue);
                continue;
            }
        }
    }
    READABLE_FUNCTION_WAITS(nextToken, Verified);

    // ---- typed reads -------------------------------------------------------------

    // readNumberToken: the next token as an integer: "0x" / "0X" and "$" hex,
    // else decimal; whether it was one.
    int32_t readNumberToken(ee::Address value)
    {
        ee::Local<uint8_t[0xA0]> number;
        if (nextToken(number, kTokenBytes) == 0)
            return 0;
        uint32_t digits = number.address();
        uint32_t format = kHexFormat;
        bool hex = compareStringsN(number, kHexPrefixUpper, 2) == 0;
        if (!hex)
            hex = compareStringsN(number, kHexPrefix, 2) == 0;
        if (hex)
        {
            digits = number.address() | 2; // past the "0x" (the frame is 16-byte aligned)
        }
        else
        {
            if (compareStringsN(number, kHexMark, 1) == 0)
                digits = number.address() | 1; // past the "$"
            else
                format = kDecimalFormat;
        }
        const int32_t scanned = sce::libc::sscanf(ee::Ptr<uint8_t>(digits), format, value);
        return scanned == 1 ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(readNumberToken, Verified);

    // readFloatToken: the next token as a float ("%f"); whether it was one.
    int32_t readFloatToken(ee::Address value)
    {
        ee::Local<uint8_t[0xA0]> token2;
        if (nextToken(token2, kTokenBytes) == 0)
            return 0;
        const int32_t scanned = sce::libc::sscanf(token2, kFloatFormat, value);
        return scanned == 1 ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(readFloatToken, Verified);

    // readStringToken: the next token as a string; whether there was one.
    int32_t readStringToken(ee::Address token, uint32_t bytes)
    {
        return nextToken(token, bytes) != 0 ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(readStringToken, Verified);

    // expectToken: the next token compared with text (upper-cased in place
    // first); whether it is that ("ZTOKEN assert failed" in the other builds).
    int32_t expectToken(ee::Address text)
    {
        ee::Local<uint8_t[0xA0]> token2;
        const ee::Ptr<uint8_t> expected(text);
        if (expected[0] != 0)
            upperCase(expected);
        if (nextToken(token2, kTokenBytes) == 0)
            return 0;
        const int32_t differs = compareStrings(token2, text);
        return differs == 0 ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(expectToken, Verified);

    // ---- positions -----------------------------------------------------------------

    // tokenLineNumber: the current file's line number.
    int32_t tokenLineNumber()
    {
        return currentFile().lineNumber;
    }
    READABLE_FUNCTION(tokenLineNumber, Verified);

    // reportTokenPosition: a script error's position, each include level's file and
    // line ("File:%s,  Line:%d", "Included from:" between): the loop over
    // the levels is all that is left in this build.
    void reportTokenPosition()
    {
        int32_t level = g_tokenReader->includeDepth;
        if (level >= 0)
        {
            for (;;)
            {
                --level;
                if (level < 0)
                    break;
            }
        }
    }
    READABLE_FUNCTION(reportTokenPosition, Verified);

    // restOfTokenLine: what is left of the current line.
    ee::Ptr<char> restOfTokenLine()
    {
        return currentLine() + currentFile().cursor;
    }
    READABLE_FUNCTION(restOfTokenLine, Verified);

    // skipTokenLine: ZGE4 only: the rest of the current line passed over
    // (the cursor to the line's end), unless the cursor is at its start.
    inline void skipTokenLine()
    {
        if (currentFile().cursor != 0)
            currentFile().cursor = static_cast<uint16_t>(stringLength(currentLine()));
    }
    READABLE_FUNCTION_ONLY(bmx, skipTokenLine);

    // rewindTokenLine: the current line given back: the file put back to the
    // line's start (its position less the line's length) and the line cut at
    // the cursor. What it returns is the file record's address plus the
    // cursor: short of the cut by the line's place in the record (kept).
    ee::Ptr<char> rewindTokenLine()
    {
        ee::Local<int32_t> position;
        if (currentFile().cursor != 0)
        {
            filePosition(currentFile().file, position, 0);
            const int32_t lineBytes = stringLength(currentLine());
            const ee::Ptr<ZFile> file = currentFile().file;
            *position = *position - lineBytes;
            fileSeek(file, static_cast<uint32_t>(*position), kSeekFromStart, 0);
        }
        const ee::Ptr<char> cut = currentLine() - offsetof(TokenFile, line) + currentFile().cursor;
        currentFile().line[currentFile().cursor] = 0;
        return cut;
    }

    // cutTokenLineAtCursor: ZGE2's rewindTokenLine only cuts the line at the
    // cursor (ZGE3 added putting the file back to the line's start) and
    // answers nothing.
    void cutTokenLineAtCursor()
    {
        currentFile().line[currentFile().cursor] = 0;
    }
    READABLE_FUNCTION_WAITS_OWN(rewindTokenLine, Verified, dm2, cutTokenLineAtCursor);

    // ---- settings ----------------------------------------------------------------

    // setTokenTrace .. tokenDirectivesOn: the reader's settings (resetTokenReader's
    // defaults: trace and hash comments off, capitalize and directives on).
    void setTokenTrace(uint32_t on)
    {
        g_tokenReader->trace = static_cast<int32_t>(on);
    }
    READABLE_FUNCTION(setTokenTrace, Verified);

    void setTokenCapitalize(uint32_t on)
    {
        g_tokenReader->capitalize = static_cast<int32_t>(on);
    }
    READABLE_FUNCTION(setTokenCapitalize, Verified);

    void setTokenHashComments(uint32_t on)
    {
        g_tokenReader->hashComments = static_cast<int32_t>(on);
    }
    READABLE_FUNCTION(setTokenHashComments, Verified);

    void setTokenDirectives(uint32_t on)
    {
        g_tokenReader->directives = static_cast<int32_t>(on);
    }
    READABLE_FUNCTION(setTokenDirectives, Verified);

    uint32_t tokenDirectivesOn()
    {
        return static_cast<uint32_t>(g_tokenReader->directives);
    }
    READABLE_FUNCTION(tokenDirectivesOn, Verified);

    // ---- the state saved and restored --------------------------------------------

    // saveTokenReader: the reader's whole state copied into a heap block (for
    // restoreTokenReader); the block.
    uint32_t saveTokenReader()
    {
        const uint32_t saved = heapAllocate(static_cast<uint32_t>(sizeof(TokenReader)));
        memoryCopy(saved, kTokenReaderAddress, sizeof(TokenReader));
        return saved;
    }
    READABLE_FUNCTION_WAITS(saveTokenReader, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        uint32_t heapFree(uint32_t block) { return ee::tailCall<uint32_t>(kHeapFree, block); }
        }
    }

    // restoreTokenReader: the state saveTokenReader kept put back and its block
    // freed (the heap's free is the original's tail call, with the block).
    uint32_t restoreTokenReader(uint32_t saved)
    {
        memoryCopy(kTokenReaderAddress, saved, sizeof(TokenReader));
        return tail::heapFree(saved); // the original's tail call
    }
    READABLE_FUNCTION_WAITS(restoreTokenReader, Verified);

    namespace
    {
        inline ee::Global<int32_t, sym::g_tokenPoolReady> g_tokenPoolReady;
        inline ee::Global<int32_t, sym::g_searchTreePool> g_searchTreePool;
    }

    // InitEngine's: g_tokenPoolReady = 1 and a pool of 16-byte records (g_searchTreePool)
    uint32_t initTokenPool()
    {
        *g_tokenPoolReady = 1;
        // (ZGE4 grows the pool 4096 records at a time, ZGE2 and ZGE3 by 256)
        const int32_t poolResult = createPool(0x10, game::readable::perGame(0x100u, 0x100u, 0x1000u), 0);
        *g_searchTreePool = poolResult;
        return 1;
    }
    READABLE_FUNCTION_WAITS(initTokenPool, Verified);
}
