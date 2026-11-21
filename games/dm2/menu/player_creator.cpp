// Create Player (player_creator.h): DM2's own code, cut from the game. Its
// three script functions are registered by bmxPlayerCreatorInitialize, which
// nothing in any release calls ([extras] create_player starts it).

#include "player_creator.h"
#include "../game/players.h"

#include "ee/abi.h"
#include "ee/field.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/render/material.h"
#include "zge/render/part.h"
#include "zge/render/terrain.h"
#include "zge/render/texture.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/text/token_reader.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"
#include "zge/zgui/widget_parts.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/menu/player_creator.cpp.h"

namespace game::dm2
{
    namespace
    {
        // zgui's root node.
        inline ee::Global<ee::Ptr<zge::zgui::Node>, sym::g_uiRoot> g_uiRoot;

        // CREATEPL.DAT's words.
        constexpr uint32_t kCreatorDataFile = sym::creatorDataFile;                     // "ASSETS\CREATEPL.DAT"
        constexpr uint32_t kOutfitsWord = sym::g_textOutfits;                           // "OUTFITS"
        constexpr uint32_t kBeginWord = sym::g_textBegin;                               // "BEGIN"
        constexpr uint32_t kEndWord = sym::g_textEnd;                                   // "END"
        constexpr uint32_t kNumberOfOutfitTypesWord = sym::g_textNumberOfOutfitTypes;   // "NUMBER_OF_OUTFIT_TYPES"
        constexpr uint32_t kNumberOfTextureTonesWord = sym::g_textNumberOfTextureTones; // "NUMBER_OF_TEXTURE_TONES"
        constexpr uint32_t kNumberOfSectionsWord = sym::g_textNumberOfGeometrySections; // "NUMBER_OF_GEOMETRY_SECTIONS"
        constexpr uint32_t kNumberOfOptionsWord = sym::g_textNumberOfGeometryOptions;   // "NUMBER_OF_GEOMETRY_OPTIONS"
        constexpr uint32_t kNumberOfSubPartsWord = sym::g_textNumberOfSubParts;         // "NUMBER_OF_SUB_PARTS"
        constexpr uint32_t kTextureNameWord = sym::g_textTextureName;                   // "TEXTURE_NAME"
        constexpr uint32_t kSubPartIdWord = sym::g_textSubPartId;                       // "SUB_PART_ID"
        constexpr uint32_t kNumberOfChoicesWord = sym::g_textNumberOfChoices;           // "NUMBER_OF_CHOICES"
        constexpr uint32_t kGroupIdWord = sym::g_textGroupId;                           // "GROUP_ID"
        constexpr uint32_t kFileNamesWord = sym::g_textFileNames;                       // "FILE_NAMES"
        constexpr uint32_t kTextureToneWord = sym::g_textTextureTone;                   // "TEXTURE_TONE"
        // A token's room as the reader reads them.
        constexpr uint32_t kTokenBytes = 0x400;

        // The screen and its variables.
        constexpr uint32_t kScreenName = sym::g_textPlayerCreator;            // "PlayerCreator"
        constexpr uint32_t kPlayerModelPath = sym::creatorPlayerModelPath;    // "PlayerCreator.PlayerModel"
        constexpr uint32_t kActiveMenuName = sym::g_textActiveMenu;           // "ActiveMenu"
        constexpr ee::Ptr<const char> kGeometryMenu{sym::g_textGeometry};     // "Geometry"
        constexpr ee::Ptr<const char> kTextureMenu{sym::g_textTexture};       // "Texture"
        constexpr uint32_t kOptionFormat = sym::creatorOptionFormat;          // "%sOption%1d"
        constexpr uint32_t kMaxOptionFormat = sym::creatorMaxOptionFormat;    // "MaxOption%1d"
        constexpr uint32_t kOptionCountName = sym::g_textNumberOfOptions;     // "NumberOfOptions"
        constexpr uint32_t kSlotName = sym::g_textSlot;                       // "Slot"
        constexpr uint32_t kOutfitTypeName = sym::g_textOutfitType;           // "OutfitType"
        constexpr uint32_t kVariableNameBytes = 0x40;
        // The MaxOption variables the screen has: MaxOption0 .. MaxOption12.
        constexpr uint32_t kMaxOptionVariables = 13;
        // The texture menu's options: one per sub-part id.
        constexpr int32_t kTextureMenuOptions = kCreatorSubPartIds;

