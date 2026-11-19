// zgui's script commands (script.h): SET and the arithmetic, IF, PLAY,
// and the value helpers they share.

#include "zge/zgui/script.h"
#include "zge/zgui/widget_parts.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/scene/message.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "sce/libgcc.h"
#include "zge/math/random.h"

#include <cstring>
#include "sym/zge/zgui/commands.cpp.h"

namespace zge::zgui
{
    namespace
    {
        constexpr uint32_t kOperateVariables = sym::operateVariables; // (destination, source, type hash, operation) -> result

        // The token buffer the commands share (tokenBuffer): a token, and at
        // +0x100 a type name.
        constexpr uint32_t kTokenBuffer = sym::tokenBuffer;
        constexpr uint32_t kTokenTypeName = kTokenBuffer + 0x100u;
        // The commands' four token buffers (the second and third are
        // argumentBuffer and nameBuffer).
        inline ee::Global<char[4][0x100], sym::tokenBuffer> g_tokens;

        // Messages.
        constexpr uint32_t kInvalidIdentifier = sym::invalidIdentifier;    // "zgui: (Invalid Identifier '%s')"
        constexpr uint32_t kInvalidIdentifier2 = sym::invalidIdentifier2;   // "zgui: (Invalid Identifier: '%s')"
        constexpr uint32_t kNotAProperty = sym::notAProperty;         // "zgui: (Destination not a property) "
        constexpr uint32_t kActionNotFound = sym::actionNotFound;       // "zgui: (Action not Found: '%s')"
        constexpr uint32_t kEventTypeName = sym::eventTypeName;        // "Event"

        // IF's comparisons (their text at g_textLessEquals ..), as evaluate's operations.
        struct Comparison
        {
            uint32_t text;
            Operation operation;
        };

        // A literal's value held for one evaluation: a node-like temporary on
        // the stack, its value in itself.
        struct Temporary
        {
            uint32_t _00[2];
            ee::Ptr<Node> parent;           // +0x08 none
            uint32_t _0c[3];
            ee::Address value;              // +0x18 -> the value (+0x20)
            ee::Ptr<VariableType> type;     // +0x1C
            ee::Ptr<char> text;             // +0x20 the value: for int, float and string the literal's text
        };
        static_assert(offsetof(Temporary, value) == 0x18 && offsetof(Temporary, text) == 0x20);

        // What the type's methods see: a variable.
        ee::Ptr<Node> asVariable(const ee::Local<Temporary> &temporary)
        {
            return classCast<Node>(ee::Ptr<Temporary>(temporary));
        }

        void copyString(ee::Address to, ee::Address from) { ee::game(zge::kStrcpy, to, from); }
    }

    namespace
    {
        uint32_t stringLength(ee::Ptr<char> text) { return ee::game<uint32_t>(zge::kStrlen, text); }
    }

    void valueText(ee::Ptr<const char> token, ee::Ptr<char> out, int32_t report)
    {
        ee::Local<char> name;
        ee::Local<char> typeName;
        ee::Local<ee::Ptr<Node>> node;
        const int32_t kind = resolvePath(token, ee::Ptr<char>(name.address()), ee::Ptr<char>(typeName.address()), node);
        if (kind == kVariableKind)
        {
            variableText(ee::Ptr<Node>(*node)->parent, ee::Ptr<const char>(name.address()), out);
            return;
        }
        if (kind == kPathLiteral)
        {
            // {text} or {"text"}: the text inside.
            const bool quoted = token[1] == '"';
            copyString(out, token + (quoted ? 2 : 1));
            const uint32_t length = stringLength(out);
            out[length - (quoted ? 2 : 1)] = 0;
        }
        else
        {
            copyString(out, token);
            if (report)
                zge::zgui::report(kInvalidIdentifier, token);
        }
    }
    READABLE_FUNCTION_OWN(valueText, Verified, dm2, valueTextAtPath);

    void tokenValue(ee::Ptr<const char> token, ee::Address out, ee::Ptr<char> typeName)
    {
        ee::Local<char> name;
        ee::Local<char> foundType;
        ee::Local<Temporary> temporary;
        ee::Local<ee::Ptr<Node>> node;
        const int32_t kind = resolvePath(token, ee::Ptr<char>(name.address()), ee::Ptr<char>(foundType.address()), node);
        const auto leave = [] {  };
        if (kind == kVariableKind)
        {
            copyVariable(ee::Ptr<Node>(*node)->parent, ee::Ptr<const char>(name.address()), out);
            if (typeName)
                copyString(typeName, ee::Ptr<char>(foundType.address()));
            return leave();
        }
        if (kind == kPathLiteral)
        {
            // A literal of the type named: made by the type's setText into a
            // temporary whose value is `out`.
            const ee::Ptr<VariableType> type = findVariableType(zge::nameHash(typeName.get()));
            if (!type)
                return leave();
            temporary->value = out;
            type->setText(asVariable(temporary), token + 1);
            return leave();
        }
        report(kInvalidIdentifier2, token);
        leave();
    }
    READABLE_FUNCTION_OWN(tokenValue, Verified, dm2, tokenValueAtPath);

