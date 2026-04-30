// Tiny software rasterizer that turns latex3d::MeshData into a PNG. Used by
// the `latex3d_samples` build target to regenerate the gallery shown in the
// README. Not production code — there's no perspective, no MSAA beyond a 2×
// supersample, no gamma-correct blending. Goal is "readable thumbnail".
//
// Pipeline per pixel:
//   1. Apply a small yaw + pitch so the extrusion shows.
//   2. Orthographic project to screen space.
//   3. Barycentric edge test + Z-buffer.
//   4. Lambert shading from a soft-key direction.
//   5. 2× → 1× box-filter downsample.

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <latex3d/latex3d.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{

    struct V3
    {
        float x = 0, y = 0, z = 0;
        V3 operator+(V3 o) const { return {x + o.x, y + o.y, z + o.z}; }
        V3 operator-(V3 o) const { return {x - o.x, y - o.y, z - o.z}; }
        V3 operator*(float s) const { return {x * s, y * s, z * s}; }
    };

    float dot3(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    float len3(V3 a) { return std::sqrt(dot3(a, a)); }
    V3 norm3(V3 a)
    {
        float l = len3(a);
        return l > 1e-9f ? a * (1.f / l) : V3{0, 0, 1};
    }

    V3 rotY(V3 v, float a)
    {
        float c = std::cos(a), s = std::sin(a);
        return {c * v.x + s * v.z, v.y, -s * v.x + c * v.z};
    }
    V3 rotX(V3 v, float a)
    {
        float c = std::cos(a), s = std::sin(a);
        return {v.x, c * v.y - s * v.z, s * v.y + c * v.z};
    }

    struct Image
    {
        int W, H;
        std::vector<float> rgb;   // linear, 3 floats per pixel
        std::vector<float> depth; // smaller = closer
        Image(int w, int h) : W(w), H(h), rgb(w * h * 3, 0.f), depth(w * h, 1e30f) {}
        void fill(float r, float g, float b)
        {
            for (int i = 0; i < W * H; ++i)
            {
                rgb[3 * i + 0] = r;
                rgb[3 * i + 1] = g;
                rgb[3 * i + 2] = b;
            }
        }
    };

    void shadePixel(Image &img, int x, int y, float z, V3 color, V3 normal)
    {
        if (x < 0 || x >= img.W || y < 0 || y >= img.H) return;
        int idx = y * img.W + x;
        if (z >= img.depth[idx]) return;
        img.depth[idx] = z;
        const V3 lightDir = norm3({0.45f, 0.75f, 0.55f});
        float lambert = std::max(0.f, dot3(norm3(normal), lightDir));
        const float ambient = 0.55f;
        float lit = ambient + (1.f - ambient) * lambert;
        img.rgb[3 * idx + 0] = color.x * lit;
        img.rgb[3 * idx + 1] = color.y * lit;
        img.rgb[3 * idx + 2] = color.z * lit;
    }

    float edgeFn(float ax, float ay, float bx, float by, float px, float py)
    {
        return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
    }

    void drawTri(Image &img,
                 V3 v0, V3 v1, V3 v2,
                 V3 c0, V3 c1, V3 c2,
                 V3 n0, V3 n1, V3 n2)
    {
        int minX = std::max(0, (int)std::floor(std::min({v0.x, v1.x, v2.x})));
        int maxX = std::min(img.W - 1, (int)std::ceil(std::max({v0.x, v1.x, v2.x})));
        int minY = std::max(0, (int)std::floor(std::min({v0.y, v1.y, v2.y})));
        int maxY = std::min(img.H - 1, (int)std::ceil(std::max({v0.y, v1.y, v2.y})));
        if (minX > maxX || minY > maxY) return;

        float area = edgeFn(v0.x, v0.y, v1.x, v1.y, v2.x, v2.y);
        if (std::abs(area) < 1e-6f) return;
        float invArea = 1.f / area;

        for (int y = minY; y <= maxY; ++y)
        {
            for (int x = minX; x <= maxX; ++x)
            {
                float px = x + 0.5f, py = y + 0.5f;
                float w0 = edgeFn(v1.x, v1.y, v2.x, v2.y, px, py) * invArea;
                float w1 = edgeFn(v2.x, v2.y, v0.x, v0.y, px, py) * invArea;
                float w2 = edgeFn(v0.x, v0.y, v1.x, v1.y, px, py) * invArea;
                bool ccw = (w0 >= 0 && w1 >= 0 && w2 >= 0);
                bool cw  = (w0 <= 0 && w1 <= 0 && w2 <= 0);
                if (!ccw && !cw) continue;
                if (cw) { w0 = -w0; w1 = -w1; w2 = -w2; }
                float z = w0 * v0.z + w1 * v1.z + w2 * v2.z;
                V3 c = c0 * w0 + c1 * w1 + c2 * w2;
                V3 n = n0 * w0 + n1 * w1 + n2 * w2;
                shadePixel(img, x, y, z, c, n);
            }
        }
    }

    bool render(const std::string &latex,
                const std::string &outPath,
                int outW, int outH)
    {
        latex3d::LatexOptions opt;
        opt.charHeight = 1.0f;
        opt.depth = 0.20f;
        opt.color = glm::vec3(0.18f, 0.16f, 0.28f); // dark indigo, reads on cream
        opt.outlineThickness = 0.0f;                // no outline shell on light bg
        opt.outlineColor = glm::vec3(0.f);

        latex3d::MeshData mesh = latex3d::generateLatexMesh(latex, opt);
        if (mesh.vertices.empty() || mesh.indices.empty())
        {
            std::fprintf(stderr, "[samples] empty mesh: %s\n", latex.c_str());
            return false;
        }

        // 2× supersample → box-filter down. Cheap, dramatically nicer edges.
        const int SS = 2;
        const int W = outW * SS;
        const int H = outH * SS;
        Image img(W, H);
        img.fill(0.97f, 0.96f, 0.94f); // off-white paper

        // Mesh AABB in object space.
        V3 mn{1e9f, 1e9f, 1e9f}, mx{-1e9f, -1e9f, -1e9f};
        for (const auto &v : mesh.vertices)
        {
            mn.x = std::min(mn.x, v.position[0]);
            mn.y = std::min(mn.y, v.position[1]);
            mn.z = std::min(mn.z, v.position[2]);
            mx.x = std::max(mx.x, v.position[0]);
            mx.y = std::max(mx.y, v.position[1]);
            mx.z = std::max(mx.z, v.position[2]);
        }
        const V3 center = (mn + mx) * 0.5f;

        // Light yaw + pitch so the extrusion is visible.
        const float yaw = -14.f * 3.14159265f / 180.f;
        const float pitch = 7.f * 3.14159265f / 180.f;

        // Project each AABB corner to find tight screen bounds after rotation.
        float pminX = 1e9f, pmaxX = -1e9f, pminY = 1e9f, pmaxY = -1e9f;
        for (int corner = 0; corner < 8; ++corner)
        {
            V3 c{
                (corner & 1) ? mx.x : mn.x,
                (corner & 2) ? mx.y : mn.y,
                (corner & 4) ? mx.z : mn.z,
            };
            V3 p = rotX(rotY(c - center, yaw), pitch);
            pminX = std::min(pminX, p.x); pmaxX = std::max(pmaxX, p.x);
            pminY = std::min(pminY, p.y); pmaxY = std::max(pmaxY, p.y);
        }
        const float spanX = std::max(pmaxX - pminX, 1e-3f);
        const float spanY = std::max(pmaxY - pminY, 1e-3f);
        const float scale = std::min((W * 0.84f) / spanX, (H * 0.78f) / spanY);
        const float ox = W * 0.5f - 0.5f * (pmaxX + pminX) * scale;
        const float oy = H * 0.5f + 0.5f * (pmaxY + pminY) * scale;

        auto xform = [&](const latex3d::Vertex &v, V3 &outP, V3 &outN) {
            V3 p = V3{v.position[0], v.position[1], v.position[2]} - center;
            V3 n{v.normal[0], v.normal[1], v.normal[2]};
            p = rotX(rotY(p, yaw), pitch);
            n = rotX(rotY(n, yaw), pitch);
            outP = {ox + p.x * scale, oy - p.y * scale, p.z};
            outN = n;
        };

        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
        {
            const auto &va = mesh.vertices[mesh.indices[i + 0]];
            const auto &vb = mesh.vertices[mesh.indices[i + 1]];
            const auto &vc = mesh.vertices[mesh.indices[i + 2]];
            V3 pa, pb, pc, na, nb, nc;
            xform(va, pa, na);
            xform(vb, pb, nb);
            xform(vc, pc, nc);
            V3 ca{va.color[0], va.color[1], va.color[2]};
            V3 cb{vb.color[0], vb.color[1], vb.color[2]};
            V3 cc{vc.color[0], vc.color[1], vc.color[2]};
            drawTri(img, pa, pb, pc, ca, cb, cc, na, nb, nc);
        }

        // Box-filter downsample SSx → 1x with simple gamma encoding.
        std::vector<uint8_t> out(outW * outH * 3);
        const float invN = 1.f / float(SS * SS);
        auto clamp01 = [](float x) { return std::max(0.f, std::min(1.f, x)); };
        for (int y = 0; y < outH; ++y)
        {
            for (int x = 0; x < outW; ++x)
            {
                float r = 0, g = 0, b = 0;
                for (int dy = 0; dy < SS; ++dy)
                {
                    for (int dx = 0; dx < SS; ++dx)
                    {
                        int idx = (y * SS + dy) * W + (x * SS + dx);
                        r += img.rgb[3 * idx + 0];
                        g += img.rgb[3 * idx + 1];
                        b += img.rgb[3 * idx + 2];
                    }
                }
                r *= invN; g *= invN; b *= invN;
                int idx = y * outW + x;
                out[3 * idx + 0] = (uint8_t)std::round(std::pow(clamp01(r), 1.f / 2.2f) * 255.f);
                out[3 * idx + 1] = (uint8_t)std::round(std::pow(clamp01(g), 1.f / 2.2f) * 255.f);
                out[3 * idx + 2] = (uint8_t)std::round(std::pow(clamp01(b), 1.f / 2.2f) * 255.f);
            }
        }

        if (!stbi_write_png(outPath.c_str(), outW, outH, 3, out.data(), outW * 3))
        {
            std::fprintf(stderr, "[samples] failed to write %s\n", outPath.c_str());
            return false;
        }
        std::printf("[samples] %s\n", outPath.c_str());
        return true;
    }

    void diagLog(latex3d::LogLevel, const char *msg, void *)
    {
        std::fprintf(stderr, "[latex3d] %s\n", msg);
    }

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr,
                     "Usage: %s <math-font.otf> <output-dir>\n"
                     "  Renders a fixed gallery of LaTeX expressions to PNGs in <output-dir>.\n",
                     argv[0]);
        return 1;
    }
    const char *fontPath = argv[1];
    const std::string outDir = argv[2];

    latex3d::setLogCallback(&diagLog, nullptr);

    if (!latex3d::loadFont(fontPath, latex3d::FontSlot::Latex))
    {
        std::fprintf(stderr, "[samples] failed to load font: %s\n", fontPath);
        return 1;
    }

    struct Sample
    {
        const char *name;
        const char *latex;
        int w;
        int h;
    };
    const Sample samples[] = {
        {"mass_energy", "E = mc^2",                                                       900,  280},
        {"euler",       "e^{i\\pi} + 1 = 0",                                              900,  280},
        {"derivative",  "\\frac{d}{dx} \\sin(x) = \\cos(x)",                              1100, 380},
        {"sum_squares", "\\sum_{n=1}^{\\infty} \\frac{1}{n^2} = \\frac{\\pi^2}{6}",       1100, 480},
        {"gauss",       "\\int_{-\\infty}^{\\infty} e^{-x^2} dx = \\sqrt{\\pi}",          1100, 380},
        {"accents",     "\\hat{a} \\, \\bar{b} \\, \\widetilde{xyz}",                     900,  280},
    };

    int failed = 0;
    for (const auto &s : samples)
    {
        const std::string out = outDir + "/" + s.name + ".png";
        if (!render(s.latex, out, s.w, s.h)) ++failed;
    }

    latex3d::unloadFont(latex3d::FontSlot::Latex);
    return failed ? 1 : 0;
}