        // The libraries: "UILIB:T%1d%1d%1d%1d.ZIF" (outfit, tone, section,
        // option) whose material assets are "T....<id:02><choice:02>", and
        // "UILIB:G<outfit><head><body><legs>.ZIF" with the model asset
        // "G....G....".
        constexpr uint32_t kChoicePathFormat = sym::creatorChoicePathFormat;         // "UILIB:T%1d%1d%1d%1d.ZIF"
        constexpr uint32_t kChoiceLibraryFormat = sym::creatorChoiceLibraryFormat;   // "T%1d%1d%1d%1d"
        constexpr uint32_t kChoiceAssetFormat = sym::creatorChoiceAssetFormat;       // "%s.T%1d%1d%1d%1d%02d%02d"
        constexpr uint32_t kGeometryLibraryFormat = sym::creatorGeometryLibraryFormat; // "G%1d"
        constexpr uint32_t kUilibDevice = sym::g_textUilibDevice;                    // "UILIB:"
        constexpr uint32_t kZifExtension = sym::g_textZifExtension;                  // ".ZIF"
        constexpr uint32_t kAssetSeparator = sym::g_textDot;                         // "."
        // findGuiAsset's kind of a model (gui_model.cpp's guiModel asks for it too).
        constexpr uint32_t kModelAsset = 4;

        // The three script functions the creator's start registers.
        constexpr uint32_t kInitializeMenuName = sym::g_textInitializePlayerCreatorMenu;
        constexpr uint32_t kHarvestMenuName = sym::g_textHarvestPlayerCreatorMenu;
        constexpr uint32_t kApplyTexturesName = sym::g_textApplyPlayerCreatorTexturesToModel;

        // 16 bytes the creator's start clears with the outfits; nothing in
        // the game reads them.
        constexpr uint32_t kCreatorClearedWords = sym::g_creatorClearedWords;
        constexpr uint32_t kCreatorClearedBytes = 0x10;

        ee::Ptr<zge::zgui::Node> creatorScreen()
        {
            return zge::zgui::findNode(*g_uiRoot, kScreenName, 0);
        }

        // A zgui widget as the scene object it is (a zgui node is one in ZGE2).
        ee::Ptr<zge::SceneObject> widgetObject(ee::Ptr<zge::zgui::Node> widget)
        {
            return widget.cast<zge::SceneObject>();
        }

        // The characters' records by number, an outfit type and a section's
        // option by index: as guest addresses, so an index the script set
        // reads where the original's arithmetic does.
        ee::Ptr<RiderCharacter> riderCharacter(uint32_t number)
        {
            return ee::Ptr<RiderCharacter>(sym::g_riderCharacters) + static_cast<ptrdiff_t>(number);
        }
        ee::Ptr<CreatorOutfit> creatorOutfit(uint32_t outfit)
        {
            return ee::Ptr<CreatorOutfit>(kCreatorOutfits->outfits) + static_cast<ptrdiff_t>(outfit);
        }
        ee::Ptr<CreatorGeometryOption> geometryOption(ee::Ptr<CreatorOutfit> type, uint32_t section, uint32_t option)
        {
            const ee::Ptr<CreatorSection> sections(type->sections);
            return ee::Ptr<CreatorGeometryOption>(sections[section].options) + static_cast<ptrdiff_t>(option);
        }
        // A byte of a character's look, the looks' bytes taken as one run.
        ee::Ptr<uint8_t> lookByte(uint32_t character, uint32_t outfit, uint32_t byte)
        {
            return ee::Ptr<uint8_t>(riderCharacter(character)->creatorLooks[0]) +
                   static_cast<ptrdiff_t>(outfit * kCreatorLookBytes + byte);
        }

        // Scaffolding (address-call): nameHash of setModelTexture's stale
        // word, any 32 bits, handed on as the original hands it (zge's
        // nameHash takes a host pointer, which keeps only an address in RAM).
        namespace calls
        {
            uint32_t nameHash(ee::Ptr<const char> name) { return ee::game<uint32_t>(sym::nameHash, name); }
        }

        // A library name's room in dressCreatedModel's list of them.
        struct LibraryName
        {
            char text[kCreatorNameBytes];
        };
    }

    // ---- CREATEPL.DAT ----------------------------------------------------------

