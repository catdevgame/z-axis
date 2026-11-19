#pragma once

// Collision data made on the host from the GEO files (native asset
// loading), for the readable collision code (zge/collision) to read instead
// of the game's RAM copy.
//
// A collision part (the mesh a node's collision frame names) as the game
// has it after loading: positions (float4, w = 1, as the TGVP loader
// writes them), faces (TGFP), a normal per face (TGFN, or TIFN through its
// index table), the face tree of its object's TGCI, and the two tag tables
// (TGFT face tags, TGFE face edges) with the words the level's setup
// writes into them at load (the game's own setup, modelled by the game:
// LevelSetupStep). Read from the disc file the part came from (its disc
// origin, through the game's file layer), else from the GEO stream the
// game read into RAM before parsing it (geo_meshes.h filePartFor).
//
// PS2X_NATIVE_COLLISION (on by default since round 9; =0 is the fallback,
// the game's RAM copy as the recompiled code reads it): the readable face
// tests read faces, positions, face normals and face trees from here
// (zge/collision/faces.cpp), the part accessors face normals, positions and
// face vertex counts (mesh.cpp), and the tag table accessors a part's face
// tags and edges (zge/assets/tag_table.cpp, through the mirrors below).
// =verify: the same, each read compared with RAM. PS2X_NATIVE_COLLISION_CHECK=1 compares every part the readable
// collision code tests with RAM, the setup's words included
// (collision_check.h).

