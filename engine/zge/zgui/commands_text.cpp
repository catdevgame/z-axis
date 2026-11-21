// zgui's script commands on variables and text (script.h): DECLARE and
// UNDECLARE, DEBUG_PRINT, FORMAT, STRCAT, STRCMP, STRTOKEN, STREXTRACT and
// ROTQUATX/Y/Z. A command gets its arguments' text (the statement after the
// command's name) and reads them as tokens (readToken); names are paths
// (resolvePath), a {"text"} token is a literal.

#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"
#include "zge/zgui/widget_parts.h"

#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/quaternion.h"
#include "zge/math/types.h"
#include "zge/text/strings.h"
#include "zge/text/cstring.h"
#include "sce/libgcc.h"

#include <cstring>
#include "sym/zge/zgui/commands_text.cpp.h"

namespace zge::zgui
{
    namespace
    {
        // A buffer the command wrote, read as text.
        ee::Ptr<const char> in(ee::Ptr<char> text) { return text.cast<const char>(); }

        constexpr uint32_t kHeapTake = sym::heapTake;   // (heap, size) -> a block (engine)
        constexpr uint32_t kMemset = sym::memset;     // libc memset (dst, byte, bytes)
        constexpr uint32_t kValueText = sym::valueText;  // valueText, called by address
        constexpr uint32_t kSetVariableAt = sym::setVariable; // setVariable, called by address (a game override hooks it)
        constexpr uint32_t kSetTokenValueAt = sym::setTokenValue; // setTokenValue, called by address

        // The type names the commands test (case-insensitive).
        constexpr uint32_t kBoolName = sym::boolName, kFloatName = sym::floatName, kUint8Name = sym::uint8Name,
                           kSint32Name = sym::sint32Name, kStringName = sym::stringName, kEventName = sym::eventTypeName,
                           kAssetName = sym::assetName; // "ZassetID"

        // zgui's text buffers: the shared one (0x2000 bytes) and the one
        // after it, and the commands' token buffers.
        constexpr uint32_t kSharedText = sym::sharedText;
        inline constexpr ee::Ptr<char> g_sharedText{sym::sharedText};
        inline ee::Global<char, sym::g_commandFormatInitializer0> g_emptyFormat; // ""
        constexpr uint32_t kSecondText = sym::secondText;
        constexpr uint32_t kTokenBuffer = sym::tokenBuffer;
        constexpr uint32_t kArgumentBuffer = sym::argumentBuffer;
        constexpr uint32_t kBufferHigh = ee::high(sym::tokenBuffer); // the token buffers' high half (a register FORMAT keeps)
        constexpr uint32_t kStringsHigh = ee::high(sym::notTextDestination); // the messages' high half

        // Messages.
        constexpr uint32_t kPropertyNotFound = sym::propertyNotFound;   // "zgui: (Property '%s' not found)"
        constexpr uint32_t kNotTextDestination = sym::notTextDestination; // "zgui: (Destination must be string,event or assetID)"
        constexpr uint32_t kBadSource = sym::badSource;          // "zgui: (Invalid Source Identifier: '%s')"
        constexpr uint32_t kBadDestination = sym::badDestination;     // "zgui: (Invalid Destination Identifier: '%s')"
        constexpr uint32_t kResultNotInteger = sym::resultNotInteger;   // "zgui: (Result must be an integer)"
        constexpr uint32_t kNotTextDestination2 = sym::notTextDestination2; // "zgui: (Destination must be a string,event or assetID)"
        constexpr uint32_t kBadSource2 = sym::badSource2;         // "zgui: (Invalid Source identifier '%s')"
        constexpr uint32_t kBadDestination2 = sym::badDestination2;    // "zgui: (Invalid Destintion identifier '%s')"
        constexpr uint32_t kRangeNotIntegers = kStringsHigh + 0x41D0u;  // "zgui: (Range must be integers)"
        constexpr uint32_t kNotStrings = kStringsHigh + 0x41F0u;        // "zgui: (Source and Destination must be strings)"
        constexpr uint32_t kBadSource3 = sym::badSource3;         // "zgui: (Invalid Source Identifier '%s')"
        constexpr uint32_t kBadDestination3 = sym::badDestination3;    // "zgui: (Invalid Destination Identifier '%s')"

        bool isTextType(ee::Ptr<char> typeName)
        {
            // String, Event or ZAssetID (the three tests in turn).
            if (zge::stringCompareNoCase(typeName, kStringName) == 0)
                return true;
            if (zge::stringCompareNoCase(typeName, kEventName) == 0)
                return true;
            return zge::stringCompareNoCase(typeName, kAssetName) == 0;
        }

        ee::Ptr<Node> parentOf(ee::Ptr<Node> node) { return node->parent; }
    }

    namespace
    {
        uint32_t heapTake(uint32_t heap, uint32_t bytes) { return ee::game<uint32_t>(kHeapTake, heap, bytes); }
    }

    void commandDeclare(ee::Ptr<char> arguments)
    {
        // DECLARE type path: a variable of the type at path (its last
        // component the new name), unless something is there already; its
        // value a zeroed word on zgui's heap.
        ee::Local<char> type;
        ee::Local<char> path;
        ee::Local<char> name;
        ee::Local<ee::Ptr<Node>> parent;
        const ee::Ptr<char> rest(readToken(arguments, 0, ee::Ptr<char>(type.address())));
        readToken(rest, 0, ee::Ptr<char>(path.address()));
        if (resolvePath(in(ee::Ptr<char>(path.address())), ee::Ptr<char>(name.address()), ee::Ptr<char>(0u), parent) == kPathNotFound)
        {
            const ee::Ptr<uint32_t> value(heapTake(*g_heap, 4u));
            *value = 0;
            declareVariable(ee::Ptr<Node>(*parent), in(ee::Ptr<char>(name.address())), in(ee::Ptr<char>(type.address())), value);
        }
    }
    READABLE_FUNCTION_WAITS_OWN(commandDeclare, Verified, dm2, commandDeclareAtPath);

