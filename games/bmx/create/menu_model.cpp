// The front end's rider model (menu_model.h). BMX XXX's own code.

#include "menu_model.h"

#include "bmx/profile/players.h"
#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/container/tree.h"
#include "zge/input/pads.h"
#include "zge/math/quaternion.h"
#include "zge/math/types.h"
#include "zge/platform/platform.h"
#include "zge/scene/animator.h"
#include "zge/scene/model_records.h"
#include "zge/scene/scene.h"
#include "zge/scene/scene_database.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <bit>
#include <cstdint>
#include "sym/bmx/create/menu_model.cpp.h"

namespace game::bmx
{
    using zge::zgui::Node;

    // Scaffolding (address-call): the asset libraries' loads (r9bmxrider3's
    // names) and the original's tail calls. A model keeps its assets by
    // aspect, its parts first.
    namespace calls
    {
        inline uint32_t loadLibraryAsset(ee::Ptr<const char> library, ee::Ptr<const char> asset)
        {
            return ee::game<uint32_t>(sym::loadLibraryAsset, library, asset);
        }
        inline void releaseLibraryAsset(uint32_t asset) { ee::game(sym::releaseLibraryAsset, asset); }
        // The original's tail calls of three of them.
        inline void closeLibraryFilesTail() { ee::tailCall<void>(sym::closeLibraryFiles); }
        inline void bindModelToObjectTail(ee::Ptr<zge::SceneObject> object, ee::Ptr<zge::ModelRecord> model)
        {
            ee::tailCall<void>(sym::bindModelToObject, object, model);
        }
        inline void copyModelRecordTail(ee::Ptr<zge::ModelRecord> destination, ee::Ptr<zge::ModelRecord> source)
        {
            ee::tailCall<void>(sym::copyModelRecord, destination, source);
        }
    }

    namespace
    {
        constexpr uint32_t kFrameNode = 1; // findNode's kind: a frame
        // The model: its scene object, and whether the menus show it (the
        // model's frame keeps the object's shown flag to it).
        struct MenuModel
        {
            ee::Ptr<zge::SceneObject> object; // +0x00
            uint32_t shown;                   // +0x04
        };
        inline ee::Global<MenuModel, sym::g_menuModel> g_menuModel;
        // Its two places (x, y, z, 1).
        inline ee::Global<zge::Vec4, sym::g_menuModelPlace> g_menuModelPlace;
        inline ee::Global<zge::Vec4, sym::g_menuModelSecondPlace> g_menuModelSecondPlace;
        constexpr uint32_t kStopLoading = sym::g_textMenuStopStartLoading;     // "STOP UI_Menu.StartLoading"

        constexpr uint32_t kFirstNameField = sym::g_textRiderEditFirstName;   // "Background.Field_0"
        constexpr uint32_t kLastNameField = sym::g_textRiderEditLastName;     // "Background.Field_1"
        constexpr uint32_t kTextName = sym::g_textMenuModelText;              // "Text"
        constexpr uint32_t kMenuName = sym::g_textMenuModelMenu;              // "Menu"
        constexpr uint32_t kItemsName = sym::g_textMenuModelItems;            // "Items"
        constexpr uint32_t kSelectedIndexName = sym::g_textMenuModelSelectedIndex; // "SelectedIndex"
        // The gender page's lines, the character's own greyed ("!").
        constexpr uint32_t kFemaleRiderChoice = sym::g_textGenderChoiceFemaleRider; // "MALE\n!FEMALE"
        constexpr uint32_t kMaleRiderChoice = sym::g_textGenderChoiceMaleRider;     // "!MALE\nFEMALE"
        constexpr int32_t kChoiceMale = 0, kChoiceFemale = 1;

