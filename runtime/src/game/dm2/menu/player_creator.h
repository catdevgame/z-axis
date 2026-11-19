#pragma once

// Create Player (player_creator.cpp): the front end's rider creation, cut
// from every release of the game. Its code is the game's own, but nothing
// calls it and no release has its screen ("PlayerCreator"), its data file or
// its libraries; [extras] create_player (game_overrides/dave_mirra_creator.cpp)
// makes all three and starts it.
//
// ASSETS\CREATEPL.DAT names, for each outfit type, the rider's geometry
// sections (head, body, legs), each section's geometry options, and each
// option's sub-parts: a material of the model whose texture the rider picks
// from a list of choices. A created character (17 .. 20) keeps its picks in
// its look (RiderCharacter::creatorLooks); the screen's variables show them
// and take them back, and newCreatedRiderModel makes the character's model
// from them.

#include "../game/players.h"

#include "ee/ee.h"
#include "ee/plain.h"
#include "zge/render/part.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/menu/player_creator.h.h"

namespace zge
{
    struct Material;
}

namespace game::dm2
{
    // The data file's limits, as its reader takes them (each count read must
    // be below the next number: NUMBER_OF_OUTFIT_TYPES under 3, ...).
    constexpr uint32_t kCreatorTonesMost = 3;      // NUMBER_OF_TEXTURE_TONES
    constexpr uint32_t kCreatorSections = 3;       // NUMBER_OF_GEOMETRY_SECTIONS
    constexpr uint32_t kCreatorOptions = 3;        // NUMBER_OF_GEOMETRY_OPTIONS
    constexpr uint32_t kCreatorSubParts = 4;       // NUMBER_OF_SUB_PARTS
    constexpr uint32_t kCreatorChoices = 0x40;     // NUMBER_OF_CHOICES
    constexpr uint32_t kCreatorSubPartIds = 9;     // SUB_PART_ID 0 .. 8
    constexpr uint32_t kCreatorNameBytes = 0x20;   // TEXTURE_NAME, a library's name

    // A sub-part: the material of the option's model it dresses (by its
    // texture's name), where its choice sits in the look, and its choices:
    // each choice is open to a character once the sponsor of its group is
    // (RiderCharacter::sponsorShown).
    struct CreatorSubPart
    {
        char textureName[kCreatorNameBytes];   // +0x00 TEXTURE_NAME
        uint8_t choiceCount;                   // +0x20 NUMBER_OF_CHOICES
        uint8_t lookByte;                      // +0x21 SUB_PART_ID + kLookFirstChoice: its choice's byte of the look
        uint8_t choiceGroups[kCreatorChoices]; // +0x22 GROUP_ID: each choice's sponsor number
    };
    static_assert(sizeof(CreatorSubPart) == 0x62 && offsetof(CreatorSubPart, choiceGroups) == 0x22);
    // A section's geometry option: a model and the sub-parts it has.
    struct CreatorGeometryOption
    {
        uint32_t subPartCount;                       // +0x00
        CreatorSubPart subParts[kCreatorSubParts];   // +0x04
    };
    static_assert(sizeof(CreatorGeometryOption) == 0x18C);
    // A geometry section (head, body, legs) and its options.
    struct CreatorSection
    {
        uint32_t optionCount;                           // +0x00
        CreatorGeometryOption options[kCreatorOptions]; // +0x04
    };
    static_assert(sizeof(CreatorSection) == 0x4A8);
    // An outfit type: its texture tones and its sections. The data file then
    // names a file for every combination of the sections' options, which the
    // reader passes over.
    struct CreatorOutfit
    {
        uint32_t toneCount;                         // +0x00
        uint32_t sectionCount;                      // +0x04
        CreatorSection sections[kCreatorSections];  // +0x08
    };
    static_assert(sizeof(CreatorOutfit) == 0xE00);
    struct CreatorOutfits
    {
        uint32_t count;                               // +0x00 NUMBER_OF_OUTFIT_TYPES (0 until the creator starts)
        CreatorOutfit outfits[kCreatorOutfitTypes];   // +0x04
    };
    static_assert(sizeof(CreatorOutfits) == 0x1C04);
    // What bmxPlayerCreatorInitialize reads (its count 0 until then).
    constexpr ee::Ptr<CreatorOutfits> kCreatorOutfits{sym::g_playerCreatorOutfits};

    // The bytes of a look (RiderCharacter::creatorLooks): the texture tone,
    // each section's geometry option, then each sub-part's choice by its id.
    enum CreatorLookByte : uint32_t
    {
        kLookTone = 0,
        kLookFirstGeometry = 1,  // + the section
        kLookFirstChoice = 4,    // + the sub-part's id
    };

    // The created characters, 17 .. 20: newCreatedRiderModel's index is the
    // character less this.
    constexpr uint32_t kFirstCreatedCharacter = kBuiltInCharacters;

