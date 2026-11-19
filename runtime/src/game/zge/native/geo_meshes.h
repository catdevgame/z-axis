#pragma once

// Meshes made from the GEO files on the host (native asset loading): the
// triangles the game's packet builders would give VU1, made from the file's
// streams and strip groups instead of from the VIF packet in guest RAM
// (runtime/ps2_zaxis_mesh.h decodes that). The readable TGPT loader notes
// which file chunk each part it loads came from (notePart); a renderer can
// then ask for a part's mesh by its address (fileMeshForPart).
//
// PS2X_NATIVE_MESHES_CHECK=1: the first time the readable submitNodeDraw
// draws a part, the mesh from the file is compared with the packet's,
// vertex for vertex; a summary is logged every 64 parts and the
// first differences are described.

#include "geo_file.h"
#include "ee/plain.h"

#include "ee/ee.h"

#include "runtime/ps2_zaxis_mesh.h"

#include <cstdint>
#include <memory>
#include <string>

namespace zge
{
    struct ZgePart; // zge/render/part.h
}

namespace zge::native
{
    // The packet kind a part's render type is drawn with (part +0x60 >> 26);
    // false for a type with no packet builder known.
    bool packetKindOf(uint32_t renderType, ps2x::zaxis::PacketKind &kind);

    // The mesh a part's packet would decode to, made from the file's part
    // (skinned: as the game's builder sends the weights and fills the
    // material table, `form`). False (and why) for what is not modelled (a
    // stream missing).
    bool buildFileMesh(const GeoFilePart &part, ps2x::zaxis::PacketKind kind, ps2x::zaxis::RigidMesh &out,
                       std::string *why = nullptr, const ps2x::zaxis::SkinnedDrawForm &form = {});

    // A vertex's skin weights ({bone (1-based, 0 none), weight} x 4, the
    // stream's record) as the form's builder sends them.
    void packetSkinWeights(ps2x::zaxis::SkinnedDrawForm::Weights form, uint8_t record[8]);

    // The readable TGPT loader: the part at partAddress was loaded from the
    // TGPT chunk at chunkOffset of the file the game opened as gamePath. The
    // game reads level and model files into RAM and parses them from a
    // memory stream (zfile's "rd00" device, path "<address>:<size>"); those
    // are read from guest RAM (rdram) the first time one of their parts is
    // noted, while the stream is live.
    void notePart(const uint8_t *rdram, uint32_t partAddress, const std::string &gamePath, uint32_t chunkOffset);
    // The same, given the part as the game's code holds it.
    void notePart(const uint8_t *rdram, ee::Address part, const std::string &gamePath, uint32_t chunkOffset);

    // The readable clonePart: clone's streams are made from source's.
    void notePartClone(uint32_t cloneAddress, uint32_t sourceAddress);

    // The readable allocateMeshStreams: the part's streams made anew by the
    // game (BMX XXX's riders: the model's parts joined into one mesh over
    // the skeleton's, mergeSceneMeshes). No file has that mesh: it is made
    // from the part's own streams, as they are when its packet is built
    // (notePartStreams; nothing until then).
    void notePartStreamsMade(uint32_t partAddress);
    void notePartStreamsMade(ee::Ptr<zge::ZgePart> part);
    // Whether the part's streams were made by the game (and not since noted
    // from a file).
    bool partStreamsMade(uint32_t partAddress);
    // Such a part's streams, read from RAM as its packet was built
    // (zge/host/class_draws.cpp): its mesh is made from them from now on.
    void notePartStreams(uint32_t partAddress, std::shared_ptr<const GeoFilePart> streams);

    // What a part noted anew or cloned does to the host parts collision keeps of its own (collision_data.cpp sets
    // them; unset, nothing).
    using OwnHostPartForget = void (*)(uint32_t partAddress);
    using OwnHostPartClone = void (*)(uint32_t cloneAddress, uint32_t sourceAddress);
    void setOwnHostPartHooks(OwnHostPartForget forget, OwnHostPartClone clone);

    // The disc file a loaded part's data was read from and its TGPT chunk's
    // offset there (memory streams: through the disc layer's reads,
    // disc_files.h discOrigin). Thread-safe.
    bool partDiscOrigin(uint32_t partAddress, std::string &path, uint64_t &chunkOffset);

    // The file's part a loaded part came from (nullptr if not noted, or the
    // file could not be read). Thread-safe; the file is read on first use.
    std::shared_ptr<const GeoFilePart> filePartFor(uint32_t partAddress);
    // The same part read from the disc file it came from (partDiscOrigin:
    // the file through the game's file layer, the TGPT at that offset), not
    // from the stream the game read into RAM; nullptr if its disc origin is
    // not known or the file has no part there. Thread-safe; each file is
    // read and parsed once.
    std::shared_ptr<const GeoFilePart> discFilePartFor(uint32_t partAddress);
    // Counts every notePart / notePartClone: a cache by part address is
    // current while this has not changed.
    uint64_t notedPartsEpoch();

    // The mesh made from the file for a part (nullptr if none), for its
    // render type. Cached per part. Thread-safe.
    std::shared_ptr<const ps2x::zaxis::RigidMesh> fileMeshForPart(uint32_t partAddress, uint32_t renderType);
    // The same for the packet kind the renderer draws the part with (the
    // native draw path, game_overrides/aggressive_inline_native.cpp, which
    // declares it itself).
    std::shared_ptr<const ps2x::zaxis::RigidMesh> fileMeshForPacketKind(uint32_t partAddress,
                                                                        ps2x::zaxis::PacketKind kind);
    // The same with the game's skinned packet form (the records' host form, zge/host/class_draws.cpp).
    std::shared_ptr<const ps2x::zaxis::RigidMesh> fileMeshForPacketKind(uint32_t partAddress, ps2x::zaxis::PacketKind kind,
                                                                        const ps2x::zaxis::SkinnedDrawForm &form);

    // Time spent reading GEO data (streams copied and parsed at notePart),
    // and how many streams.
    uint64_t geoParseNanos();
    uint64_t geoStreamsParsed();

    // PS2X_NATIVE_MESHES_CHECK: the readable submitNodeDraw, for each part
    // it draws the first time (again once the part is noted anew).
    // The file's mesh is made as the part's packet is built (`kind`, for its render type: zge/host/class_draws.h
    // packetKindOf), with the game's skinned packet form.
    bool meshCheckEnabled();
    void checkPartMesh(const uint8_t *rdram, uint32_t partAddress, uint32_t renderType, ps2x::zaxis::PacketKind kind,
                       const ps2x::zaxis::SkinnedDrawForm &form = {});
    void checkPartMeshOnce(const uint8_t *rdram, uint32_t partAddress, uint32_t renderType, ps2x::zaxis::PacketKind kind,
                           const ps2x::zaxis::SkinnedDrawForm &form = {});

    // The same two for the readable callers, which hold the parts as
    // pointers: the registry keys them by address.
    void checkPartMeshOnce(const uint8_t *rdram, ee::Ptr<zge::ZgePart> part, uint32_t renderType,
                           ps2x::zaxis::PacketKind kind, const ps2x::zaxis::SkinnedDrawForm &form = {});
    void notePartClone(ee::Ptr<zge::ZgePart> clone, ee::Ptr<zge::ZgePart> source);
}
