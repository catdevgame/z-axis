#pragma once

// Photo mode helpers, free of the game and the window: a free camera
// (position, yaw, pitch, roll, field of view, moved by keys, mouse and
// sticks) and the picture filters (letterbox, vignette, black and white)
// applied to the presented picture (PS2Runtime::setPictureFilter). A game
// override (aggressive_inline_photo.cpp) puts the camera into the game.

#include <cstdint>
#include <string>
#include <vector>

namespace ps2x::photo
{
    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    Vec3 add(const Vec3 &a, const Vec3 &b);
    Vec3 scale(const Vec3 &a, float s);
    float dot(const Vec3 &a, const Vec3 &b);
    Vec3 cross(const Vec3 &a, const Vec3 &b);
    float length(const Vec3 &a);
    Vec3 normalized(const Vec3 &a);

    // One frame of camera input, in -1..1 per axis unless said otherwise.
    struct Controls
    {
        float moveForward = 0.0f; // + forward
        float moveRight = 0.0f;   // + right
        float moveUp = 0.0f;      // + up (the world's up)
        float lookYaw = 0.0f;     // degrees this frame, + turns right (mouse)
        float lookPitch = 0.0f;   // degrees this frame, + tilts up (mouse)
        float turnYaw = 0.0f;     // stick, -1..1: turns at turnRate degrees a second
        float turnPitch = 0.0f;
        float roll = 0.0f;        // -1..1: rolls at rollRate degrees a second, + clockwise
        float zoom = 0.0f;        // field of view steps (+ narrower)
        float speedFactor = 1.0f; // held fast / slow keys
    };

    struct FreeCamera
    {
        // The world's up direction (Aggressive Inline: y down, so 0, -1, 0).
        Vec3 worldUp{0.0f, -1.0f, 0.0f};
        Vec3 position;
        float yaw = 0.0f;   // degrees; 0 looks along +z, + turns right
        float pitch = 0.0f; // degrees above the horizon, within +-89
        float roll = 0.0f;  // degrees, -45..45
        float fov = 75.0f;  // degrees, minFov..maxFov
        float speed = 4.0f; // units a second at speedFactor 1

        float minFov = 15.0f;
        float maxFov = 110.0f;
        float fovStep = 5.0f;
        float turnRate = 120.0f;
        float rollRate = 45.0f;
        float maxRoll = 45.0f;
        // How far from `home` the camera may go (0: no limit): the game only
        // streams in what is near the player.
        Vec3 home;
        float maxDistance = 0.0f;

        // Starts at a game camera: eye, the point it looks at, its FOV.
        void lookFrom(const Vec3 &eye, const Vec3 &target, float fieldOfView);
        Vec3 forward() const;
        // Right and up of the view without roll.
        Vec3 right() const;
        Vec3 up() const;
        void update(const Controls &controls, float seconds);
        // The point one unit ahead (for a look-at camera).
        Vec3 target() const { return add(position, forward()); }
    };

    // Rolls a camera's right and up rows about its forward axis by
    // `degrees` (+ clockwise as seen through the camera).
    void rollRows(Vec3 &right, Vec3 &up, float degrees);

    // Picture filters, combined as bits.
    enum Filter : uint32_t
    {
        kFilterLetterbox = 1u, // 2.39:1 black bars
        kFilterVignette = 2u,  // darker corners
        kFilterMono = 4u,      // black and white
    };

    // The presets photo mode cycles through: None, Letterbox, Vignette,
    // Black & White, Cinema (letterbox and vignette), Noir (all three).
    struct Preset
    {
        const char *name;
        uint32_t filters;
    };
    const std::vector<Preset> &presets();

    // Applies `filters` to a top-down RGBA picture shown at `displayAspect`
    // (width / height on screen: 4:3 or the widescreen one). Alpha is left.
    void applyFilters(uint8_t *rgba, uint32_t width, uint32_t height, uint32_t stride, uint32_t filters,
                      float displayAspect);
    // Rows covered by each letterbox bar.
    uint32_t letterboxRows(uint32_t height, float displayAspect);
}
