// ZGE4's layered images (image_layers.h; BMX XXX only). It sits between the image readers' palette expansion and
// the BMP reader in the link order.

#include "image_layers.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/assets/images.h"
#include "zge/container/tree.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/assets/image_layers.cpp.h"

namespace zge
{
    uint32_t freeSearchTreeRecord(ee::Ptr<SearchTreeNode> node, ee::Ptr<ee::Ptr<Pool>> pool); // (tree.cpp)
    int32_t searchTreeForEachDescending(ee::Ptr<SearchTree> tree, SearchTreeVisit visit, uint32_t context);

    namespace
    {
        // The pictures' and the layers' pools, each made with its first record and destroyed with its last.
        inline ee::Global<ee::Ptr<Pool>, sym::bmx::g_imageLayersPool> g_imageLayersPool;
        inline ee::Global<ee::Ptr<Pool>, sym::bmx::g_imageLayerPool> g_imageLayerPool;
        constexpr uint8_t kOpaque = 0xFF;
    }

    // blendPixelsOver: each pixel of `over` painted onto `under` by its alpha (none: under is left as it is),
    // under's alpha made opaque where it was painted. Each channel is under * (255 - alpha) + over * alpha, over
    // 256.
    inline void blendPixelsOver(ee::Ptr<Rgba8> under, ee::Ptr<const Rgba8> over, uint32_t width, uint32_t height)
    {
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x, ++under, ++over)
            {
                const uint32_t alpha = over->alpha;
                if (alpha == 0)
                    continue;
                const uint32_t left = kOpaque - alpha;
                under->red = static_cast<uint8_t>((under->red * left + over->red * alpha) >> 8);
                under->green = static_cast<uint8_t>((under->green * left + over->green * alpha) >> 8);
                under->alpha = kOpaque;
                under->blue = static_cast<uint8_t>((under->blue * left + over->blue * alpha) >> 8);
            }
        }
    }
    READABLE_FUNCTION_ONLY(bmx, blendPixelsOver);

    // paintImageLayer (flattenImageLayers' visit): the layer's image expanded and painted over the picture. 1: on.
    inline int32_t paintImageLayer(ee::Ptr<ImageLayer> layer, ee::Ptr<LayerPainting> painting)
    {
        expandPalettedImage(layer->image, painting->layerPixels);
        blendPixelsOver(painting->pixels, painting->layerPixels, painting->width, painting->height);
        return 1;
    }
    READABLE_FUNCTION_ONLY(bmx, paintImageLayer);

    inline ee::Ptr<ImageLayers> newImageLayers()
    {
        if (!*g_imageLayersPool)
            *g_imageLayersPool = ee::Ptr<Pool>(createPool(sizeof(ImageLayers), 0, 0));
        const ee::Ptr<ImageLayers> layers(poolAlloc(*g_imageLayersPool));
        initSearchTree(&layers->layers, sym::lessWord);
        return layers;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, newImageLayers);

    inline void freeImageLayers(ee::Ptr<ImageLayers> layers)
    {
        if (*g_imageLayerPool)
        {
            searchTreeClear(&layers->layers, SearchTreeVisit(sym::freeSearchTreeRecord), g_imageLayerPool.address);
            if (poolCount(*g_imageLayerPool) == 0)
            {
                destroyPool(*g_imageLayerPool);
                *g_imageLayerPool = ee::Ptr<Pool>();
            }
        }
        poolFree(*g_imageLayersPool, layers);
        if (poolCount(*g_imageLayersPool) == 0)
        {
            destroyPool(*g_imageLayersPool);
            *g_imageLayersPool = ee::Ptr<Pool>();
        }
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, freeImageLayers);

    inline void setImageLayer(ee::Ptr<ImageLayers> layers, int32_t layer, ee::Ptr<Image> image)
    {
        ee::Local<int32_t> number;
        *number = layer;
        if (!image)
        {
            const ee::Ptr<SearchTreeNode> gone = searchTreeRemove(&layers->layers, number);
            if (gone)
                poolFree(*g_imageLayerPool, gone);
            return;
        }
        if (layers->layers.count == 0)
        {
            layers->width = image->width;
            layers->height = image->height;
        }
        ee::Ptr<ImageLayer> entry = treeRecordOf<ImageLayer>(searchTreeFind(&layers->layers, number));
        if (entry)
        {
            entry->image = image;
            return;
        }
        if (!*g_imageLayerPool)
            *g_imageLayerPool = ee::Ptr<Pool>(createPool(sizeof(ImageLayer), 0, 0));
        entry = ee::Ptr<ImageLayer>(poolAlloc(*g_imageLayerPool));
        entry->image = image;
        entry->layer = *number;
        searchTreeInsert(&layers->layers, &entry->node);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, setImageLayer);

    inline uint32_t imageLayersWidth(ee::Ptr<ImageLayers> layers)
    {
        return layers->layers.count != 0 ? static_cast<uint32_t>(layers->width) : 0u;
    }
    READABLE_FUNCTION_ONLY(bmx, imageLayersWidth);
    inline uint32_t imageLayersHeight(ee::Ptr<ImageLayers> layers)
    {
        return layers->layers.count != 0 ? static_cast<uint32_t>(layers->height) : 0u;
    }
    READABLE_FUNCTION_ONLY(bmx, imageLayersHeight);
    inline int32_t imageLayerCount(ee::Ptr<ImageLayers> layers) { return layers->layers.count; }
    READABLE_FUNCTION_ONLY(bmx, imageLayerCount);

    inline void flattenImageLayers(ee::Ptr<ImageLayers> layers, uint32_t rising, ee::Ptr<Rgba8> pixels)
    {
        constexpr uint32_t kPixelBytes = sizeof(Rgba8);
        const uint32_t width = imageLayersWidth(layers);
        const uint32_t bytes = width * (imageLayersHeight(layers) * kPixelBytes);
        ee::Local<LayerPainting> painting;
        painting->layerPixels = ee::Ptr<Rgba8>(heapAllocate(bytes));
        painting->pixels = pixels;
        memoryFill(pixels, 0, bytes);
        painting->width = imageLayersWidth(layers);
        painting->height = imageLayersHeight(layers);
        if (rising)
            searchTreeForEach(&layers->layers, SearchTreeVisit(sym::bmx::paintImageLayer), painting);
        else
            searchTreeForEachDescending(&layers->layers, SearchTreeVisit(sym::bmx::paintImageLayer), painting.address());
        heapFree(painting->layerPixels);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, flattenImageLayers);
}