    void commandUndeclare(ee::Ptr<char> arguments)
    {
        // UNDECLARE path: the variable destroyed.
        ee::Local<char> path;
        ee::Local<char> name;
        ee::Local<ee::Ptr<Node>> variable;
        readToken(arguments, 0, ee::Ptr<char>(path.address()));
        if (resolvePath(in(ee::Ptr<char>(path.address())), ee::Ptr<char>(name.address()), ee::Ptr<char>(0u), variable) == kVariableKind)
            destroyNode(ee::Ptr<Node>(*variable));
        else
            report(kPropertyNotFound, name);
    }
    READABLE_FUNCTION_WAITS_OWN(commandUndeclare, Verified, dm2, commandUndeclareAtPath);

    void commandDebugPrint(ee::Ptr<char> arguments)
    {
        // DEBUG_PRINT value: the value's text made (the release build prints
        // nothing); ZGE4 reports no value that is not found.
        ee::Local<char> token;
        ee::Local<char> text;
        *text = 0;
        readToken(arguments, 0, ee::Ptr<char>(token.address()));
        valueText(ee::Ptr<char>(token.address()), ee::Ptr<char>(text.address()), game::readable::perGame(1, 1, 0));
    }
    READABLE_FUNCTION_WAITS(commandDebugPrint, Verified);

    namespace
    {
        void memset(ee::Ptr<char> m, uint32_t c, uint32_t n) { ee::game(kMemset, m, c, n); }
    }

