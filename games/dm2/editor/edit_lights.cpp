// DM2's park editor lights (edit_lights.h). DM2's own code.

#include "edit_lights.h"
#include "edit_core.h"
#include "edit_tags.h"
#include "edit_gui.h"

#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/assets/chunk_reader.h"
#include "zge/assets/file.h"
#include "zge/assets/tag_table.h"
#include "zge/collision/collision.h"
#include "zge/container/list.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/vec.h"
#include "zge/memory/regions.h"
#include "zge/render/part.h"
#include "zge/scene/locators.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/text/token_reader.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/dm2/editor/edit_lights.cpp.h"

namespace game::dm2
{
    namespace
    {
        inline ee::Global<ParkLights, sym::g_parkLights> g_parkLights;
        inline ee::Global<ee::Ptr<zge::zgui::Node>, sym::g_uiRoot> g_uiRoot;
        inline ee::Global<EditGuiWidgets, sym::g_editGuiWidgets> g_editGuiWidgets;
        // 1 from the editor UI's set-up to its end; 1 while the editor
        // state's enter runs (a load's progress is reported then).
        inline ee::Global<uint32_t, sym::g_editGuiReady> g_editGuiReady;
        inline ee::Global<uint32_t, sym::g_editorEntering> g_editorEntering;

        // The relight's walk over the world's meshes: how many there are,
        // the place in this pass's walk, how many a pass may light, how many
        // this pass has lit, and the place the next pass starts lighting at.
        inline ee::Global<uint32_t, sym::g_relightMeshCount> g_relightMeshCount;
        inline ee::Global<uint32_t, sym::g_relightPosition> g_relightPosition;
        inline ee::Global<uint32_t, sym::g_relightMeshesPerPass> g_relightMeshesPerPass;
        inline ee::Global<uint32_t, sym::g_relightDoneThisPass> g_relightDoneThisPass;
        inline ee::Global<uint32_t, sym::g_relightResumeAt> g_relightResumeAt;

        // The icon roll's turn for one icon, of 20 and of 19 (4096 a turn).
        inline ee::Global<ee::Float, sym::iconRollStep20> g_iconRollStep20;
        inline ee::Global<ee::Float, sym::iconRollStep19> g_iconRollStep19;

        constexpr ee::Ptr<const char> kEntryLights{sym::toolEntryLights};        // "LIGHTS"
        constexpr ee::Ptr<const char> kTextString{sym::lightTextString};         // "String"
        constexpr ee::Ptr<const char> kTextTotalIcon{sym::lightTextTotalIcon};   // "TotalIcon"
        constexpr ee::Ptr<const char> kTextInc{sym::lightTextInc};               // "inc"
        constexpr ee::Ptr<const char> kTextDec{sym::lightTextDec};               // "dec"
        constexpr ee::Ptr<const char> kTextItems{sym::lightTextItems};           // "Items"
        constexpr ee::Ptr<const char> kTextLight{sym::lightTextLight};           // "LIGHT"
        constexpr ee::Ptr<const char> kTextWidth{sym::lightTextWidth};           // "Width"
        constexpr ee::Ptr<const char> kTextCurValue{sym::lightTextCurValue};     // "curValue"
        constexpr ee::Ptr<const char> kKeyEnd{sym::lightsKeyEnd};                // "END"
        constexpr ee::Ptr<const char> kKeyCone{sym::lightsKeyCone};              // "CONE"
        constexpr ee::Ptr<const char> kKeyOmni{sym::lightsKeyOmni};              // "OMNI"
        constexpr ee::Ptr<const char> kLightsFilePath{sym::lightsFilePath};      // "assets\editor\light.txt"
        // "RFPLIJUACDBGEQOHSMNK" and, without the light tool's I, "RFPLJUACDBGEQOHSMNK".
        constexpr ee::Ptr<const char> kIconsWithLights{sym::iconRollIconsWithLights};
        constexpr ee::Ptr<const char> kIconsNoLights{sym::iconRollIconsNoLights};
        // The pause menu's items, with "Light Options" or "!Light Options".
        constexpr ee::Ptr<const char> kPauseItemsWithLights{sym::pauseItemsWithLights};
        constexpr ee::Ptr<const char> kPauseItemsNoLights{sym::pauseItemsNoLights};

