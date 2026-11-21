#pragma once

// ZGE4's layered images (image_layers.cpp; BMX XXX only): images kept by a layer number, painted over one another
// into one picture of the first layer's size. A model's texture slot with several images (a rider's skin, its
// tattoos, its clothes' prints, ranked by the MODEL script) is made from them (model_records.cpp).

#include "ee/ee.h"
#include "ee/plain.h"
#include "zge/assets/images.h"
#include "zge/container/tree.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    // One layer (0x10 bytes, from a pool of its own): a node of its picture's tree by its number.
    struct ImageLayer
    {
        SearchTreeNode node;        // +0x00
        int32_t layer;              // +0x08 its number (what the tree sorts by)
        ee::Ptr<Image> image;       // +0x0C
    };
    static_assert(offsetof(ImageLayer, layer) == 0x08 && offsetof(ImageLayer, image) == 0x0C && sizeof(ImageLayer) == 0x10);

    // A picture of layers (0x14 bytes, from a pool of its own).
    struct ImageLayers
    {
        int32_t width;              // +0x00 the first layer's image's, once there is one
        int32_t height;             // +0x04
        SearchTree layers;          // +0x08 ImageLayer by number
    };
    static_assert(offsetof(ImageLayers, layers) == 0x08 && sizeof(ImageLayers) == 0x14);

    // What flattenImageLayers hands each layer's painting (on its stack).
    struct LayerPainting
    {
        ee::Ptr<Rgba8> pixels;      // +0x00 the picture painted so far
        ee::Ptr<Rgba8> layerPixels; // +0x04 the layer's image expanded to RGBA
        uint32_t width;             // +0x08
        uint32_t height;            // +0x0C
    };
    static_assert(sizeof(LayerPainting) == 0x10);

    // newImageLayers: a picture with no layers (its size is unset until the first).
    ee::Ptr<ImageLayers> newImageLayers();
    // freeImageLayers: the picture and its layers freed (not their images).
    void freeImageLayers(ee::Ptr<ImageLayers> layers);
    // setImageLayer: the image as that layer (in place of the one it had); no image takes the layer out.
    void setImageLayer(ee::Ptr<ImageLayers> layers, int32_t layer, ee::Ptr<Image> image);
    // imageLayersWidth, imageLayersHeight: the picture's size, 0 with no layers; imageLayerCount: its layers.
    uint32_t imageLayersWidth(ee::Ptr<ImageLayers> layers);
    uint32_t imageLayersHeight(ee::Ptr<ImageLayers> layers);
    int32_t imageLayerCount(ee::Ptr<ImageLayers> layers);
    // flattenImageLayers: the layers painted into pixels (width x height RGBA, cleared first), each over the ones
    // before it: in rising layer order when `rising`, else falling (the lowest layer on top).
    void flattenImageLayers(ee::Ptr<ImageLayers> layers, uint32_t rising, ee::Ptr<Rgba8> pixels);
}