    uint64_t evaluate(ee::Ptr<char> text, Operation operation)
    {
        namespace here = points::evaluate;
        ee::Local<Temporary> temporary;
        ee::Local<uint8_t[0x10]> buffer2;
        ee::Local<ee::Ptr<Node>> destination;
        ee::Local<ee::Ptr<Node>> source;
        const ee::Ptr<char> token(kTokenBuffer);
        const ee::Ptr<char> typeName(kTokenTypeName);

        const ee::Ptr<char> rest(readToken(text, 0, token));
        // The ways out: the error (then the common return), the
        // result.
        const auto leave = [](uint64_t result) {
            return result;
        };
        const auto notAProperty = [&] {
            report(kNotAProperty);
            return leave(0);
        };
        if (resolvePath(token, ee::Ptr<char>(), typeName, destination) !=
            kVariableKind)
            return notAProperty();
        const uint32_t typeHash = zge::nameHash(typeName.get());
        readToken(rest, 0, token);
        const int32_t kind =
            resolvePath(token, ee::Ptr<char>(), typeName, source);
        if (kind == kPathLiteral)
        {
            // A literal: read as a value of the destination's type (text
            // types take the literal's text in place, others are made (its type
            // name is still in the buffer: a literal leaves it alone).
            const uint32_t hash = zge::nameHash(typeName.get());
            const ee::Ptr<VariableType> type = findVariableType(hash);
            if (!type)
                return notAProperty();
            temporary->value = buffer2;
            temporary->type = type;
            temporary->parent = {};
            // The text inside {...} and "...": the end cut, the start skipped.
            const int32_t length = static_cast<int32_t>(stringLength(token));
            int32_t first = token[0] == '{' ? 1 : 0;
            int32_t last = token[length - 1] == '}' ? length - 2 : length - 1;
            const bool quotedEnd = token[last] == '"';
            const bool quotedStart = token[first] == '"';
            if (quotedEnd)
                last -= 1;
            if (quotedStart)
                first += 1;
            token[last + 1] = 0;
            uint64_t result;
            if (hash == *g_stringType || hash == *g_assetType || hash == *g_eventType)
            {
                temporary->text = token + first;
                result = static_cast<uint64_t>(operateVariables(*destination, asVariable(temporary), typeHash, operation));
            }
            else
            {
                type->setText(asVariable(temporary), token + first);
                result = static_cast<uint64_t>(operateVariables(*destination, asVariable(temporary), typeHash, operation));
                if (type->destroy)
                    type->destroy(asVariable(temporary));
            }
            return leave(result);
        }
        if (kind != kVariableKind)
            return notAProperty();
        const uint64_t result = static_cast<uint64_t>(operateVariables(*destination, *source, typeHash, operation));
        return result;
    }
    READABLE_FUNCTION(evaluate, Verified);

    // The commands that are evaluate with an operation: they jump to it.
    uint64_t commandSet(ee::Ptr<char> arguments) { return game::tail(zge::zgui::evaluate, arguments, Operation::Set); }
    READABLE_FUNCTION_OWN(commandSet, Verified, dm2, commandSetAtPath);
    uint64_t commandAdd(ee::Ptr<char> arguments) { return game::tail(zge::zgui::evaluate, arguments, Operation::Add); }
    READABLE_FUNCTION_OWN(commandAdd, Verified, dm2, commandAddAtPath);
    uint64_t commandSub(ee::Ptr<char> arguments) { return game::tail(zge::zgui::evaluate, arguments, Operation::Subtract); }
    READABLE_FUNCTION_OWN(commandSub, Verified, dm2, commandSubAtPath);
    uint64_t commandMult(ee::Ptr<char> arguments) { return game::tail(zge::zgui::evaluate, arguments, Operation::Multiply); }
    READABLE_FUNCTION_OWN(commandMult, Verified, dm2, commandMultAtPath);
    uint64_t commandDiv(ee::Ptr<char> arguments) { return game::tail(zge::zgui::evaluate, arguments, Operation::Divide); }
    READABLE_FUNCTION_OWN(commandDiv, Verified, dm2, commandDivAtPath);
    // commandRound (ZGE4 only): ROUND destination value: evaluate with its
    // Round, the value rounded to a whole number into the destination.
    inline uint64_t commandRound(ee::Ptr<char> arguments) { return game::tail(zge::zgui::evaluate, arguments, Operation::Round); }
    READABLE_FUNCTION_ONLY(bmx, commandRound);

    // ZGE2's SET, ADD, SUB, MULT and DIV (ZGE3's are evaluate): the
    // destination is the line's first token and the other value its second.
    // SET goes by text; the others work on the two values themselves, by the
    // name of the destination's type (Bool as Sint32), and set the result.
    namespace
    {
        constexpr uint32_t kBoolTypeName = sym::boolName, kFloatTypeName = sym::floatName, kSint32TypeName = sym::sint32Name;
        constexpr uint32_t kRgbaTypeName = sym::dm2::rgbaName, kVectorTypeName = sym::vectorName;
        constexpr uint32_t kQuaternionTypeName = sym::quaternionName;

        bool isTypeNamed(ee::Ptr<const char> typeName, uint32_t name)
        {
            return zge::stringCompareNoCase(typeName, name) == 0;
        }

        // A colour channel from a number: 0 below 0, 255 above 255.
        uint8_t channelOf(int32_t value)
        {
            if (value < 0)
                return 0;
            return value < 0x100 ? static_cast<uint8_t>(value) : 0xFF;
        }

