#pragma once

// The map converter's inside (collection/levels/convert.h): the model read
// into level terms (surfaces, textures, pieces, placements, grind marks,
// spawns) and those written as one game's records. LevelConverter is Aggressive
// Inline's converter (ai_level_convert.py's Converter); Dm2LevelConverter
// and BmxLevelConverter are the other two games' (dm2_level_convert.py,
// bmx_level_convert.py), each its own records, budgets, sky and grind edges.

#include "collection/levels/convert.h"
#include "collection/levels/scene.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace ps2x::app::levels
{
    // ---- tags ----------------------------------------------------------------------

    // A tag's value: present with no value (`#qp`), or a text (`#slope=0.5`).
    struct TagValue
    {
        bool hasText = false;
        std::string text;
    };
    using TagMap = std::map<std::string, TagValue>;

    // "name#tag#key=value" -> the name and its tags (keys lower case).
    std::pair<std::string, TagMap> splitTags(const std::string &name);
    bool hasTag(const TagMap &tags, const char *key);
    // A distinct name of at most 31 bytes, distinct as the games' name hash
    // tells names apart (without case): `used` holds the names upper-cased.
    std::string shortName(const std::string &name, std::set<std::string> &used);
    // A name distinct as shortName's, its length kept: an object's whole
    // name, which its header cuts and hashes (GeoHeader::make).
    std::string distinctName(const std::string &name, std::set<std::string> &used);

    // ---- textures (levels/textures.cpp) ----------------------------------------------

    struct Report
    {
        std::vector<std::string> lines; // the level's numbers
        std::vector<std::string> done;  // what the converter changed to fit
        std::vector<std::string> errors;

        void say(std::string text) { lines.push_back(std::move(text)); }
        void did(std::string text) { done.push_back(std::move(text)); }
        void error(std::string text) { errors.push_back(std::move(text)); }
        std::vector<std::string> text() const;
    };

    constexpr int kClearAlpha = 8;     // a texel's alpha under this is clear
    constexpr int kSolidAlpha = 247;   // over this, solid
    constexpr int kCutOutPartial = 10; // a texture with fewer than this percent of its texels neither is a cut-out
    constexpr int kCutOutKeep = 77;    // a cut-out's halved texel this covered (30% of 255) stays solid

    struct Texture
    {
        std::string name;
        int width = 0, height = 0;
        std::vector<Rgba> pixels; // rows top first

        size_t texels() const { return static_cast<size_t>(width) * static_cast<size_t>(height); }
        bool hasAlpha() const;
        const Rgba &at(int x, int y) const { return pixels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)]; }
        // A cut-out (a fence's or railing's bars, leaves): clear texels, and
        // under kCutOutPartial percent of them neither clear nor solid.
        bool cutOut() const;
        // Half the size each way (a 2 x 2 box filter). A cut-out stays one: a
        // texel kCutOutKeep covered or more is solid, the rest clear, its
        // colour the solid texels' (averaged as they are, one-texel bars came
        // out half transparent and darkened by the clear texels' colour, and
        // the blended material drew them as a faint ghost: a converted map's
        // arches and railings, 2026-11-17).
        void halve();
        // Nearest-neighbour to w x h.
        void resample(int w, int h);
    };

    // Every texture within `side` (resampled to powers of two, halved), then
    // the whole set within the texel budget, the largest halved first.
    void fitTextures(std::vector<Texture *> &textures, Report &report, int side, int budgetKb);
    // At most 256 colours: the distinct ones as they are when they fit, else a
    // median cut over them weighted by use; each pixel its nearest entry.
    void palettize(const std::vector<Rgba> &pixels, bool withAlpha, std::vector<Rgba> &palette, std::vector<uint8_t> &indices);
    // AI's and BMX XXX's TXR of the textures (8-bit BMPs, TZMP when a texture
    // has alpha).
    Bytes writeTextureFile(const std::vector<const Texture *> &textures);
    // An 8-bit BMP as DM2's TBMP holds it (bottom-up, the palette's alpha 0).
    Bytes dm2Bmp8(int width, int height, const std::vector<uint8_t> &indices, const std::vector<Rgba> &palette);

    // ---- strips (levels/strips.cpp) ---------------------------------------------------

    // Triangle strips covering the triangles, each once with its winding.
    std::vector<std::vector<int>> stripify(const std::vector<std::array<int, 3>> &triangles);
    // The strips as strip groups of at most 60 entries, several strips a group.
    std::vector<StripGroup> packStrips(int submesh, const std::vector<std::vector<int>> &strips);

    Vec3 faceNormal(const Vec3 &a, const Vec3 &b, const Vec3 &c);

    // ---- the level -----------------------------------------------------------------------

    // One material as the level sees it.
    struct Surface
    {
        std::string name;
        std::string texture;
        int kind = 0;
        double gravity = 0.0;
        bool twoSided = false;      // #2sided: drawn (the MATI's flag) and collided on both sides
        bool facesTwoSided = false; // #2sided=collide too: collided on both sides
        Colour baseColour{1.0, 1.0, 1.0, 1.0};
        std::optional<Bytes> layer; // #layer=<32 hex digits>
        int source = -1;            // the model's material index (-1: none)
        // The bike games' forms: the surface kind and launch byte, BMX XXX's sound.
        bool bikeRead = false;
        int bikeKind = 0;
        int launch = 50;
        int sound = 0;
    };

    // A level object to be: vertices in object space (game axes), faces with
    // their surface.
    struct Piece
    {
        std::string name;
        std::vector<Vec3> positions;
        std::vector<Vec3> normals;
        std::vector<Vec2> uvs;
        std::vector<Rgba> colours;
        std::vector<std::array<int, 4>> faces; // surface, a, b, c
        bool collides = true;
        bool sky = false;
        // The object's name hash when the model gives it (a stock object whose
        // name the field cut); none: its name's.
        std::optional<uint32_t> nameHash;
        // #dynamics: a dynamics prop's definition.
        std::optional<DynamicsDefinition> dynamics;
    };

    struct Placement
    {
        std::string nodeName;
        size_t pieces = 0; // the index of its pieces' entry
        Vec3 position{};
        Quat rotation{};
        bool collides = true;
        bool hidden = false;
        bool sky = false;
        bool dynamics = false; // a dynamics prop (DynamicsForm's flags and tags)
    };

    struct EdgeMark
    {
        Vec3 a{}, b{}; // world, game axes
        int kind = 0;
        int flags = 0;
        int sound = 0;
    };

    struct Spawn
    {
        std::string name;
        Vec3 position{};
        Quat rotation{};
    };

    // An idle camera: the eye (the games' axes), the view's rotation (+z
    // forward) and the field of view in degrees.
    struct Camera
    {
        Vec3 position{};
        Quat rotation{0.0, 0.0, 0.0, 1.0};
        int fov = 60;
    };

    // The rotation that turns +z to the unit direction d with no roll: a
    // pitch about x, then a yaw about y (by square roots alone, as the
    // tooling's look_rotation).
    Quat lookRotation(const Vec3 &d);

    // What a sky picture gives the level: the zenith's colour (the clear
    // colour) and the horizon's (the fog's).
    struct SkyLook
    {
        std::array<int, 3> zenith{}, horizon{};
    };

    // A level's Globals point in no game's layout.
    struct Globals
    {
        std::array<int, 3> clear{}, fog{};
        int fogAlpha = 0;
        double fogStart = 0, fogEnd = 0, farClip = 0;
        std::array<int, 3> light{0x80, 0x80, 0x80};
    };

    // How one game draws a converted level's sky and reads its Globals point.
    struct SkyForm
    {
        uint16_t partFlags;
        uint32_t instanceFlags;
        int fogAlpha;
        double fogStart, fogEnd, farClip;
        Bytes (*tags)(const Globals &);
    };

    // A dynamics prop (#dynamics) is an object of the DynamicsObject type:
    // at its set-up the game takes the ZDYN definition whose DDEF is the
    // object's name hash and knocks the prop about as its bodies. What one
    // game writes for a prop besides its geometry and ZDYN: the TGTD bytes
    // after the type word, the instance's flags and tags. A definition from
    // the same game keeps its own class data and tags; one from another game
    // takes these (the games' sounds are their own: AI's sample numbers,
    // DM2's object sound, BMX XXX's names).
    struct DynamicsForm
    {
        Engine game;
        uint32_t instanceFlags; // the stock props' (AI's and BMX XXX's 0x63, DM2's 0x23)
        Bytes classData;
        Bytes instanceTags;
    };

    class LevelConverter
    {
    public:
        LevelConverter(const Model &model, std::string slot, Engine engine);
        virtual ~LevelConverter() = default;

        // The level's files by extension (GEO, INS, DAT, TXR, ZIT).
        virtual std::map<std::string, Bytes> build();
        // An error in the report when the files are over the level bytes:
        // the level would not fit the game's memory (it is refused, never
        // started).
        void checkLevelBytes(const std::map<std::string, Bytes> &files);
        Report report;
        std::vector<Spawn> spawns;
        std::vector<Camera> cameras;

    protected:
        // The game's numbers and forms (AI's by default).
        Engine m_engine;
        int m_texelBudgetKb = 2560;
        int m_textureSide = 128;
        size_t m_partsInView = 1083;
        // The level files' bytes (GEO, INS, DAT, TXR) at most: the game reads
        // them whole into its 32 MB heap, so a level no larger than its largest
        // stock one fits as that one does (AI's: the Museum's).
        size_t m_levelBytes = 10577384;
        const SkyForm *m_sky;
        const DynamicsForm *m_dynamics;
        uint32_t m_firstObjectId = 1;
        // The spawns' names in the game: the model's #spawn=respawn is the
        // game's own name (AI's "Respawn"), PlayerTwo none where the game has
        // no second start. Whether a level with no idle camera gets one at its
        // start (AI reads none), and what the game adds to a camera point's
        // position for the eye (DM2's level-point camera: 5 m above it).
        std::string m_respawnName = "Respawn";
        bool m_hasPlayerTwo = true;
        bool m_makesCamera = false;
        double m_cameraLift = 0.0;
        Bytes m_spawnTags; // the tag record each spawn point gets

        virtual void readModel();
        virtual int surfaceOf(std::optional<int> material);
        virtual void addEdges(const Node &node, const Mesh &mesh, const TagMap &tags);
        virtual GeoObject geoObject(const Piece &piece, uint32_t objectId);
        virtual void tagEdges(Geometry &geo, const std::vector<Instance> &instances);
        virtual SceneData sceneData();
        virtual Bytes textureFile();
        virtual Bytes sceneFile(const Vec3 &low, const Vec3 &high);
        virtual void useSkyLook(const SkyLook &look);

        // A piece's object header: its bounds, the id, its name and hash.
        static GeoHeader objectHeader(const Piece &piece, uint32_t objectId);
        // A prop's TGTD bytes after the type word and its instances' tags:
        // the definition's own when it is this game's, else m_dynamics's.
        Bytes dynamicsClassData(const DynamicsDefinition &definition) const;
        Bytes dynamicsInstanceTags(const DynamicsDefinition &definition) const;

        ScenePoint globalsPoint();
        // An idle camera behind and above the PlayerOne spawn, looking at it
        // (none without a PlayerOne).
        std::optional<Camera> startCamera() const;
        // The idle cameras as "CameraPos" points named "idle", 1 to N, the
        // first tag byte the field of view (one made at the start when the
        // model has none and m_makesCamera).
        std::vector<ScenePoint> cameraPoints();
        // The spawn points then the camera points, in the game's records.
        std::vector<ScenePoint> spawnAndCameraPoints();
        void worldBox(const std::vector<Instance> &instances, Vec3 &low, Vec3 &high) const;

        const Model &m_model;
        std::string m_slot;
        std::vector<Surface> m_surfaces;
        // The textures by name, in the order they were made.
        std::vector<Texture> m_textures;
        Texture *texture(const std::string &name);
        // The pieces of each (mesh, scale, collides, sky, dynamics), in the
        // order made.
        struct PieceKey
        {
            int mesh;
            Vec3 scale;
            bool collides, sky, dynamics;
            bool operator==(const PieceKey &o) const
            {
                return mesh == o.mesh && scale == o.scale && collides == o.collides && sky == o.sky && dynamics == o.dynamics;
            }
        };
        std::vector<std::pair<PieceKey, std::vector<Piece>>> m_pieces;
        std::vector<Placement> m_placements;
        std::vector<EdgeMark> m_edgeMarks;
        std::set<std::string> m_names; // upper-cased (shortName)
        std::optional<Globals> m_globals;

    private:
        void addObject(const std::string &name, const Node &node, const Mesh &mesh, const TagMap &tags);
        std::vector<Piece> makePieces(const std::string &name, const Mesh &mesh, const Vec3 &scale, bool collides, bool sky);
        std::vector<Piece> cut(const Piece &piece);
        std::optional<SkyLook> skyLookOf(const std::vector<const Piece *> &pieces);
        void budgets(const Geometry &geo, const std::vector<Instance> &instances);
    };

    // The grind marks by the 4 m cells their segments cross, so a face edge
    // looks only at the marks near it, in their own order.
    class MarkIndex
    {
    public:
        explicit MarkIndex(const std::vector<EdgeMark> &marks);
        std::vector<const EdgeMark *> near(const Vec3 &p) const;

    private:
        std::map<std::array<long long, 3>, std::vector<size_t>> m_cells;
        const std::vector<EdgeMark> &m_marks;
    };
    // p within 0.01 m of the mark's segment.
    bool onSegment(const Vec3 &p, const EdgeMark &mark);

    // The material record of AI and BMX XXX (0xA0 bytes): one layer showing
    // the texture, with a #layer's blending and tests.
    Chunk aiMaterial(const std::string &name, const std::string &texture, bool twoSided, const std::optional<Bytes> &layer);
    // A MATI layer's four state words: `base`, with the blending and the
    // tests of `given` (a #layer tag's 16 bytes).
    std::array<uint32_t, 4> layerState(const std::array<uint32_t, 4> &base, const std::optional<Bytes> &given);

    extern const uint32_t kTerrainClass; // "Terrain": every terrain object's and instance's class
    constexpr double kLodDistance = 500.0;
    constexpr size_t kFacesPerObject = 1024;
    constexpr size_t kVerticesPerPart = 16384;
    constexpr uint32_t kInstanceCollides = 0x63;
    constexpr uint32_t kInstanceDecoration = 0x61;
    constexpr uint32_t kInstanceShown = 0x01;
    constexpr uint32_t kInstanceSky = 0x10;
    constexpr uint16_t kPartFlags = 1;
    constexpr uint16_t kPartFlagsSky = 3;

    // Python's float(), int() of a tag's text; false when it does not read.
    bool parseFloat(const std::string &text, double &out);
    bool parseInt(const std::string &text, long long &out);
    // Python's x // y of two doubles.
    double floorDiv(double x, double y);

    // The other two games' converters (levels/convert_dm2.cpp, convert_bmx.cpp).
    Converted convertForDm2(const Model &model);
    Converted convertForBmx(const Model &model);
}
