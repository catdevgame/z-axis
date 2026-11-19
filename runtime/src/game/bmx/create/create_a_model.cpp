// BMX XXX's create-a-model (create_a_model.h): the module's start and end,
// and the libraries' names. BMX XXX's own code. The rest of the module (the
// models, the parts' lists, the script functions' bodies) is not written yet.

#include "create_a_model.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/container/list.h"
#include "zge/memory/pool.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/text/token_reader.h"
#include "zge/zgui/script.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/create/create_a_model.cpp.h"

namespace game::bmx
{
    namespace
    {
        // A category of parts, a line of "assets\create\category.txt": the
        // start of its parts' names ("Shoe_", "Hair_", "TattooBack_") and
        // the line's number.
        struct PartCategory
        {
            zge::ListNode link;    // +0x00
            char namePrefix[0x20]; // +0x08
            int32_t prefixLength;  // +0x28
            int32_t number;        // +0x2C the file's second column (10 .. 350, rising down the file)
        };
        static_assert(offsetof(PartCategory, prefixLength) == 0x28 && sizeof(PartCategory) == 0x30);

        // A part's texture when none is chosen, a line of
        // "assets\create\default.txt": <part name> <texture name>
        // <material name> <zal file name> (the file's own words).
        struct DefaultTexture
        {
            zge::ListNode link;    // +0x00
            uint32_t partName;     // +0x08 its hash
            char zalFile[0x40];    // +0x0C
            char textureName[0x40]; // +0x4C
            uint32_t library;      // +0x8C the material name as a library's index (createAModelLibraryIndex)
        };
        static_assert(offsetof(DefaultTexture, zalFile) == 0x0C && offsetof(DefaultTexture, textureName) == 0x4C);
        static_assert(offsetof(DefaultTexture, library) == 0x8C && sizeof(DefaultTexture) == 0x90);

        constexpr uint32_t kRecordsABlock = 0x40;
        // Scaffolding (unnamed-field): the module's first pool, of 0x5C-byte
        // records; what they are is in the code not written yet.
        constexpr uint32_t kFirstPoolRecordBytes = 0x5C;
        inline ee::Global<ee::Ptr<zge::Pool>, sym::g_createFirstPool> g_createFirstPool;
        inline ee::Global<ee::Ptr<zge::Pool>, sym::g_partCategoryPool> g_partCategoryPool;
        inline ee::Global<ee::Ptr<zge::Pool>, sym::g_defaultTexturePool> g_defaultTexturePool;
        constexpr uint32_t kPartCategories = sym::g_partCategories;   // a zge::List of PartCategory
        constexpr uint32_t kDefaultTextures = sym::g_defaultTextures; // a zge::List of DefaultTexture

        constexpr uint32_t kCategoryFile = sym::createCategoryFileName; // "assets\create\category.txt"
        constexpr uint32_t kDefaultFile = sym::createDefaultFileName;   // "assets\create\default.txt"

        // The libraries a model's assets come from, by name: "parts",
        // "lattices", "skeleton", "texture00" .. "texture15"; and the names'
        // hashes, made at the first question.
        constexpr uint32_t kLibraries = 19;
        constexpr uint32_t kLibraryNames = sym::createLibraryNames;
        inline ee::Global<uint32_t[kLibraries], sym::g_createLibraryNameHashes> g_createLibraryNameHashes;

        constexpr uint32_t kTokenRead = 1; // the token readers' answer for a token read
    }

    // createAModelLibraryIndex: which library has the name (0 .. 18), or 19
    // for a name that is none of them. A first hash of 0 means the hashes
    // are not made yet.
    uint32_t createAModelLibraryIndex(ee::Ptr<const char> name)
    {
        if ((*g_createLibraryNameHashes)[0] == 0)
        {
            ee::Local<uint32_t[kLibraries]> names;
            const ee::Ptr<uint32_t> table(kLibraryNames);
            for (uint32_t library = 0; library < kLibraries; ++library)
                names[library] = table[library];
            for (uint32_t library = 0; library < kLibraries; ++library)
                (*g_createLibraryNameHashes)[library] = zge::nameHash(ee::Ptr<const char>(names[library]).get());
        }
        const uint32_t hash = zge::nameHash(name.get());
        uint32_t library = 0;
        while (library < kLibraries && (*g_createLibraryNameHashes)[library] != hash)
            ++library;
        return library;
    }
    READABLE_FUNCTION_WAITS(createAModelLibraryIndex, Draft);