        // ADD and SUB: `left` and `right` of the same type, piece by piece
        // (a colour's channels wrap).
        template <typename Combine, typename CombineWords>
        void combineByType(ee::Ptr<const char> typeName, ScriptValue &left, const ScriptValue &right, Combine combine,
                           CombineWords combineWords)
        {
            if (isTypeNamed(typeName, kBoolTypeName))
                left.integer = combineWords(left.integer, right.integer);
            else if (isTypeNamed(typeName, kFloatTypeName))
                left.real = combine(left.real, right.real);
            else if (isTypeNamed(typeName, kSint32TypeName))
                left.integer = combineWords(left.integer, right.integer);
            else if (isTypeNamed(typeName, kRgbaTypeName))
            {
                for (int channel = 0; channel < 4; ++channel)
                    left.colour.rgba[channel] =
                        static_cast<uint8_t>(combineWords(left.colour.rgba[channel], right.colour.rgba[channel]));
            }
            else if (isTypeNamed(typeName, kVectorTypeName))
            {
                for (int axis = 0; axis < 3; ++axis)
                    left.vector[axis] = combine(left.vector[axis], right.vector[axis]);
            }
            else if (isTypeNamed(typeName, kQuaternionTypeName))
            {
                for (int part = 0; part < 4; ++part)
                    left.vector[part] = combine(left.vector[part], right.vector[part]);
            }
        }

        // MULT and DIV: `left` scaled by `right` read as one number (a
        // colour's channels kept within a byte).
        template <typename Scale, typename ScaleWords>
        void scaleByType(ee::Ptr<const char> typeName, ScriptValue &left, const ScriptValue &right, Scale scale,
                         ScaleWords scaleWords)
        {
            if (isTypeNamed(typeName, kBoolTypeName))
                left.integer = scaleWords(left.integer, right);
            else if (isTypeNamed(typeName, kFloatTypeName))
                left.real = scale(left.real, right.real);
            else if (isTypeNamed(typeName, kSint32TypeName))
                left.integer = scaleWords(left.integer, right);
            else if (isTypeNamed(typeName, kRgbaTypeName))
            {
                for (int channel = 0; channel < 4; ++channel)
                    left.colour.rgba[channel] =
                        channelOf(ee::toInt(scale(ee::toFloat(static_cast<int32_t>(left.colour.rgba[channel])), right.real)));
            }
            else if (isTypeNamed(typeName, kVectorTypeName))
            {
                for (int axis = 0; axis < 3; ++axis)
                    left.vector[axis] = scale(left.vector[axis], right.real);
            }
            else if (isTypeNamed(typeName, kQuaternionTypeName))
            {
                for (int part = 0; part < 4; ++part)
                    left.vector[part] = scale(left.vector[part], right.real);
            }
        }
    }

    // commandSetAtPath: SET destination value: the value's text (a variable's,
    // or the token itself) set as the destination's.
    void commandSetAtPath(ee::Ptr<char> arguments)
    {
        ee::Local<char[0x100]> text;
        char *const token = (*g_tokens)[0];
        readToken(arguments, 1, token);
        valueText(token, text, 1);
        readToken(arguments, 0, token);
        setVariableTextAtPath(token, text);
    }