        constexpr uint32_t kTokenBytes = 0x100;
        constexpr uint32_t kDefaultBrightness = 5;
        constexpr ee::Float kProgressBarWidth = 640.0f;

        // DM2's findNode takes no kind: the shared declaration's is passed as 0.
        ee::Ptr<zge::zgui::Node> editWidget(uint32_t path)
        {
            return zge::zgui::findNode(*g_uiRoot, ee::Ptr<const char>(path), 0);
        }

        // An unsigned word as a float (the compiled code's two ways).
        ee::Float unsignedToFloat(uint32_t w)
        {
            if (int32_t(w) >= 0)
                return ee::toFloat(int32_t(w));
            const ee::Float half = ee::toFloat(int32_t((w & 1u) | (w >> 1)));
            return half + half;
        }

        ee::Ptr<ParkLight> firstLight()
        {
            return zge::recordOfNode<ParkLight>(g_parkLights->list.head);
        }
    }

    void setParkAmbientColour(ee::Ptr<const zge::VertexColour> colour)
    {
        g_parkLights->ambient = *colour;
    }
    READABLE_FUNCTION(setParkAmbientColour, Verified);

    void initParkLights()
    {
        ee::Local<zge::VertexColour> white;

        zge::initList(&g_parkLights->list);
        white->red = 0xFF;
        white->alpha = 0xFF;
        white->blue = 0xFF;
        white->green = 0xFF;
        setParkAmbientColour(white);
        g_parkLights->brightness = kDefaultBrightness;
        showBrightness();
        g_parkLights->count = 0;
        for (uint32_t kind = 0; kind < kParkLightKindCount; ++kind)
            g_parkLights->kinds[kind] = ee::Ptr<ParkLightKind>();
        readLightKinds();
        g_parkLights->ready = 1;
    }
    READABLE_FUNCTION_WAITS(initParkLights, Verified);

    void offerLightTool()
    {
        ee::Local<uint32_t> iconCount;
        ee::Local<ee::Float> forward;
        ee::Local<ee::Float> backward;

        setEditToolEntryEnabled(kEntryLights, 1);
        const ee::Ptr<zge::zgui::Node> roll = editWidget(sym::iconRollPath);
        zge::zgui::setVariable(roll, kTextString, kIconsWithLights);
        *iconCount = 20;
        zge::zgui::setVariable(roll, kTextTotalIcon, iconCount);
        const ee::Float step = *g_iconRollStep20;
        *forward = step;
        *backward = -step;
        zge::zgui::setVariable(editWidget(sym::topHeaderPath), kTextInc, forward);
        zge::zgui::setVariable(editWidget(sym::topHeaderPath), kTextDec, backward);
        zge::zgui::setVariable(editWidget(sym::pauseMenuPath), kTextItems, kPauseItemsWithLights);
    }
    READABLE_FUNCTION(offerLightTool, Verified);

    void withdrawLightTool()
    {
        ee::Local<uint32_t> iconCount;
        ee::Local<ee::Float> forward;
        ee::Local<ee::Float> backward;

        const ee::Ptr<zge::zgui::Node> roll = editWidget(sym::iconRollPath);
        setEditToolEntryEnabled(kEntryLights, 0);
        zge::zgui::setVariable(roll, kTextString, kIconsNoLights);
        *iconCount = 19;
        zge::zgui::setVariable(roll, kTextTotalIcon, iconCount);
        const ee::Float step = *g_iconRollStep19;
        *forward = step;
        *backward = -step;
        zge::zgui::setVariable(editWidget(sym::topHeaderPath), kTextInc, forward);
        zge::zgui::setVariable(editWidget(sym::topHeaderPath), kTextDec, backward);
        zge::zgui::setVariable(editWidget(sym::pauseMenuPath), kTextItems, kPauseItemsNoLights);
    }
    READABLE_FUNCTION(withdrawLightTool, Draft);