        // The model's skeleton (its library asset, loaded while the menus
        // show the model), its idle animation, and the default models by
        // gender.
        constexpr ee::Ptr<const char> kSkeletonLibrary{sym::g_textMenuModelSkeletonLibrary}; // "assets\\create\\skeleton.zal"
        constexpr ee::Ptr<const char> kSkeletonAsset{sym::g_textMenuModelSkeletonAsset};     // "SkeletonBoobs"
        inline ee::Global<uint32_t, sym::g_menuModelSkeleton> g_menuModelSkeleton;
        constexpr ee::Ptr<const char> kIdleAnimation{sym::g_textMenuModelIdle};              // "menu.create.maleidle00"
        constexpr ee::Ptr<const char> kDefaultMale{sym::g_textMenuDefaultMale};             // "Default Male"
        constexpr ee::Ptr<const char> kDefaultFemale{sym::g_textMenuDefaultFemale};         // "Default Female"
        constexpr uint32_t kModelPartsAspect = 0;
        constexpr uint32_t kPlayLooped = 1u << 0;
        constexpr uint32_t kNoHook = 0;
        // The module's tree of records by name (its order: menuModelNameLess).
        inline ee::Global<zge::SearchTree, sym::g_menuModelTree> g_menuModelTree;
        constexpr uint32_t kMenuModelNameLess = sym::menuModelNameLess;
        // The model turns about its up while L1 or L2 (one way) or R1 or R2
        // (the other) is held on the current player's pad: 36 angle units a
        // millisecond of the frame.
        constexpr uint32_t kTurnLeftButtons = zge::kPadL1 | zge::kPadL2, kTurnRightButtons = zge::kPadR1 | zge::kPadR2;
        constexpr int16_t kTurnPerMillisecond = 36;
        constexpr ee::Float kFrameSeconds = std::bit_cast<float>(0x3C888889u); // 1 / 60
        // A female part: an asset whose name's second letter is 'F'.
        constexpr char kFemalePartLetter = 'F';
        // The skeleton model's name, as the original copies it.
        struct SkeletonName
        {
            char text[9];
        };
        inline ee::Global<SkeletonName, sym::g_textMenuModelSkeleton> g_skeletonName; // "skeleton"
    }

    void showMenuModel(uint32_t shown, uint32_t secondPlace)
    {
        ee::Local<zge::Vec4> place;
        ee::Local<zge::Vec4> second;
        *place = *g_menuModelPlace;
        *second = *g_menuModelSecondPlace;
        zge::setObjectPosition(g_menuModel->object, secondPlace ? ee::Ptr<zge::Vec4>(second.address()) : ee::Ptr<zge::Vec4>(place.address()));
        g_menuModel->shown = shown;
        if (shown)
            return;
        zge::zgui::runScript(ee::Ptr<Node>(), ee::Ptr<char>(kStopLoading), 0);
        zge::setSceneObjectShown(g_menuModel->object, 0);
    }
    READABLE_FUNCTION(showMenuModel, Verified);

    void showCharacterNames(ee::Ptr<Node> frame)
    {
        const ee::Ptr<RiderCharacter> character = playerCharacter(currentPlayer());
        zge::zgui::setVariable(zge::zgui::findNode(frame, kFirstNameField, kFrameNode), kTextName, &character->firstName);
        game::tail(zge::zgui::setVariable, zge::zgui::findNode(frame, kLastNameField, kFrameNode), kTextName, &character->lastName);
    }
    READABLE_FUNCTION(showCharacterNames, Verified);

    void keepCharacterNames(ee::Ptr<Node> frame)
    {
        const ee::Ptr<RiderCharacter> character = playerCharacter(currentPlayer());
        zge::zgui::copyVariable(zge::zgui::findNode(frame, kFirstNameField, kFrameNode), kTextName, &character->firstName);
        game::tail(zge::zgui::copyVariable, zge::zgui::findNode(frame, kLastNameField, kFrameNode), kTextName, &character->lastName);
    }
    READABLE_FUNCTION(keepCharacterNames, Verified);

    void showGenderChoice(ee::Ptr<Node> frame)
    {
        ee::Local<int32_t> selected;
        if (isFemaleRider(&(*g_playerSetups)[currentPlayer()]))
        {
            zge::zgui::setVariable(zge::zgui::findNode(frame, kMenuName, kFrameNode), kItemsName, kFemaleRiderChoice);
            *selected = kChoiceMale;
        }
        else
        {
            zge::zgui::setVariable(zge::zgui::findNode(frame, kMenuName, kFrameNode), kItemsName, kMaleRiderChoice);
            *selected = kChoiceFemale;
        }
        zge::zgui::setVariable(zge::zgui::findNode(frame, kMenuName, kFrameNode), kSelectedIndexName, selected);
    }
    READABLE_FUNCTION(showGenderChoice, Verified);