    // commandAddAtPath, commandSubAtPath: ADD / SUB destination value.
    // Nothing without a destination variable.
    void commandAddAtPath(ee::Ptr<char> arguments)
    {
        ee::Local<ScriptValue> right;
        ee::Local<ScriptValue> left;
        ee::Local<char[0x40]> typeName;
        char *const leftToken = (*g_tokens)[0];
        char *const rightToken = (*g_tokens)[1];
        readToken(arguments, 0, leftToken);
        typeName[0] = 0;
        tokenValue(leftToken, left, typeName);
        if (typeName[0] == 0)
            return;
        readToken(arguments, 1, rightToken);
        tokenValue(rightToken, right, typeName);
        combineByType(typeName, *left, *right, [](ee::Float a, ee::Float b) { return a + b; },
                      [](int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b)); });
        setTokenValue(leftToken, left);
    }

    void commandSubAtPath(ee::Ptr<char> arguments)
    {
        ee::Local<ScriptValue> right;
        ee::Local<ScriptValue> left;
        ee::Local<char[0x40]> typeName;
        char *const leftToken = (*g_tokens)[0];
        char *const rightToken = (*g_tokens)[1];
        readToken(arguments, 0, leftToken);
        typeName[0] = 0;
        tokenValue(leftToken, left, typeName);
        if (typeName[0] == 0)
            return;
        readToken(arguments, 1, rightToken);
        tokenValue(rightToken, right, typeName);
        combineByType(typeName, *left, *right, [](ee::Float a, ee::Float b) { return a - b; },
                      [](int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b)); });
        setTokenValue(leftToken, left);
    }

    // commandMultAtPath: MULT destination value: the value read as one of
    // the destination's type (its type's name in a copy, which decides the
    // multiplication). The original clears the copy's first byte, not the
    // destination type's, before it asks for the destination: without a
    // destination variable it goes by what its frame held.
    void commandMultAtPath(ee::Ptr<char> arguments)
    {
        ee::Local<ScriptValue> right;
        ee::Local<ScriptValue> left;
        ee::Local<char[0x40]> typeName;
        ee::Local<char[0x40]> rightType;
        char *const leftToken = (*g_tokens)[0];
        char *const rightToken = (*g_tokens)[1];
        readToken(arguments, 0, leftToken);
        rightType[0] = 0;
        tokenValue(leftToken, left, typeName);
        if (typeName[0] == 0)
            return;
        zge::stringCopy(rightType, typeName);
        readToken(arguments, 1, rightToken);
        tokenValue(rightToken, right, rightType);
        zge::compareStrings(typeName, rightType);
        scaleByType(rightType, *left, *right, [](ee::Float a, ee::Float b) { return a * b; },
                    [](int32_t a, const ScriptValue &b) {
                        return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b.integer));
                    });
        setTokenValue(leftToken, left);
    }

    // commandDivAtPath: DIV destination value. A Sint32 (or Bool) is divided
    // as a number by the value's word read as a float (not converted: the
    // original's), and the quotient made whole.
    void commandDivAtPath(ee::Ptr<char> arguments)
    {
        ee::Local<ScriptValue> right;
        ee::Local<ScriptValue> left;
        ee::Local<char[0x40]> typeName;
        char *const leftToken = (*g_tokens)[0];
        char *const rightToken = (*g_tokens)[1];
        readToken(arguments, 0, leftToken);
        typeName[0] = 0;
        tokenValue(leftToken, left, typeName);
        if (typeName[0] == 0)
            return;
        readToken(arguments, 1, rightToken);
        tokenValue(rightToken, right, typeName);
        scaleByType(typeName, *left, *right, [](ee::Float a, ee::Float b) { return a / b; },
                    [](int32_t a, const ScriptValue &b) { return ee::toInt(ee::toFloat(a) / b.real); });
        setTokenValue(leftToken, left);
    }

    void commandIf(ee::Ptr<char> arguments)
    {
        namespace here = points::commandIf;
        // IF a op b statement: evaluate's comparison of "a b", then the
        // statement when it holds.
        ee::Local<char> comparison;
        ee::Local<char> left;
        ee::Local<char> right;
        const ee::Ptr<char> afterLeft(readToken(arguments, 0, ee::Ptr<char>(left.address())));
        const ee::Ptr<char> afterComparison(readToken(afterLeft, 0, ee::Ptr<char>(comparison.address())));
        const ee::Ptr<char> statement(readToken(afterComparison, 0, ee::Ptr<char>(right.address())));
        appendString(ee::Ptr<char>(left.address()), sym::g_textSpace);
        appendString(ee::Ptr<char>(left.address()), ee::Ptr<char>(right.address()));

        // "<=", ">=", "<", ">", then "=" or "==", then "<>" or "!=".
        static constexpr Comparison kComparisons[] = {
            {sym::g_textLessEquals, Operation::LessOrEqual}, {sym::g_textGreaterEquals, Operation::GreaterOrEqual},
            {sym::g_textLess, Operation::Less},        {sym::g_textGreater, Operation::Greater},
            {sym::g_textEquals, Operation::Equal},       {sym::g_textEqualsEquals, Operation::Equal},
            {sym::g_textLessGreater, Operation::NotEqual},    {sym::g_textNotEquals, Operation::NotEqual}};
        static constexpr uint32_t kTestBlocks[8] = {0, here::taken, here::taken2, here::taken3, here::taken4, 0, here::taken6, 0};
        static constexpr uint32_t kMatchBlocks[8] = {0, 0, 0, 0, here::taken5, here::taken5, here::taken7, here::taken7};
        bool holds = false;
        for (int i = 0; i < 8; ++i)
        {
            const Comparison &c = kComparisons[i];
            if (stringCompareNoCase(comparison, c.text) != 0)
                continue;
            holds = evaluate(left, c.operation) != 0;
            break;
        }
        if (holds)
            runStatement(statement);
    }
    READABLE_FUNCTION_WAITS(commandIf, Verified);

    namespace
    {
        void memset(ee::Address m, uint32_t c, uint32_t n) { fillBytes(m, c, n); }
    }

    void commandPlay(ee::Ptr<char> arguments)
    {
        namespace here = points::commandPlay;
        // PLAY name [target]: an Event variable's script, or "@" (the
        // animation the script runs for) played again on the target.
        ee::Local<char> name;
        ee::Local<char> targetName;
        ee::Local<char> typeName;
        ee::Local<ee::Ptr<Node>> node;
        ee::Local<ee::Ptr<Node>> target;
        *node = 0;
        *target = 0;
        memset(ee::Ptr<char>(targetName.address()), 0, 0xFF);
        const ee::Ptr<char> rest(readToken(arguments, 0, ee::Ptr<char>(name.address())));
        readToken(rest, 0, ee::Ptr<char>(targetName.address()));
        const auto saves = [&](uint32_t returnsTo)
        {
        };
        const int32_t kind = resolvePath(ee::Ptr<const char>(name.address()), ee::Ptr<char>(), ee::Ptr<char>(typeName.address()), node);
        if (kind == kPathArgument)
        {
            if (*targetName == 0 ||
                resolvePath(targetName, ee::Ptr<char>(), ee::Ptr<char>(), target) != static_cast<int32_t>(NodeKind::Widget))
                *target = (*node)->parent;
            playWidget(classCast<Animation>(*node), ee::Ptr<Node>(*target));
            return;
        }
        if (kind == kVariableKind && stringCompareNoCase(typeName, kEventTypeName) == 0)
        {
            const ee::Ptr<Node> event(*node);
            // An Event variable's value is its script.
            runScript(event->parent, *event->valueAs<ee::Ptr<char>>(), 0u);
            return;
        }
        report(kActionNotFound, name);
    }
    READABLE_FUNCTION_WAITS_OWN(commandPlay, Verified, dm2, commandPlayAtPath);
}

namespace zge::zgui
{
    void commandStop(ee::Ptr<char> arguments)
    {
        namespace here = points::commandStop;
        // STOP @ [target]: the animation the script runs for, on the target
        // (its parent when the target is not a widget).
        ee::Local<char> name;
        ee::Local<char> targetName;
        ee::Local<ee::Ptr<Node>> node;
        ee::Local<ee::Ptr<Node>> target;
        *node = 0;
        *target = 0;
        const auto saves = [&](uint32_t returnsTo)
        {
        };
        memset(ee::Ptr<char>(targetName.address()), 0, 0xFF);
        const ee::Ptr<char> rest(readToken(arguments, 0, ee::Ptr<char>(name.address())));
        readToken(rest, 0, ee::Ptr<char>(targetName.address()));
        if (resolvePath(ee::Ptr<const char>(name.address()), ee::Ptr<char>(), ee::Ptr<char>(), node) !=
            kPathArgument)
        {
            report(kActionNotFound, name);
            return;
        }
        if (*targetName == 0 ||
            resolvePath(targetName, ee::Ptr<char>(), ee::Ptr<char>(), target) != static_cast<int32_t>(NodeKind::Widget))
            *target = (*node)->parent;
        stopPlaying(classCast<Animation>(*node), ee::Ptr<Node>(*target));
    }
    READABLE_FUNCTION_OWN(commandStop, Verified, dm2, commandStopAtPath);
}

