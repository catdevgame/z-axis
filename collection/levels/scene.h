#pragma once

// A level's placement and scene files as typed records: the instances
// (.INS: TGIH, each a geometry object placed in the world), the scene points
// and visibility zones (.DAT: SDPT, PVSO) and the scene file (.ZIT: the
// collision world's box and the level's file names). The engine's loaders'
// layouts (zge/scene/node.h, locators.h, scene_points.cpp); ZGE2's TGIH is
// 0x6C bytes without the region word and its SDPT index a word; ZGE3's and
// ZGE4's are the same.

#include "collection/levels/geometry.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace zx::app::levels
{
    // TGIH: a geometry object placed in the world (or under its parent
    // instance). flags: bit 1 collides (0x10 with it: not collided with),
    // 0x01 shown, 0x10 the sky, 0x400 mirrored. The world box is the
    // exporter's (the game reads neither corner).
    struct Instance
    {
        static constexpr uint32_t kCollides = 0x2;
        static constexpr uint32_t kNoCollision = 0x10;

        uint32_t flags = 0;
        Quat rotation{0.0, 0.0, 0.0, 1.0};
        Vec3 position{};
        uint32_t geometryId = 0;
        uint32_t instanceId = 0;
        std::string name;
        uint32_t nameHash = 0;
        uint32_t parent = 0;
        uint32_t region = 0; // a zone's mesh hash, 0: by position (not in ZGE2)
        uint8_t level = 0;
        uint8_t materialSet = 0;
        Vec3 worldMin{}, worldMax{};
        std::optional<Bytes> tags; // TGIT

        static Instance read(const Bytes &data, std::optional<Bytes> tags, Engine engine);
        Bytes write(Engine engine) const;
        // An instance of the object at `position`, its world box the object's
        // bounds turned (all eight corners), its id its name's hash.
        static Instance make(const std::string &name, const GeoHeader &object, const Vec3 &position, const Quat &rotation,
                             bool collides, std::optional<Bytes> tags);
    };

    // An INS file: FORM { TGIN { TGIH, TGIT? } ... }.
    struct Instances
    {
        std::vector<Instance> instances;

        static Instances read(const Bytes &data, Engine engine);
        Bytes write(Engine engine) const;
    };

    // The scene point kinds and names the converters use.
    extern const uint32_t kSpawnPoint;   // "Spawn"
    extern const uint32_t kGlobalsPoint; // "Globals"
    extern const uint32_t kPlayerOne;    // "PlayerOne"
    extern const uint32_t kPlayerTwo;    // "PlayerTwo"
    extern const uint32_t kCameraPoint;  // "CameraPos": a camera point's kind (BMX XXX's camera.c)
    extern const uint32_t kIdleCamera;   // "idle": the idle cameras' name (BMX XXX's idleCameraMode)

    // The named spawn points only one game's scripts or modes read
    // (ai_level_convert.py SCRIPT_SPAWNS): AI's Tutorial lesson points
    // (TutorialAssignerN's setRespawnLocation, the career's TUT.BIN) and the
    // Movie Lot's wickedTree; BMX XXX's paintball points (spawnPlayer in
    // kPaintballMode). The export carries them by name; a converter writes
    // them only for their own game (no other reads them, and BMX XXX would
    // take a lesson point as a respawn).
    struct ScriptSpawns
    {
        Engine engine;                  // the game that reads them
        std::vector<std::string> names; // as its scripts name them
    };
    const std::vector<ScriptSpawns> &scriptSpawns();

    // SDPT: a scene point (its kind a class name's hash, its name's hash,
    // index, position, orientation), then its class's record.
    struct ScenePoint
    {
        static constexpr uint32_t kHeader = 0x2C;

        uint32_t kind = 0;
        uint32_t name = 0;
        Vec3 position{};
        Quat orientation{0.0, 0.0, 0.0, 1.0};
        uint32_t index = 0;
        Bytes tags;

        static ScenePoint read(const Bytes &data, Engine engine);
        Bytes write(Engine engine) const;
        static ScenePoint spawn(const std::string &name, const Vec3 &position, const Quat &orientation, Bytes tags = {});
    };

    // PVSO: a visibility zone (its mesh hash, box, the zones hidden from it);
    // `flag` the newer body's word, none for the older body (DM2's only form).
    struct ViewRegion
    {
        uint32_t mesh = 0;
        Vec3 low{}, high{};
        std::vector<uint32_t> hidden;
        std::optional<uint32_t> flag = 0u;

        static ViewRegion read(const Bytes &data);
        Bytes write() const;
    };

    // A DAT file: FORM { SDPT ..., ZLIT, ZPVS { PVSO ... } } in its order.
    struct SceneData
    {
        std::vector<ScenePoint> points;
        std::optional<std::vector<ViewRegion>> regions;
        std::vector<Tag> order;
        std::vector<Chunk> other;

        static SceneData read(const Bytes &data, Engine engine);
        Bytes write(Engine engine) const;
    };

    // A ZIT: FSCN { TERR { TEHD (the collision world's cell, box and grid
    // counts), ZFTX, ZFGE, ZFIS, ZFSD (the level's file names) } }: the same
    // form in all three games.
    struct SceneFile
    {
        Vec3 cell{};
        Vec3 low{}, high{};
        std::array<uint32_t, 3> counts{};
        std::map<std::string, std::string> names; // by tag text: ZFTX, ZFGE, ZFIS, ZFSD

        static SceneFile read(const Bytes &data);
        Bytes write() const;
        // The ZIT of files named <level>.TXR, .GEO, .INS and .DAT (the case
        // kept) around the box.
        static SceneFile make(const std::string &level, const Vec3 &low, const Vec3 &high, double cell = 50.0);
    };

    // A texture of a TXR (TXRC: TXRH + its levels), as DM2's library keeps
    // them: the id (the name's hash), size, two reserved halfwords, the name
    // and each level's BMP bytes (TBMP).
    struct LibraryTexture
    {
        uint32_t id = 0;
        uint16_t width = 0, height = 0;
        std::string name;
        std::vector<Bytes> levels;
        uint16_t reserved08 = 0, reserved0a = 0;

        static LibraryTexture read(const Chunk &chunk);
        Chunk chunk() const;
    };

    // A TXR file of DM2's textures: FORM { TXRC ... }.
    struct TextureLibrary
    {
        std::vector<LibraryTexture> textures;

        static TextureLibrary read(const Bytes &data);
        Bytes write() const;
    };

    // The small files a converted level folder has besides its five: AI's
    // empty level script (<slot>.BIN, <slot>T.BIN: one object with one empty
    // state), an asset library with no entries (AI's MOTIONS.ZAL, BMX XXX's
    // four .ZAL) and BMX XXX's objectives file with no objectives.
    Bytes emptyLevelScript();
    Bytes emptyAssetLibrary();
    Bytes emptyObjectivesFile();
}