    // startMenuModel (inferred name): the front end's loading makes the
    // model (the first character's), shown, playing its idle animation;
    // the module's tree set up and the skeleton loaded.
    void startMenuModel()
    {
        const ee::Ptr<zge::SceneObject> object = bmxcreateMenuCreateZobjModel(0);
        g_menuModel->shown = 1;
        g_menuModel->object = object;
        const ee::Ptr<zge::Motion> idle = menuModelIdleAnimation();
        if (g_menuModel->object && idle)
            zge::playAnimationWithHooks(g_menuModel->object, idle, kPlayLooped, kNoHook, 0, ee::Float(0.0f));
        zge::initSearchTree(&*g_menuModelTree, kMenuModelNameLess);
        *g_menuModelSkeleton = calls::loadLibraryAsset(kSkeletonLibrary, kSkeletonAsset);
    }
    READABLE_FUNCTION(startMenuModel, Verified);

    // stopMenuModel (inferred name): the front end's exit lets the model,
    // the skeleton and the libraries' files go.
    void stopMenuModel()
    {
        zge::unbindObjectModel(g_menuModel->object);
        g_menuModel->object = {};
        calls::releaseLibraryAsset(*g_menuModelSkeleton);
        calls::closeLibraryFilesTail();
    }
    READABLE_FUNCTION(stopMenuModel, Verified);

    void menuModelFrame()
    {
        game::tail(updateMenuModel);
    }
    READABLE_FUNCTION(menuModelFrame, Verified);

    // setupModelIsFemale: the model of a player's character is female.
    uint32_t setupModelIsFemale(ee::Ptr<const PlayerSetup> setup)
    {
        return modelIsFemale(g_savedGame->characters[setup->character].model);
    }
    READABLE_FUNCTION(setupModelIsFemale, Verified);

    // changeCharacterGender (inferred name): the current player's character
    // takes the other gender's default model, and the menus' model wears it.
    void changeCharacterGender()
    {
        const ee::Ptr<RiderCharacter> character = playerCharacter(currentPlayer());
        zge::unbindObjectModel(g_menuModel->object);
        setGenderDefaultModel(character);
        calls::bindModelToObjectTail(g_menuModel->object, character->model);
    }
    READABLE_FUNCTION(changeCharacterGender, Verified);

    // setMenuModel (inferred name): the menus' model put on a character's
    // model, unless it wears it already.
    void setMenuModel(ee::Ptr<zge::ModelRecord> model)
    {
        if (zge::objectWearsModel(g_menuModel->object, model))
            return;
        zge::unbindObjectModel(g_menuModel->object);
        calls::bindModelToObjectTail(g_menuModel->object, model);
    }
    READABLE_FUNCTION(setMenuModel, Verified);

    uint32_t modelIsFemale(ee::Ptr<zge::ModelRecord> model)
    {
        ee::Local<uint32_t> female;
        *female = 0;
        zge::forEachModelEntry(model, kModelPartsAspect, zge::ModelEntryVisit(sym::femalePartVisit), female.address());
        return *female;
    }
    READABLE_FUNCTION(modelIsFemale, Verified);