    void freeParkLights()
    {
        while (const ee::Ptr<ParkLight> light = firstLight())
        {
            removeParkLight(light);
            zge::heapFree(light);
        }
    }
    READABLE_FUNCTION(freeParkLights, Verified);

    void shutDownParkLights()
    {
        if (g_parkLights->ready)
        {
            for (uint32_t kind = 0; kind < kParkLightKindCount; ++kind)
                if (g_parkLights->kinds[kind])
                    zge::heapFree(g_parkLights->kinds[kind]);
            freeParkLights();
            g_parkLights->ready = 0;
        }
        g_parkLights->count = 0;
    }
    READABLE_FUNCTION(shutDownParkLights, Verified);

    void addParkLight(ee::Ptr<ParkLight> light)
    {
        zge::listPush(&g_parkLights->list, light);
        ++g_parkLights->count;
    }
    READABLE_FUNCTION(addParkLight, Verified);

    namespace
    {
        // The tags of an "EditLightOmni" or "EditLightCone" scene point:
        // the light's colour and range, and a cone's outer and inner angle.
        struct EditLightTags
        {
            zge::VertexColour colour; // +0x00
            ee::Float range;          // +0x04
            ee::Float outerAngle;     // +0x08 a cone's
            ee::Float innerAngle;     // +0x0C a cone's
        };
        static_assert(sizeof(EditLightTags) == 0x10);

        // An omni light lights in full within this distance.
        constexpr ee::Float kOmniInner = 2.5f;

        ee::Ptr<zge::ScenePoint> locatorPoint(ee::Ptr<zge::Locator> locator)
        {
            return ee::Ptr<zge::ScenePoint>(locator->point);
        }
        ee::Ptr<EditLightTags> lightTags(ee::Ptr<zge::ScenePoint> point)
        {
            return zge::tagRecordAs<EditLightTags>(point->tags);
        }
        ee::Ptr<ParkLight> newThemeLight(ee::Ptr<zge::ScenePoint> point)
        {
            const ee::Ptr<EditLightTags> tags = lightTags(point);
            const ee::Ptr<ParkLight> light(zge::heapAllocate(sizeof(ParkLight)));
            light->colour = tags->colour;
            light->range = tags->range;
            return light;
        }
    }

    // collectThemeLights: the park theme's own lights, one for each of the
    // scene's "EditLightOmni" and "EditLightCone" points (an omni pointing
    // along its point's axis, a cone along its point's second row), and the
    // world's meshes' lights after them.
    void collectThemeLights()
    {
        ee::Local<zge::LocatorList> locators;
        ee::Local<zge::Matrix> turn;
        zge::findLocators(locators.address(), zge::nameHash(ee::pointerAt<char>(sym::editLightOmniKind)), 0);
        for (ee::Ptr<zge::Locator> locator = locators->head; locator; locator = locator->next)
        {
            const ee::Ptr<ParkLight> light = newThemeLight(locatorPoint(locator));
            light->inner = kOmniInner;
            light->unused = 0;
            light->position = locatorPoint(locator)->position;
            zge::quaternionToAxisAngle(&light->direction.xyz(), &locatorPoint(locator)->orientation);
            light->owner = ee::Ptr<zge::Node>();
            light->kind = kParkLightOmni;
            addParkLight(light);
        }
        zge::freeLocators(locators.address());
        zge::findLocators(locators.address(), zge::nameHash(ee::pointerAt<char>(sym::editLightConeKind)), 0);
        for (ee::Ptr<zge::Locator> locator = locators->head; locator; locator = locator->next)
        {
            const ee::Ptr<ParkLight> light = newThemeLight(locatorPoint(locator));
            const ee::Ptr<EditLightTags> tags = lightTags(locatorPoint(locator));
            light->outerAngle = tags->outerAngle;
            light->unused = 0;
            light->inner = tags->innerAngle;
            light->position = locatorPoint(locator)->position;
            zge::quaternionToMatrix(turn, &locatorPoint(locator)->orientation);
            light->direction = zge::matrixRow(*turn, 1);
            light->kind = kParkLightCone;
            light->owner = ee::Ptr<zge::Node>();
            addParkLight(light);
        }
        zge::freeLocators(locators.address());
        if (g_parkLights->ready)
            zge::forEachWorldMesh(sym::addMeshLights, 0);
    }
    READABLE_FUNCTION(collectThemeLights, Verified);

