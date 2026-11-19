#pragma once

// A level as a model: the form the map converter carries a level in between
// one game's files and another's (collection/levels/export.h makes it from
// a level, convert.h makes a level from it). It is a glTF model's content
// in the converters' terms (the tooling's gltf_model.py and the tags of
// ai_level_convert.py's docstring): nodes placing meshes, meshes of
// triangle and line primitives, materials naming a picture, the pictures.
// The tooling's export writes this as a .gltf file and its converters read
// it back; here it stays in memory, holding what that file would give back
// (positions and vertex data as 32-bit floats, a node's transform composed
// into a matrix and taken apart again), so the files made from it are the
// Python converters' byte for byte.

#include "collection/levels/geometry.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace ps2x::app::levels
{
    using Vec2 = std::array<double, 2>;
    using Colour = std::array<double, 4>; // r, g, b, a; 1.0 the texture as it is
    using Rgba = std::array<uint8_t, 4>;

    // One primitive: triangles (counter-clockwise front) or line segments,
    // the vertex streams they index, the material (an index into the
    // model's, or none).
    struct Primitive
    {
        std::vector<Vec3> positions;
        std::vector<std::array<int, 3>> triangles;
        std::vector<std::array<int, 2>> lines;
        std::optional<std::vector<Vec3>> normals;
        std::optional<std::vector<Vec2>> uvs;
        std::optional<std::vector<Colour>> colours;
        std::optional<int> material;
    };

    // A dynamics prop's definition (a #dynamics node's mesh): the ZDYN's
    // chunks after its DDEF (the bodies and links: the same form in all
    // three games), and what the game it came from writes for it, which a
    // level of that game keeps and another game's level does not (each
    // game's sounds are its own): the object's TGTD bytes after the type
    // word and its instance's tags.
    struct DynamicsDefinition
    {
        std::vector<Chunk> chunks;
        Engine game = Engine::Zge3;
        Bytes classData;
        Bytes instanceTags;
    };

    // What a mesh's glTF extras carry (the tooling's export writes them as
    // "name_hash" and "dynamics").
    struct MeshExtras
    {
        // The object's name hash where its name field cut the name: the
        // stock word, the hash of the whole name.
        std::optional<uint32_t> nameHash;
        // A dynamics prop's definition.
        std::optional<DynamicsDefinition> dynamics;
    };

    struct Mesh
    {
        std::string name;
        std::vector<Primitive> primitives;
        MeshExtras extras;
    };

    struct Material
    {
        std::string name; // "<texture>#<tags>"
        Colour baseColour{1.0, 1.0, 1.0, 1.0};
        std::optional<int> texture; // an index into the model's images
    };

    // A picture, rows top first.
    struct Image
    {
        std::string name;
        int width = 0, height = 0;
        std::vector<Rgba> pixels; // width * height, row by row

        const Rgba &at(int x, int y) const { return pixels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)]; }
    };

    // A node with its world transform (glTF's axes: +y up).
    struct Node
    {
        std::string name; // "<name>#<tags>"
        Vec3 translation{};
        Quat rotation{0.0, 0.0, 0.0, 1.0};
        Vec3 scale{1.0, 1.0, 1.0};
        std::optional<int> mesh;
    };

    struct Model
    {
        std::vector<Node> nodes;
        std::vector<Mesh> meshes;
        std::vector<Material> materials;
        std::vector<Image> images;
    };

    // Builds a Model as the tooling's gltf_model.Builder writes a file and
    // its Reader reads it back: what each call gives is what the converters
    // would read from the file.
    class ModelBuilder
    {
    public:
        // A picture; its index (a material's texture).
        int image(std::string name, int width, int height, std::vector<Rgba> pixels);
        int material(std::string name, const Colour &colour, std::optional<int> texture);
        // A mesh of the primitives (their vertex data narrowed to 32-bit
        // floats; triangles with a repeated vertex left out) with its extras;
        // its index.
        int mesh(std::string name, std::vector<Primitive> primitives, MeshExtras extras = {});
        // A node at the transform (composed and taken apart as a file's
        // reader does).
        void node(std::string name, std::optional<int> mesh, const Vec3 &translation = {0.0, 0.0, 0.0},
                  const Quat &rotation = {0.0, 0.0, 0.0, 1.0});
        // Whether an image of that name is in the model.
        bool hasImage(const std::string &name) const;

        Model &model() { return m_model; }
        const Model &model() const { return m_model; }

    private:
        Model m_model;
    };
}