namespace zge::zgui
{
    namespace
    {
        constexpr uint32_t kMustBeProperty = sym::mustBeProperty;     // "zgui: (Identifier '%s' must be a property)"
        constexpr uint32_t kInvalidDestination = sym::invalidDestination; // "zgui: (Invalid Destination Type:'%s')"
        // The type names LINEAR compares (their text, boolName ..).
        constexpr uint32_t kBoolName = sym::boolName, kFloatName = sym::floatName, kUint8Name = sym::uint8Name,
                           kSint32Name = sym::sint32Name, kColorName = sym::colorName, kVectorName = sym::vectorName;

        // CVT.S.W of an unsigned value (halved, keeping the low bit, and doubled past
        // 2^31).
        ee::Float unsignedToFloat(uint32_t u, uint32_t negative = 0)
        {
            if (static_cast<int32_t>(u) >= 0)
                return ee::toFloat(static_cast<int32_t>(u));
            const ee::Float half = ee::toFloat(static_cast<int32_t>((u >> 1) | (u & 1)));
            return half + half;
        }
        // from + (to - from) * t for one channel of integers, truncated.
        int32_t lerpInteger(int32_t from, int32_t to, ee::Float t)
        {
            return ee::toInt(ee::toFloat(from) + ee::toFloat(to - from) * t);
        }
    }

    void setTokenValue(ee::Ptr<const char> token, ee::Address value)
    {
        ee::Local<char> name;
        ee::Local<char> typeName;
        ee::Local<ee::Ptr<Node>> node;
        if (resolvePath(token, ee::Ptr<char>(name.address()), ee::Ptr<char>(typeName.address()), node) == kVariableKind)
        {
            setVariable(ee::Ptr<Node>(*node)->parent, ee::Ptr<const char>(name.address()), value);
            return;
        }
        report(kMustBeProperty, token);
    }
    READABLE_FUNCTION_OWN(setTokenValue, Verified, dm2, setTokenValueAtPath);

    void commandLinear(ee::Ptr<char> arguments)
    {
        ee::Local<char[0x100]> type;
        ee::Local<char[0x100]> tType;
        ee::Local<ScriptValue> result;
        ee::Local<ScriptValue> from;
        ee::Local<ScriptValue> to;
        ee::Local<ScriptValue> t;
        char *const destinationToken = (*g_tokens)[0];
        char *const fromToken = (*g_tokens)[1];
        char *const toToken = (*g_tokens)[2];
        char *const tToken = (*g_tokens)[3];
        memset(type, 0, 0x100);
        memset(tType, 0, 0x100);
        ee::Ptr<char> rest = readToken(arguments, 0, destinationToken);
        tokenValue(destinationToken, result, type);
        if (type[0] == 0)
            return;
        rest = readToken(rest, 0, fromToken);
        tokenValue(fromToken, from, type);
        rest = readToken(rest, 0, toToken);
        tokenValue(toToken, to, type);
        readToken(rest, 0, tToken);
        copyString(tType, kFloatName);
        tokenValue(tToken, t, tType);
        // t a ZFloat, or else taken as an integer.
        const bool floatT = zge::stringCompareNoCase(tType, kFloatName) == 0;
        const ee::Float amount = floatT ? t->real : ee::toFloat(t->integer);

        const auto isType = [&](uint32_t name) { return zge::stringCompareNoCase(type, name) == 0; };
        if (isType(kBoolName))
        {
            const uint32_t a = from->boolean, b = to->boolean;
            const ee::Float start = unsignedToFloat(a);
            const ee::Float span = unsignedToFloat(b - a);
            result->integer = sce::libgcc::fptoui(start + span * amount);
        }
        else if (isType(kFloatName))
        {
            const ee::Float a = from->real, b = to->real;
            result->real = a + (b - a) * amount;
        }
        else if (isType(kSint32Name))
            result->integer = lerpInteger(from->integer, to->integer, amount);
        else if (isType(kUint8Name))
            result->byte = static_cast<uint8_t>(lerpInteger(from->byte, to->byte, amount));
        else if (isType(kColorName))
        {
            uint8_t lerped[4];
            for (int c = 0; c < 4; ++c)
                lerped[c] = static_cast<uint8_t>(lerpInteger(from->colour.rgba[c], to->colour.rgba[c], amount));
            result->colour.paletteName = 0;
            std::memcpy(result->colour.rgba, lerped, 4);
        }
        else if (isType(kVectorName))
        {
            for (int c = 0; c < 3; ++c)
                result->vector[c] = from->vector[c] + (to->vector[c] - from->vector[c]) * amount;
        }
        else
            report(kInvalidDestination, type);
        setTokenValue(destinationToken, result);
    }
    READABLE_FUNCTION_OWN(commandLinear, Verified, dm2, commandLinearAtPath);