    void removeParkLight(ee::Ptr<ParkLight> light)
    {
        zge::listRemove(&g_parkLights->list, light);
        --g_parkLights->count;
    }
    READABLE_FUNCTION(removeParkLight, Verified);

    namespace
    {
        // An EditLight face's byte 9: its light kind (ParkLights::kinds).
        constexpr uint32_t kFaceTagBytes = 2;
        constexpr uint32_t kLightKindByte = 9;
    }

    void addFaceLights(ee::Ptr<zge::Node> node, ee::Ptr<uint32_t> faces, uint32_t count)
    {
        ee::Local<zge::Vec4> position;
        ee::Local<zge::Vec4> normal;
        ee::Local<ee::Ptr<uint16_t>> corners;

        if (!hasCollisionFrame(node))
            return;
        const ee::Ptr<zge::ZgePart> mesh = node->lods->part;
        if (!mesh)
            return;
        zge::updateMovedNode(zge::rootNode(node));
        const ee::Ptr<zge::Matrix> matrix = zge::nodeMatrix(node, 1);
        for (uint32_t i = count; i-- != 0;)
        {
            const uint32_t face = faces[i];
            zge::faceVertices(mesh, face, corners);
            zge::vertexPosition(mesh, (*corners)[0], position);
            zge::faceNormal(mesh, face, normal);
            zge::transformPoint(position, matrix, position);
            *normal = zge::rotated(*matrix, *normal);
            const uint32_t kind = zge::tagByte(editFaceTagRecord(node, face), kFaceTagBytes, kLightKindByte);
            const ee::Ptr<ParkLightKind> lightKind = g_parkLights->kinds[kind];
            if (!lightKind)
                return;
            const ee::Ptr<ParkLight> light(zge::heapAllocate(sizeof(ParkLight)));
            light->position = *position;
            light->colour = lightKind->colour;
            light->unused = 0;
            light->kind = lightKind->kind;
            light->range = unsignedToFloat(static_cast<uint32_t>(lightKind->range));
            light->direction = *normal;
            light->inner = unsignedToFloat(static_cast<uint32_t>(lightKind->inner));
            light->outerAngle = unsignedToFloat(static_cast<uint32_t>(lightKind->outerAngle));
            light->owner = node;
            light->vertex = (*corners)[0];
            addParkLight(light);
        }
    }
    READABLE_FUNCTION(addFaceLights, Verified);

    void addNodeLights(ee::Ptr<zge::Node> node)
    {
        ee::Local<uint32_t[16]> faces;
        ee::Local<uint32_t> count;

        if (findEditLightFaces(node, faces, count))
            addFaceLights(node, faces, *count);
    }
    READABLE_FUNCTION(addNodeLights, Verified);

    namespace
    {
        // The world's backdrop, which the park lights leave as it is.
        constexpr ee::Ptr<const char> kBackdropNode{sym::g_textPanorama01}; // "panorama_01"
        // Without allLights, only this many of the park lights light vertices.
        constexpr uint32_t kLightsWithoutAll = 17;