    void commandFormat(ee::Ptr<char> arguments)
    {
        // FORMAT destination source format: the source's value (an integer
        // type, or ZFloat as a double) through the printf format into the
        // text variable at destination.
        ee::Local<uint32_t> value;
        ee::Local<char> destinationPath;
        ee::Local<char> sourcePath;
        ee::Local<char> formatToken;
        ee::Local<char> destinationName;
        ee::Local<char> sourceName;
        ee::Local<char> destinationType;
        ee::Local<char> sourceType;
        ee::Local<char> format;
        ee::Local<ee::Ptr<Node>> destination;
        ee::Local<ee::Ptr<Node>> source;
        const uint32_t out = kTokenBuffer;
        *format = *g_emptyFormat;
        memset(ee::Ptr<char>(format.address()) + 1, 0, 63);
        const ee::Ptr<char> rest(readToken(arguments, 0, ee::Ptr<char>(destinationPath.address())));
        const ee::Ptr<char> rest2(readToken(rest, 0, ee::Ptr<char>(sourcePath.address())));
        readToken(rest2, 0, ee::Ptr<char>(formatToken.address()));
        resolvePath(in(ee::Ptr<char>(destinationPath.address())), ee::Ptr<char>(destinationName.address()), ee::Ptr<char>(destinationType.address()), destination);
        resolvePath(in(ee::Ptr<char>(sourcePath.address())), ee::Ptr<char>(sourceName.address()), ee::Ptr<char>(sourceType.address()), source);
        valueText(in(ee::Ptr<char>(formatToken.address())), ee::Ptr<char>(format.address()), 1);
        if (!isTextType(destinationType))
        {
            report(kNotTextDestination);
            return;
        }
        tokenValue(in(ee::Ptr<char>(sourcePath.address())), ee::Ptr<char>(value.address()), ee::Ptr<char>(sourceType.address()));
        // The integer types: "%d"-like formats of the value.
        bool integer = zge::stringCompareNoCase(sourceType, kBoolName) == 0;
        if (!integer)
        {
            integer = zge::stringCompareNoCase(sourceType, kSint32Name) == 0;
            if (!integer)
                integer = zge::stringCompareNoCase(sourceType, kUint8Name) == 0;
        }
        if (integer)
            zge::formatString(out, format, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(*value))));
        // ZFloat: the value as a double.
        if (zge::stringCompareNoCase(sourceType, kFloatName) == 0)
        {
            const uint64_t asDouble = sce::libgcc::fptodp(*ee::Ptr<ee::Float>(value.address()));
            zge::formatString(out, format, asDouble);
        }
        setVariableText(parentOf(*destination), ee::Ptr<const char>(destinationName.address()), ee::Ptr<const char>(out));
    }
    READABLE_FUNCTION_WAITS_OWN(commandFormat, Verified, dm2, commandFormatAtPath);

    // commandFormatAtPath: ZGE2's FORMAT destination source format: the
    // source's value (Bool or Sint32 as an integer, ZFloat as a double)
    // through the printf format into the text variable of the widget the
    // destination leads to. Nothing is reported; ZGE2 has no Uint8.
    void commandFormatAtPath(ee::Ptr<char> arguments)
    {
        ee::Local<ScriptValue> value;
        ee::Local<char[0x100]> destinationPath;
        ee::Local<char[0x100]> sourcePath;
        ee::Local<char[0x100]> formatToken;
        ee::Local<char[0x40]> destinationName;
        ee::Local<char[0x40]> sourceName;
        ee::Local<char[0x40]> destinationType;
        ee::Local<char[0x40]> sourceType;
        ee::Local<char[0x40]> format;
        ee::Local<ee::Address> destination;
        ee::Local<ee::Address> source;
        const ee::Ptr<char> out(kTokenBuffer);
        format[0] = *g_emptyFormat;
        fillBytes(ee::Ptr<char>(format) + 1, 0, 63);
        readToken(arguments, 0, destinationPath);
        readToken(arguments, 1, sourcePath);
        readToken(arguments, 2, formatToken);
        resolveObjectPath(destinationPath, destinationName, destinationType, destination);
        resolveObjectPath(sourcePath, sourceName, sourceType, source);
        valueText(formatToken, format, 1);
        if (!isTextType(destinationType))
            return;
        tokenValue(sourcePath, value, sourceType);
        // The integer types: "%d"-like formats of the value.
        if (zge::stringCompareNoCase(sourceType, kBoolName) == 0 || zge::stringCompareNoCase(sourceType, kSint32Name) == 0)
            zge::formatString(out, format, static_cast<uint64_t>(static_cast<int64_t>(value->integer)));
        // ZFloat: the value as a double.
        if (zge::stringCompareNoCase(sourceType, kFloatName) == 0)
            zge::formatString(out, format, sce::libgcc::fptodp(value->real));
        setWidgetVariableText(foundWidget(*destination), destinationName, out);
    }

    void commandStrCat(ee::Ptr<char> arguments)
    {
        // STRCAT destination source: the source's text (a variable, or a
        // {"literal"}) appended to the text variable at destination.
        ee::Local<char> destinationPath;
        ee::Local<char> sourcePath;
        ee::Local<char> destinationName;
        ee::Local<char> sourceName;
        ee::Local<char> destinationType;
        ee::Local<char> sourceType;
        ee::Local<ee::Ptr<Node>> destination;
        ee::Local<ee::Ptr<Node>> source;
        const ee::Ptr<char> rest(readToken(arguments, 0, ee::Ptr<char>(destinationPath.address())));
        readToken(rest, 0, ee::Ptr<char>(sourcePath.address()));
        const int32_t destinationKind = resolvePath(in(ee::Ptr<char>(destinationPath.address())), ee::Ptr<char>(destinationName.address()), ee::Ptr<char>(destinationType.address()), destination);
        zge::stringCopy(sourceType, destinationType); // the source's type by default
        const int32_t sourceKind = resolvePath(in(ee::Ptr<char>(sourcePath.address())), ee::Ptr<char>(sourceName.address()), ee::Ptr<char>(sourceType.address()), source);
        if (!isTextType(destinationType))
        {
            report(kNotTextDestination);
            return;
        }
        if (destinationKind != kVariableKind)
        {
            report(kBadDestination, destinationPath);
            return;
        }
        variableText(parentOf(*destination), ee::Ptr<const char>(destinationName.address()), ee::Ptr<char>(kSharedText));
        if (sourceKind == destinationKind)
        {
            // A variable's text.
            variableText(parentOf(*source), ee::Ptr<const char>(sourceName.address()), ee::Ptr<char>(kSecondText));
            zge::appendString(kSharedText, kSecondText);
        }
        else
        {
            if (sourceKind != kPathLiteral)
            {
                report(kBadSource, sourcePath);
                return;
            }
            // A {"literal"}: its text without the braces and quotes.
            zge::appendString(kSharedText, sourcePath.address() + 2u);
            const int32_t length = zge::stringLength(kSharedText);
            g_sharedText[length - 2] = 0; // the closing quote and brace
        }
        setVariableText(parentOf(*destination), ee::Ptr<const char>(destinationName.address()), ee::Ptr<const char>(kSharedText));
    }
    READABLE_FUNCTION_WAITS_OWN(commandStrCat, Verified, dm2, commandStrCatAtPath);

    // commandStrCatAtPath: ZGE2's STRCAT destination source: the source's
    // text (a widget's variable, an ACTION's, or a {"literal"}) appended to
    // the text variable at destination (a widget's or an ACTION's), each
    // taken as its toText gives it, without the quotes round it. A widget's
    // ZassetID tells its widget (Refresh). Nothing is reported. For an
    // ACTION's variable and a literal the original sets the variable from
    // the literal's token, not from the joined text.
    void commandStrCatAtPath(ee::Ptr<char> arguments)
    {
        ee::Local<char[0x100]> destinationPath;
        ee::Local<char[0x100]> sourcePath;
        ee::Local<char[0x40]> destinationName;
        ee::Local<char[0x40]> sourceName;
        ee::Local<char[0x40]> destinationType;
        ee::Local<char[0x40]> sourceType;
        ee::Local<char[0x400]> valueText; // a variable's text as its toText gives it
        ee::Local<char[0x400]> joined;
        ee::Local<ee::Address> destination;
        ee::Local<ee::Address> source;
        constexpr char kQuote = '"', kTab = '\t';
        readToken(arguments, 0, destinationPath);
        readToken(arguments, 1, sourcePath);
        const int32_t destinationKind = resolveObjectPath(destinationPath, destinationName, destinationType, destination);
        zge::stringCopy(sourceType, destinationType); // the source's type by default
        const int32_t sourceKind = resolveObjectPath(sourcePath, sourceName, sourceType, source);
        if (!isTextType(destinationType))
            return;

        // The source's text after its opening quote: a widget's variable
        // ("name<TAB>value") or an ACTION's (the value alone).
        const auto widgetSourceText = [&]() {
            widgetVariableText(foundWidget(*source), sourceName, valueText);
            const ee::Ptr<char> tab(zge::stringFind(valueText, kTab));
            return tab[1] != kQuote ? tab + 1 : tab + 2;
        };
        const auto actionSourceText = [&]() {
            listedVariableText(actionProperties(foundAction(*source)), sourceName, valueText);
            return valueText[0] != kQuote ? ee::Ptr<char>(valueText) : ee::Ptr<char>(valueText) + 1;
        };
        // Appended, and the closing quote taken off.
        const auto append = [&](ee::Ptr<char> text) {
            zge::appendString(joined, text);
            const ee::Ptr<char> last = ee::Ptr<char>(joined) + (zge::stringLength(joined) - 1);
            if (*last == kQuote)
                *last = 0;
        };
        // A {"literal"}: its text without the braces and quotes.
        const auto appendLiteral = [&]() {
            zge::appendString(joined, ee::Ptr<char>(sourcePath) + 2);
            joined[zge::stringLength(joined) - 2] = 0;
        };

        if (destinationKind == kWidgetVariablePath)
        {
            widgetVariableText(foundWidget(*destination), destinationName, valueText);
            const ee::Ptr<char> tab(zge::stringFind(valueText, kTab));
            zge::stringCopy(joined, tab + 2);
            joined[zge::stringLength(joined) - 1] = 0;
            if (sourceKind == kWidgetVariablePath || sourceKind == kActionVariablePath)
            {
                append(sourceKind == kWidgetVariablePath ? widgetSourceText() : actionSourceText());
                setWidgetVariableText(foundWidget(*destination), destinationName, joined);
            }
            else if (sourceKind == kObjectPathLiteral)
            {
                appendLiteral();
                setWidgetVariableText(foundWidget(*destination), destinationName, joined);
            }
            if (zge::stringCompareNoCase(destinationType, kAssetName) == 0)
                zge::sendNewMessage(*g_scriptOwner, *destination, static_cast<uint32_t>(Message::Refresh), 0u, zge::kMessageNow);
        }
        else if (destinationKind == kActionVariablePath)
        {
            const ee::Ptr<zge::List> properties = actionProperties(foundAction(*destination));
            listedVariableText(properties, destinationName, valueText);
            zge::stringCopy(joined, ee::Ptr<char>(valueText) + 1);
            joined[zge::stringLength(joined) - 1] = 0;
            if (sourceKind == kWidgetVariablePath || sourceKind == kActionVariablePath)
            {
                append(sourceKind == kWidgetVariablePath ? widgetSourceText() : actionSourceText());
                setListedVariableText(actionProperties(foundAction(*destination)), destinationName, joined);
            }
            else if (sourceKind == kObjectPathLiteral)
            {
                appendLiteral();
                setListedVariableText(actionProperties(foundAction(*destination)), destinationName, ee::Ptr<char>(sourcePath) + 1);
            }
        }
    }

    void commandStrCmp(ee::Ptr<char> arguments)
    {
        // STRCMP a b result: strcmp of a's text (a variable) and b's (a
        // variable or a {"literal"}) into the Sint32 variable result.
        ee::Local<char> aPath;
        ee::Local<char> bPath;
        ee::Local<char> resultPath;
        ee::Local<char> aName;
        ee::Local<char> bName;
        ee::Local<char> resultName;
        ee::Local<char> aType;
        ee::Local<char> bType;
        ee::Local<char> resultType;
        ee::Local<ee::Ptr<Node>> a;
        ee::Local<ee::Ptr<Node>> b;
        ee::Local<ee::Ptr<Node>> result;
        ee::Local<int> comparison;
        ee::Local<int> resultKind;
        *comparison = 0;
        const ee::Ptr<char> rest(readToken(arguments, 0, ee::Ptr<char>(aPath.address())));
        const ee::Ptr<char> rest2(readToken(rest, 0, ee::Ptr<char>(bPath.address())));
        readToken(rest2, 0, ee::Ptr<char>(resultPath.address()));
        const int32_t aKind = resolvePath(in(ee::Ptr<char>(aPath.address())), ee::Ptr<char>(aName.address()), ee::Ptr<char>(aType.address()), a);
        const int32_t bKind = resolvePath(in(ee::Ptr<char>(bPath.address())), ee::Ptr<char>(bName.address()), ee::Ptr<char>(bType.address()), b);
        *resultKind = resolvePath(in(ee::Ptr<char>(resultPath.address())), ee::Ptr<char>(resultName.address()), ee::Ptr<char>(resultType.address()), result);
        if (zge::stringCompareNoCase(resultType, kSint32Name) != 0)
        {
            report(kResultNotInteger);
            return;
        }
        if (!isTextType(aType))
        {
            report(kNotTextDestination2);
            return;
        }
        bool reported = false;
        if (aKind == kVariableKind)
        {
            variableText(parentOf(*a), ee::Ptr<const char>(aName.address()), ee::Ptr<char>(kSharedText));
            uint32_t bText = 0;
            if (bKind == aKind)
            {
                variableText(parentOf(*b), ee::Ptr<const char>(bName.address()), ee::Ptr<char>(kSecondText));
                bText = kSecondText;
            }
            else
            {
                if (bKind != kPathLiteral)
                {
                    report(kBadSource2, bPath);
                    reported = true;
                }
                else
                {
                    const int32_t length = zge::stringLength(bPath);
                    ee::Ptr<char>(bPath.address())[length - 2] = 0;
                    bText = bPath.address() + 2u;
                }
            }
            if (!reported)
                *comparison = zge::compareStrings(kSharedText, bText);
        }
        else
            report(kBadDestination2, aPath);
        if (*resultKind == kVariableKind)
            setVariable(parentOf(*result), ee::Ptr<char>(resultName.address()), ee::Ptr<int>(comparison.address()));
    }
    // Draft: no UI script of the nine releases uses STRCMP (never recorded).
    READABLE_FUNCTION_WAITS_OWN(commandStrCmp, Draft, dm2, commandStrCmpAtPath);

    // commandStrCmpAtPath: ZGE2's STRCMP a b result: a's text (a widget's or
    // an ACTION's text variable) compared with b's (a variable's or a
    // {"literal"}), each without the quotes round it, and the result (as
    // compareStrings gives it; 0 without a comparable b) set as the Sint32 at
    // result, a widget's variable. A widget's ZassetID a is told to refresh
    // (as STRCAT's destination is). Nothing is reported.
    void commandStrCmpAtPath(ee::Ptr<char> arguments)
    {
        ee::Local<char[0x100]> leftPath;
        ee::Local<char[0x100]> rightPath;
        ee::Local<char[0x100]> resultPath;
        ee::Local<char[0x40]> leftName;
        ee::Local<char[0x40]> rightName;
        ee::Local<char[0x40]> resultName;
        ee::Local<char[0x40]> leftType;
        ee::Local<char[0x40]> rightType;
        ee::Local<char[0x40]> resultType;
        ee::Local<char[0x400]> valueText; // a variable's text as its toText gives it
        ee::Local<char[0x400]> left;
        ee::Local<ee::Address> leftFound;
        ee::Local<ee::Address> rightFound;
        ee::Local<ee::Address> resultFound;
        ee::Local<int32_t> result;
        constexpr char kQuote = '"', kTab = '\t';
        *result = 0;
        readToken(arguments, 0, leftPath);
        readToken(arguments, 1, rightPath);
        readToken(arguments, 2, resultPath);
        const int32_t leftKind = resolveObjectPath(leftPath, leftName, leftType, leftFound);
        const int32_t rightKind = resolveObjectPath(rightPath, rightName, rightType, rightFound);
        resolveObjectPath(resultPath, resultName, resultType, resultFound);
        if (zge::stringCompareNoCase(resultType, kSint32Name) != 0)
            return;
        if (!isTextType(leftType))
            return;

        // b's text after its opening quote, its closing quote taken off, compared.
        const auto compareWith = [&](ee::Ptr<char> right) {
            const ee::Ptr<char> last = right + (zge::stringLength(right) - 1);
            if (*last == kQuote)
                *last = 0;
            *result = zge::compareStrings(left, right);
        };
        const auto compareRight = [&]() {
            if (rightKind == kWidgetVariablePath)
            {
                widgetVariableText(foundWidget(*rightFound), rightName, valueText);
                const ee::Ptr<char> tab(zge::stringFind(valueText, kTab));
                compareWith(tab[1] != kQuote ? tab + 1 : tab + 2);
            }
            else if (rightKind == kActionVariablePath)
            {
                listedVariableText(actionProperties(foundAction(*rightFound)), rightName, valueText);
                compareWith(valueText[0] != kQuote ? ee::Ptr<char>(valueText) : ee::Ptr<char>(valueText) + 1);
            }
            else if (rightKind == kObjectPathLiteral)
            {
                // A {"literal"}: its text without the braces and quotes.
                rightPath[zge::stringLength(rightPath) - 2] = 0;
                *result = zge::compareStrings(left, ee::Ptr<char>(rightPath) + 2);
            }
        };

        if (leftKind == kWidgetVariablePath)
        {
            widgetVariableText(foundWidget(*leftFound), leftName, valueText);
            const ee::Ptr<char> tab(zge::stringFind(valueText, kTab));
            zge::stringCopy(left, tab + 2);
            left[zge::stringLength(left) - 1] = 0;
            compareRight();
            if (zge::stringCompareNoCase(leftType, kAssetName) == 0)
                zge::sendNewMessage(*g_scriptOwner, *leftFound, static_cast<uint32_t>(Message::Refresh), 0u, zge::kMessageNow);
        }
        else if (leftKind == kActionVariablePath)
        {
            listedVariableText(actionProperties(foundAction(*leftFound)), leftName, valueText);
            zge::stringCopy(left, ee::Ptr<char>(valueText) + 1);
            left[zge::stringLength(left) - 1] = 0;
            compareRight();
        }
        setWidgetVariable(foundWidget(*resultFound), resultName, result);
    }

    void commandStrToken(ee::Ptr<char> arguments)
    {
        // STRTOKEN destination source line: line `line` (0 the first; lines
        // end at a carriage return, a line feed after it skipped) of the
        // source's text (a variable or a {"literal"}) into the String
        // variable at destination ("" past the end).
        ee::Local<char> destinationPath;
        ee::Local<char> sourcePath;
        ee::Local<char> linePath;
        ee::Local<char> destinationName;
        ee::Local<char> sourceName;
        ee::Local<char> lineName;
        ee::Local<char> destinationType;
        ee::Local<char> sourceType;
        ee::Local<char> lineType;
        ee::Local<int> lineValue;
        ee::Local<ee::Ptr<Node>> destination;
        ee::Local<ee::Ptr<Node>> source;
        ee::Local<ee::Ptr<Node>> lineNode;
        ee::Local<ee::Ptr<char>> destinationNameAt;
        g_sharedText[0] = 0;
        const ee::Ptr<char> rest(readToken(arguments, 0, ee::Ptr<char>(destinationPath.address())));
        const ee::Ptr<char> rest2(readToken(rest, 0, ee::Ptr<char>(sourcePath.address())));
        readToken(rest2, 0, ee::Ptr<char>(linePath.address()));
        *destinationNameAt = destinationName;
        const int32_t destinationKind = resolvePath(in(ee::Ptr<char>(destinationPath.address())), ee::Ptr<char>(destinationName.address()), ee::Ptr<char>(destinationType.address()), destination);
        const int32_t sourceKind = resolvePath(in(ee::Ptr<char>(sourcePath.address())), ee::Ptr<char>(sourceName.address()), ee::Ptr<char>(sourceType.address()), source);
        zge::stringCopy(lineType, kSint32Name); // a literal line is a Sint32
        resolvePath(in(ee::Ptr<char>(linePath.address())), ee::Ptr<char>(lineName.address()), ee::Ptr<char>(lineType.address()), lineNode);
        if (zge::stringCompareNoCase(lineType, kSint32Name) != 0)
        {
            report(kRangeNotIntegers);
            return;
        }
        tokenValue(in(ee::Ptr<char>(linePath.address())), ee::Ptr<char>(lineValue.address()), ee::Ptr<char>(lineType.address()));
        int32_t line = *lineValue;
        bool strings = zge::stringCompareNoCase(destinationType, kStringName) == 0;
        if (strings)
            strings = zge::stringCompareNoCase(sourceType, kStringName) == 0;
        if (!strings)
        {
            report(kNotStrings);
            return;
        }
        if (destinationKind == kVariableKind)
        {
            // The source's text and length.
            int32_t length = 0;
            uint32_t text = 0;
            if (sourceKind == destinationKind)
            {
                text = kSecondText;
                variableText(parentOf(*source), ee::Ptr<const char>(sourceName.address()), ee::Ptr<char>(kSecondText));
                length = zge::stringLength(text);
            }
            else
            {
                if (sourceKind == kPathLiteral)
                {
                    const int32_t literal = zge::stringLength(sourcePath);
                    text = sourcePath.address() + 2u;
                    length = literal - 4;
                    ee::Ptr<char>(sourcePath.address())[literal - 2] = 0;
                }
                else
                    report(kBadSource3, sourcePath);
            }
            g_sharedText[0] = 0;
            if (length > 0 && text)
            {
                if (line < 0 || length - 1 < line)
                {
                    text = 0; // past the text: ""
                }
                else
                {
                    // Past `line` carriage returns.
                    ee::Ptr<char> at(text);
                    int8_t c = static_cast<int8_t>(*at);
                    for (;;)
                    {
                        if (c == 0 || line == 0)
                            break;
                        const int8_t passed = c;
                        ++at;
                        c = static_cast<int8_t>(*at);
                        if (passed == '\r')
                            --line;
                    }
                    if (c == '\n')
                        ++at;
                    if (*at != 0)
                    {
                        // The line, to the next carriage return or the end.
                        ee::Ptr<char> end = at;
                        if (*end != '\r')
                        {
                            ++end;
                            for (;;)
                            {
                                if (*end == 0 || *end == '\r')
                                    break;
                                ++end;
                            }
                        }
                        if (end != at)
                        {
                            const uint32_t bytes = static_cast<uint32_t>(&*end - &*at);
                            zge::stringCopyN(kSharedText, at, bytes);
                            g_sharedText[bytes] = 0;
                        }
                    }
                }
            }
        }
        else
            report(kBadDestination3, destinationPath);
        setVariable(parentOf(*destination), *destinationNameAt, kSharedText);
    }
    READABLE_FUNCTION_WAITS(commandStrToken, Verified);

    void commandStrExtract(ee::Ptr<char> arguments)
    {
        // STREXTRACT destination source first last: characters first ..
        // last (last clipped to the text's end) of the source's text (a
        // variable or a {"literal"}) into the String variable at destination
        // ("" for a range outside the text).
        ee::Local<char> destinationPath;
        ee::Local<char> sourcePath;
        ee::Local<char> firstPath;
        ee::Local<char> lastPath;
        ee::Local<char> destinationName;
        ee::Local<char> sourceName;
        ee::Local<char> firstName;
        ee::Local<char> lastName;
        ee::Local<char> destinationType;
        ee::Local<char> sourceType;
        ee::Local<char> firstType;
        ee::Local<char> lastType;
        ee::Local<int> firstValue;
        ee::Local<int> lastValue;
        ee::Local<ee::Ptr<Node>> destination;
        ee::Local<ee::Ptr<Node>> source;
        ee::Local<ee::Ptr<Node>> node;
        ee::Local<ee::Ptr<Node>> node2;
        ee::Local<int> destinationKindAt;
        ee::Local<ee::Ptr<char>> destinationNameAt;
        ee::Local<ee::Ptr<char>> sourceNameAt;
        // (The original keeps these in its frame: the destination's kind and
        // the two names' addresses.)
        g_sharedText[0] = 0;
        const ee::Ptr<char> rest(readToken(arguments, 0, ee::Ptr<char>(destinationPath.address())));
        const ee::Ptr<char> rest2(readToken(rest, 0, ee::Ptr<char>(sourcePath.address())));
        const ee::Ptr<char> rest3(readToken(rest2, 0, ee::Ptr<char>(firstPath.address())));
        readToken(rest3, 0, ee::Ptr<char>(lastPath.address()));
        *destinationNameAt = destinationName;
        const int32_t destinationKind = resolvePath(in(ee::Ptr<char>(destinationPath.address())), ee::Ptr<char>(destinationName.address()), ee::Ptr<char>(destinationType.address()), destination);
        *sourceNameAt = sourceName;
        *destinationKindAt = destinationKind;
        const int32_t sourceKind = resolvePath(in(ee::Ptr<char>(sourcePath.address())), *sourceNameAt, ee::Ptr<char>(sourceType.address()), source);
        zge::stringCopy(firstType, kSint32Name); // literal bounds are Sint32s
        zge::stringCopy(lastType, kSint32Name);
        resolvePath(in(ee::Ptr<char>(firstPath.address())), ee::Ptr<char>(firstName.address()), ee::Ptr<char>(firstType.address()), node);
        resolvePath(in(ee::Ptr<char>(lastPath.address())), ee::Ptr<char>(lastName.address()), ee::Ptr<char>(lastType.address()), node2);
        bool integers = zge::stringCompareNoCase(firstType, kSint32Name) == 0;
        if (integers)
            integers = zge::stringCompareNoCase(lastType, kSint32Name) == 0;
        if (!integers)
        {
            report(kRangeNotIntegers);
            return;
        }
        tokenValue(in(ee::Ptr<char>(firstPath.address())), ee::Ptr<char>(firstValue.address()), ee::Ptr<char>(firstType.address()));
        tokenValue(in(ee::Ptr<char>(lastPath.address())), ee::Ptr<char>(lastValue.address()), ee::Ptr<char>(lastType.address()));
        const int32_t first = *firstValue;
        int32_t last = *lastValue;
        bool strings = zge::stringCompareNoCase(destinationType, kStringName) == 0;
        if (strings)
            strings = zge::stringCompareNoCase(sourceType, kStringName) == 0;
        if (!strings)
        {
            report(kNotStrings);
            return;
        }
        if (*destinationKindAt == kVariableKind)
        {
            int32_t length = 0;
            uint32_t text = 0;
            if (sourceKind == *destinationKindAt)
            {
                text = kSecondText;
                variableText(parentOf(*source), *sourceNameAt, ee::Ptr<char>(kSecondText));
                length = zge::stringLength(text);
            }
            else
            {
                if (sourceKind == kPathLiteral)
                {
                    const int32_t literal = zge::stringLength(sourcePath);
                    text = sourcePath.address() + 2u;
                    length = literal - 4;
                    ee::Ptr<char>(sourcePath.address())[literal - 2] = 0;
                }
                else
                    report(kBadSource3, sourcePath);
            }
            g_sharedText[0] = 0;
            if (length > 0 && text)
            {
                const int32_t lastCharacter = length - 1;
                if (lastCharacter < last)
                    last = lastCharacter;
                if (last < 0 || first < 0 || last < first || lastCharacter < first)
                    text = 0;
                const int32_t span = last - first;
                if (text)
                {
                    zge::stringCopyN(kSharedText, text + static_cast<uint32_t>(first), static_cast<uint32_t>(span + 1));
                    g_sharedText[static_cast<uint32_t>(span) + 1u] = 0;
                }
            }
        }
        else
            report(kBadDestination3, destinationPath);
        setVariable(parentOf(*destination), *destinationNameAt, kSharedText);
    }
    READABLE_FUNCTION_WAITS_OWN(commandStrExtract, Verified, dm2, commandStrExtractAtPath);

    // commandStrExtractAtPath: ZGE2's STREXTRACT destination source first
    // last: the source's characters first..last (two Sint32 values; last
    // kept inside the text) set as the String at destination, a widget's
    // variable. The source is a widget's String (its text without the quotes
    // round it) or a {"literal"}. An empty source, a range outside it or
    // back to front, or an ACTION's destination gives the empty text.
    // Nothing is reported.
    void commandStrExtractAtPath(ee::Ptr<char> arguments)
    {
        ee::Local<char[0x100]> destinationPath;
        ee::Local<char[0x100]> sourcePath;
        ee::Local<char[0x100]> firstPath;
        ee::Local<char[0x100]> lastPath;
        ee::Local<char[0x40]> destinationName;
        ee::Local<char[0x40]> sourceName;
        ee::Local<char[0x40]> firstName;
        ee::Local<char[0x40]> lastName;
        ee::Local<char[0x40]> destinationType;
        ee::Local<char[0x40]> sourceType;
        ee::Local<char[0x40]> firstType;
        ee::Local<char[0x40]> lastType;
        ee::Local<char[0x400]> text;
        ee::Local<ScriptValue> firstValue;
        ee::Local<ScriptValue> lastValue;
        ee::Local<ee::Address> destination;
        ee::Local<ee::Address> source;
        ee::Local<ee::Address> firstFound;
        ee::Local<ee::Address> lastFound;
        constexpr char kQuote = '"', kTab = '\t';
        text[0] = 0;
        readToken(arguments, 0, destinationPath);
        readToken(arguments, 1, sourcePath);
        readToken(arguments, 2, firstPath);
        readToken(arguments, 3, lastPath);
        const int32_t destinationKind = resolveObjectPath(destinationPath, destinationName, destinationType, destination);
        const int32_t sourceKind = resolveObjectPath(sourcePath, sourceName, sourceType, source);
        zge::stringCopy(firstType, kSint32Name); // a literal's type by default
        zge::stringCopy(lastType, kSint32Name);
        resolveObjectPath(firstPath, firstName, firstType, firstFound);
        resolveObjectPath(lastPath, lastName, lastType, lastFound);
        if (zge::stringCompareNoCase(firstType, kSint32Name) != 0 || zge::stringCompareNoCase(lastType, kSint32Name) != 0)
            return;
        tokenValue(firstPath, firstValue, firstType);
        tokenValue(lastPath, lastValue, lastType);
        const int32_t first = firstValue->integer;
        int32_t last = lastValue->integer;
        if (zge::stringCompareNoCase(destinationType, kStringName) != 0 || zge::stringCompareNoCase(sourceType, kStringName) != 0)
            return;
        if (destinationKind == kWidgetVariablePath)
        {
            int32_t length = 0;
            ee::Ptr<char> from;
            if (sourceKind == kWidgetVariablePath)
            {
                widgetVariableText(foundWidget(*source), sourceName, text);
                const ee::Ptr<char> tab(zge::stringFind(text, kTab));
                from = tab[1] != kQuote ? tab + 1 : tab + 2;
                length = zge::stringLength(from);
                if (from[length - 1] == kQuote)
                {
                    length -= 1;
                    from[length] = 0;
                }
            }
            else if (sourceKind == kObjectPathLiteral)
            {
                // A {"literal"}: its text without the braces and quotes.
                const int32_t tokenLength = zge::stringLength(sourcePath);
                from = ee::Ptr<char>(sourcePath) + 2;
                length = tokenLength - 4;
                sourcePath[tokenLength - 2] = 0;
            }
            text[0] = 0;
            if (length > 0 && from)
            {
                if (length - 1 < last)
                    last = length - 1;
                if (!(length - 1 < first) && !(last < first) && first > -1 && last > -1)
                {
                    const uint32_t count = static_cast<uint32_t>(last - first + 1);
                    zge::stringCopyN(text, from + first, count);
                    text[count] = 0;
                }
            }
        }
        setWidgetVariable(foundWidget(*destination), destinationName, text);
    }

    namespace
    {
        // ROTQUATX/Y/Z quaternion turns: the quaternion variable turned by
        // `turns` (a ZFloat, 1 a whole turn) about the axis, the turn applied
        // after it. The three commands are one code at three addresses:
        void rotateQuaternion(ee::Ptr<char> arguments, ee::Ptr<const zge::Vec4> axis)
        {
            ee::Local<ee::Float> turns;          // the second token: how far, in turns
            ee::Local<zge::Quaternion> value;    // the first token's quaternion
            ee::Local<zge::Quaternion> turn;
            ee::Local<zge::Vec4> axisCopy;
            ee::Local<char[0x20]> floatType;     // "ZFloat", padded with zeros
            *axisCopy = *axis;
            std::memcpy(&floatType[0], ee::Ptr<const char>(kFloatName).get(), 7);
            memset(ee::Ptr<char>(floatType) + 7, 0, 25);
            const ee::Ptr<char> rest(readToken(arguments, 0, ee::Ptr<char>(kTokenBuffer)));
            tokenValue(ee::Ptr<const char>(kTokenBuffer), value, ee::Ptr<char>());
            readToken(rest, 0, ee::Ptr<char>(kArgumentBuffer));
            tokenValue(ee::Ptr<const char>(kArgumentBuffer), turns, floatType);
            const zge::Angle angle = static_cast<zge::Angle>(ee::toInt(*turns * ee::Float(65536.0f)));
            zge::quaternionFromAxisAngle(turn, &axisCopy->xyz(), angle);
            zge::quaternionMultiply(value, value, turn);
            setTokenValue(ee::Ptr<const char>(kTokenBuffer), value);
        }
        constexpr uint32_t kAxisX = sym::g_quaternionAxisX, kAxisY = sym::g_quaternionAxisY, kAxisZ = sym::g_quaternionAxisZ; // (1,0,0,0) ..

        // ZGE2's ROTQUATX/Y/Z: the same turn; the turns are the line's second
        // token and the quaternion its first, each read from the line's start
        // into the one token buffer.
        void rotateQuaternionAtPath(ee::Ptr<char> arguments, ee::Ptr<const zge::Vec4> axis)
        {
            ee::Local<ee::Float[4]> turns;       // the second token: how far, in turns (a value's room)
            ee::Local<zge::Quaternion> value;    // the first token's quaternion
            ee::Local<zge::Quaternion> turn;
            ee::Local<zge::Vec4> axisCopy;
            ee::Local<char[0x20]> floatType;     // "ZFloat", padded with zeros
            *axisCopy = *axis;
            std::memcpy(&floatType[0], ee::Ptr<const char>(kFloatName).get(), 7);
            fillBytes(ee::Ptr<char>(floatType) + 7, 0, 25);
            readToken(arguments, 1, ee::Ptr<char>(kTokenBuffer));
            tokenValue(ee::Ptr<const char>(kTokenBuffer), turns, floatType);
            readToken(arguments, 0, ee::Ptr<char>(kTokenBuffer));
            tokenValue(ee::Ptr<const char>(kTokenBuffer), value, ee::Ptr<char>());
            const zge::Angle angle = static_cast<zge::Angle>(ee::toInt(turns[0] * ee::Float(65536.0f)));
            zge::quaternionFromAxisAngle(turn, &axisCopy->xyz(), angle);
            zge::quaternionMultiply(value, value, turn);
            setTokenValue(ee::Ptr<const char>(kTokenBuffer), value);
        }
    }

    void commandRotQuatX(ee::Ptr<char> arguments)
    {
        rotateQuaternion(arguments, ee::Ptr<const zge::Vec4>(kAxisX));
    }
    // Draft: no UI script of the nine releases uses ROTQUATX (never recorded).
    READABLE_FUNCTION_WAITS_OWN(commandRotQuatX, Draft, dm2, commandRotQuatXAtPath);
    void commandRotQuatXAtPath(ee::Ptr<char> arguments)
    {
        rotateQuaternionAtPath(arguments, ee::Ptr<const zge::Vec4>(kAxisX));
    }

    void commandRotQuatY(ee::Ptr<char> arguments)
    {
        rotateQuaternion(arguments, ee::Ptr<const zge::Vec4>(kAxisY));
    }
    READABLE_FUNCTION_WAITS_OWN(commandRotQuatY, Verified, dm2, commandRotQuatYAtPath);
    void commandRotQuatYAtPath(ee::Ptr<char> arguments)
    {
        rotateQuaternionAtPath(arguments, ee::Ptr<const zge::Vec4>(kAxisY));
    }

    void commandRotQuatZ(ee::Ptr<char> arguments)
    {
        rotateQuaternion(arguments, ee::Ptr<const zge::Vec4>(kAxisZ));
    }
    // Draft: no UI script of the nine releases uses ROTQUATZ (never recorded).
    READABLE_FUNCTION_WAITS_OWN(commandRotQuatZ, Draft, dm2, commandRotQuatZAtPath);
    void commandRotQuatZAtPath(ee::Ptr<char> arguments)
    {
        rotateQuaternionAtPath(arguments, ee::Ptr<const zge::Vec4>(kAxisZ));
    }
}

