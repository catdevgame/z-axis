#pragma once

// Text of Z-Axis ZGE3 games (Aggressive Inline): the string tables and the
// strings of the level scripts, and an English -> other language table made
// from two releases' files, for game overrides that show the game in
// another language. Everything works on bytes; no guest memory.
//
// String table (assets\string.eus, .fre, ...; little endian):
//     u32 file size, u32 count, count x {u32 key, u32 offset}, then the
//     NUL-terminated strings (Latin-1). The key is keyHash of the string's
//     name ("KSTR_RESUMEGAME" for the UI scripts' "$KSTR_RESUMEGAME").
//
// Level script (levels\<L>\<L>.BIN; the Xbox release has one per language,
// .bni English, .bnf French, .bng German, .bns Spanish):
//     f32 version, u32, u32, u32, u32 string block size, then the block
//     (NUL-terminated strings) and the script body, which refers to strings
//     by their offset in the block. Two translations of one script have the
//     same body apart from those offsets (and a few lengths), so the
//     strings pair up by the body words that point at them.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ps2x::zaxis_text
{
    // The engine's name hash (0x148888 in Aggressive Inline): ELF hash of
    // the upper-cased name.
    uint32_t keyHash(std::string_view name);

    struct StringTable
    {
        std::vector<std::pair<uint32_t, std::string>> entries; // in file order
        const std::string *find(uint32_t key) const;
    };

    bool readStringTable(const std::vector<uint8_t> &file, StringTable &out);
    std::vector<uint8_t> writeStringTable(const StringTable &table);

    struct LevelText
    {
        std::map<uint32_t, std::string> strings; // by offset in the block
        std::vector<uint8_t> body;               // the bytes after the block
    };

    bool readLevelText(const std::vector<uint8_t> &file, LevelText &out);
    // (string of a, string of b) for every string of `a` the body of `a`
    // points at, when both bodies have the same size (else nothing): at each
    // 4-byte position where a's word is the offset of one of a's strings
    // and b's word that of one of b's, the pair is counted; each string of
    // `a` takes the string of `b` it was paired with most often. Empty
    // strings are left out.
    std::vector<std::pair<std::string, std::string>> pairLevelText(const LevelText &a, const LevelText &b);

    // English text -> text in another language. Lookups are exact first;
    // then without regard to ASCII case (an upper-case English text gets the
    // translation in upper case, as the game upper-cases ASCII letters
    // only), with surrounding spaces kept; then the words before a colon
    // ("Jump: 3"), in brackets, after a number ("8 pts"); then a known
    // ending after a name ("Jaren's Stats", also before a colon); then
    // sentence by sentence when every sentence has a translation ("Start at
    // the title screen. Applies at next start."); then line by line and
    // between the engine's inline markup ("#RGB{83 130 186}Next Level").
    class Translation
    {
    public:
        // Adds a pair unless the English text has one already (the first
        // wins), both are the same or the English is a single character. Texts with "\n" written as backslash
        // and n are added with real line breaks too.
        void add(const std::string &english, const std::string &other);
        // Replaces any pair for this English text.
        void set(const std::string &english, const std::string &other);
        const std::string *find(const std::string &english) const;
        // The text to show for `english`: its translation, else empty
        // (the caller keeps the English).
        std::string translate(const std::string &english) const;
        [[nodiscard]] size_t size() const { return m_exact.size(); }

    private:
        std::string lookup(const std::string &core) const;
        std::string lookupEnding(const std::string &core) const;
        std::string translateRun(const std::string &text) const;

        std::map<std::string, std::string> m_exact;
        std::map<std::string, std::string> m_folded; // upper-cased English -> translation
        // Texts starting with an apostrophe ("'s Stats"), upper-cased: the
        // end of a longer text (a name before them).
        std::vector<std::pair<std::string, std::string>> m_endings;
    };

    // UTF-8 -> Latin-1 (the games' text encoding); characters outside
    // Latin-1 become '?'.
    std::string latin1FromUtf8(std::string_view utf8);
    // ASCII letters upper-cased, the rest unchanged.
    std::string asciiUpper(std::string_view text);
}