    // initCreateAModel: the module's pools and lists made, the part
    // categories and the default textures read from their files (a line that
    // does not read ends its file), and the module's script functions
    // registered.
    void initCreateAModel()
    {
        ee::Local<char[0x20]> prefix;
        ee::Local<char[0x40]> partName;
        ee::Local<char[0x40]> textureName;
        ee::Local<char[0x40]> materialName;
        ee::Local<char[0x40]> zalFile;
        ee::Local<int32_t> number;
        *g_createFirstPool = ee::Ptr<zge::Pool>(zge::createPool(kFirstPoolRecordBytes, kRecordsABlock, 0));
        *g_partCategoryPool = ee::Ptr<zge::Pool>(zge::createPool(sizeof(PartCategory), kRecordsABlock, 0));
        *g_defaultTexturePool = ee::Ptr<zge::Pool>(zge::createPool(sizeof(DefaultTexture), kRecordsABlock, 0));

        zge::initList(kPartCategories);
        zge::beginScript(kCategoryFile);
        while (zge::readStringToken(prefix, sizeof(PartCategory::namePrefix)) == kTokenRead && zge::readNumberToken(number) == kTokenRead)
        {
            const ee::Ptr<PartCategory> category(zge::poolAlloc(*g_partCategoryPool));
            category->prefixLength = zge::stringLength(prefix);
            zge::stringCopy(category->namePrefix, prefix);
            category->number = *number;
            zge::listPush(kPartCategories, category);
        }
        zge::endScript();

        zge::initList(kDefaultTextures);
        zge::beginScript(kDefaultFile);
        while (zge::readStringToken(partName, 0x40) == kTokenRead && zge::readStringToken(textureName, 0x40) == kTokenRead &&
               zge::readStringToken(materialName, 0x40) == kTokenRead && zge::readStringToken(zalFile, 0x40) == kTokenRead)
        {
            const ee::Ptr<DefaultTexture> texture(zge::poolAlloc(*g_defaultTexturePool));
            texture->partName = zge::nameHash(ee::Ptr<const char>(partName.address()).get());
            zge::stringCopy(texture->zalFile, zalFile);
            zge::stringCopy(texture->textureName, textureName);
            texture->library = createAModelLibraryIndex(ee::Ptr<const char>(materialName.address()));
            zge::listPush(kDefaultTextures, texture);
        }
        zge::endScript();

        zge::zgui::registerScriptFunction(sym::createAModelGetFilteredListOfAssetsInLibraryName, sym::createAModelGetFilteredListOfAssetsInLibrary);
        zge::zgui::registerScriptFunction(sym::createAModelReplaceTextureName, sym::createAModelReplaceTexture);
        zge::zgui::registerScriptFunction(sym::createAModelMatchSkinTxrToFaceTxrName, sym::createAModelMatchSkinTxrToFaceTxr);
        zge::zgui::registerScriptFunction(sym::createAModelSetPropertyName, sym::createAModelSetProperty);
        zge::zgui::registerScriptFunction(sym::createAModelGetPropertyName, sym::createAModelGetProperty);
    }
    READABLE_FUNCTION_WAITS(initCreateAModel, Verified);

    // shutDownCreateAModel: the pools destroyed and both lists emptied.
    void shutDownCreateAModel()
    {
        zge::destroyPool(*g_createFirstPool);
        *g_createFirstPool = ee::Ptr<zge::Pool>();
        zge::destroyPool(*g_partCategoryPool);
        *g_partCategoryPool = ee::Ptr<zge::Pool>();
        zge::destroyPool(*g_defaultTexturePool);
        *g_defaultTexturePool = ee::Ptr<zge::Pool>();
        zge::initList(kPartCategories);
        game::tail(zge::initList, ee::Address(kDefaultTextures));
    }
    READABLE_FUNCTION_WAITS(shutDownCreateAModel, Verified);
}