#include "zge/collision/collision.h"
#include "ee/plain.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace zge::native
{
    struct GeoFilePart;

    // A tag table as it is in RAM from +0x10 (collision.h TagTable): the
    // indices, then the records at the next 16 bytes.
    struct HostTagTable
    {
        bool present = false;
        uint32_t descriptor = 0; // TagTable::descriptor (the chunk's, without bit 3)
        uint32_t count = 0;      // indices
        uint32_t width = 0;      // bytes per index (0: a kind with no records)
        uint32_t recordsAt = 0;  // the records' offset in `body`
        uint32_t recordBytes = 0, records = 0;
        std::vector<uint8_t> body;

        // tagElementRecord (tagRecordAt): the element's record's offset in
        // `body`, or -1 for none (index 0xFF / 0xFFFF, or a kind without
        // records). The game does not check the index against `records`;
        // an index past the body is -1 here.
        int64_t recordOffset(uint32_t element) const;
        uint8_t *record(uint32_t element)
        {
            const int64_t at = recordOffset(element);
            return at < 0 ? nullptr : body.data() + at;
        }
        const uint8_t *record(uint32_t element) const
        {
            const int64_t at = recordOffset(element);
            return at < 0 ? nullptr : body.data() + at;
        }
    };

    // What a level setup step wrote (LevelSetupStep; AI's words).
    struct LevelSetupWords
    {
        uint32_t edgeElements = 0;   // edge records' +0x04
        uint32_t taggedFaces = 0;    // kind 0xB face tags' +0x08
        uint32_t nearestEdges = 0;   // kind 1 / 2 face tags' +0x04 written (each face that found one)
        uint32_t searchedFaces = 0;  // kind 1 / 2 faces searched
        // Candidates the model could not work out: a kind 1 / 2 face or a
        // matching edge with a vertex past the positions (the game reads RAM
        // there). Its words for those faces may differ from the game's.
        uint32_t undetermined = 0;
    };

    struct HostCollisionPart
    {
        uint32_t partId = 0;
        uint32_t vertexCount = 0, faceCount = 0;
        std::vector<Vec4> positions;
        std::vector<Face> faces;
        bool hasFaceNormals = false;
        std::vector<Vec3> faceNormals; // one per face, resolved
        bool hasTree = false;
        std::vector<uint8_t> tree;     // the object's TGCI tree when it names this part
        HostTagTable faceTags, faceEdges;
        LevelSetupWords setup;
        bool fromDisc = false;         // read from the disc file (else the RAM stream's copy)
        // Faces with a vertex index past the positions (some level parts'
        // last faces, flagged as colliding: their indices look like strip
        // entries). The file does not say where those vertices are; the game
        // reads whatever follows the positions stream in RAM, so the face
        // tests read those vertices from RAM too (vertexInFile false).
        uint32_t facesPastPositions = 0;
        bool vertexInFile(uint32_t vertex) const { return vertex < positions.size(); }
    };

    // A game's level setup on a host part: the words its own setup pass
    // writes into the part's tag tables at load (AI's: game/aggressive/level/
    // collision_setup_host.cpp). The setup is game code, not the engine's:
    // a game that registers none (DM2, BMX XXX: their setups are not
    // modelled) gets the file's words. (DM2's edge records are 8 bytes with
    // no element word: AI's +0x04 write landed on their kind and flags, and
    // the readable tag reads answered from bytes the game's RAM never held.)
    using LevelSetupStep = void (*)(HostCollisionPart &part);
    // The running game's install registers its step (this instance's: each
    // game's instance of zge has its own); none by default.
    void setLevelSetupStep(LevelSetupStep step);

    // Builds the part from a GEO file's part and applies `setup` (none:
    // nullptr). `indexWidth` is the engine's table of index bytes by kind
    // (collision.h g_tagIndexWidth).
    std::shared_ptr<HostCollisionPart> makeHostCollisionPart(const GeoFilePart &file, const uint32_t indexWidth[16],
                                                             LevelSetupStep setup);

    // The host part for a part the game loaded (nullptr if the readable
    // TGPT loader did not note it, or its file could not be read). Cached
    // per part until parts are noted again (geo_meshes.h notedPartsEpoch)
    // or a positions stream is written (notePartPosition).
    std::shared_ptr<const HostCollisionPart> hostCollisionPart(uint32_t partAddress);
    // The same, given the part as the game's code holds it.
    std::shared_ptr<const HostCollisionPart> hostCollisionPart(ee::Address part);

    // A part's positions as the game changes them: setPartPosition (the park editors' towers, DM2's water, ...)
    // watched around whatever runs it, the readable body or the original (collision_data.cpp installs the watch
    // once the running game's override has applied). A positions stream written to gets a host part of its own,
    // a copy of the one its part shared with the other parts made from its file part, with each written position
    // in it, so the face tests read what RAM holds; every part on that stream is served it. Kept while RAM holds
    // those positions, and until a part noted anew holds that stream (notePart; notePartClone: a clone's copied
    // stream takes a copy of it). In the game only (never under ps2xFnCheck), with the switch on.
    void notePartPosition(uint32_t partAddress, uint32_t index);
    void forgetOwnHostPart(uint32_t partAddress);
    void noteOwnHostPartClone(uint32_t cloneAddress, uint32_t sourceAddress);

    // PS2X_NATIVE_COLLISION unset, =1 or =verify (=0: off).
    bool nativeCollisionEnabled();
    // PS2X_NATIVE_COLLISION=verify: as 1, and every value read from the host
    // part is compared with the game's RAM (noteVerifiedRead), the count of
    // reads and differences logged with the face tests'.
    bool nativeCollisionVerify();
    void noteVerifiedRead(const void *host, const void *ram, size_t bytes);

    // Counts a face test with the switch on: on host data, or on RAM (no
    // host part for it, or no tree in its file); logged every 20000.
    void countNativeCollisionTest(bool host);

    // ---- the tag tables the game holds, mirrored on the host ----------------
    //
    // The game reaches a part's face tags and face edges through the tag
    // table accessors (zge/assets/tag_table.cpp: tagElementRecord an element's
    // record, tagByte / tagWord a byte or word of it, and the
    // writes setTagByte / setTagWord), handing round record addresses in
    // the RAM table. With PS2X_NATIVE_COLLISION on, a table the TGFT / TGFE
    // loader filled for a part (noteTagTable) gets a host mirror: the host
    // part's table (collision_data above, the level setup's words included),
    // made the first time the table is used, kept for that RAM table alone
    // (instances of a file part each have their own table in RAM, and the
    // game writes bytes into them as it plays), until its last release
    // (forgetTagTable). The accessors read the mirror where an access falls
    // in one, and write both. The game's addresses stay the game's.

    enum class TagTableKind : uint8_t
    {
        FaceTags,  // TGFT, ZgePart::faceTags (+0x44)
        FaceEdges, // TGFE, ZgePart::faceEdges (+0x4C)
    };

    struct TagMirror
    {
        uint32_t table = 0;      // the RAM table (its 16-byte header)
        uint32_t descriptor = 0; // TagTable::descriptor
        uint32_t count = 0;      // TagTable::count
        std::vector<uint8_t> body; // RAM from table + 0x10: the indices, then the records

        uint32_t bodyAt() const { return table + 0x10u; }
        bool holds(uint32_t address, uint32_t bytes) const
        {
            return address >= bodyAt() && uint64_t(address) + bytes <= uint64_t(bodyAt()) + body.size();
        }
        uint8_t *at(uint32_t address) { return body.data() + (address - bodyAt()); }
        // The same, given the address as the game's code holds it.
        uint8_t *at(ee::Address address);
        // The body by offset from its start (the first index).
        bool holdsAtOffset(uint32_t offset, uint32_t bytes) const { return uint64_t(offset) + bytes <= body.size(); }
        const uint8_t *atOffset(uint32_t offset) const { return body.data() + offset; }
    };

    // The TGFT / TGFE loader (zge/assets/geo.cpp): `table` was filled with
    // `bodyBytes` from the file for `part`. Host only (never under
    // ps2xFnCheck), with the switch on.
    void noteTagTable(uint32_t table, uint32_t part, TagTableKind kind, uint32_t bodyBytes);
    // The table's last reference went (releaseTagTable frees it).
    void forgetTagTable(uint32_t table);
    // The same, given the addresses as the game's code holds them.
    void noteTagTable(ee::Address table, ee::Address part, TagTableKind kind, uint32_t bodyBytes);
    void forgetTagTable(ee::Address table);
    // The mirror of the table at `table` (its header's address), or of the
    // table whose body holds [address, address + bytes); nullptr: read RAM.
    // Made on first use; nullptr when the host has no such table for the part
    // (its file part not found, or a different size). Outside ps2xFnCheck
    // and with the switch on only; the EE thread's.
    TagMirror *tagMirror(uint32_t table);
    TagMirror *tagMirror(ee::Address table);
    TagMirror *tagMirrorHolding(uint32_t address, uint32_t bytes);
    TagMirror *tagMirrorHolding(ee::Address address, uint32_t bytes);
    // PS2X_NATIVE_COLLISION_POISON=1 (a check, with the switch on): when a
    // face test first reads a part from the host, the game's RAM copy of
    // what the host now serves is overwritten with 0x7F bytes: the part's
    // positions (as many as the file has), its face normals, the face tree
    // and the bodies of its tag tables with mirrors. A reader left on RAM
    // then sees garbage, so a run identical to one without it shows that
    // nothing reads them there any more. The faces stay (faceVertices hands
    // their RAM address to its callers).
    bool nativeCollisionPoison();
    void poisonRamCopy(uint32_t partAddress, uint32_t treeAddress);
    void poisonRamCopy(ee::Address part, ee::Address tree);

    // Counts reads the accessors served from a mirror, and a table the
    // element accessor met that has none (logged with the face tests').
    void countTagRead(bool host);
}