    // commandLinearAtPath: ZGE2's LINEAR destination from to t: the same
    // blend by the destination's type (Bool as Sint32, ZRGBA channel by
    // channel, no Uint8), the tokens by index; a type it does not blend
    // leaves the value as it was read, and nothing is reported.
    void commandLinearAtPath(ee::Ptr<char> arguments)
    {
        ee::Local<char[0x100]> type; // the destination's type's name
        ee::Local<char[0x100]> tType;
        ee::Local<ScriptValue> result;
        ee::Local<ScriptValue> from;
        ee::Local<ScriptValue> to;
        ee::Local<ScriptValue> t;
        char *const destinationToken = (*g_tokens)[0];
        char *const fromToken = (*g_tokens)[1];
        char *const toToken = (*g_tokens)[2];
        char *const tToken = (*g_tokens)[3];
        readToken(arguments, 0, destinationToken);
        type[0] = 0;
        tokenValue(destinationToken, result, type);
        if (type[0] == 0)
            return;
        readToken(arguments, 1, fromToken);
        tokenValue(fromToken, from, type);
        readToken(arguments, 2, toToken);
        tokenValue(toToken, to, type);
        readToken(arguments, 3, tToken);
        copyString(tType, kFloatName);
        tokenValue(tToken, t, tType);
        // t a ZFloat, or else taken as an integer.
        const bool floatT = zge::stringCompareNoCase(tType, kFloatName) == 0;
        const ee::Float amount = floatT ? t->real : ee::toFloat(t->integer);

        const auto isType = [&](uint32_t name) { return zge::stringCompareNoCase(type, name) == 0; };
        const auto blendWhole = [&](int32_t a, int32_t b) { return ee::toInt(ee::toFloat(a) + ee::toFloat(b - a) * amount); };
        if (isType(kBoolName))
            result->integer = blendWhole(from->integer, to->integer);
        else if (isType(kFloatName))
        {
            const ee::Float a = from->real, b = to->real;
            result->real = a + (b - a) * amount;
        }
        else if (isType(kSint32Name))
            result->integer = blendWhole(from->integer, to->integer);
        else if (isType(sym::dm2::rgbaName))
        {
            uint8_t blended[4];
            for (int c = 0; c < 4; ++c)
                blended[c] = static_cast<uint8_t>(blendWhole(from->colour.rgba[c], to->colour.rgba[c]));
            std::memcpy(result->colour.rgba, blended, 4);
        }
        else if (isType(kVectorName))
        {
            for (int c = 0; c < 3; ++c)
                result->vector[c] = from->vector[c] + (to->vector[c] - from->vector[c]) * amount;
        }
        setTokenValue(destinationToken, result);
    }
}

// UNLOADLIB and RAND (gamehand13; were the sweep's drafts of commandUnloadLib and
// commandRand).
namespace zge::zgui
{
    namespace
    {
        constexpr uint32_t kIncompatibleArguments = sym::incompatibleArguments; // "zgui: (Incompatible arguments)"
        constexpr uint32_t kInvalidDestinationType = sym::invalidDestinationType; // "zgui: (Invalid Destination Type: %s)"
        // (the types' names: LINEAR's kBoolName .. kSint32Name above)
        // A ZFloat's random fraction: 0 .. 10000 times 1e-4.
        constexpr uint32_t kFloatSteps = 10001;
        inline ee::Global<ee::Float, sym::g_floatStep> g_floatStep; // 1e-4
    }

    // commandUnloadLib, UNLOADLIB name: the UI library of that name unloaded (a
    // "*name" token: the variable's value). Returns unloadLibrary's.
    int32_t commandUnloadLib(ee::Ptr<char> arguments)
    {
        ee::Local<char> out;
        readToken(arguments, 0, ee::Ptr<char>(kTokenBuffer));
        if (*ee::Ptr<char>(kTokenBuffer) == '*')
        {
            valueText(ee::Ptr<const char>(kTokenBuffer + 1), ee::Ptr<char>(out.address()), 1);
            copyString(kTokenBuffer, out);
        }
        return unloadUiLibrary(kTokenBuffer);
    }
    READABLE_FUNCTION_WAITS(commandUnloadLib, Verified);

    // commandRand, RAND destination low high: the destination set to a
    // random value from low to high by its type: Bool 0 or 1, ZFloat in
    // steps of 1e-4 of the range, Sint32 and Uint8 whole numbers (low and
    // high included); reported when low's or high's type is not the
    // destination's, or the type is none of these. (The original hands back
    // what its last call left, which runStatement drops.)
    void commandRand(ee::Ptr<char> arguments)
    {
        ee::Local<char[0x100]> type; // the destination's type's name
        ee::Local<char[0x100]> lowType;
        ee::Local<char[0x100]> highType;
        ee::Local<ScriptValue> result;
        ee::Local<ScriptValue> low;
        ee::Local<ScriptValue> high;
        char *const destinationToken = (*g_tokens)[0];
        char *const lowToken = (*g_tokens)[1];
        char *const highToken = (*g_tokens)[2];
        const ee::Ptr<char> rest = readToken(arguments, 0, destinationToken);
        type[0] = 0;
        tokenValue(destinationToken, result, type);
        if (type[0] == 0)
            return;
        // Low and high are read as values of the destination's type.
        zge::stringCopy(lowType, type);
        zge::stringCopy(highType, type);
        const ee::Ptr<char> next = readToken(rest, 0, lowToken);
        tokenValue(lowToken, low, lowType);
        readToken(next, 0, highToken);
        tokenValue(highToken, high, highType);

        // Low's and high's types the destination's, matched with case (the
        // type names below without).
        if (zge::compareStrings(lowType, type) != 0 || zge::compareStrings(highType, type) != 0)
        {
            report(kIncompatibleArguments);
            return;
        }
        const auto isType = [&](uint32_t name) { return zge::stringCompareNoCase(type, name) == 0; };
        if (isType(kBoolName))
            result->boolean = randomNumber() & 1u;
        else if (isType(kFloatName))
        {
            const uint32_t step = randomNumber() % kFloatSteps;
            result->real = low->real + (high->real - low->real) * ee::toFloat(static_cast<int32_t>(step)) * *g_floatStep;
        }
        else if (isType(kSint32Name))
        {
            const uint32_t random = randomNumber();
            const uint32_t span = static_cast<uint32_t>(high->integer - low->integer + 1);
            result->integer = low->integer + static_cast<int32_t>(random % span);
        }
        else if (isType(kUint8Name))
        {
            const uint32_t random = randomNumber();
            const uint32_t span = static_cast<uint32_t>(high->byte - low->byte + 1);
            result->byte = static_cast<uint8_t>(low->byte + random % span);
        }
        else
            report(kInvalidDestinationType, type);
        setTokenValue(destinationToken, result);
    }
    READABLE_FUNCTION_WAITS_OWN(commandRand, Verified, dm2, commandRandAtPath);
}