    // ---- CREATEPL.DAT's reader (a section each: a count, then each item in BEGIN .. END) ----
    uint32_t readCreatorOutfits(ee::Ptr<CreatorOutfits> outfits);
    uint32_t readCreatorOutfit(ee::Ptr<CreatorOutfit> outfit);
    uint32_t readCreatorSection(ee::Ptr<CreatorSection> section, uint32_t tones);
    uint32_t readCreatorGeometryOption(ee::Ptr<CreatorGeometryOption> option, uint32_t tones);
    uint32_t readCreatorSubPart(ee::Ptr<CreatorSubPart> subPart, uint32_t tones);

    // ---- the screen's script functions and what they do ----
    // "InitializePlayerCreatorMenu": the slot's look onto the screen's
    // variables, for the menu PlayerCreator.ActiveMenu names ("Geometry" or
    // "Texture"); "HarvestPlayerCreatorMenu": the variables back into the look.
    void initializePlayerCreatorMenu();
    void harvestPlayerCreatorMenu();
    void initializeGeometryMenu();
    void harvestGeometryMenu();
    void initializeTextureMenu();
    void harvestTextureMenu();
    // Every sub-part's choice of the character's looks back to the first.
    void clearCreatorTextureChoices(uint32_t character);
    // MaxOption0 .. 12 back to 0.
    void clearCreatorMaxOptions();

    // The screen's variables: "<kind>Option<n>" (kind "Geometry" or
    // "Texture"), MaxOption<n>, NumberOfOptions, Slot (the character) and
    // OutfitType.
    int32_t creatorMenuOption(ee::Ptr<const char> kind, uint32_t number);
    void setCreatorMenuOption(ee::Ptr<const char> kind, uint32_t number, int32_t value);
    void setCreatorMaxOption(uint32_t number, int32_t value);
    void setCreatorOptionCount(int32_t count);
    uint32_t creatorSlot();
    uint32_t creatorOutfitType();

    // A byte of a character's look in an outfit type (CreatorLookByte).
    uint8_t creatorLook(uint32_t character, uint32_t outfit, uint32_t byte);
    void setCreatorLook(uint32_t character, uint32_t outfit, uint32_t byte, uint8_t value);
    // Whether the character has a sub-part's choice open (its group's
    // sponsor shown), and which choice is its `open`-th open one (0 if none).
    uint32_t creatorChoiceOpen(uint32_t choice, ee::Ptr<const CreatorSubPart> subPart, uint32_t character);
    uint32_t openCreatorChoice(uint32_t open, ee::Ptr<const CreatorSubPart> subPart, uint32_t character);

    // ---- the model ----
    // A created model's scene dressed from the character's look in the
    // outfit type: each sub-part of each section's chosen option gets its
    // choice's texture.
    void dressCreatedModel(ee::Ptr<zge::Scene> scene, uint32_t character, uint32_t outfit);
    // A choice's material: library UILIB:T<outfit><tone><section><option>.ZIF
    // loaded (its name into libraryName when given), its asset
    // T....<sub-part id><choice> found; 0 if none.
    ee::Ptr<zge::Material> creatorChoiceTexture(uint32_t outfit, uint32_t tone, uint32_t section, uint32_t option,
                                                uint32_t subPartId, uint32_t choice, ee::Ptr<char> libraryName);
    // A geometry's model: library UILIB:G<outfit><head><body><legs>.ZIF loaded
    // (its path into libraryPath), its scene asset G....G.... found; 0 if none.
    ee::Ptr<zge::Scene> creatorGeometryScene(uint32_t outfit, ee::Ptr<const uint32_t> options, ee::Ptr<char> libraryPath);
    // The texture of `choice` (a choice's material) on every material of the
    // scene's meshes that its sub-part names; the texture cache flushed.
    void setModelTexture(ee::Ptr<zge::Scene> scene, ee::Ptr<CreatorSubPart> subPart, ee::Ptr<zge::Material> choice);

    // What setModelTexture hands setMeshTexture for each mesh of the scene.
    struct MeshTexture
    {
        ee::Ptr<zge::Material> choice;   // +0x00 the choice's material: its first texture is put on
        uint32_t nameHash;               // +0x04 the materials it goes on (their texture's id)
        ee::Ptr<const char> name;        // +0x08 the sub-part's TEXTURE_NAME: the texture's name after
        uint32_t applied;                // +0x0C 1 once a material took it
    };
    static_assert(sizeof(MeshTexture) == 0x10);
    int32_t setMeshTexture(ee::Ptr<zge::ZgePart> mesh, ee::Ptr<MeshTexture> texture);

    // "ApplyPlayerCreatorTexturesToModel": the screen's model
    // (PlayerCreator.PlayerModel) dressed from the slot's look, the texture
    // menu taken back first.
    void applyPlayerCreatorTexturesToModel();
    // The creator's start: CREATEPL.DAT read (OUTFITS BEGIN ... END), then
    // the three script functions registered. 1, or 0 when the file does not
    // read.
    uint32_t bmxPlayerCreatorInitialize();
    // A created character's model (created 0 .. 3: character 17 .. 20) in an
    // outfit type: its geometry's scene cloned and dressed.
    ee::Ptr<zge::Scene> newCreatedRiderModel(uint32_t created, uint32_t outfit);
}