    void updateMenuModel()
    {
        ee::Local<zge::EulerAngles> turn;
        ee::Local<zge::Quaternion> orientation;
        const ee::Ptr<zge::SceneObject> object = g_menuModel->object;
        if (object->flags & zge::kObjectMoved)
        {
            turn->x = 0;
            turn->y = kTurnPerMillisecond;
            turn->z = 0;
            const uint32_t held = rider::padInput(currentPlayer())->buttons.held;
            if (held & kTurnLeftButtons)
            {
                const ee::Ptr<zge::Quaternion> was = zge::objectOrientation(g_menuModel->object);
                turn->y = static_cast<int16_t>(turn->y * zge::frameMilliseconds());
                zge::quaternionFromEuler(orientation, turn);
                zge::quaternionMultiply(orientation, was, orientation);
                zge::setObjectOrientation(g_menuModel->object, orientation);
            }
            else if (held & kTurnRightButtons)
            {
                const int32_t back = -zge::frameMilliseconds();
                const ee::Ptr<zge::Quaternion> was = zge::objectOrientation(g_menuModel->object);
                turn->y = static_cast<int16_t>(turn->y * back);
                zge::quaternionFromEuler(orientation, turn);
                zge::quaternionMultiply(orientation, was, orientation);
                zge::setObjectOrientation(g_menuModel->object, orientation);
            }
        }
        if (zge::updateObjectModel(g_menuModel->object, 0, 0) && g_menuModel->shown &&
            (g_menuModel->object->flags & zge::kObjectMoved) == 0)
        {
            zge::setSceneObjectShown(g_menuModel->object, 1);
            zge::zgui::runScript(ee::Ptr<Node>(), ee::Ptr<char>(kStopLoading), 0);
        }
        // Shown again as it was (each way round, so the change is made).
        if (g_menuModel->object->flags & zge::kObjectMoved)
        {
            zge::setSceneObjectShown(g_menuModel->object, 0);
            zge::setSceneObjectShown(g_menuModel->object, 1);
        }
        else
        {
            zge::setSceneObjectShown(g_menuModel->object, 1);
            zge::setSceneObjectShown(g_menuModel->object, 0);
        }
        zge::updateAnimators(kFrameSeconds);
    }
    READABLE_FUNCTION(updateMenuModel, Verified);

    // setGenderDefaultModel (inferred name): a character's model made the
    // other gender's default.
    void setGenderDefaultModel(ee::Ptr<RiderCharacter> character)
    {
        if (modelIsFemale(character->model))
            calls::copyModelRecordTail(character->model, zge::findModelByName(kDefaultMale));
        else
            calls::copyModelRecordTail(character->model, zge::findModelByName(kDefaultFemale));
    }
    READABLE_FUNCTION(setGenderDefaultModel, Verified);

    // currentPlayerIsMale (inferred name): the current player's character's
    // model has no female part.
    uint32_t currentPlayerIsMale()
    {
        return setupModelIsFemale(&(*g_playerSetups)[currentPlayer()]) == 0;
    }
    READABLE_FUNCTION(currentPlayerIsMale, Verified);

    // bmxcreateMenuCreateZobjModel (the module's error text names it): a
    // scene object of the "skeleton" model wearing a character's model; 0
    // if the object is not made.
    ee::Ptr<zge::SceneObject> bmxcreateMenuCreateZobjModel(uint32_t character)
    {
        ee::Local<SkeletonName> name;
        *name = *g_skeletonName;
        const ee::Ptr<zge::SceneObject> object = zge::newSceneModelByName(0, ee::Ptr<const char>(name.address()));
        if (object)
            zge::bindModelToObject(object, g_savedGame->characters[character].model);
        return object;
    }
    READABLE_FUNCTION(bmxcreateMenuCreateZobjModel, Verified);

    // menuModelIdleAnimation (inferred name): the motion "menu.create.maleidle00".
    ee::Ptr<zge::Motion> menuModelIdleAnimation()
    {
        return zge::findLibraryAnimation(zge::nameHash(kIdleAnimation));
    }
    READABLE_FUNCTION(menuModelIdleAnimation, Verified);

    // femalePartVisit (inferred name): forEachModelEntry's visit for
    // modelIsFemale: a female part sets the user's word and stops the walk.
    int32_t femalePartVisit(ee::Ptr<zge::ModelRecord> model, uint32_t aspect, ee::Ptr<const char> library,
                            ee::Ptr<const char> asset, int32_t rank, uint32_t user)
    {
        if (asset[1] == kFemalePartLetter)
        {
            *ee::Ptr<uint32_t>(user) = 1;
            return 0;
        }
        return 1;
    }
    READABLE_FUNCTION(femalePartVisit, Verified);

    // menuModelNameLess (inferred name): the module's tree's order, names
    // without regard to case.
    uint32_t menuModelNameLess(ee::Ptr<const char> a, ee::Ptr<const char> b)
    {
        return zge::stringCompareNoCase(a, b) < 0 ? 1u : 0u;
    }
    READABLE_FUNCTION(menuModelNameLess, Verified);
}
