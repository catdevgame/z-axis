// DM2's park editor themes: the parks a new park starts from, read from
// assets\editor\themes.txt (a THEME block each: NAME, FILE, WIDTH, HEIGHT,
// CURSOR, OFFSET, AMBIANT, SUPPORTLIGHT, BORDER, MAP), kept on a list in
// the file's order. DM2's own code (its editor block, after editGui.c).
#pragma once

#include "park_editor.h"

#include "ee/ee.h"
#include "zge/container/list.h"
#include "zge/render/part.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2
{
    // A theme's map is 64 cells wide in memory, whatever its WIDTH.
    constexpr uint32_t kThemeMapStride = 64;
    constexpr uint32_t kThemeMapRows = 64;

    // What a MAP number makes of its cell (setThemeCellKind).
    // Scaffolding (unnamed-field): the kinds are named by their values until
    // the editor core's cell code says what each is.
    constexpr uint8_t kThemeCellKind0 = 0; // the MAP's 0
    constexpr uint8_t kThemeCellKind1 = 1; // a negative number: the height is its size
    constexpr uint8_t kThemeCellKind3 = 3; // a positive number: the height

    struct EditTheme
    {
        zge::ListNode link;              // +0x00
        char name[0x32];                 // +0x08 NAME
        char file[0x32];                 // +0x3A FILE: the theme's scene
        zge::VertexColour ambient;       // +0x6C AMBIANT red green blue, alpha 255
        uint32_t supportsLights;         // +0x70 1 with SUPPORTLIGHT
        int32_t offset;                  // +0x74 OFFSET
        uint8_t width;                   // +0x78 WIDTH: the park's columns
        uint8_t height;                  // +0x79 HEIGHT: the park's rows
        uint8_t border;                  // +0x7A BORDER
        uint8_t _7b;
        int32_t cursorColumn;            // +0x7C CURSOR: where the editor's cursor starts
        int32_t cursorRow;               // +0x80
        uint8_t index;                   // +0x84 its place in the file, from 0
        uint8_t cellHeights[kThemeMapRows * kThemeMapStride]; // +0x85 MAP, a row after a row
        uint8_t cellKinds[kThemeMapRows * kThemeMapStride];   // +0x1085 kThemeCellKind...
        uint8_t _2085[3];
    };
    static_assert(offsetof(EditTheme, file) == 0x3A && offsetof(EditTheme, ambient) == 0x6C);
    static_assert(offsetof(EditTheme, width) == 0x78 && offsetof(EditTheme, cursorColumn) == 0x7C);
    static_assert(offsetof(EditTheme, index) == 0x84 && offsetof(EditTheme, cellHeights) == 0x85);
    static_assert(offsetof(EditTheme, cellKinds) == 0x1085 && sizeof(EditTheme) == 0x2088);

    // The themes read from the file; the park's cells emptied and the list
    // forgotten.
    void initEditThemes();
    void clearEditThemes(ee::Ptr<ParkEditor> editor);

    // The theme of that index (its place in the file), none: 0.
    ee::Ptr<EditTheme> findTheme(uint8_t theme);
    uint8_t themeWidth(uint8_t theme);
    uint8_t themeHeight(uint8_t theme);
    uint8_t themeBorder(uint8_t theme);
    ee::Ptr<char> themeFile(uint8_t theme);
    int32_t themeOffset(uint8_t theme);
    uint8_t themeCellHeight(uint8_t theme, uint8_t column, uint8_t row);
    uint8_t themeCellKind(uint8_t theme, uint8_t column, uint8_t row);
    void setThemeCellKind(ee::Ptr<EditTheme> theme, uint8_t column, uint8_t row, uint8_t kind);

    // The theme's lighting: the light tool offered if the theme supports
    // lights, and its ambient colour the park's. Returns supportsLights.
    uint32_t applyThemeLighting(uint8_t theme);

    // The editor opened on the park's theme: the Rails, Start and Gaps tool
    // entries, the theme set up, the cursor where the theme says.
    void openParkTheme();

    // One THEME block read into a new record; the whole file.
    ee::Ptr<EditTheme> readTheme();
    void readThemes();

    // The theme the park was set up with.
    ee::Ptr<EditTheme> currentTheme();
    void setCurrentTheme(uint32_t theme);
    uint8_t currentThemeIndex();
    ee::Ptr<char> currentThemeName();
}
