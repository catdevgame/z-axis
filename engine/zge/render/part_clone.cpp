// Part clones: a model's instances (props like the Tutorial's roaches, NPCs)
// get their own parts made from the loaded model's, sharing or copying each
// stream.
#include "part_clone.h"

#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/native/geo_meshes.h"
#include "zge/native/host.h"
#include "zge/render/part.h"
#include "sym/zge/render/part_clone.cpp.h"

namespace zge
{
    namespace
    {
        struct StreamShare
        {
            StreamKind kind;
            uint32_t bit;
            uint32_t shareBlock, nextBlock;
        };
        constexpr StreamShare kStreams[] = {
            {StreamKind::Positions, 0x800, points::clonePart::taken, points::clonePart::taken2},
            {StreamKind::Faces, 0x400, points::clonePart::taken3, points::clonePart::taken4},
            {StreamKind::FaceNormals, 0x200, points::clonePart::taken5, points::clonePart::taken6},
            {StreamKind::Normals, 0x100, points::clonePart::taken7, points::clonePart::taken8},
            {StreamKind::Uvs, 0x080, points::clonePart::taken9, points::clonePart::taken10},
            {StreamKind::Colours, 0x040, points::clonePart::taken11, points::clonePart::taken12},
        };
        constexpr StreamShare kLaterStreams[] = {
            {StreamKind::ShadowEdges, 0x010, points::clonePart::taken15, points::clonePart::taken16},
            {StreamKind::Groups, 0x008, points::clonePart::taken17, points::clonePart::taken18},
            {StreamKind::FaceTags, 0x004, points::clonePart::taken19, points::clonePart::taken20},
            {StreamKind::VertexTags, 0x002, points::clonePart::taken21, points::clonePart::taken22},
            {StreamKind::FaceEdges, 0x001, points::clonePart::taken23, points::clonePart::taken24},
            {StreamKind::MaterialSets, kAlwaysCopied, points::clonePart::taken25, points::clonePart::taken26},
        };

        // What clonePart keeps across its calls: the clone, the source, the flags.

        void copyOrShare(ee::Ptr<ZgePart> clone, ee::Ptr<ZgePart> source, const StreamShare &s, uint32_t copyFlags)
        {
            if (copyFlags & s.bit)
                copyStream(clone, source, s.kind, copyFlags);
            else
                shareStream(clone, source, s.kind);
        }
    }

    ee::Ptr<ZgePart> clonePart(ee::Ptr<ZgePart> source, uint32_t copyFlags)
    {
        copyFlags |= kAlwaysCopied;
        const ee::Ptr<ZgePart> clone = newPart();
        ZgePart &to = *clone;
        const ZgePart &from = *source;
        if constexpr (!game::readable::kIsDm2) // (ZGE2 leaves the clone's flags as newPart made them)
            to.flags |= from.flags & kPartFlagsFromDisc;
        to.header = from.header;
        to.vertexCount = from.vertexCount;
        to.faceCount = from.faceCount;
        to.groupCount = from.groupCount;
        to.reserved[0] = from.reserved[0];
        to.reserved[1] = from.reserved[1];
        to.boneCount = from.boneCount;
        to.materialCount = from.materialCount;

        for (const StreamShare &s : kStreams)
            copyOrShare(clone, source, s, copyFlags);
        // The bone list and the skin weights go together.
        if (copyFlags & kCopyBones)
        {
            copyStream(clone, source, StreamKind::Bones, copyFlags);
            copyStream(clone, source, StreamKind::SkinWeights, copyFlags);
        }
        else
        {
            shareStream(clone, source, StreamKind::Bones);
            shareStream(clone, source, StreamKind::SkinWeights);
        }
        for (const StreamShare &s : kLaterStreams)
            copyOrShare(clone, source, s, copyFlags);

        if constexpr (!game::readable::kIsDm2) // (ZGE2 has neither step)
        {
            cloneRenderState(source, clone, copyFlags);
            // The rest of the source's chain, cloned the same way.
            if (source->next)
                clone->next = clonePart(source->next, copyFlags);
        }

        // Host side: the clone's streams are its source's, so is its mesh
        // from the file.
        if (native::hostExtrasOn())
            native::notePartClone(clone, source);
        return clone;
    }
    READABLE_FUNCTION(clonePart, Verified);
}
