// A level as a model (collection/levels/model.h).
#include "collection/levels/model.h"

#include "collection/levels/arithmetic.h"

#include <cmath>

namespace zx::app::levels
{
    namespace
    {
        using Matrix = std::array<double, 16>; // column-major

        double narrow(double v) { return static_cast<double>(static_cast<float>(v)); }

        // T * R * S (gltf_model.py compose).
        Matrix compose(const Vec3 &t, const Quat &q, const Vec3 &s)
        {
            const double x = q[0], y = q[1], z = q[2], w = q[3];
            const double r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                                    {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                                    {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
            Matrix m{};
            for (int col = 0; col < 3; ++col)
                for (int row = 0; row < 3; ++row)
                    m[col * 4 + row] = r[row][col] * s[col];
            m[12] = t[0];
            m[13] = t[1];
            m[14] = t[2];
            m[15] = 1.0;
            return m;
        }

        Matrix multiply(const Matrix &a, const Matrix &b)
        {
            Matrix out{};
            for (int col = 0; col < 4; ++col)
                for (int row = 0; row < 4; ++row)
                    out[col * 4 + row] = sumOf({a[0 * 4 + row] * b[col * 4 + 0], a[1 * 4 + row] * b[col * 4 + 1],
                                                a[2 * 4 + row] * b[col * 4 + 2], a[3 * 4 + row] * b[col * 4 + 3]});
            return out;
        }

        // (translation, rotation, scale) of a T * R * S matrix, the scale's
        // sign from the determinant (gltf_model.py decompose).
        void decompose(const Matrix &m, Vec3 &t, Quat &q, Vec3 &scale)
        {
            t = {m[12], m[13], m[14]};
            const double cols[3][3] = {{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[8], m[9], m[10]}};
            for (int c = 0; c < 3; ++c)
            {
                scale[c] = std::sqrt(sumOf({cols[c][0] * cols[c][0], cols[c][1] * cols[c][1], cols[c][2] * cols[c][2]}));
                if (scale[c] == 0.0)
                    scale[c] = 1.0;
            }
            const double det = cols[0][0] * (cols[1][1] * cols[2][2] - cols[1][2] * cols[2][1]) -
                               cols[0][1] * (cols[1][0] * cols[2][2] - cols[1][2] * cols[2][0]) +
                               cols[0][2] * (cols[1][0] * cols[2][1] - cols[1][1] * cols[2][0]);
            if (det < 0)
                scale[0] = -scale[0];
            double r[3][3];
            for (int row = 0; row < 3; ++row)
                for (int c = 0; c < 3; ++c)
                    r[row][c] = cols[c][row] / scale[c];
            const double trace = r[0][0] + r[1][1] + r[2][2];
            if (trace > 0)
            {
                const double s = std::sqrt(trace + 1.0) * 2;
                q = {(r[2][1] - r[1][2]) / s, (r[0][2] - r[2][0]) / s, (r[1][0] - r[0][1]) / s, 0.25 * s};
            }
            else if (r[0][0] > r[1][1] && r[0][0] > r[2][2])
            {
                const double s = std::sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]) * 2;
                q = {0.25 * s, (r[0][1] + r[1][0]) / s, (r[0][2] + r[2][0]) / s, (r[2][1] - r[1][2]) / s};
            }
            else if (r[1][1] > r[2][2])
            {
                const double s = std::sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]) * 2;
                q = {(r[0][1] + r[1][0]) / s, 0.25 * s, (r[1][2] + r[2][1]) / s, (r[0][2] - r[2][0]) / s};
            }
            else
            {
                const double s = std::sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]) * 2;
                q = {(r[0][2] + r[2][0]) / s, (r[1][2] + r[2][1]) / s, 0.25 * s, (r[1][0] - r[0][1]) / s};
            }
        }

        template <size_t N>
        std::array<double, N> narrowAll(const std::array<double, N> &v)
        {
            std::array<double, N> out{};
            for (size_t i = 0; i < N; ++i)
                out[i] = narrow(v[i]);
            return out;
        }
    }

    int ModelBuilder::image(std::string name, int width, int height, std::vector<Rgba> pixels)
    {
        m_model.images.push_back(Image{std::move(name), width, height, std::move(pixels)});
        return static_cast<int>(m_model.images.size()) - 1;
    }

    int ModelBuilder::material(std::string name, const Colour &colour, std::optional<int> texture)
    {
        m_model.materials.push_back(Material{std::move(name), colour, texture});
        return static_cast<int>(m_model.materials.size()) - 1;
    }

    int ModelBuilder::mesh(std::string name, std::vector<Primitive> primitives, MeshExtras extras)
    {
        for (Primitive &p : primitives)
        {
            for (Vec3 &v : p.positions)
                v = narrowAll(v);
            if (p.normals)
            {
                if (p.normals->empty())
                    p.normals.reset();
                else
                    for (Vec3 &n : *p.normals)
                        n = narrowAll(n);
            }
            if (p.uvs)
            {
                if (p.uvs->empty())
                    p.uvs.reset();
                else
                    for (Vec2 &uv : *p.uvs)
                        uv = narrowAll(uv);
            }
            if (p.colours)
            {
                if (p.colours->empty())
                    p.colours.reset();
                else
                    for (Colour &c : *p.colours)
                        c = narrowAll(c);
            }
            std::vector<std::array<int, 3>> kept;
            for (const auto &t : p.triangles)
                if (t[0] != t[1] && t[1] != t[2] && t[0] != t[2])
                    kept.push_back(t);
            p.triangles = std::move(kept);
        }
        m_model.meshes.push_back(Mesh{std::move(name), std::move(primitives), std::move(extras)});
        return static_cast<int>(m_model.meshes.size()) - 1;
    }

    void ModelBuilder::node(std::string name, std::optional<int> mesh, const Vec3 &translation, const Quat &rotation)
    {
        // A file leaves out a translation of zeros and the identity rotation:
        // its reader composes the defaults instead.
        const bool noTranslation = translation[0] == 0.0 && translation[1] == 0.0 && translation[2] == 0.0;
        const bool noRotation = rotation[0] == 0.0 && rotation[1] == 0.0 && rotation[2] == 0.0 && rotation[3] == 1.0;
        const Matrix identity = compose({0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}, {1.0, 1.0, 1.0});
        const Matrix local = compose(noTranslation ? Vec3{0.0, 0.0, 0.0} : translation,
                                     noRotation ? Quat{0.0, 0.0, 0.0, 1.0} : rotation, {1.0, 1.0, 1.0});
        Node n;
        n.name = std::move(name);
        n.mesh = mesh;
        decompose(multiply(identity, local), n.translation, n.rotation, n.scale);
        m_model.nodes.push_back(std::move(n));
    }

    bool ModelBuilder::hasImage(const std::string &name) const
    {
        for (const Image &i : m_model.images)
            if (i.name == name)
                return true;
        return false;
    }
}
