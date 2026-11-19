// The skater's juice (the HUD's carton): it fills with tricks, drains
// while riding plainly, warns when low and, in the Career, ends the run
// when empty (General / Death by Juice).

#include "skater.h"

#include "aggressive/game_state.h"
#include "aggressive/objects.h"
#include "aggressive/tuning.h"
#include "zge/zgui/script.h"
#include "aggressive/zgui.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/collision/collision.h"
#include "action_tricks.h"
#include "moves.h"
#include "sym/aggressive/skater/juice.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        constexpr uint32_t kLowJuiceSound = 0x2D;

        // "CartonSize": the meter's capacity property.
        constexpr ee::Ptr<const char> kCartonSizeName{sym::cartonSizeName};
        // Low-juice marks as fractions of the capacity (0.1 both).
        inline ee::Global<ee::Float, sym::g_lowJuiceWhileDraining> g_lowJuiceWhileDraining;
        inline ee::Global<ee::Float, sym::g_lowJuice> g_lowJuice;
        // When the low-juice sound last played (ms).
        inline ee::Global<uint32_t, sym::g_lowJuiceSoundTime> g_lowJuiceSoundTime;
        // What is being played (aggressive/globals.h GameType; 1 Career).
        inline ee::Global<uint8_t, sym::g_gameTypeByte> g_gameTypeByte;
        constexpr uint8_t kCareer = 1;

        ee::Float tuning(uint32_t category, uint32_t entry)
        {
            return (*g_tuningCategories)[category].entries[entry].value;
        }
        ee::Float juiceDrainPerSecond() { return tuning(6, 3); }   // Scoring: Juice / Max Juice Degradation/sec
        ee::Float jizzedOffFraction() { return tuning(6, 4); }     // Scoring: Juice / Jizzed->Non-Jizzed Percent
        ee::Float deathByJuice() { return tuning(0, 5); }          // General / Death by Juice

        // The low-juice sound, at most about once a second.
        void warnLowJuice()
        {
            const uint32_t now = zge::collisionClock();
            const uint32_t last = *g_lowJuiceSoundTime;
            if (now - last < 1001u && now >= last)
                return;
            aggressive::soundEvent(kLowJuiceSound, 0, 0, 0, 0);
            *g_lowJuiceSoundTime = now;
        }

        bool jizzed(ee::Ptr<Player> player)
        {
            return isJizzed(player) != 0;
        }
    }

    int32_t potentialJuice(ee::Ptr<zge::zgui::Node> meter, ee::Ptr<ee::Float> out);

    int32_t playJizzed(ee::Ptr<zge::zgui::Node> meter);

    int32_t stopJizzed(ee::Ptr<zge::zgui::Node> meter);

    int32_t juiceUpdate(ee::Ptr<Player> player, ee::Float frameTime)
    {
        // The meter's values come back through locals.
        ee::Local<ee::Float> juice;
        ee::Local<int32_t> cartonSize;
        ee::Local<ee::Float> out;
        *juice = 0.0f;
        *out = 0.0f;
        juiceAmount(player->juiceMeter, juice);
        *cartonSize = -1;
        ee::Float capacity = 0.0f;
        if (zgui::copyVariable(player->juiceMeter, kCartonSizeName, cartonSize))
            capacity = ee::toFloat(*cartonSize);
        const int32_t hasPotentialTang = potentialJuice(player->juiceMeter, out);

        // Full ("jizzed") within half a unit of the capacity; back to normal
        // below Jizzed->Non-Jizzed Percent of it.
        if (!jizzed(player))
        {
            const int32_t full = capacity - 0.5f <= *juice ? 1 : 0;
            setJizzed(player, full);
            if (jizzed(player))
            {
                *juice = capacity;
                setJuiceMeter(player->juiceMeter, capacity);
                playJizzed(player->juiceMeter);
            }
        }
        else if (*juice / capacity < jizzedOffFraction() && noTrickPending(player))
        {
            setJizzed(player, 0);
            stopJizzed(player->juiceMeter);
        }

        // It drains while standing (or full) where the level drains it, unless down.
        const bool standingOrFull = player->state == State::Stand || jizzed(player);
        bool draining = false;
        if (!standingOrFull || juiceDrains())
            draining = standingOrFull && !(player->flags & kDown);
        if (draining)
        {
            const ee::Float lowMark = capacity * *g_lowJuiceWhileDraining;
            *juice = *juice - juiceDrainPerSecond() * frameTime;
            if (*juice < lowMark)
            {
                warnLowJuice();
                playJizzed(player->juiceMeter);
            }
            else if (!jizzed(player))
                stopJizzed(player->juiceMeter);
        }
        else if (*juice < capacity * *g_lowJuice)
            warnLowJuice();
        else if (!jizzed(player))
            stopJizzed(player->juiceMeter);

        if (*g_gameTypeByte == kCareer)
        {
            if (*juice <= 0.0f)
            {
                if (deathByJuice() == 0.0f)
                {
                    // Without Death by Juice the carton refills to half.
                    *juice = capacity * 0.5f;
                    stopJizzed(player->juiceMeter);
                }
                else
                {
                    if (noTrickPending(player) && !(player->flags & kDown))
                    {
                        // The run ends: the level stops, out of juice, and pauses.
                        setLevelStopReason(currentLevel(), kStopOutOfJuice);
                        enterGameMode(kModePaused);
                    }
                    *juice = 0.0f;
                }
            }
        }
        else if (*juice <= 0.0f)
            *juice = 0.0f;
        setJuiceMeter(player->juiceMeter, *juice);
        return hasPotentialTang;
    }
    READABLE_FUNCTION(juiceUpdate, Verified);

    namespace
    {
        constexpr uint32_t kTangAmount = sym::tangAmount;    // "TangAmount"
        constexpr uint32_t kPlaySetJuice = sym::playSetJuice;  // "PLAY SetJuice"
    }

    // setJuiceMeter (meter, juice): the HUD's juice carton set: its
    // TangAmount variable, then its SetJuice animation played. 0 without a
    // meter or for a negative amount, else setVariable's result.
    int32_t setJuiceMeter(ee::Ptr<zge::zgui::Node> meter, ee::Float juice)
    {
        ee::Local<ee::Float> value;
        *value = juice;
        if (!meter || juice < ee::Float(0.0f))
            return 0;
        const int32_t set = zgui::setVariable(meter, kTangAmount, value);
        if (set)
            zgui::runScript(meter, ee::Ptr<char>(kPlaySetJuice), 0);
        return set;
    }
    READABLE_FUNCTION_WAITS(setJuiceMeter, Verified);

    // updateCurrentObject (player, frame time): while the skater has a current
    // object (Player::hasCurrentObject) it is told "TrickUpdate" with
    // {player, frame time} (objects.h's TrickUpdate: a trick's worth a
    // second, the trails); its answer when it handles it, else 1.
    uint32_t updateCurrentObject(ee::Ptr<Player> player, ee::Float frameTime)
    {
        ee::Local<TrickUpdate> update;
        ee::Local<uint32_t> answer;
        if (player->hasCurrentObject)
        {
            *answer = 0;
            const ee::Ptr<GameObject> object = skaterCurrentObject(player);
            const uint32_t message = *g_msgTrickUpdate;
            update->player = player;
            update->seconds = frameTime;
            if (sendObjectMessage(object, message, update.address(), answer.address()))
                return *answer;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(updateCurrentObject, Verified);

    // ---- the juice carton's jizzed animation (were the sweep's playJizzed,
    // stopJizzed, crashStopJizzed) and the juice step (juiceStep) --------------------
    namespace
    {
        // runScript (zgui): (widget, command, 0), called through the table
        // as the original's tail call.
        constexpr uint32_t kRunWidgetCommand = sym::runScript;
        constexpr uint32_t kPlayJizzedCommand = sym::playJizzedCommand;      // "PLAY Jizzed"
        constexpr uint32_t kStopJizzedCommand = sym::stopJizzedCommand;      // "STOP Jizzed"
        constexpr uint32_t kCrashStopJizzedCommand = sym::crashStopJizzedCommand; // "STOP Jizzed" (its own copy)
    }

    namespace
    {
        namespace tail
        {
        int32_t runScript(ee::Address widget, uint32_t text, uint32_t argument) { return ee::tailCall<int32_t>(kRunWidgetCommand, widget, text, argument); }
        }
    }

    // playJizzed / stopJizzed: the carton's "PLAY Jizzed" and "STOP
    // Jizzed" (setJizzed's display).
    int32_t playJizzed(ee::Ptr<zge::zgui::Node> meter)
    {
        return tail::runScript(meter, kPlayJizzedCommand, 0);
    }
    READABLE_FUNCTION_WAITS(playJizzed, Verified);
    int32_t stopJizzed(ee::Ptr<zge::zgui::Node> meter)
    {
        return tail::runScript(meter, kStopJizzedCommand, 0);
    }
    READABLE_FUNCTION_WAITS(stopJizzed, Verified);

    // crashStopJizzed: "STOP Jizzed" when a crash empties the carton
    // (crashJuiceLoss, score.cpp).
    int32_t crashStopJizzed(ee::Ptr<zge::zgui::Node> meter)
    {
        return tail::runScript(meter, kCrashStopJizzedCommand, 0);
    }
    READABLE_FUNCTION_WAITS(crashStopJizzed, Verified);

    // juiceStep: the juice's frame step: juiceUpdate.
    int32_t juiceStep(ee::Ptr<Player> player, ee::Float frameTime)
    {
        return juiceUpdate(player, frameTime);
    }
    READABLE_FUNCTION_WAITS(juiceStep, Verified);

    // ---- the juice carton's script variables (were the sweep's potentialJuice
    // .. cartonSize) -----------------------------------------------------------
    //
    // The HUD's carton (Player::juiceMeter, a zgui widget) keeps the juice
    // in its script variables: TangAmount (the juice), PotentialTangAmount
    // (what the line in progress would add) and CartonSize (its size, an
    // integer); setting one plays the widget's matching animation.
    namespace
    {
        constexpr uint32_t kTangAmountName = sym::tangAmount;          // "TangAmount"
        constexpr uint32_t kPotentialTangAmountName = sym::potentialTangAmountName; // "PotentialTangAmount"
        constexpr uint32_t kCartonSizeVariable = sym::g_textCartonSize;      // "CartonSize" (its own copy of the name)
        constexpr uint32_t kSetPotentialJuiceCommand = sym::setPotentialJuiceCommand;   // "PLAY SetPotentialJuice"
        constexpr uint32_t kAwardPotentialJuiceCommand = sym::awardPotentialJuiceCommand; // "PLAY AwardPotentialJuice"
        constexpr uint32_t kResizeCartonCommand = sym::resizeCartonCommand;        // "PLAY ResizeCarton"
    }

    // potentialJuice: the carton's potential juice into `out`; whether it has one.
    int32_t potentialJuice(ee::Ptr<zge::zgui::Node> meter, ee::Ptr<ee::Float> out)
    {
        return zgui::copyVariable(meter, kPotentialTangAmountName, out);
    }
    READABLE_FUNCTION_WAITS(potentialJuice, Verified);

    // setPotentialJuice: the carton's potential juice set, and shown.
    int32_t setPotentialJuice(ee::Ptr<zge::zgui::Node> meter, ee::Float juice)
    {
        ee::Local<ee::Float> value;
        *value = juice;
        const int32_t set = zgui::setVariable(meter, kPotentialTangAmountName, value);
        if (set)
            zgui::runScript(meter, ee::Ptr<char>(kSetPotentialJuiceCommand), 0);
        return set;
    }
    READABLE_FUNCTION_WAITS(setPotentialJuice, Verified);

    // clearPotentialJuice: the potential juice back to 0.
    int32_t clearPotentialJuice(ee::Ptr<zge::zgui::Node> meter)
    {
        return setPotentialJuice(meter, ee::Float(0.0f));
    }
    READABLE_FUNCTION_WAITS(clearPotentialJuice, Verified);

    // awardPotentialJuice: the potential juice poured in ("PLAY AwardPotentialJuice").
    void awardPotentialJuice(ee::Ptr<zge::zgui::Node> meter)
    {
        zgui::runScript(meter, ee::Ptr<char>(kAwardPotentialJuiceCommand), 0);
    }
    READABLE_FUNCTION_WAITS(awardPotentialJuice, Verified);

    // juiceAmount: the carton's juice into `out` (-1 when it has none);
    // whether it has it.
    int32_t juiceAmount(ee::Ptr<zge::zgui::Node> meter, ee::Ptr<ee::Float> out)
    {
        int32_t found = 0;
        if (!(!meter || !out))
        {
            found = zgui::copyVariable(meter, kTangAmountName, out);
            if (found)
                return found;
            *out = ee::Float(-1.0f);
        }
        return found;
    }
    READABLE_FUNCTION_WAITS(juiceAmount, Verified);

    // setCartonSize: the carton resized to *size (whole units; a negative
    // size is refused) and the resize played. 1 when done or no size given.
    uint32_t setCartonSize(ee::Ptr<zge::zgui::Node> meter, ee::Ptr<ee::Float> size)
    {
        ee::Local<int32_t> units;
        *units = -1;
        if (meter)
        {
            if (!size)
                return 1;
            const ee::Float wanted = *size;
            if (wanted < ee::Float(0.0f))
                return 0;
            *units = ee::toInt(wanted);
            if (zgui::setVariable(meter, kCartonSizeVariable, units))
            {
                zgui::runScript(meter, ee::Ptr<char>(kResizeCartonCommand), 0);
                return 1;
            }
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(setCartonSize, Verified);

    // cartonSize: the carton's size into `out`; 1 when found (or no `out`).
    uint32_t cartonSize(ee::Ptr<zge::zgui::Node> meter, ee::Ptr<ee::Float> out)
    {
        ee::Local<int32_t> units;
        if (meter)
        {
            if (out)
            {
                *units = -1;
                if (!zgui::copyVariable(meter, kCartonSizeVariable, units))
                    return 0;
                *out = ee::toFloat(*units);
            }
            return 1;
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(cartonSize, Verified);

    uint32_t gameStubZero()
    {
        return 0;
    }
    READABLE_FUNCTION(gameStubZero, Verified);

    uint32_t gameStubTrueB(uint32_t /*arg0*/, uint32_t /*arg1*/, uint32_t /*arg2*/, uint32_t arg3)
    {
        return 1;
    }
    READABLE_FUNCTION(gameStubTrueB, Draft);

    namespace
    {
        int copyVariable(ee::Ptr<zge::zgui::Node> meter, ee::Ptr<const char> cartonSizeVariable, ee::Ptr<char> x0) { return aggressive::zgui::copyVariable(meter, cartonSizeVariable, x0); }
    }

    // (value, ..): value != 0.
    int32_t isNonZeroB(uint32_t value, uint32_t arg1)
    {
        return static_cast<int32_t>(0x0u < value);
    }
    READABLE_FUNCTION(isNonZeroB, Draft);

    // (a, b): both set.
    uint32_t bothNonNull(uint32_t enabled, uint32_t enabled2)
    {
        if (enabled != 0)
        {
            if (enabled2 != 0)
                return 1;
            return 0;
        }
        return 0;
    }
    READABLE_FUNCTION(bothNonNull, Draft);

    uint32_t tangStubZeroA()
    {
        return 0;
    }
    READABLE_FUNCTION(tangStubZeroA, Draft);

    uint32_t tangStubZeroB()
    {
        return 0;
    }
    READABLE_FUNCTION(tangStubZeroB, Draft);

    // ---- juice by the carton's size; the carton's corner colours (were the
    // sweep's addJuiceByCartonSize .. potentialTangTopLeftColor)
    namespace
    {
        constexpr uint32_t kTangCornerColor = sym::g_textTangTopLeftColor;                  // "Tang.TopLeftColor"
        constexpr uint32_t kPotentialTangCornerColor = sym::g_textTangPotentialTangTopLeftColor; // "Tang.PotentialTang.TopLeftColor"
        inline constexpr ee::Float kPercent = 0.01f;

        // A colour variable of the carton set from a copy of `color`
        // (made first); 0 without a widget.
        int32_t setCartonColor(uint32_t widget, uint32_t name, ee::Ptr<zgui::Color> color)
        {
            ee::Local<zgui::Color> copy;
            *copy = *color;
            if (!widget)
                return 0;
            return zgui::setVariable(widget, name, copy);
        }

        // A colour variable of the carton copied out; 0 without both.
        int32_t cartonColor(uint32_t node, uint32_t name, ee::Ptr<zgui::Color> out)
        {
            if (!node || !out)
                return 0;
            return zgui::copyVariable(ee::Ptr<zgui::Node>(node), ee::Ptr<const char>(name), out);
        }
    }

    // addJuiceByCartonSize: `percent` of the carton's size (-1 for a carton
    // with none) added to its potential juice, and shown. Returns 1 for
    // nothing to add.
    int32_t addJuiceByCartonSize(ee::Ptr<zge::zgui::Node> meter, ee::Float percent)
    {
        ee::Local<int32_t> size;
        ee::Local<ee::Float> juice;
        ee::Float cartonSize = -1.0f;
        *juice = cartonSize;
        if (percent == ee::Float(0.0f))
            return 1;
        *size = -1;
        if (zgui::copyVariable(meter, ee::Ptr<const char>(kCartonSizeVariable), size))
            cartonSize = ee::toFloat(*size);
        potentialJuice(meter, juice);
        *juice = *juice + percent * kPercent * cartonSize;
        return setPotentialJuice(meter, *juice);
    }
    READABLE_FUNCTION_WAITS(addJuiceByCartonSize, Draft);

    // The carton's top left colours: the juice's and the potential juice's,
    // set (from a copy) or read out.
    int32_t setTangTopLeftColor(uint32_t widget, ee::Ptr<zgui::Color> color)
    {
        return setCartonColor(widget, kTangCornerColor, color);
    }
    READABLE_FUNCTION_WAITS(setTangTopLeftColor, Draft);

    int32_t tangTopLeftColor(uint32_t node, ee::Ptr<zgui::Color> out)
    {
        return cartonColor(node, kTangCornerColor, out);
    }
    READABLE_FUNCTION_WAITS(tangTopLeftColor, Draft);

    int32_t setPotentialTangTopLeftColor(uint32_t widget, ee::Ptr<zgui::Color> color)
    {
        return setCartonColor(widget, kPotentialTangCornerColor, color);
    }
    READABLE_FUNCTION_WAITS(setPotentialTangTopLeftColor, Draft);

    int32_t potentialTangTopLeftColor(uint32_t node, ee::Ptr<zgui::Color> out)
    {
        return cartonColor(node, kPotentialTangCornerColor, out);
    }
    READABLE_FUNCTION_WAITS(potentialTangTopLeftColor, Draft);
}
