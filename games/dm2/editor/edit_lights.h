// DM2's park editor lights: the lights a park's pieces and its theme carry,
// the kinds of light read from assets\editor\light.txt (CONE and OMNI
// lines), and the park's vertex colours lit from them a few meshes a frame.
// DM2's own code (its editor block, after editGui.c).
#pragma once

#include "ee/ee.h"
#include "zge/container/list.h"
#include "zge/math/types.h"
#include "zge/render/part.h"
#include "zge/scene/node.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2
{
    constexpr uint32_t kParkLightCone = 0; // light.txt's CONE, the "EditLightCone" locators
    constexpr uint32_t kParkLightOmni = 1; // OMNI, "EditLightOmni"

    // A kind of light, by its number in light.txt: `CONE n red green blue
    // range inner outer`, `OMNI n red green blue range inner`.
    struct ParkLightKind
    {
        uint8_t kind;              // +0x00 kParkLightCone, kParkLightOmni
        zge::VertexColour colour;  // +0x01 alpha 255
        uint8_t _05[3];
        int32_t range;             // +0x08 how far it reaches
        int32_t inner;             // +0x0C lit in full within it: a cone's angle, an omni's distance
        int32_t outerAngle;        // +0x10 a cone's edge
    };
    static_assert(offsetof(ParkLightKind, colour) == 1 && offsetof(ParkLightKind, range) == 8 && sizeof(ParkLightKind) == 0x14);

    // A light in the park.
    struct ParkLight
    {
        zge::ListNode link;        // +0x00
        uint32_t kind;             // +0x08 kParkLightCone, kParkLightOmni
        zge::VertexColour colour;  // +0x0C
        zge::Vec4 position;        // +0x10
        uint32_t unused;           // +0x20 made 0 with each light (collectThemeLights, addFaceLights); read nowhere
        ee::Float range;           // +0x24
        uint8_t _28[8];
        zge::Vec4 direction;       // +0x30
        ee::Float inner;           // +0x40
        ee::Float outerAngle;      // +0x44
        ee::Ptr<zge::Node> owner;  // +0x48 the node whose face carries it, 0: the theme's own
        uint32_t vertex;           // +0x4C the owner's vertex it sits at
    };
    static_assert(offsetof(ParkLight, position) == 0x10 && offsetof(ParkLight, unused) == 0x20 && offsetof(ParkLight, range) == 0x24);
    static_assert(offsetof(ParkLight, direction) == 0x30 && offsetof(ParkLight, owner) == 0x48 && sizeof(ParkLight) == 0x50);

    constexpr uint32_t kParkLightKindCount = 100;
    struct ParkLights
    {
        uint32_t ready;                 // +0x00 the kinds are read
        zge::List list;                 // +0x04 the ParkLights
        zge::VertexColour ambient;      // +0x10 the theme's AMBIANT
        uint32_t count;                 // +0x14 the lights on the list
        uint32_t brightness;            // +0x18 added to the ambient colour: the light menu's bar
        uint32_t _1c;
        ee::Ptr<ParkLightKind> kinds[kParkLightKindCount]; // +0x20
    };
    static_assert(offsetof(ParkLights, ambient) == 0x10 && offsetof(ParkLights, brightness) == 0x18 && offsetof(ParkLights, kinds) == 0x20);

    void setParkAmbientColour(ee::Ptr<const zge::VertexColour> colour);
    void initParkLights();
    void shutDownParkLights();
    void freeParkLights();

    // The LIGHTS tool offered (a theme that supports lights) or not: its
    // tool entry, the top header's icon roll and the pause menu's items.
    void offerLightTool();
    void withdrawLightTool();

    void addParkLight(ee::Ptr<ParkLight> light);
    // The theme's EditLightOmni and EditLightCone points made park lights,
    // then the world meshes' own.
    void collectThemeLights();
    void removeParkLight(ee::Ptr<ParkLight> light);
    // The lights of a node's faces tagged for one, made; the node's lights
    // removed and freed.
    // A light for each of the node's faces listed (findEditLightFaces):
    // at the face's first corner, along its normal, of the light kind the
    // face's tags name; none past a face whose kind is not read.
    void addFaceLights(ee::Ptr<zge::Node> node, ee::Ptr<uint32_t> faces, uint32_t count);
    void addNodeLights(ee::Ptr<zge::Node> node);
    // The node's collision mesh's vertex colours lit again: the park's
    // ambient colour raised by the brightness, then each park light's share
    // (the first 17 lights only, without allLights) by how squarely a vertex
    // faces it and how far into its reach it is. Not the backdrop.
    void lightNodeVertices(ee::Ptr<zge::Node> node, uint32_t allLights);
    void removeNodeLights(ee::Ptr<zge::Node> node);

    // Callbacks of forEachWorldMesh (1: go on, 0: stop).
    uint32_t addMeshLights(ee::Ptr<zge::Node> node);
    uint32_t countWorldMesh();
    uint32_t resetNodeVertexColours(ee::Ptr<zge::Node> node);
    uint32_t relightMesh(ee::Ptr<zge::Node> node);

    // The script function "editLightReset": every mesh's vertices white.
    uint32_t editLightReset();
    // Every mesh of the world lit from the park's lights.
    uint32_t relightWorld();

    // A second entry that only goes on to relightWorld.
    uint32_t relightWorldEntry();
    // The script function "EditLightProcessPark": the relight started pass
    // by pass: the owned lights moved to their nodes, the brightness read,
    // the progress bar shown and the world's meshes counted.
    void editLightProcessPark();
    // A pass of that relight: the next mesh lit; with every mesh lit the
    // progress bar and the light menu close and the relight is over.
    uint32_t continueRelight();
    // Every light that sits on a node's face moved to where its vertex is
    // now.
    void moveOwnedLights();

    // The brightness from and to the light menu's bar.
    void readBrightness();
    void showBrightness();

    ee::Ptr<ParkLightKind> readLightKind();
    int32_t readConeLightKind();
    int32_t readOmniLightKind();
    void readLightKinds();
}