        // A light's angle in degrees as an engine angle.
        zge::Angle degreesAngle(ee::Float degrees) { return static_cast<zge::Angle>(ee::toInt(degrees * zge::kDegreesToAngle)); }
        // A colour channel with a light's share added, 255 at most.
        uint8_t litChannel(uint32_t lit) { return lit < 0x100 ? static_cast<uint8_t>(lit) : 0xFF; }
    }

    void lightNodeVertices(ee::Ptr<zge::Node> node, uint32_t allLights)
    {
        ee::Local<zge::Vec4> firstPosition;
        ee::Local<zge::VertexColour> colour;
        ee::Local<zge::Vec4> position;
        ee::Local<zge::Vec4> normal;
        ee::Local<zge::Vec4> toLight;
        ee::Local<zge::Vec4> lightPosition;
        ee::Local<zge::Vec4> vertexCopy; // (the original measures from a copy of the position)
        // How squarely the vertex faces the light and how far into the
        // light's reach it is: a light of a kind that is neither cone nor
        // omni lights with the last vertex's.
        ee::Float facing = 0.0f;
        ee::Float reach = 0.0f;
        uint32_t lightsDone = 0;

        if (!hasCollisionFrame(node))
            return;
        const ee::Ptr<zge::ZgePart> mesh = node->lods->part;
        if (!mesh)
            return;
        if (zge::stringCompareNoCase(zge::nodeName(node), kBackdropNode) == 0)
            return;
        zge::updateMovedNode(zge::rootNode(node));
        const uint32_t vertexCount = mesh->vertexCount;
        const ee::Ptr<zge::Matrix> matrix = zge::nodeMatrix(node, 1);

        // Every vertex the ambient colour first, raised by the brightness.
        *colour = g_parkLights->ambient;
        const uint8_t brightness = static_cast<uint8_t>(g_parkLights->brightness);
        colour->red = static_cast<uint8_t>(colour->red + brightness);
        colour->green = static_cast<uint8_t>(colour->green + brightness);
        colour->blue = static_cast<uint8_t>(colour->blue + brightness);
        zge::vertexPosition(mesh, 0, firstPosition);
        zge::setPartPosition(mesh, 0, firstPosition);
        for (uint32_t vertex = 0; vertex < vertexCount; ++vertex)
            zge::setPartColour(mesh, vertex, colour);
        colour->alpha = 0xFF;

        for (ee::Ptr<ParkLight> light = firstLight(); light; light = zge::recordOfNode<ParkLight>(light->link.next))
        {
            if (!allLights && lightsDone >= kLightsWithoutAll)
                return;
            *lightPosition = light->position;
            const ee::Float outerCosine = zge::angleCos(degreesAngle(light->outerAngle));
            const ee::Float innerCosine = zge::angleCos(degreesAngle(light->inner));
            ++lightsDone;
            for (uint32_t vertex = 0; vertex < vertexCount; ++vertex)
            {
                zge::vertexPosition(mesh, vertex, position);
                zge::partNormal(mesh, vertex, normal);
                zge::transformPoint(position, matrix, position);
                *normal = zge::rotated(*matrix, *normal);
                *vertexCopy = *position;
                *toLight = *lightPosition - *vertexCopy;
                ee::Float distance;
                *toLight = zge::unit(*toLight, distance);
                *normal = zge::unit(*normal);
                if (light->kind == kParkLightOmni)
                {
                    if (light->range < distance)
                        continue;
                    if (distance < light->inner)
                    {
                        reach = 1.0f;
                        facing = 1.0f;
                    }
                    else
                    {
                        reach = 1.0f - distance / light->range;
                        facing = zge::dot(*normal, *toLight);
                    }
                }
                else if (light->kind == kParkLightCone)
                {
                    facing = -zge::dot(*toLight, light->direction);
                    if (facing < outerCosine)
                        continue;
                    reach = innerCosine < facing ? ee::Float(1.0f) : (facing - outerCosine) / (innerCosine - outerCosine);
                    facing = -zge::dot(*normal, light->direction);
                }
                if (!(0.0f < facing))
                    continue;
                // The light's share added to the vertex's colour.
                zge::partColour(mesh, vertex, colour);
                const uint32_t red = sce::libgcc::fptoui(ee::toFloat(int32_t{light->colour.red}) * facing * reach +
                                                         ee::toFloat(int32_t{colour->red}));
                const uint32_t green = sce::libgcc::fptoui(ee::toFloat(int32_t{light->colour.green}) * facing * reach +
                                                           ee::toFloat(int32_t{colour->green}));
                const uint32_t blue = sce::libgcc::fptoui(ee::toFloat(int32_t{light->colour.blue}) * facing * reach +
                                                          ee::toFloat(int32_t{colour->blue}));
                colour->red = litChannel(red);
                colour->green = litChannel(green);
                colour->blue = litChannel(blue);
                zge::setPartColour(mesh, vertex, colour);
            }
        }
    }
    READABLE_FUNCTION(lightNodeVertices, Verified);

