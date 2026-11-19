#pragma once
// The front end's rider model (create-a-model's menu side, menu_model.cpp:
// the module's own error text names one of its functions
// bmxcreateMenuCreateZobjModel): the rider shown beside the rider select
// and the rider edit's menus, and the rider edit's name and gender pages.
// BMX XXX's own code. Names are inferred from use but
// bmxcreateMenuCreateZobjModel's (the Xbox and GameCube builds' error text).

#include "bmx/profile/players.h"
#include "ee/ee.h"
#include "zge/scene/animator.h"
#include "zge/scene/scene.h"
#include "zge/zgui/widget.h"

#include <cstdint>

namespace zge
{
    struct ModelRecord; // (zge/scene/model_records.h, ZGE4's MODEL module; not in this branch's base yet)
}

namespace game::bmx
{
    // The model shown or hidden (hidden: the menus' loading animation
    // stopped too), in its first place or, for the rider edit's tattoo
    // pages, its second.
    void showMenuModel(uint32_t shown, uint32_t secondPlace);
    // The rider edit's name page: the current player's character's first
    // and last name put in its two fields, and read back from them.
    void showCharacterNames(ee::Ptr<zge::zgui::Node> frame);
    void keepCharacterNames(ee::Ptr<zge::zgui::Node> frame);
    // The rider edit's gender page: the other gender selected, the
    // character's own greyed.
    void showGenderChoice(ee::Ptr<zge::zgui::Node> frame);

    // The front end's loading makes the model (the first character's),
    // shown and idling, with the module's tree and the skeleton's asset;
    // the front end's exit lets them go. Its frame: menuModelFrame.
    void startMenuModel();
    void stopMenuModel();
    void menuModelFrame();
    // The model turned by the shoulder buttons, its object shown once its
    // model is loaded (the loading animation stopped), the animators run
    // for the frame.
    void updateMenuModel();
    // The menus' model put on a character's model (unless it wears it).
    void setMenuModel(ee::Ptr<zge::ModelRecord> model);
    // The current player's character takes the other gender's default
    // model ("Default Male" / "Default Female"), worn by the menus' model.
    void changeCharacterGender();
    void setGenderDefaultModel(ee::Ptr<RiderCharacter> character);
    // Whether a model's parts include a female one (a part whose asset's
    // second letter is 'F'); a player's character's model; the current
    // player's (male: none).
    uint32_t modelIsFemale(ee::Ptr<zge::ModelRecord> model);
    uint32_t setupModelIsFemale(ee::Ptr<const PlayerSetup> setup);
    uint32_t currentPlayerIsMale();
    int32_t femalePartVisit(ee::Ptr<zge::ModelRecord> model, uint32_t aspect, ee::Ptr<const char> library,
                            ee::Ptr<const char> asset, int32_t rank, uint32_t user);
    // A scene object of the "skeleton" model wearing a character's model
    // (0: none made), and the menus' idle motion.
    ee::Ptr<zge::SceneObject> bmxcreateMenuCreateZobjModel(uint32_t character);
    ee::Ptr<zge::Motion> menuModelIdleAnimation();
    // The module's tree's order: names without regard to case.
    uint32_t menuModelNameLess(ee::Ptr<const char> a, ee::Ptr<const char> b);
}