// ZGE2's forms of the token helpers: a token's path leads to a widget's
// scene object or an ACTION (resolveObjectPath), whose variable is read or
// set; a token that is no variable is left as it is, not reported.
namespace zge::zgui
{
    namespace
    {
        constexpr uint32_t kAssetTypeName = sym::assetName; // "ZassetID"

        // A literal's value held for one evaluation: a ListedVariable-shaped
        // temporary on the stack (its two words of padding place the next
        // local where the original's is).
        struct alignas(16) LiteralTemporary
        {
            ListedVariable variable;
            uint32_t _18[2];
        };
        static_assert(sizeof(LiteralTemporary) == 0x20);

    }

    // valueTextAtPath: a widget variable's text has its name and a tab in
    // front (widgetVariableText): the value after them, its quotes dropped.
    void valueTextAtPath(ee::Ptr<const char> token, ee::Ptr<char> out)
    {
        ee::Local<char[0x40]> name;
        ee::Local<char[0x40]> typeName;
        ee::Local<char[0x400]> text;
        ee::Local<ee::Address> found;
        const int32_t kind = resolveObjectPath(token, name, typeName, found);
        if (kind == kWidgetVariablePath)
        {
            widgetVariableText(foundWidget(*found), name, text);
            ee::Ptr<const char> value(zge::stringFind(text, '\t'));
            value = value + (value[1] == '"' ? 2 : 1);
            zge::stringCopy(out, value);
            if (out[zge::stringLength(out) - 1] == '"')
                out[zge::stringLength(out) - 1] = 0;
        }
        else if (kind == kActionVariablePath)
            listedVariableText(actionProperties(foundAction(*found)), name, out);
        else if (kind == kObjectPathLiteral)
        {
            // {text}: the text inside.
            zge::stringCopy(out, token + 1);
            out[zge::stringLength(out) - 1] = 0;
        }
        else
            zge::stringCopy(out, token);
    }

    // tokenValueAtPath: a {literal} is read by the type typeName names (found
    // by name, no case) into a ListedVariable-shaped temporary whose value is
    // `out`; typeName must be given for one.
    void tokenValueAtPath(ee::Ptr<const char> token, ee::Address out, ee::Ptr<char> typeName)
    {
        ee::Local<char[0x40]> name;
        ee::Local<char[0x40]> foundType;
        ee::Local<LiteralTemporary> temporary;
        ee::Local<ee::Address> found;
        const int32_t kind = resolveObjectPath(token, name, foundType, found);
        if (kind == kWidgetVariablePath)
            copyWidgetVariable(foundWidget(*found), name, out);
        else if (kind == kActionVariablePath)
            copyListedVariable(actionProperties(foundAction(*found)), name, out);
        else if (kind == kObjectPathLiteral)
        {
            ee::Ptr<VariableType> type = *g_variableTypes;
            while (type && zge::stringCompareNoCase(type->name, typeName) != 0)
                type = type->next;
            if (!type)
                return;
            temporary->variable.value = out;
            type->setText(classCast<Node>(ee::Ptr<ListedVariable>(temporary.address())), token + 1);
            return;
        }
        else
            return;
        if (typeName)
            zge::stringCopy(typeName, foundType);
    }

    // setTokenValueAtPath: a widget's ZassetID variable set tells the widget
    // to refresh (Message::Refresh from the running script's widget, at once).
    void setTokenValueAtPath(ee::Ptr<const char> token, ee::Address value)
    {
        ee::Local<char[0x40]> name;
        ee::Local<char[0x40]> typeName;
        ee::Local<ee::Address> found;
        const int32_t kind = resolveObjectPath(token, name, typeName, found);
        if (kind == kWidgetVariablePath)
        {
            setWidgetVariable(foundWidget(*found), name, value);
            if (zge::stringCompareNoCase(typeName, kAssetTypeName) == 0)
                zge::sendNewMessage(*g_scriptOwner, *found, static_cast<uint32_t>(Message::Refresh), 0u, zge::kMessageNow);
        }
        else if (kind == kActionVariablePath)
            setListedVariable(actionProperties(foundAction(*found)), name, value);
    }
}