// ZGE2's forms of DECLARE and UNDECLARE: the variable is a ListedVariable of
// the widget or ACTION the path leads to (resolveObjectPath).
namespace zge::zgui
{
    // commandDeclareAtPath: DECLARE type path: a variable of the type on the
    // widget or ACTION at path (its last component the new name), its value a
    // zeroed word of zgui's memory. ZGE2 does not check that the name is free.
    void commandDeclareAtPath(ee::Ptr<char> arguments)
    {
        ee::Local<char[0x40]> typeName;
        ee::Local<char[0x40]> path;
        ee::Local<char[0x40]> name;
        ee::Local<ee::Address> found;
        readToken(arguments, 0, typeName);
        readToken(arguments, 1, path);
        const int32_t kind = resolveObjectPath(path, name, typeName, found);
        if (kind != kObjectPathWidget && kind != kObjectPathAction)
            return;
        const ee::Ptr<uint32_t> value(zguiAllocate(*g_heap, 4u));
        *value = 0;
        const ee::Ptr<zge::List> variables =
            kind == kObjectPathWidget ? widgetProperties(foundWidget(*found)) : actionProperties(foundAction(*found));
        declareListedVariable(variables, name, typeName, value);
    }

    // commandUndeclareAtPath: UNDECLARE path: the variable taken off its widget
    // or ACTION and freed; a path to none does nothing (ZGE3 reports it).
    void commandUndeclareAtPath(ee::Ptr<char> arguments)
    {
        ee::Local<char[0x40]> typeName;
        ee::Local<char[0x40]> name;
        ee::Local<ee::Address> found;
        const int32_t kind = resolveObjectPath(arguments, name, typeName, found);
        if (kind == kWidgetVariablePath)
            removeListedVariable(widgetProperties(foundWidget(*found)), name);
        else if (kind == kActionVariablePath)
            removeListedVariable(actionProperties(foundAction(*found)), name);
    }
}