    // readCreatorOutfits: NUMBER_OF_OUTFIT_TYPES (at most 2), then each
    // outfit type in BEGIN .. END. 1, or 0 at the first word out of place.
    uint32_t readCreatorOutfits(ee::Ptr<CreatorOutfits> outfits)
    {
        ee::Local<char[kTokenBytes]> token;
        ee::Local<int32_t> number;
        if (!zge::nextToken(token, kTokenBytes) || zge::stringCompareNoCase(token, kNumberOfOutfitTypesWord) != 0)
            return 0;
        if (!zge::readNumberToken(number))
            return 0;
        outfits->count = static_cast<uint32_t>(*number);
        if (outfits->count > kCreatorOutfitTypes)
            return 0;
        for (uint32_t outfit = 0; outfit < outfits->count; ++outfit)
        {
            if (!zge::expectToken(kBeginWord) || readCreatorOutfit(&outfits->outfits[outfit]) != 1 ||
                !zge::expectToken(kEndWord))
                return 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(readCreatorOutfits, Verified);

    // readCreatorOutfit: NUMBER_OF_TEXTURE_TONES (at most 3),
    // NUMBER_OF_GEOMETRY_SECTIONS (at most 3), each section in BEGIN .. END,
    // then FILE_NAMES BEGIN with a name for every combination of the
    // sections' options (passed over) END.
    uint32_t readCreatorOutfit(ee::Ptr<CreatorOutfit> outfit)
    {
        ee::Local<char[kTokenBytes]> token;
        ee::Local<int32_t> number;
        if (!zge::nextToken(token, kTokenBytes) || zge::stringCompareNoCase(token, kNumberOfTextureTonesWord) != 0)
            return 0;
        if (!zge::readNumberToken(number))
            return 0;
        outfit->toneCount = static_cast<uint32_t>(*number);
        if (outfit->toneCount > kCreatorTonesMost)
            return 0;
        if (!zge::nextToken(token, kTokenBytes) || zge::stringCompareNoCase(token, kNumberOfSectionsWord) != 0)
            return 0;
        if (!zge::readNumberToken(number))
            return 0;
        outfit->sectionCount = static_cast<uint32_t>(*number);
        if (outfit->sectionCount > kCreatorSections)
            return 0;
        uint32_t combinations = 1;
        for (uint32_t section = 0; section < outfit->sectionCount; ++section)
        {
            if (!zge::expectToken(kBeginWord) ||
                readCreatorSection(&outfit->sections[section], outfit->toneCount) != 1 || !zge::expectToken(kEndWord))
                return 0;
            combinations *= outfit->sections[section].optionCount;
        }
        if (!zge::expectToken(kFileNamesWord) || !zge::expectToken(kBeginWord))
            return 0;
        for (uint32_t name = 0; name < combinations; ++name)
        {
            if (!zge::nextToken(token, kTokenBytes))
                return 0;
        }
        return zge::expectToken(kEndWord) != 0;
    }
    READABLE_FUNCTION_WAITS(readCreatorOutfit, Verified);

    // readCreatorSection: NUMBER_OF_GEOMETRY_OPTIONS (at most 3), each option
    // in BEGIN .. END.
    uint32_t readCreatorSection(ee::Ptr<CreatorSection> section, uint32_t tones)
    {
        ee::Local<char[kTokenBytes]> token;
        ee::Local<int32_t> number;
        if (!zge::nextToken(token, kTokenBytes) || zge::stringCompareNoCase(token, kNumberOfOptionsWord) != 0)
            return 0;
        if (!zge::readNumberToken(number))
            return 0;
        section->optionCount = static_cast<uint32_t>(*number);
        if (section->optionCount > kCreatorOptions)
            return 0;
        for (uint32_t option = 0; option < section->optionCount; ++option)
        {
            if (!zge::expectToken(kBeginWord) || readCreatorGeometryOption(&section->options[option], tones) != 1 ||
                !zge::expectToken(kEndWord))
                return 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(readCreatorSection, Verified);

    // readCreatorGeometryOption: NUMBER_OF_SUB_PARTS (at most 4), each
    // sub-part in BEGIN .. END.
    uint32_t readCreatorGeometryOption(ee::Ptr<CreatorGeometryOption> option, uint32_t tones)
    {
        ee::Local<char[kTokenBytes]> token;
        ee::Local<int32_t> number;
        if (!zge::nextToken(token, kTokenBytes) || zge::stringCompareNoCase(token, kNumberOfSubPartsWord) != 0)
            return 0;
        if (!zge::readNumberToken(number))
            return 0;
        option->subPartCount = static_cast<uint32_t>(*number);
        if (option->subPartCount > kCreatorSubParts)
            return 0;
        for (uint32_t subPart = 0; subPart < option->subPartCount; ++subPart)
        {
            if (!zge::expectToken(kBeginWord) || readCreatorSubPart(&option->subParts[subPart], tones) != 1 ||
                !zge::expectToken(kEndWord))
                return 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(readCreatorGeometryOption, Verified);

    // readCreatorSubPart: TEXTURE_NAME, SUB_PART_ID (0 .. 8),
    // NUMBER_OF_CHOICES (at most 64), GROUP_ID BEGIN with each choice's group
    // END, then FILE_NAMES BEGIN with, for each texture tone, TEXTURE_TONE
    // BEGIN and a file name per choice (passed over) END, END.
    uint32_t readCreatorSubPart(ee::Ptr<CreatorSubPart> subPart, uint32_t tones)
    {
        ee::Local<char[kTokenBytes]> token;
        ee::Local<int32_t> number;
        if (!zge::nextToken(token, kTokenBytes) || zge::stringCompareNoCase(token, kTextureNameWord) != 0)
            return 0;
        if (!zge::nextToken(subPart->textureName, kCreatorNameBytes))
            return 0;
        if (!zge::nextToken(token, kTokenBytes) || zge::stringCompareNoCase(token, kSubPartIdWord) != 0)
            return 0;
        if (!zge::readNumberToken(number))
            return 0;
        const uint8_t id = static_cast<uint8_t>(*number);
        subPart->lookByte = static_cast<uint8_t>(id + kLookFirstChoice);
        if (id >= kCreatorSubPartIds)
            return 0;
        if (!zge::nextToken(token, kTokenBytes) || zge::stringCompareNoCase(token, kNumberOfChoicesWord) != 0)
            return 0;
        if (!zge::readNumberToken(number))
            return 0;
        subPart->choiceCount = static_cast<uint8_t>(*number);
        if (subPart->choiceCount > kCreatorChoices)
            return 0;
        if (!zge::nextToken(token, kTokenBytes) || zge::stringCompareNoCase(token, kGroupIdWord) != 0)
            return 0;
        if (!zge::expectToken(kBeginWord))
            return 0;
        for (uint32_t choice = 0; choice < subPart->choiceCount; ++choice)
        {
            if (!zge::readNumberToken(number))
                return 0;
            subPart->choiceGroups[choice] = static_cast<uint8_t>(*number);
        }
        if (!zge::expectToken(kEndWord) || !zge::expectToken(kFileNamesWord) || !zge::expectToken(kBeginWord))
            return 0;
        for (uint32_t tone = 0; tone < tones; ++tone)
        {
            if (!zge::expectToken(kTextureToneWord) || !zge::expectToken(kBeginWord))
                return 0;
            for (uint32_t choice = 0; choice < subPart->choiceCount; ++choice)
            {
                if (!zge::nextToken(token, kTokenBytes))
                    return 0;
            }
            if (!zge::expectToken(kEndWord))
                return 0;
        }
        return zge::expectToken(kEndWord) != 0;
    }
    READABLE_FUNCTION_WAITS(readCreatorSubPart, Verified);

    // ---- the screen ------------------------------------------------------------

    void initializePlayerCreatorMenu()
    {
        ee::Local<char[0x100]> activeMenu;
        zge::zgui::copyVariable(creatorScreen(), kActiveMenuName, activeMenu);
        if (zge::compareStrings(activeMenu, kGeometryMenu) == 0)
            initializeGeometryMenu();
        else if (zge::compareStrings(activeMenu, kTextureMenu) == 0)
            initializeTextureMenu();
    }
    READABLE_FUNCTION(initializePlayerCreatorMenu, Verified);

    void harvestPlayerCreatorMenu()
    {
        ee::Local<char[0x100]> activeMenu;
        zge::zgui::copyVariable(creatorScreen(), kActiveMenuName, activeMenu);
        if (zge::compareStrings(activeMenu, kGeometryMenu) == 0)
            harvestGeometryMenu();
        else if (zge::compareStrings(activeMenu, kTextureMenu) == 0)
            harvestTextureMenu();
    }
    READABLE_FUNCTION(harvestPlayerCreatorMenu, Verified);

    // initializeGeometryMenu: NumberOfOptions the outfit type's sections;
    // each section's GeometryOption<n> the look's option and MaxOption<n> its
    // option count.
    void initializeGeometryMenu()
    {
        const uint32_t character = creatorSlot();
        clearCreatorMaxOptions();
        const uint32_t outfit = creatorOutfitType();
        const ee::Ptr<CreatorOutfit> type = creatorOutfit(outfit);
        const uint32_t sections = type->sectionCount;
        setCreatorOptionCount(static_cast<int32_t>(sections));
        for (uint32_t section = 0; section < sections; ++section)
        {
            const uint8_t option = creatorLook(character, outfit, kLookFirstGeometry + section);
            setCreatorMenuOption(kGeometryMenu, section, option);
            setCreatorMaxOption(section, static_cast<int32_t>(type->sections[section].optionCount));
        }
    }
    READABLE_FUNCTION(initializeGeometryMenu, Verified);

    // harvestGeometryMenu: each section's option from GeometryOption<n>; once
    // one changed, every sub-part's choice goes back to the first (the new
    // option's sub-parts are others).
    void harvestGeometryMenu()
    {
        bool changed = false;
        const uint32_t character = creatorSlot();
        const uint32_t outfit = creatorOutfitType();
        const uint32_t sections = creatorOutfit(outfit)->sectionCount;
        for (uint32_t section = 0; section < sections; ++section)
        {
            const uint8_t option = static_cast<uint8_t>(creatorMenuOption(kGeometryMenu, section));
            if (creatorLook(character, outfit, kLookFirstGeometry + section) != option)
                changed = true;
            setCreatorLook(character, outfit, kLookFirstGeometry + section, option);
        }
        if (changed)
            game::tail(clearCreatorTextureChoices, character);
    }
    READABLE_FUNCTION(harvestGeometryMenu, Verified);

    // initializeTextureMenu: NumberOfOptions 9 (the sub-part ids); for each
    // sub-part of each section's chosen option, TextureOption<id> its choice
    // and MaxOption<id> how many of its choices are open; each section's
    // GeometryOption<n> too.
    void initializeTextureMenu()
    {
        const uint32_t character = creatorSlot();
        clearCreatorMaxOptions();
        const uint32_t outfit = creatorOutfitType();
        setCreatorOptionCount(kTextureMenuOptions);
        const ee::Ptr<CreatorOutfit> type = creatorOutfit(outfit);
        for (uint32_t section = 0; section < type->sectionCount; ++section)
        {
            const uint8_t chosen = creatorLook(character, outfit, kLookFirstGeometry + section);
            const ee::Ptr<CreatorGeometryOption> option = geometryOption(type, section, chosen);
            const uint32_t subParts = option->subPartCount;
            for (uint32_t index = 0; index < subParts; ++index)
            {
                const ee::Ptr<CreatorSubPart> subPart(&option->subParts[index]);
                const uint32_t id = subPart->lookByte - kLookFirstChoice;
                const uint8_t choice = creatorLook(character, outfit, subPart->lookByte);
                setCreatorMenuOption(kTextureMenu, id, choice);
                int32_t open = 0;
                for (uint32_t other = 0; other < subPart->choiceCount; ++other)
                {
                    if (creatorChoiceOpen(other, subPart, character))
                        ++open;
                }
                setCreatorMaxOption(id, open);
            }
            const uint8_t shown = creatorLook(character, outfit, kLookFirstGeometry + section);
            setCreatorMenuOption(kGeometryMenu, section, shown);
        }
    }
    READABLE_FUNCTION(initializeTextureMenu, Verified);

    // harvestTextureMenu: each sub-part's choice is the TextureOption<id>-th
    // of its open choices.
    void harvestTextureMenu()
    {
        const uint32_t character = creatorSlot();
        const uint32_t outfit = creatorOutfitType();
        const ee::Ptr<CreatorOutfit> type = creatorOutfit(outfit);
        for (uint32_t section = 0; section < type->sectionCount; ++section)
        {
            const uint8_t chosen = creatorLook(character, outfit, kLookFirstGeometry + section);
            const ee::Ptr<CreatorGeometryOption> option = geometryOption(type, section, chosen);
            const uint32_t subParts = option->subPartCount;
            for (uint32_t index = 0; index < subParts; ++index)
            {
                const ee::Ptr<CreatorSubPart> subPart(&option->subParts[index]);
                const uint8_t byte = subPart->lookByte;
                const int32_t shown = creatorMenuOption(kTextureMenu, byte - kLookFirstChoice);
                const uint32_t choice = openCreatorChoice(static_cast<uint32_t>(shown), subPart, character);
                setCreatorLook(character, outfit, byte, static_cast<uint8_t>(choice));
            }
        }
    }
    READABLE_FUNCTION(harvestTextureMenu, Verified);

    void clearCreatorTextureChoices(uint32_t character)
    {
        for (uint32_t byte = kLookFirstChoice; byte < kCreatorLookBytes; ++byte)
        {
            for (uint32_t outfit = 0; outfit < kCreatorOutfitTypes; ++outfit)
                setCreatorLook(character, outfit, byte, 0);
        }
    }
    READABLE_FUNCTION(clearCreatorTextureChoices, Verified);

    void clearCreatorMaxOptions()
    {
        for (uint32_t number = 0; number < kMaxOptionVariables; ++number)
            setCreatorMaxOption(number, 0);
    }
    READABLE_FUNCTION(clearCreatorMaxOptions, Verified);

    int32_t creatorMenuOption(ee::Ptr<const char> kind, uint32_t number)
    {
        ee::Local<char[kVariableNameBytes]> name;
        ee::Local<int32_t> value;
        zge::formatString(name, kOptionFormat, kind, number);
        zge::zgui::copyVariable(creatorScreen(), name, value);
        return *value;
    }
    READABLE_FUNCTION(creatorMenuOption, Verified);

    void setCreatorMenuOption(ee::Ptr<const char> kind, uint32_t number, int32_t value)
    {
        ee::Local<char[kVariableNameBytes]> name;
        ee::Local<int32_t> held;
        *held = value;
        zge::formatString(name, kOptionFormat, kind, number);
        zge::zgui::setVariable(creatorScreen(), name, held);
    }
    READABLE_FUNCTION(setCreatorMenuOption, Verified);

    void setCreatorMaxOption(uint32_t number, int32_t value)
    {
        ee::Local<char[kVariableNameBytes]> name;
        ee::Local<int32_t> held;
        *held = value;
        zge::formatString(name, kMaxOptionFormat, number);
        zge::zgui::setVariable(creatorScreen(), name, held);
    }
    READABLE_FUNCTION(setCreatorMaxOption, Verified);

    void setCreatorOptionCount(int32_t count)
    {
        ee::Local<int32_t> held;
        *held = count;
        zge::zgui::setVariable(creatorScreen(), kOptionCountName, held);
    }
    READABLE_FUNCTION(setCreatorOptionCount, Verified);

    // creatorSlot, creatorOutfitType: the screen's Slot and OutfitType (what
    // the stack held, should the screen have no such variable).
    uint32_t creatorSlot()
    {
        ee::Local<uint32_t> value;
        zge::zgui::copyVariable(creatorScreen(), kSlotName, value);
        return *value;
    }
    READABLE_FUNCTION(creatorSlot, Verified);

    uint32_t creatorOutfitType()
    {
        ee::Local<uint32_t> value;
        zge::zgui::copyVariable(creatorScreen(), kOutfitTypeName, value);
        return *value;
    }
    READABLE_FUNCTION(creatorOutfitType, Verified);

    uint8_t creatorLook(uint32_t character, uint32_t outfit, uint32_t byte)
    {
        return *lookByte(character, outfit, byte);
    }
    READABLE_FUNCTION(creatorLook, Verified);

    void setCreatorLook(uint32_t character, uint32_t outfit, uint32_t byte, uint8_t value)
    {
        *lookByte(character, outfit, byte) = value;
    }
    READABLE_FUNCTION(setCreatorLook, Verified);

    // creatorChoiceOpen: the choice's group is a sponsor number; a group past
    // the sponsors (8) reads the character's later fields.
    uint32_t creatorChoiceOpen(uint32_t choice, ee::Ptr<const CreatorSubPart> subPart, uint32_t character)
    {
        const uint8_t group = subPart->choiceGroups[choice];
        const ee::Ptr<uint32_t> sponsors(riderCharacter(character)->sponsorShown);
        return sponsors[group] != 0;
    }
    READABLE_FUNCTION(creatorChoiceOpen, Verified);

    uint32_t openCreatorChoice(uint32_t open, ee::Ptr<const CreatorSubPart> subPart, uint32_t character)
    {
        const uint32_t choices = subPart->choiceCount;
        uint32_t seen = 0;
        for (uint32_t choice = 0; choice < choices; ++choice)
        {
            if (creatorChoiceOpen(choice, subPart, character) && seen++ == open)
                return choice;
        }
        return 0;
    }
    READABLE_FUNCTION(openCreatorChoice, Verified);

    // ---- the model -------------------------------------------------------------

    // dressCreatedModel: every choice library it loads is let go at the end
    // (the textures stay with the scene's materials).
    void dressCreatedModel(ee::Ptr<zge::Scene> scene, uint32_t character, uint32_t outfit)
    {
        ee::Local<LibraryName[kCreatorSections * kCreatorSubParts]> libraries;
        const ee::Ptr<CreatorOutfit> type = creatorOutfit(outfit);
        uint32_t loaded = 0;
        for (uint32_t section = 0; section < type->sectionCount; ++section)
        {
            const uint8_t chosen = creatorLook(character, outfit, kLookFirstGeometry + section);
            const ee::Ptr<CreatorGeometryOption> option = geometryOption(type, section, chosen);
            for (uint32_t index = 0; index < option->subPartCount; ++index)
            {
                const ee::Ptr<CreatorSubPart> subPart(&option->subParts[index]);
                const uint8_t byte = subPart->lookByte;
                const uint8_t choice = creatorLook(character, outfit, byte);
                const uint8_t tone = creatorLook(character, outfit, kLookTone);
                const ee::Ptr<zge::Material> material =
                    creatorChoiceTexture(outfit, tone, section, chosen, byte - kLookFirstChoice, choice,
                                         ee::Ptr<char>(libraries[loaded].text));
                ++loaded;
                setModelTexture(scene, subPart, material);
            }
        }
        for (uint32_t library = 0; library < loaded; ++library)
            zge::zgui::unloadUiLibraryByName(ee::Address(libraries[library].text));
    }
    READABLE_FUNCTION_WAITS(dressCreatedModel, Verified);

    ee::Ptr<zge::Material> creatorChoiceTexture(uint32_t outfit, uint32_t tone, uint32_t section, uint32_t option,
                                                uint32_t subPartId, uint32_t choice, ee::Ptr<char> libraryName)
    {
        ee::Local<char[kCreatorNameBytes]> path;
        ee::Local<char[kCreatorNameBytes]> library;
        ee::Local<char[kCreatorNameBytes]> asset;
        zge::formatString(path, kChoicePathFormat, outfit, tone, section, option);
        zge::formatString(library, kChoiceLibraryFormat, outfit, tone, section, option);
        zge::formatStringWithStackWords(asset, kChoiceAssetFormat, library, outfit, tone, section, option, subPartId, choice);
        if (libraryName)
            zge::stringCopy(libraryName, library);
        zge::zgui::loadUiLibraryByName(ee::Address(library), ee::Address(path));
        const ee::Ptr<zge::zgui::UiAsset> found = zge::zgui::findGuiAsset(asset, zge::zgui::kAssetTexture);
        return found ? ee::Ptr<zge::Material>(found->object) : ee::Ptr<zge::Material>();
    }
    READABLE_FUNCTION_WAITS(creatorChoiceTexture, Verified);

    // creatorGeometryScene: kept, the path goes out where the library's name
    // should: newCreatedRiderModel lets go of a library by that path, which
    // names none, so the geometry's library stays loaded.
    ee::Ptr<zge::Scene> creatorGeometryScene(uint32_t outfit, ee::Ptr<const uint32_t> options, ee::Ptr<char> libraryPath)
    {
        ee::Local<char[kCreatorNameBytes]> path;
        ee::Local<char[kCreatorNameBytes]> library;
        ee::Local<char[kCreatorNameBytes]> asset;
        ee::Local<char[2]> digit;
        zge::formatString(library, kGeometryLibraryFormat, outfit);
        for (uint32_t section = 0; section < kCreatorSections; ++section)
        {
            digit[1] = 0;
            digit[0] = static_cast<char>('0' + static_cast<uint8_t>(options[section]));
            zge::appendString(library, digit);
        }
        zge::stringCopy(path, kUilibDevice);
        zge::appendString(path, library);
        zge::appendString(path, kZifExtension);
        zge::stringCopy(asset, library);
        zge::appendString(asset, kAssetSeparator);
        zge::appendString(asset, library);
        zge::stringCopy(libraryPath, path);
        zge::zgui::loadUiLibraryByName(ee::Address(library), ee::Address(path));
        const ee::Ptr<zge::zgui::UiAsset> found = zge::zgui::findGuiAsset(asset, kModelAsset);
        return found ? ee::Ptr<zge::Scene>(found->object) : ee::Ptr<zge::Scene>();
    }
    READABLE_FUNCTION_WAITS(creatorGeometryScene, Verified);

    void setModelTexture(ee::Ptr<zge::Scene> scene, ee::Ptr<CreatorSubPart> subPart, ee::Ptr<zge::Material> choice)
    {
        ee::Local<MeshTexture> texture;
        const ee::Ptr<zge::Node> root = zge::sceneRootNode(scene);
        // Kept: the name is hashed before it is stored, so the hash is of the
        // name this frame's record still holds from the call before (the
        // sub-part dressed before this one): each texture goes on the
        // previous sub-part's materials, and the first call's on none.
        const ee::Ptr<const char> staleName = texture->name;
        texture->choice = choice;
        texture->nameHash = calls::nameHash(staleName);
        texture->name = ee::field(subPart, &CreatorSubPart::textureName);
        texture->applied = 0;
        zge::forEachObjectMesh(root, zge::MeshVisit(sym::setMeshTexture), texture.address());
        zge::requestTextureCacheFlush();
    }
    READABLE_FUNCTION_WAITS(setModelTexture, Verified);

    // setMeshTexture: each material of the mesh whose texture's id is the
    // name hash takes the choice's texture, which is renamed after the
    // sub-part (its id the hash). 1: the walk goes on.
    int32_t setMeshTexture(ee::Ptr<zge::ZgePart> mesh, ee::Ptr<MeshTexture> texture)
    {
        const uint32_t nameHash = texture->nameHash;
        const uint32_t picture = zge::pictureLevel(texture->choice, 0);
        const uint32_t sets = static_cast<uint32_t>(zge::materialSetCount(mesh));
        for (uint32_t set = 0; set < sets; ++set)
        {
            const ee::Ptr<zge::MaterialTable> table = zge::meshMaterials(mesh, set);
            const uint32_t count = zge::materialCount(table);
            for (uint32_t index = 0; index < count; ++index)
            {
                const ee::Ptr<zge::Material> material = zge::materialAt(table, index);
                if (!material || zge::materialNameHash(material) != nameHash)
                    continue;
                zge::setMaterialTexture(material, picture, 0, 0);
                texture->applied = 1;
                // (the texture through its own address: a loaded texture's is
                // the uncached mirror)
                const ee::Ptr<zge::Texture> renamed(picture);
                zge::stringCopy(ee::field(renamed, &zge::Texture::name), texture->name);
                renamed->id = nameHash;
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(setMeshTexture, Verified);

    void applyPlayerCreatorTexturesToModel()
    {
        const ee::Ptr<zge::zgui::Node> model = zge::zgui::findNode(*g_uiRoot, kPlayerModelPath, 0);
        const ee::Ptr<zge::SceneObject> object = zge::firstAttachedChild(widgetObject(model));
        harvestTextureMenu();
        const ee::Ptr<zge::Scene> scene = zge::sceneOf(object);
        const uint32_t character = creatorSlot();
        const uint32_t outfit = creatorOutfitType();
        game::tail(dressCreatedModel, scene, character, outfit);
    }
    READABLE_FUNCTION_WAITS(applyPlayerCreatorTexturesToModel, Verified);

    // bmxPlayerCreatorInitialize: kept, a file that does not read leaves the
    // token reader on it (endScript only after a whole file).
    uint32_t bmxPlayerCreatorInitialize()
    {
        ee::Local<char[kTokenBytes]> token;
        zge::memoryFill(kCreatorOutfits, 0, sizeof(CreatorOutfits));
        zge::memoryFill(kCreatorClearedWords, 0, kCreatorClearedBytes);
        zge::beginScript(kCreatorDataFile);
        zge::setTokenCapitalize(0);
        zge::setTokenHashComments(0);
        if (!zge::nextToken(token, kTokenBytes) || zge::stringCompareNoCase(token, kOutfitsWord) != 0)
            return 0;
        if (!zge::expectToken(kBeginWord) || readCreatorOutfits(kCreatorOutfits) != 1 || !zge::expectToken(kEndWord))
            return 0;
        if (zge::nextToken(token, kTokenBytes))
            return 0;
        zge::endScript();
        zge::zgui::registerScriptFunction(kInitializeMenuName, sym::initializePlayerCreatorMenu);
        zge::zgui::registerScriptFunction(kHarvestMenuName, sym::harvestPlayerCreatorMenu);
        zge::zgui::registerScriptFunction(kApplyTexturesName, sym::applyPlayerCreatorTexturesToModel);
        return 1;
    }
    READABLE_FUNCTION_WAITS(bmxPlayerCreatorInitialize, Verified);

    ee::Ptr<zge::Scene> newCreatedRiderModel(uint32_t created, uint32_t outfit)
    {
        ee::Local<uint32_t[kCreatorSections]> options;
        ee::Local<char[kCreatorNameBytes]> libraryPath;
        const uint32_t character = created + kFirstCreatedCharacter;
        zge::memoryFill(options, 0, sizeof(uint32_t) * kCreatorSections);
        const ee::Ptr<CreatorOutfit> type = creatorOutfit(outfit);
        for (uint32_t section = 0; section < type->sectionCount; ++section)
            options[section] = creatorLook(character, outfit, kLookFirstGeometry + section);
        const ee::Ptr<zge::Scene> geometry = creatorGeometryScene(outfit, options, libraryPath);
        const ee::Ptr<zge::Scene> scene = zge::cloneScene(geometry);
        zge::zgui::unloadUiLibraryByName(ee::Address(libraryPath));
        dressCreatedModel(scene, character, outfit);
        return scene;
    }
    READABLE_FUNCTION_WAITS(newCreatedRiderModel, Verified);
}