namespace zge::zgui
{
    // commandRandAtPath: ZGE2's RAND destination low high: low and high are
    // the line's second and third tokens (ZGE3 takes the first of what is
    // left each time), read as values of the destination's type; their types
    // are compared with it but nothing comes of it (ZGE3 reports a
    // mismatch), and ZGE2 has no Uint8 type.
    void commandRandAtPath(ee::Ptr<char> arguments)
    {
        ee::Local<char[0x100]> type; // the destination's type's name
        ee::Local<char[0x100]> lowType;
        ee::Local<char[0x100]> highType;
        ee::Local<ScriptValue> result;
        ee::Local<ScriptValue> low;
        ee::Local<ScriptValue> high;
        char *const destinationToken = (*g_tokens)[0];
        char *const lowToken = (*g_tokens)[1];
        char *const highToken = (*g_tokens)[2];
        readToken(arguments, 0, destinationToken);
        type[0] = 0;
        tokenValue(destinationToken, result, type);
        if (type[0] == 0)
            return;
        zge::stringCopy(lowType, type);
        zge::stringCopy(highType, type);
        readToken(arguments, 1, lowToken);
        tokenValue(lowToken, low, lowType);
        readToken(arguments, 2, highToken);
        tokenValue(highToken, high, highType);
        if (zge::compareStrings(lowType, type) == 0)
            zge::compareStrings(highType, type);

        const auto isType = [&](uint32_t name) { return zge::stringCompareNoCase(type, name) == 0; };
        if (isType(kBoolName))
            result->boolean = randomNumber() & 1u;
        else if (isType(kFloatName))
        {
            const uint32_t step = randomNumber() % kFloatSteps;
            result->real = low->real + (high->real - low->real) * ee::toFloat(static_cast<int32_t>(step)) * *g_floatStep;
        }
        else if (isType(kSint32Name))
        {
            const uint32_t random = randomNumber();
            const uint32_t span = static_cast<uint32_t>(high->integer - low->integer + 1);
            result->integer = low->integer + static_cast<int32_t>(random % span);
        }
        setTokenValue(destinationToken, result);
    }
}

// ZGE2's PLAY and QUEUE.
namespace zge::zgui
{
    namespace
    {
        // The ACTION of the widget called `name` (no case), or 0.
        ee::Ptr<ListedAction> widgetActionNamed(ee::Ptr<zge::SceneObject> widget, ee::Ptr<const char> name)
        {
            for (ee::Ptr<zge::ListNode> entry = widgetRecord(widget)->actions.head; entry; entry = entry->next)
            {
                const ee::Ptr<ListedAction> action = zge::recordOfNode<ListedAction>(entry);
                if (zge::stringCompareNoCase(action->name, name) == 0)
                    return action;
            }
            return ee::Ptr<ListedAction>(0u);
        }
    }

    void commandPlayAtPath(ee::Ptr<char> arguments)
    {
        ee::Local<char[0x100]> path;
        ee::Local<char[0x40]> name;
        ee::Local<char[0x40]> typeName;
        ee::Local<ee::Address> found;
        *found = 0u;
        readToken(arguments, 0, path);
        if (resolveObjectPath(path, name, typeName, found) == kObjectPathNotFound)
            return;
        if (const ee::Ptr<ListedAction> action = widgetActionNamed(foundWidget(*found), name))
            zge::sendNewMessage(*g_scriptOwner, *g_root, static_cast<uint32_t>(Message::PlayAction), action, zge::kMessageNow);
    }

    // commandQueue (ZGE2 only): the first path is followed whatever it finds.
    inline void commandQueue(ee::Ptr<char> arguments)
    {
        ee::Local<char[0x100]> path;
        ee::Local<char[0x100]> senderPath;
        ee::Local<char[0x40]> name;
        ee::Local<char[0x40]> senderName;
        ee::Local<char[0x40]> typeName;
        ee::Local<char[0x40]> senderType;
        ee::Local<ee::Address> found;
        ee::Local<ee::Address> sender;
        *found = 0u;
        *sender = 0u;
        readToken(arguments, 0, path);
        senderPath[0] = 0;
        readToken(arguments, 1, senderPath);
        resolveObjectPath(path, name, typeName, found);
        if (senderPath[0])
            resolveObjectPath(senderPath, senderName, senderType, sender);
        if (const ee::Ptr<ListedAction> action = widgetActionNamed(foundWidget(*found), name))
        {
            const ee::Address from = static_cast<uint32_t>(*sender) ? *sender : ee::Address(*g_scriptOwner);
            zge::sendNewMessage(from, *g_root, static_cast<uint32_t>(Message::PlayAction), action, 0u);
        }
    }
    READABLE_FUNCTION_WAITS_ONLY(dm2, commandQueue);

    // commandStopAtPath: ZGE2's STOP name [sender]: the ACTION called by the
    // path's last component, of the widget the path leads to, is sent
    // StopAction at the root, from the widget the second path leads to (the
    // script's owner without one). The first path is followed whatever it
    // finds.
    void commandStopAtPath(ee::Ptr<char> arguments)
    {
        ee::Local<char[0x100]> path;
        ee::Local<char[0x100]> senderPath;
        ee::Local<char[0x40]> name;
        ee::Local<char[0x40]> senderName;
        ee::Local<char[0x40]> typeName;
        ee::Local<char[0x40]> senderType;
        ee::Local<ee::Address> found;
        ee::Local<ee::Address> sender;
        *found = 0u;
        *sender = 0u;
        readToken(arguments, 0, path);
        senderPath[0] = 0;
        readToken(arguments, 1, senderPath);
        resolveObjectPath(path, name, typeName, found);
        if (senderPath[0])
            resolveObjectPath(senderPath, senderName, senderType, sender);
        if (const ee::Ptr<ListedAction> action = widgetActionNamed(foundWidget(*found), name))
        {
            const ee::Address from = (*sender).address ? *sender : ee::Address(*g_scriptOwner);
            zge::sendNewMessage(from, *g_root, static_cast<uint32_t>(Message::StopAction), action, zge::kMessageNow);
        }
    }
}