    void removeNodeLights(ee::Ptr<zge::Node> node)
    {
        ee::Ptr<ParkLight> next;
        for (ee::Ptr<ParkLight> light = firstLight(); light; light = next)
        {
            next = zge::recordOfNode<ParkLight>(light->link.next);
            if (light->owner == node)
            {
                removeParkLight(light);
                zge::heapFree(light);
            }
        }
    }
    READABLE_FUNCTION(removeNodeLights, Verified);

    uint32_t countWorldMesh()
    {
        ++*g_relightMeshCount;
        return 1;
    }
    READABLE_FUNCTION(countWorldMesh, Verified);

    uint32_t resetNodeVertexColours(ee::Ptr<zge::Node> node)
    {
        ee::Local<zge::VertexColour> colour;

        // The node's collision mesh is the mesh that is lit.
        const ee::Ptr<zge::ZgePart> mesh = node->lods->part;
        if (mesh)
        {
            const uint32_t vertices = mesh->vertexCount;
            colour->red = 0xFF;
            colour->alpha = 0x80;
            colour->blue = 0xFF;
            colour->green = 0xFF;
            for (uint32_t vertex = 0; vertex < vertices; ++vertex)
                zge::setPartColour(mesh, vertex, colour);
        }
        return 1;
    }
    READABLE_FUNCTION(resetNodeVertexColours, Draft);

    // A pass lights g_relightMeshesPerPass meshes: the walk skips those
    // earlier passes lit, lights the mesh, moves the progress bar and stops
    // the walk once the pass has done its share.
    uint32_t relightMesh(ee::Ptr<zge::Node> node)
    {
        ee::Local<ee::Float> width;

        if (*g_relightPosition < *g_relightResumeAt)
        {
            ++*g_relightPosition;
            return 1;
        }
        if (*g_editorEntering)
            zge::chunkProgressHook()(kTextLight, *g_relightPosition, *g_relightMeshesPerPass);
        lightNodeVertices(node, 1);
        *width = unsignedToFloat(*g_relightPosition) / unsignedToFloat(*g_relightMeshCount) * kProgressBarWidth;
        zge::zgui::setVariable(editWidget(sym::progressBarProgressPath), kTextWidth, width);
        const uint32_t position = *g_relightPosition + 1;
        const uint32_t done = *g_relightDoneThisPass + 1;
        *g_relightResumeAt = position;
        *g_relightPosition = position;
        *g_relightDoneThisPass = done;
        return done < *g_relightMeshesPerPass;
    }
    READABLE_FUNCTION(relightMesh, Verified);

    uint32_t relightWorld()
    {
        *g_relightMeshCount = 0;
        *g_relightMeshesPerPass = 1;
        *g_relightDoneThisPass = 0;
        *g_relightResumeAt = 0;
        *g_relightPosition = 0;
        readBrightness();
        zge::forEachWorldMesh(sym::countWorldMesh, 0);
        *g_relightMeshesPerPass = *g_relightMeshCount;
        return game::tail(zge::forEachWorldMesh, sym::relightMesh, 0);
    }
    READABLE_FUNCTION(relightWorld, Verified);

