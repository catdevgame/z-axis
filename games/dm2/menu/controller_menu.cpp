// The front end's controller menu (controller_menu.h): DM2's own code,
// registered as script functions by the menu set-up.

#include "controller_menu.h"
#include "../game/players.h"
#include "../input/pads.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/zgui/widget_parts.h"

#include <cstdint>
#include "sym/dm2/menu/controller_menu.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The frame's variables: which player it is for (1 or 2), and the
        // two choices' values.
        constexpr ee::Ptr<const char> kPlayerNumberPath{sym::ctrlIndexPath};         // "MainMenu.ControllerFrame.CtrlIndex"
        constexpr ee::Ptr<const char> kConfigurationPath{sym::ctrlConfigurationPath}; // "MainMenu.ControllerFrame.Menu.SelectConfiguration.CurValue"
        constexpr ee::Ptr<const char> kVibrationPath{sym::ctrlVibrationPath};         // "MainMenu.ControllerFrame.Menu.SelectVibration.CurValue"

        // The rumble that answers vibration turned on: the small motor.
        constexpr uint32_t kSmallMotor = 0;
        constexpr uint32_t kVibrationOnLevel = 200;

        // The settings of the player the frame is for (the frame counts
        // players from 1).
        PlayerSetup &frameSetup(int32_t playerNumber)
        {
            return (*g_playerSetups)[playerNumber - 1];
        }
    }

    void bmxReadCtrlSetting()
    {
        ee::Local<int32_t> playerNumber;
        ee::Local<ee::Float> configuration;
        ee::Local<ee::Float> vibration;
        zge::zgui::copyVariableAtPath(kPlayerNumberPath, ee::Ptr<int32_t>(playerNumber));
        const PlayerSetup &setup = frameSetup(*playerNumber);
        *configuration = static_cast<float>(setup.controllerLayout);
        *vibration = static_cast<float>(setup.flags & kPlayerVibration);
        zge::zgui::setVariableAtPath(kConfigurationPath, ee::Ptr<ee::Float>(configuration));
        zge::zgui::setVariableAtPath(kVibrationPath, ee::Ptr<ee::Float>(vibration));
    }
    READABLE_FUNCTION(bmxReadCtrlSetting, Verified);

    void bmxChangeCtrlConfig()
    {
        ee::Local<int32_t> playerNumber;
        ee::Local<ee::Float> configuration;
        ee::Local<ee::Float> vibration;
        zge::zgui::copyVariableAtPath(kPlayerNumberPath, ee::Ptr<int32_t>(playerNumber));
        zge::zgui::copyVariableAtPath(kConfigurationPath, ee::Ptr<ee::Float>(configuration));
        zge::zgui::copyVariableAtPath(kVibrationPath, ee::Ptr<ee::Float>(vibration));
        PlayerSetup &setup = frameSetup(*playerNumber);
        setup.controllerLayout = static_cast<uint8_t>(ee::toInt(*configuration));
        if (*vibration == 0.0f)
        {
            setup.flags &= static_cast<uint8_t>(~kPlayerVibration);
        }
        else if (!(setup.flags & kPlayerVibration))
        {
            setup.flags |= kPlayerVibration;
            rumble(static_cast<uint32_t>(*playerNumber - 1), kSmallMotor, kVibrationOnLevel);
        }
    }
    READABLE_FUNCTION(bmxChangeCtrlConfig, Verified);
}