    uint32_t editLightReset()
    {
        return game::tail(zge::forEachWorldMesh, sym::resetNodeVertexColours, 0);
    }
    READABLE_FUNCTION(editLightReset, Draft);

    uint32_t addMeshLights(ee::Ptr<zge::Node> node)
    {
        addNodeLights(node);
        return 1;
    }
    READABLE_FUNCTION(addMeshLights, Verified);

    void readBrightness()
    {
        ee::Local<ee::Float> width;

        if (*g_editGuiReady)
        {
            zge::zgui::copyVariable(g_editGuiWidgets->brightnessBar, kTextWidth, width);
            g_parkLights->brightness = sce::libgcc::fptoui(*width);
        }
    }
    READABLE_FUNCTION(readBrightness, Verified);

    void showBrightness()
    {
        ee::Local<ee::Float> width;

        if (*g_editGuiReady)
        {
            *width = unsignedToFloat(g_parkLights->brightness);
            zge::zgui::setVariable(g_editGuiWidgets->brightnessBar, kTextWidth, width);
            zge::zgui::setVariable(editWidget(sym::lightBrightnessPath), kTextCurValue, width);
        }
    }
    READABLE_FUNCTION(showBrightness, Verified);

    ee::Ptr<ParkLightKind> readLightKind()
    {
        ee::Local<int32_t> number;
        ee::Local<int32_t> red;
        ee::Local<int32_t> green;
        ee::Local<int32_t> blue;

        zge::readNumberToken(number);
        const ee::Ptr<ParkLightKind> kind(zge::heapAllocate(sizeof(ParkLightKind)));
        ee::Ptr<ee::Ptr<ParkLightKind>>(g_parkLights->kinds)[*number] = kind;
        zge::readNumberToken(red);
        zge::readNumberToken(green);
        zge::readNumberToken(blue);
        kind->colour.red = static_cast<uint8_t>(*red);
        kind->colour.green = static_cast<uint8_t>(*green);
        kind->colour.alpha = 0xFF;
        kind->colour.blue = static_cast<uint8_t>(*blue);
        zge::readNumberToken(&kind->range);
        return kind;
    }
    READABLE_FUNCTION(readLightKind, Verified);

    int32_t readConeLightKind()
    {
        const ee::Ptr<ParkLightKind> kind = readLightKind();
        kind->kind = kParkLightCone;
        zge::readNumberToken(&kind->inner);
        return game::tail(zge::readNumberToken, &kind->outerAngle);
    }
    READABLE_FUNCTION(readConeLightKind, Verified);

    int32_t readOmniLightKind()
    {
        const ee::Ptr<ParkLightKind> kind = readLightKind();
        kind->kind = kParkLightOmni;
        return game::tail(zge::readNumberToken, &kind->inner);
    }
    READABLE_FUNCTION(readOmniLightKind, Verified);

    void readLightKinds()
    {
        ee::Local<char[kTokenBytes]> token;
        ee::Local<uint32_t> bytes;

        const ee::Ptr<zge::ZFile> file = zge::openFile(kLightsFilePath, zge::kFileModeRead, 0).file();
        zge::fileSize(file, bytes, 0);
        const uint32_t text = zge::heapAllocate(*bytes);
        zge::fileRead(file, text, *bytes, 0);
        zge::closeFile(file, 0);
        const ee::Ptr<zge::ZFile> memory = zge::openMemoryFile(text, *bytes);
        zge::beginScript(zge::filePath(memory));
        zge::nextToken(token, kTokenBytes);
        while (zge::compareStrings(kKeyEnd, token) != 0)
        {
            if (zge::compareStrings(kKeyCone, token) == 0)
                readConeLightKind();
            else if (zge::compareStrings(kKeyOmni, token) == 0)
                readOmniLightKind();
            zge::nextToken(token, kTokenBytes);
        }
        zge::endScript();
        zge::closeFile(memory, 0);
        zge::heapFree(text);
    }
    READABLE_FUNCTION_WAITS(readLightKinds, Verified);

    namespace
    {
        // 1 while the relight started by editLightProcessPark goes on.
        inline ee::Global<uint32_t, sym::g_relightRunning> g_relightRunning;
        constexpr ee::Ptr<const char> kProgressBarPath{sym::lightProgressBarPath}; // "ParkEditor.MainUI.ProgressBar"
        constexpr ee::Ptr<const char> kLightMenuPath{sym::lightMenuPath};          // "ParkEditor.MainUI.LightMenu"
        constexpr ee::Ptr<const char> kTextInit{sym::lightTextInit};               // "Init"
        constexpr ee::Ptr<const char> kTextDeInit{sym::lightTextDeInit};           // "DeInit"
        // What zgui's PLAY sends the root to run an ACTION.
        constexpr uint32_t kPlayMessage = 0x8000;

    }

    uint32_t relightWorldEntry()
    {
        return game::tail(relightWorld);
    }
    READABLE_FUNCTION(relightWorldEntry, Verified);

    void editLightProcessPark()
    {
        *g_relightMeshCount = 0;
        *g_relightMeshesPerPass = 1;
        *g_relightDoneThisPass = 0;
        *g_relightResumeAt = 0;
        *g_relightPosition = 0;
        moveOwnedLights();
        readBrightness();
        const ee::Ptr<zge::zgui::Node> bar = zge::zgui::findNode(*g_uiRoot, kProgressBarPath, 0);
        const ee::Ptr<zge::zgui::ListedAction> init = widgetAction(bar, kTextInit);
        zge::sendNewMessage(0, *g_uiRoot, kPlayMessage, init, 1);
        zge::forEachWorldMesh(sym::countWorldMesh, 0);
        *g_relightRunning = 1;
    }
    READABLE_FUNCTION(editLightProcessPark, Verified);

    uint32_t continueRelight()
    {
        if (*g_relightPosition == *g_relightMeshCount)
        {
            *g_relightRunning = 0;
            const ee::Ptr<zge::zgui::Node> bar = zge::zgui::findNode(*g_uiRoot, kProgressBarPath, 0);
            const ee::Ptr<zge::zgui::ListedAction> closeBar = widgetAction(bar, kTextDeInit);
            zge::sendNewMessage(0, *g_uiRoot, kPlayMessage, closeBar, 1);
            const ee::Ptr<zge::zgui::Node> menu = zge::zgui::findNode(*g_uiRoot, kLightMenuPath, 0);
            const ee::Ptr<zge::zgui::ListedAction> closeMenu = widgetAction(menu, kTextDeInit);
            return static_cast<uint32_t>(game::tail(zge::sendNewMessage, 0, *g_uiRoot, kPlayMessage, closeMenu, 1));
        }
        *g_relightPosition = 0;
        *g_relightDoneThisPass = 0;
        return game::tail(zge::forEachWorldMesh, sym::relightMesh, 0);
    }
    READABLE_FUNCTION(continueRelight, Verified);

    // The walk ends at the first owner without a collision mesh.
    void moveOwnedLights()
    {
        ee::Local<zge::Vec4> position;
        for (ee::Ptr<ParkLight> light = firstLight(); light; light = zge::recordOfNode<ParkLight>(light->link.next))
        {
            const ee::Ptr<zge::Node> owner = light->owner;
            if (!owner)
                continue;
            if (!hasCollisionFrame(owner))
                return;
            const ee::Ptr<zge::ZgePart> mesh = owner->lods->part;
            if (!mesh)
                return;
            const ee::Ptr<zge::Matrix> matrix = zge::nodeMatrix(owner, 1);
            zge::vertexPosition(mesh, light->vertex, position);
            zge::transformPoint(position, matrix, position);
            light->position = *position;
        }
    }
    READABLE_FUNCTION(moveOwnedLights, Verified);
}
