#include <latex3d/text3d.h>
#include <latex3d/math_table.h>
#include <latex3d/log.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_TRUETYPE_TABLES_H

#include <mapbox/earcut.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace latex3d
{
    namespace
    {
        // Convert glm::vec3 triple → flat-float Vertex POD. The library's
        // public mesh type is renderer-agnostic (no glm in its public header),
        // but internal math is much cleaner in glm — we keep the cross/normal
        // pipeline in glm and convert at the emit boundary.
        inline Vertex makeVertex(const glm::vec3 &p,
                                 const glm::vec3 &c,
                                 const glm::vec3 &n)
        {
            return Vertex{{p.x, p.y, p.z},
                          {c.x, c.y, c.z},
                          {n.x, n.y, n.z}};
        }

        // 2D polygon outline in font units (FreeType 26.6 fixed → converted to
        // floats in em-fraction space). One GlyphShape may contain multiple
        // contours: outer boundaries (CCW by our normalisation) and holes
        // (CW). Earcut accepts this directly as rings-within-polygons.
        struct GlyphShape
        {
            // Each polygon is {outer_ring, hole_ring_0, hole_ring_1, ...}.
            // A glyph may have several disjoint polygons (e.g. 'i' has a
            // separate dot) so we store a list of them. Uses the public
            // PolygonSet type so callers can operate on it directly.
            PolygonSet polygons;
            float advanceEm = 0.f;    // horizontal advance in em units
            float lineHeightEm = 1.2f; // native line-height fallback
        };

        struct FontState
        {
            FT_Library library = nullptr;
            FT_Face face = nullptr;
            float unitsPerEm = 1000.f;
            float ascenderEm = 0.8f;
            float descenderEm = -0.2f;
            float lineHeightEm = 1.2f;
            // Cache key composes the codepoint with a quantised stroke-weight
            // bucket (1/1000 em granularity). Different weights produce
            // genuinely different outline polygons (FT_Outline_Embolden or
            // variable-axis bold both mutate the points), so they cannot
            // share a cache slot.
            std::unordered_map<uint64_t, GlyphShape> cache;
            std::mutex mtx;
            bool ready = false;

            // Variable-font ('wght') axis support. Preferred path for heavy
            // text: asking the designer-drawn bold through FT_Set_Var_*
            // yields clean counters, whereas FT_Outline_Embolden self-
            // intersects outlines when the stroke offset exceeds a counter's
            // width (the insides of 'a', 'e', 'o', 'p' collapse into blobs).
            // When this face has no wght axis (e.g. STIX Two Math Regular),
            // we fall back to clamped FT_Outline_Embolden.
            bool hasWeightAxis = false;
            FT_UInt numAxes = 0;
            FT_UInt weightAxisIdx = 0;
            FT_Fixed weightAxisDefault = 0; // 16.16 design-space value
            FT_Fixed weightAxisMin = 0;
            FT_Fixed weightAxisMax = 0;
            std::vector<FT_Fixed> defaultCoords; // one entry per axis

            // Per-slot ceiling for FT_Outline_Embolden. Display fonts
            // (Bungee etc.) have wide native strokes and large counters
            // that tolerate a heavy outset, so we keep the user-facing
            // weight knob meaningful all the way to ~0.2 em. Math fonts
            // (STIX, Fira Math, Latin Modern) have tight curves on Greek
            // and operators that self-intersect well before that.
            float maxEmboldenEm = 0.08f;

            // Parsed OpenType MATH table. Populated for math fonts (Fira
            // Math, STIX Two Math, Latin Modern Math, …); has() returns
            // false otherwise and the LaTeX layouter falls back to its
            // baked-in stub constants.
            MathTable mathTable;
        };

        // Two independent font slots: Display is used by generateTextMesh()
        // for in-world decorative text (keeps the project's Cormorant Garamond
        // aesthetic). Latex is used by the LaTeX pipeline — it needs a font
        // that always works (full Greek + math Unicode coverage), so a
        // standard STIX-style face is loaded here regardless of the display
        // font choice.
        FontState &fontState(FontSlot slot = FontSlot::Display)
        {
            static FontState sDisplay;
            static FontState sLatex;
            return (slot == FontSlot::Latex) ? sLatex : sDisplay;
        }

        // ── Curve flattening ────────────────────────────────────────────────
        // Adaptive subdivision of quadratic/cubic Beziers. Tolerance is in the
        // same units as the control-point coordinates (em-space here).
        void flattenQuadratic(std::vector<std::array<float, 2>> &out,
                              float x0, float y0, float x1, float y1,
                              float x2, float y2, float tol, int depth = 0)
        {
            // Distance from midpoint to control — if small, accept as line.
            float mx = 0.25f * x0 + 0.5f * x1 + 0.25f * x2;
            float my = 0.25f * y0 + 0.5f * y1 + 0.25f * y2;
            float lx = 0.5f * (x0 + x2);
            float ly = 0.5f * (y0 + y2);
            float dx = mx - lx, dy = my - ly;
            if (depth > 14 || (dx * dx + dy * dy) < tol * tol)
            {
                out.push_back({x2, y2});
                return;
            }
            float x01 = 0.5f * (x0 + x1), y01 = 0.5f * (y0 + y1);
            float x12 = 0.5f * (x1 + x2), y12 = 0.5f * (y1 + y2);
            float xm = 0.5f * (x01 + x12), ym = 0.5f * (y01 + y12);
            flattenQuadratic(out, x0, y0, x01, y01, xm, ym, tol, depth + 1);
            flattenQuadratic(out, xm, ym, x12, y12, x2, y2, tol, depth + 1);
        }

        void flattenCubic(std::vector<std::array<float, 2>> &out,
                          float x0, float y0, float x1, float y1,
                          float x2, float y2, float x3, float y3,
                          float tol, int depth = 0)
        {
            float lx = 0.5f * (x0 + x3), ly = 0.5f * (y0 + y3);
            float mx = 0.125f * x0 + 0.375f * x1 + 0.375f * x2 + 0.125f * x3;
            float my = 0.125f * y0 + 0.375f * y1 + 0.375f * y2 + 0.125f * y3;
            float dx = mx - lx, dy = my - ly;
            if (depth > 14 || (dx * dx + dy * dy) < tol * tol)
            {
                out.push_back({x3, y3});
                return;
            }
            float x01 = 0.5f * (x0 + x1), y01 = 0.5f * (y0 + y1);
            float x12 = 0.5f * (x1 + x2), y12 = 0.5f * (y1 + y2);
            float x23 = 0.5f * (x2 + x3), y23 = 0.5f * (y2 + y3);
            float x012 = 0.5f * (x01 + x12), y012 = 0.5f * (y01 + y12);
            float x123 = 0.5f * (x12 + x23), y123 = 0.5f * (y12 + y23);
            float xm = 0.5f * (x012 + x123), ym = 0.5f * (y012 + y123);
            flattenCubic(out, x0, y0, x01, y01, x012, y012, xm, ym, tol, depth + 1);
            flattenCubic(out, xm, ym, x123, y123, x23, y23, x3, y3, tol, depth + 1);
        }

        // ── Outline decomposition via FT_Outline_Decompose callbacks ────────
        struct DecomposeCtx
        {
            std::vector<std::vector<std::array<float, 2>>> contours;
            float scale = 1.f; // font units → em
            float tol = 0.01f; // flattening tolerance in em
            float curX = 0.f, curY = 0.f;
        };

        int ftMoveTo(const FT_Vector *to, void *user)
        {
            auto *ctx = static_cast<DecomposeCtx *>(user);
            float x = to->x * ctx->scale;
            float y = to->y * ctx->scale;
            ctx->contours.push_back({});
            ctx->contours.back().push_back({x, y});
            ctx->curX = x;
            ctx->curY = y;
            return 0;
        }

        int ftLineTo(const FT_Vector *to, void *user)
        {
            auto *ctx = static_cast<DecomposeCtx *>(user);
            float x = to->x * ctx->scale;
            float y = to->y * ctx->scale;
            if (!ctx->contours.empty())
                ctx->contours.back().push_back({x, y});
            ctx->curX = x;
            ctx->curY = y;
            return 0;
        }

        int ftConicTo(const FT_Vector *ctrl, const FT_Vector *to, void *user)
        {
            auto *ctx = static_cast<DecomposeCtx *>(user);
            float cx = ctrl->x * ctx->scale, cy = ctrl->y * ctx->scale;
            float ex = to->x * ctx->scale, ey = to->y * ctx->scale;
            if (!ctx->contours.empty())
                flattenQuadratic(ctx->contours.back(),
                                 ctx->curX, ctx->curY, cx, cy, ex, ey, ctx->tol);
            ctx->curX = ex;
            ctx->curY = ey;
            return 0;
        }

        int ftCubicTo(const FT_Vector *c1, const FT_Vector *c2,
                      const FT_Vector *to, void *user)
        {
            auto *ctx = static_cast<DecomposeCtx *>(user);
            float x1 = c1->x * ctx->scale, y1 = c1->y * ctx->scale;
            float x2 = c2->x * ctx->scale, y2 = c2->y * ctx->scale;
            float ex = to->x * ctx->scale, ey = to->y * ctx->scale;
            if (!ctx->contours.empty())
                flattenCubic(ctx->contours.back(),
                             ctx->curX, ctx->curY, x1, y1, x2, y2, ex, ey, ctx->tol);
            ctx->curX = ex;
            ctx->curY = ey;
            return 0;
        }

        float signedArea(const std::vector<std::array<float, 2>> &ring)
        {
            float a = 0.f;
            const size_t n = ring.size();
            for (size_t i = 0, j = n - 1; i < n; j = i++)
                a += (ring[j][0] + ring[i][0]) * (ring[i][1] - ring[j][1]);
            return 0.5f * a;
        }

        bool pointInRing(float px, float py,
                         const std::vector<std::array<float, 2>> &ring)
        {
            bool inside = false;
            const size_t n = ring.size();
            for (size_t i = 0, j = n - 1; i < n; j = i++)
            {
                float xi = ring[i][0], yi = ring[i][1];
                float xj = ring[j][0], yj = ring[j][1];
                float dy = yj - yi;
                if (dy == 0.f) continue; // horizontal edge — cannot be crossed by a horizontal ray
                if ((yi > py) != (yj > py))
                {
                    float xCross = (xj - xi) * (py - yi) / dy + xi;
                    if (px < xCross) inside = !inside;
                }
            }
            return inside;
        }

        void reverseRing(std::vector<std::array<float, 2>> &r)
        {
            std::reverse(r.begin(), r.end());
        }

        // Pick a point guaranteed to lie strictly inside a simple ring.
        // Strategy: take the midpoint of an edge and offset it slightly along
        // the edge's inward normal (sign chosen by the ring's winding). Try
        // edges in descending length order — for concave rings (e.g. the bowl
        // counter of 'b' at saturated bold, which comes out lobed after curve
        // flattening) the LONGEST edge can sit on a re-entrant section where
        // the inward normal actually leaves the ring locally. We verify each
        // candidate with pointInRing and only fall back to a shorter edge if
        // the verification fails. Without this retry the bowl counters of
        // 'b'/'q' and the eye of 'B' get classified as outer rings (= solid
        // blob, no counter) because their probe accidentally lands outside.
        std::array<float, 2>
        interiorProbe(const std::vector<std::array<float, 2>> &ring,
                      float signedAreaVal)
        {
            const size_t n = ring.size();
            if (n < 3) return {0.f, 0.f};
            std::vector<std::pair<float, size_t>> edges;
            edges.reserve(n);
            for (size_t i = 0; i < n; ++i)
            {
                const auto &p0 = ring[i];
                const auto &p1 = ring[(i + 1) % n];
                float ex = p1[0] - p0[0];
                float ey = p1[1] - p0[1];
                edges.emplace_back(ex * ex + ey * ey, i);
            }
            std::sort(edges.begin(), edges.end(),
                      [](const auto &a, const auto &b) { return a.first > b.first; });

            std::array<float, 2> fallback{0.f, 0.f};
            bool haveFallback = false;
            for (const auto &e : edges)
            {
                if (e.first <= 0.f) continue;
                size_t idx = e.second;
                const auto &p0 = ring[idx];
                const auto &p1 = ring[(idx + 1) % n];
                float mx = 0.5f * (p0[0] + p1[0]);
                float my = 0.5f * (p0[1] + p1[1]);
                float ex = p1[0] - p0[0];
                float ey = p1[1] - p0[1];
                float len = std::sqrt(e.first);
                if (len < 1e-8f) continue;
                float nx, ny;
                if (signedAreaVal > 0.f) { nx = -ey / len; ny = ex / len; }
                else                     { nx = ey / len;  ny = -ex / len; }
                float eps = std::max(1e-5f, 1e-3f * len);
                std::array<float, 2> probe{mx + nx * eps, my + ny * eps};
                if (!haveFallback) { fallback = probe; haveFallback = true; }
                if (pointInRing(probe[0], probe[1], ring))
                    return probe;
            }
            return fallback;
        }

        // Group flat contours into polygons-with-holes. Strategy:
        //   1. For each contour, compute signed area & parity (outer or hole).
        //      We normalise so outer rings end up CCW, holes CW.
        //   2. A contour is a hole if it sits inside an odd number of other
        //      contours — the nearest containing contour becomes its parent.
        std::vector<std::vector<std::vector<std::array<float, 2>>>>
        groupContours(std::vector<std::vector<std::array<float, 2>>> contours)
        {
            struct Info
            {
                float area = 0.f;       // signed
                float absArea = 0.f;
                int parent = -1;
                bool isHole = false;
            };
            std::vector<Info> info(contours.size());
            for (size_t i = 0; i < contours.size(); ++i)
            {
                info[i].area = signedArea(contours[i]);
                info[i].absArea = std::fabs(info[i].area);
            }
            // Parent = smallest-area container. Test containment with an
            // interior probe point rather than vertex 0 — vertex 0 can land
            // on another contour's edge (shared tangents, extrema, or just
            // coincidental proximity after curve flattening) which makes the
            // ray-cast classification flip unpredictably.
            std::vector<std::array<float, 2>> probes(contours.size());
            for (size_t i = 0; i < contours.size(); ++i)
            {
                if (contours[i].size() < 3) continue;
                probes[i] = interiorProbe(contours[i], info[i].area);
            }
            for (size_t i = 0; i < contours.size(); ++i)
            {
                if (contours[i].size() < 3) continue;
                float px = probes[i][0];
                float py = probes[i][1];
                int best = -1;
                float bestArea = 1e30f;
                for (size_t j = 0; j < contours.size(); ++j)
                {
                    if (i == j) continue;
                    if (contours[j].size() < 3) continue;
                    if (info[j].absArea <= info[i].absArea) continue;
                    if (info[j].absArea >= bestArea) continue;
                    if (pointInRing(px, py, contours[j]))
                    {
                        best = static_cast<int>(j);
                        bestArea = info[j].absArea;
                    }
                }
                info[i].parent = best;
            }
            // Depth parity — outer if parent chain length is even.
            for (size_t i = 0; i < contours.size(); ++i)
            {
                int depth = 0;
                int p = info[i].parent;
                while (p >= 0) { depth++; p = info[p].parent; }
                info[i].isHole = (depth % 2) == 1;
            }
            // Normalise winding: outer = CCW (positive area), hole = CW.
            for (size_t i = 0; i < contours.size(); ++i)
            {
                bool ccw = info[i].area > 0.f;
                if (info[i].isHole ? ccw : !ccw)
                    reverseRing(contours[i]);
            }
            // Assemble polygons: each outer + its direct-child holes.
            std::vector<std::vector<std::vector<std::array<float, 2>>>> polys;
            std::unordered_map<int, int> outerIdx; // contour index → polys index
            for (size_t i = 0; i < contours.size(); ++i)
            {
                if (info[i].isHole || contours[i].size() < 3) continue;
                outerIdx[(int)i] = (int)polys.size();
                polys.push_back({std::move(contours[i])});
            }
            for (size_t i = 0; i < contours.size(); ++i)
            {
                if (!info[i].isHole || contours[i].size() < 3) continue;
                auto it = outerIdx.find(info[i].parent);
                if (it != outerIdx.end())
                    polys[it->second].push_back(std::move(contours[i]));
                // Orphan holes (parent was also a hole or missing) are dropped.
            }
            return polys;
        }

        // Compose (codepoint, weight) into a single cache key. Weight is
        // quantised to 1/1000 em so floating-point noise doesn't explode the
        // cache into a thousand near-identical entries.
        uint64_t glyphCacheKey(uint32_t codepoint, float weight)
        {
            int32_t wb = static_cast<int32_t>(std::lround(weight * 1000.f));
            uint64_t wbU = static_cast<uint64_t>(static_cast<uint32_t>(wb));
            return (wbU << 32) | static_cast<uint64_t>(codepoint);
        }

        const GlyphShape *getOrBuildGlyph(FontState &st, uint32_t codepoint,
                                          float tol, float weight)
        {
            if (!st.ready) return nullptr;

            // Note: tol is stored per-glyph at build time. We key by
            // (codepoint, weight) — the cache assumes a single tol per
            // session. Callers that want a different tol should flush by
            // re-init. For a game this is fine since the value is fixed by
            // Text3DOptions::curveTolerance.
            uint64_t key = glyphCacheKey(codepoint, weight);
            auto it = st.cache.find(key);
            if (it != st.cache.end())
                return &it->second;

            FT_UInt gi = FT_Get_Char_Index(st.face, codepoint);
            if (gi == 0)
            {
                // Render as blank space — still cache to avoid repeated lookups.
                GlyphShape blank;
                blank.advanceEm = 0.35f; // reasonable space width
                blank.lineHeightEm = st.lineHeightEm;
                auto [ins, _] = st.cache.emplace(key, std::move(blank));
                return &ins->second;
            }

            // Resolve the requested weight to the safest delivery path.
            //   • Variable font: use the wght axis exclusively. The axis is
            //     designer-drawn so curves and counters stay topologically
            //     correct at every step. Capped at axis maximum.
            //   • Non-variable font (e.g. STIX Two Math): no axis available,
            //     so use clamped FT_Outline_Embolden. Capped tight so
            //     counters survive.
            // FT_Outline_Embolden is NOT used to top up a variable axis: at
            // saturation the strokes are already wide and any further
            // embolden produces self-intersections at tight curves (the
            // tops of 'b'/'l'/'k', the eye of 'e', the bowls of 'B'). The
            // artifacts surface as broken serifs / spikes in the rendered
            // glyph and can't be cleaned up after the fact — embolden
            // mutates the FT outline before our polygon code sees it.
            // For text that needs to read as thicker surface than this
            // delivers, swap in a heavier source font (Cormorant Bold /
            // Black) instead of pushing the weight parameter higher.
            float axisW = 0.f;
            float emboldenW = 0.f;
            if (weight > 0.f)
            {
                if (st.hasWeightAxis)
                {
                    const float axisSatW =
                        static_cast<float>(st.weightAxisMax - st.weightAxisDefault)
                        / 65536.f / 3000.f;
                    axisW = std::min(weight, std::max(0.f, axisSatW));
                }
                else
                {
                    // Per-slot cap (see FontState::maxEmboldenEm): 0.20 em
                    // for the Display slot so user-tuned weight values in
                    // mpo.xml actually move the glyph thickness, 0.08 em
                    // for the Latex slot so tight math-font curves survive.
                    emboldenW = std::min(weight, st.maxEmboldenEm);
                }
            }

            // Apply the variable axis first (or reset to default when
            // unused, so a prior heavy load doesn't leak axis state).
            if (st.hasWeightAxis)
            {
                std::vector<FT_Fixed> coords = st.defaultCoords;
                if (axisW > 0.f)
                {
                    float boost = axisW * 3000.f;
                    FT_Fixed reqVal = st.weightAxisDefault
                        + static_cast<FT_Fixed>(std::lround(boost * 65536.f));
                    if (reqVal > st.weightAxisMax) reqVal = st.weightAxisMax;
                    if (reqVal < st.weightAxisMin) reqVal = st.weightAxisMin;
                    coords[st.weightAxisIdx] = reqVal;
                }
                FT_Set_Var_Design_Coordinates(st.face, st.numAxes,
                                              coords.data());
            }

            if (FT_Load_Glyph(st.face, gi, FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP) != 0)
                return nullptr;

            GlyphShape shape;
            shape.advanceEm = (float)st.face->glyph->metrics.horiAdvance / st.unitsPerEm;
            shape.lineHeightEm = st.lineHeightEm;

            FT_Outline &outline = st.face->glyph->outline;
            // Optional embolden pass — either the full weight (non-variable
            // fallback) or the overflow above the axis's max (stacked on
            // top of the axis-set bold).
            if (emboldenW > 0.f
                && outline.n_contours > 0 && outline.n_points > 0)
            {
                FT_Pos strength = static_cast<FT_Pos>(
                    std::lround(emboldenW * st.unitsPerEm));
                if (strength > 0)
                {
                    FT_Outline_Embolden(&outline, strength);
                    shape.advanceEm += emboldenW;
                }
            }
            if (outline.n_contours > 0 && outline.n_points > 0)
            {
                DecomposeCtx ctx;
                ctx.scale = 1.f / st.unitsPerEm;
                ctx.tol = tol;

                FT_Outline_Funcs funcs{};
                funcs.move_to = ftMoveTo;
                funcs.line_to = ftLineTo;
                funcs.conic_to = ftConicTo;
                funcs.cubic_to = ftCubicTo;
                funcs.shift = 0;
                funcs.delta = 0;

                if (FT_Outline_Decompose(&outline, &funcs, &ctx) == 0)
                {
                    // Cleanup pass: drop the implicit close-point duplicate,
                    // then collapse consecutive points that landed within a
                    // sub-tolerance distance of each other. Curve flattening
                    // can emit such micro-segments where two on-curve points
                    // sit close in font units (FT_LOAD_NO_SCALE) but quantise
                    // to the same em-space coord at our 1/upem scale.
                    // Earcut chokes on zero-area edges and signedArea() can
                    // flip sign for rings made mostly of micro-segments —
                    // both manifest as missing or inverted glyph parts.
                    const float dedupTol2 = 1e-12f;
                    for (auto &c : ctx.contours)
                    {
                        if (c.empty()) continue;
                        if (c.size() >= 2 && c.front() == c.back()) c.pop_back();
                        std::vector<std::array<float, 2>> cleaned;
                        cleaned.reserve(c.size());
                        for (size_t i = 0; i < c.size(); ++i)
                        {
                            if (!cleaned.empty())
                            {
                                float dx = c[i][0] - cleaned.back()[0];
                                float dy = c[i][1] - cleaned.back()[1];
                                if (dx * dx + dy * dy < dedupTol2) continue;
                            }
                            cleaned.push_back(c[i]);
                        }
                        if (cleaned.size() >= 2)
                        {
                            float dx = cleaned.front()[0] - cleaned.back()[0];
                            float dy = cleaned.front()[1] - cleaned.back()[1];
                            if (dx * dx + dy * dy < dedupTol2) cleaned.pop_back();
                        }
                        c = std::move(cleaned);
                    }
                    shape.polygons = groupContours(std::move(ctx.contours));
                }
            }

            auto [ins, _] = st.cache.emplace(key, std::move(shape));
            return &ins->second;
        }

        void pushQuad(MeshData &data,
                      const glm::vec3 &a, const glm::vec3 &b,
                      const glm::vec3 &c, const glm::vec3 &d,
                      const glm::vec3 &normal, const glm::vec3 &color)
        {
            uint32_t base = static_cast<uint32_t>(data.vertices.size());
            data.vertices.push_back(makeVertex(a, color, normal));
            data.vertices.push_back(makeVertex(b, color, normal));
            data.vertices.push_back(makeVertex(c, color, normal));
            data.vertices.push_back(makeVertex(d, color, normal));
            data.indices.push_back(base);
            data.indices.push_back(base + 1);
            data.indices.push_back(base + 2);
            data.indices.push_back(base);
            data.indices.push_back(base + 2);
            data.indices.push_back(base + 3);
        }

        // Shared extrusion of a single polygon-set at (penX, baselineY) into
        // `data`. em scales em-space to world units; depth controls Z extent
        // (<=0 emits a single flat front face, panel mode).
        void extrudePolygonsImpl(MeshData &data,
                                 const PolygonSet &polygons,
                                 float penX, float baselineY,
                                 float em, float depth,
                                 const glm::vec3 &color)
        {
            const bool flat = (depth <= 0.f);
            const float halfDepth = flat ? 0.f : 0.5f * depth;

            for (const auto &poly : polygons)
            {
                if (poly.empty() || poly[0].size() < 3) continue;

                // Triangulate polygon (outer + holes) in em-space — then
                // scale/translate into world space as we emit vertices.
                std::vector<uint32_t> tris = mapbox::earcut<uint32_t>(poly);

                // Flatten rings into a single indexable vertex array (same
                // ordering earcut used).
                std::vector<std::array<float, 2>> flat2;
                for (const auto &ring : poly)
                    flat2.insert(flat2.end(), ring.begin(), ring.end());

                const uint32_t frontBase = static_cast<uint32_t>(data.vertices.size());
                // Front face verts (+Z, normal out-of-screen)
                for (const auto &p : flat2)
                {
                    glm::vec3 pos(penX + p[0] * em,
                                  baselineY + p[1] * em,
                                  +halfDepth);
                    data.vertices.push_back(makeVertex(pos, color, glm::vec3(0.f, 0.f, 1.f)));
                }
                // Front triangles (CCW as earcut gives)
                for (size_t t = 0; t + 2 < tris.size(); t += 3)
                {
                    data.indices.push_back(frontBase + tris[t]);
                    data.indices.push_back(frontBase + tris[t + 1]);
                    data.indices.push_back(frontBase + tris[t + 2]);
                }

                if (flat)
                    continue; // panel mode: front face only

                // Back face verts (-Z, normal into-screen). Duplicate to
                // keep flat-shaded normals separate from the front face.
                const uint32_t backBase = static_cast<uint32_t>(data.vertices.size());
                for (const auto &p : flat2)
                {
                    glm::vec3 pos(penX + p[0] * em,
                                  baselineY + p[1] * em,
                                  -halfDepth);
                    data.vertices.push_back(makeVertex(pos, color, glm::vec3(0.f, 0.f, -1.f)));
                }
                // Back triangles (reverse winding so they face -Z)
                for (size_t t = 0; t + 2 < tris.size(); t += 3)
                {
                    data.indices.push_back(backBase + tris[t + 2]);
                    data.indices.push_back(backBase + tris[t + 1]);
                    data.indices.push_back(backBase + tris[t]);
                }

                // Side walls: one quad per edge of every ring. Per-vertex
                // normals are the average of the two incident edge normals,
                // so shading varies smoothly along curves instead of
                // stepping per flattened segment — this kills the sparkle
                // that small curve segments produce at long view distances.
                for (const auto &ring : poly)
                {
                    const size_t n = ring.size();
                    if (n < 2) continue;

                    std::vector<glm::vec3> edgeN(n);
                    for (size_t i = 0; i < n; ++i)
                    {
                        size_t j = (i + 1) % n;
                        glm::vec3 edge(
                            (ring[j][0] - ring[i][0]) * em,
                            (ring[j][1] - ring[i][1]) * em,
                            0.f);
                        glm::vec3 en = glm::cross(edge, glm::vec3(0.f, 0.f, 1.f));
                        float len = glm::length(en);
                        edgeN[i] = (len > 1e-9f) ? (en / len)
                                                 : glm::vec3(1.f, 0.f, 0.f);
                    }

                    std::vector<glm::vec3> vertN(n);
                    for (size_t i = 0; i < n; ++i)
                    {
                        size_t p = (i + n - 1) % n;
                        glm::vec3 sum = edgeN[p] + edgeN[i];
                        float len = glm::length(sum);
                        vertN[i] = (len > 1e-6f) ? (sum / len) : edgeN[i];
                    }

                    for (size_t i = 0; i < n; ++i)
                    {
                        size_t j = (i + 1) % n;
                        const auto &a = ring[i];
                        const auto &b = ring[j];

                        glm::vec3 af(penX + a[0] * em,
                                     baselineY + a[1] * em, +halfDepth);
                        glm::vec3 bf(penX + b[0] * em,
                                     baselineY + b[1] * em, +halfDepth);
                        glm::vec3 ab(penX + a[0] * em,
                                     baselineY + a[1] * em, -halfDepth);
                        glm::vec3 bb(penX + b[0] * em,
                                     baselineY + b[1] * em, -halfDepth);

                        uint32_t base = static_cast<uint32_t>(data.vertices.size());
                        data.vertices.push_back(makeVertex(af, color, vertN[i]));
                        data.vertices.push_back(makeVertex(bf, color, vertN[j]));
                        data.vertices.push_back(makeVertex(bb, color, vertN[j]));
                        data.vertices.push_back(makeVertex(ab, color, vertN[i]));
                        data.indices.push_back(base + 0);
                        data.indices.push_back(base + 1);
                        data.indices.push_back(base + 2);
                        data.indices.push_back(base + 0);
                        data.indices.push_back(base + 2);
                        data.indices.push_back(base + 3);
                    }
                }
            }
        }

        // Offset every ring of `src` outward by `t` em (polygon dilation).
        // Windings were normalised by groupContours so outer rings are CCW
        // and holes are CW — the right-hand edge normal (ey, -ex) always
        // points away from the polygon material for both, so one formula
        // grows outer rings AND shrinks holes, cleanly enlarging the
        // visible glyph shape by `t` on every silhouette side.
        //
        // Uses a miter join with a tight clamp (maxMiter = 2). A generous
        // clamp (we previously ran at 4) produced visible spikes at the
        // sharp V-joints of thin-stroked glyphs (v, y, x in FiraMath, also
        // W and M): the overshooting miter point creates a self-intersecting
        // offset ring, which earcut triangulates with triangles that fold
        // across the glyph body and paint the outline color over the fill —
        // the "black inside the letter" artifact. 2.0 is the standard miter
        // limit CSS/SVG use for the same reason, and preserves serif tips
        // visually while capping apex overshoot at 2× the outline width.
        // Degenerate zero-length edges are skipped — otherwise pure-straight
        // segments after curve flattening could emit NaN normals.
        PolygonSet inflatePolygonSetImpl(const PolygonSet &src, float t)
        {
            if (t <= 0.f) return src;
            PolygonSet out = src;
            const float maxMiter = 2.f;

            // Shoelace signed area — sign identifies winding (outer vs hole).
            auto signedArea = [](const std::vector<std::array<float, 2>> &ring) {
                double a = 0.0;
                const size_t n = ring.size();
                for (size_t i = 0; i < n; ++i)
                {
                    const auto &p0 = ring[i];
                    const auto &p1 = ring[(i + 1) % n];
                    a += static_cast<double>(p0[0]) * p1[1] -
                         static_cast<double>(p1[0]) * p0[1];
                }
                return 0.5f * static_cast<float>(a);
            };

            for (auto &poly : out)
            {
                // Reference sign from the first (outer) ring; any ring whose
                // area has the opposite sign is a hole.
                const float outerArea = poly.empty() ? 0.f
                                                     : signedArea(poly.front());

                std::vector<size_t> dropRings;
                for (size_t ringIdx = 0; ringIdx < poly.size(); ++ringIdx)
                {
                    auto &ring = poly[ringIdx];
                    const size_t n = ring.size();
                    if (n < 3) continue;
                    std::vector<std::array<float, 2>> normals(n);
                    for (size_t i = 0; i < n; ++i)
                    {
                        const auto &p0 = ring[i];
                        const auto &p1 = ring[(i + 1) % n];
                        float ex = p1[0] - p0[0];
                        float ey = p1[1] - p0[1];
                        float len = std::sqrt(ex * ex + ey * ey);
                        if (len < 1e-9f) { normals[i] = {0.f, 0.f}; continue; }
                        normals[i] = { ey / len, -ex / len };
                    }
                    std::vector<std::array<float, 2>> offset(n);
                    for (size_t i = 0; i < n; ++i)
                    {
                        size_t ip = (i + n - 1) % n;
                        const auto &np = normals[ip];
                        const auto &nn = normals[i];
                        float mx = np[0] + nn[0];
                        float my = np[1] + nn[1];
                        float k = 1.f + (np[0] * nn[0] + np[1] * nn[1]);
                        std::array<float, 2> shift;
                        if (k < 1.f / maxMiter || (mx * mx + my * my) < 1e-12f)
                        {
                            // Very sharp turn or collinear U-turn: fall back
                            // to the previous-edge normal scaled by the clamp
                            // so we never overshoot into a visible spike.
                            shift = { np[0] * t * maxMiter, np[1] * t * maxMiter };
                        }
                        else
                        {
                            shift = { mx * t / k, my * t / k };
                        }
                        offset[i] = { ring[i][0] + shift[0], ring[i][1] + shift[1] };
                    }

                    // Holes (rings whose winding is opposite the outer) shrink
                    // when offset. If the stroke is thinner than 2*t the hole
                    // collapses or flips. Drop it from the shell in that case
                    // so the shell is solid under the fill's hole — the fill
                    // hole then reveals outline color instead of empty space,
                    // which is the same border treatment the outside gets.
                    const bool isHole = ringIdx > 0 &&
                                        (outerArea * signedArea(ring) < 0.f);
                    if (isHole)
                    {
                        const float origArea = signedArea(ring);
                        const float newArea = signedArea(offset);
                        const bool flipped = origArea * newArea <= 0.f;
                        const bool degenerate = std::abs(newArea) <
                                                std::abs(origArea) * 0.02f;
                        if (flipped || degenerate)
                        {
                            dropRings.push_back(ringIdx);
                            continue;
                        }
                    }

                    ring = std::move(offset);
                }

                // Erase dropped holes back-to-front so earlier indices stay
                // valid during removal.
                for (auto it = dropRings.rbegin(); it != dropRings.rend(); ++it)
                    poly.erase(poly.begin() + *it);
            }
            return out;
        }

        // Internal extrusion helper with a Z-center offset. The outline shell
        // is pushed back by a small epsilon so its front face sits just
        // behind the main glyph's front face, avoiding z-fight on the
        // coplanar region where both meshes overlap in XY.
        void extrudePolygonsZ(MeshData &data,
                              const PolygonSet &polygons,
                              float penX, float baselineY,
                              float em, float depth, float zCenter,
                              const glm::vec3 &color)
        {
            // Reuse the existing extruder and then shift the appended
            // vertices in Z. Cheaper than duplicating the triangulation path.
            const size_t firstVtx = data.vertices.size();
            extrudePolygonsImpl(data, polygons, penX, baselineY, em, depth,
                                color);
            if (zCenter == 0.f) return;
            for (size_t i = firstVtx; i < data.vertices.size(); ++i)
                data.vertices[i].position[2] += zCenter;
        }

        std::vector<std::string> splitLines(const std::string &s)
        {
            std::vector<std::string> out;
            std::string cur;
            for (char c : s)
            {
                if (c == '\n') { out.push_back(cur); cur.clear(); }
                else if (c != '\r') cur.push_back(c);
            }
            out.push_back(cur);
            return out;
        }

        float measureLineWidthEm(FontState &st, const std::string &line,
                                 float extraAdvance, float tol, float weight)
        {
            float w = 0.f;
            for (char c : line)
            {
                const GlyphShape *g = getOrBuildGlyph(st, static_cast<uint32_t>(
                    static_cast<unsigned char>(c)), tol, weight);
                if (!g) continue;
                w += g->advanceEm + extraAdvance;
            }
            return w;
        }
    } // namespace

    bool loadFont(const std::string &ttfPath, FontSlot slot)
    {
        auto &st = fontState(slot);
        std::lock_guard<std::mutex> lk(st.mtx);

        if (st.face) { FT_Done_Face(st.face); st.face = nullptr; }
        if (st.library) { FT_Done_FreeType(st.library); st.library = nullptr; }
        st.cache.clear();
        st.ready = false;

        if (FT_Init_FreeType(&st.library) != 0)
        {
            LATEX3D_LOG_ERROR("[text3d] FT_Init_FreeType failed");
            return false;
        }
        if (FT_New_Face(st.library, ttfPath.c_str(), 0, &st.face) != 0)
        {
            LATEX3D_LOG_ERROR("[text3d] FT_New_Face failed for '%s'", ttfPath.c_str());
            FT_Done_FreeType(st.library);
            st.library = nullptr;
            return false;
        }
        st.unitsPerEm = st.face->units_per_EM > 0
            ? static_cast<float>(st.face->units_per_EM) : 1000.f;
        st.ascenderEm = static_cast<float>(st.face->ascender) / st.unitsPerEm;
        st.descenderEm = static_cast<float>(st.face->descender) / st.unitsPerEm;
        st.lineHeightEm = static_cast<float>(st.face->height) / st.unitsPerEm;

        // Display slot loads signage faces (Bungee, Archivo Black…) with
        // wide native strokes — safe to embolden far beyond the math-face
        // limit. Latex slot disables synthetic embolden entirely: math
        // faces (Fira Math, STIX Two Math) have tight Greek and operator
        // counters where even small embolden offsets self-intersect after
        // FT_Outline_Embolden — the canonical symptom is β's bowls
        // filling solid because the post-embolden hole bowtie-crosses
        // itself near the waist and earcut can't recover. Variable math
        // fonts (none today, but room for them later) still get the
        // axis-driven bold path which produces designer-drawn outlines.
        if (slot == FontSlot::Display)
            st.maxEmboldenEm = 0.20f;
        else
            st.maxEmboldenEm = 0.f;

        // Detect a 'wght' variable axis (OpenType variable fonts). When
        // present, we prefer it over FT_Outline_Embolden for weight > 0: the
        // axis gives us the type designer's drawn bold, so counters stay
        // clean even at heavy weights.
        st.hasWeightAxis = false;
        st.numAxes = 0;
        st.defaultCoords.clear();
        FT_MM_Var *mm = nullptr;
        if (FT_Get_MM_Var(st.face, &mm) == 0 && mm != nullptr)
        {
            st.numAxes = mm->num_axis;
            st.defaultCoords.resize(st.numAxes);
            for (FT_UInt a = 0; a < mm->num_axis; ++a)
            {
                st.defaultCoords[a] = mm->axis[a].def;
                if (mm->axis[a].tag == FT_MAKE_TAG('w', 'g', 'h', 't'))
                {
                    st.hasWeightAxis = true;
                    st.weightAxisIdx = a;
                    st.weightAxisDefault = mm->axis[a].def;
                    st.weightAxisMin = mm->axis[a].minimum;
                    st.weightAxisMax = mm->axis[a].maximum;
                }
            }
            FT_Done_MM_Var(st.library, mm);
        }

        // Parse the OpenType MATH table if the face has one (Fira Math /
        // STIX Two Math / Latin Modern Math do; Bungee, Cormorant Garamond
        // and most display faces do not). We don't gate on the slot — a
        // user could plausibly load a math font into Display, or a non-
        // math face into Latex; the layouter checks .has() at call time.
        st.mathTable.load(st.face);
        if (st.mathTable.has())
        {
            LATEX3D_LOG_INFO("[text3d] OpenType MATH table parsed (axis=%.3f em, "
                             "fracBar=%.3f em, supShift=%.3f em)",
                             st.mathTable.constants().axisHeightEm,
                             st.mathTable.constants().fractionRuleThicknessEm,
                             st.mathTable.constants().superscriptShiftUpEm);
        }

        st.ready = true;
        LATEX3D_LOG_INFO("[text3d] Loaded font '%s' (upem=%u, lineHeight=%.3fem)",
                         ttfPath.c_str(), st.face->units_per_EM, st.lineHeightEm);
        return true;
    }

    void unloadFont(FontSlot slot)
    {
        auto &st = fontState(slot);
        std::lock_guard<std::mutex> lk(st.mtx);
        st.cache.clear();
        if (st.face) { FT_Done_Face(st.face); st.face = nullptr; }
        if (st.library) { FT_Done_FreeType(st.library); st.library = nullptr; }
        st.ready = false;
    }

    MeshData generateTextMesh(const std::string &text,
                                          const Text3DOptions &opt)
    {
        MeshData data;
        auto &st = fontState(FontSlot::Display);
        std::lock_guard<std::mutex> lk(st.mtx);
        if (!st.ready) return data;

        const float em = opt.charHeight;            // em → world units
        const float halfDepth = 0.5f * opt.depth;
        const float tol = std::max(1e-4f, opt.curveTolerance);
        const float weight = std::max(0.f, opt.weight);
        const float lineStride = opt.charHeight * st.lineHeightEm * opt.lineSpacing;

        auto lines = splitLines(text);
        if (lines.empty()) return data;

        // Vertical layout: first line's baseline sits at y=0 (top of block),
        // successive lines go into -Y. Optionally recenter the whole block.
        float blockTop = 0.f;
        if (opt.centerY)
        {
            float totalH = static_cast<float>(lines.size() - 1) * lineStride
                         + opt.charHeight * (st.ascenderEm - st.descenderEm);
            blockTop = 0.5f * totalH - opt.charHeight * st.ascenderEm;
        }

        const glm::vec3 color = opt.color;
        const float outlineT = std::max(0.f, opt.outlineThickness);

        for (size_t li = 0; li < lines.size(); ++li)
        {
            const std::string &line = lines[li];
            float lineBaselineY = blockTop - static_cast<float>(li) * lineStride;

            // Horizontal alignment: measure line width and left-shift if centered.
            float lineW = opt.centerX
                ? measureLineWidthEm(st, line, opt.tracking, tol, weight) * em
                : 0.f;
            float penX = opt.centerX ? -0.5f * lineW : 0.f;

            for (char c : line)
            {
                uint32_t cp = static_cast<uint32_t>(static_cast<unsigned char>(c));
                const GlyphShape *g = getOrBuildGlyph(st, cp, tol, weight);
                if (!g) continue;

                appendGlyphWithOutline(data, g->polygons,
                                             penX, lineBaselineY, em, opt.depth,
                                             color, outlineT, opt.outlineColor);

                penX += (g->advanceEm + opt.tracking) * em;
            }
        }

        return data;
    }

    // ── Public facade used by the LaTeX pipeline ───────────────────────────

    bool fontReady(FontSlot slot)
    {
        auto &st = fontState(slot);
        std::lock_guard<std::mutex> lk(st.mtx);
        return st.ready;
    }

    float fontAscenderEm(FontSlot slot)
    {
        auto &st = fontState(slot);
        std::lock_guard<std::mutex> lk(st.mtx);
        return st.ready ? st.ascenderEm : 0.f;
    }

    float fontDescenderEm(FontSlot slot)
    {
        auto &st = fontState(slot);
        std::lock_guard<std::mutex> lk(st.mtx);
        return st.ready ? st.descenderEm : 0.f;
    }

    float fontLineHeightEm(FontSlot slot)
    {
        auto &st = fontState(slot);
        std::lock_guard<std::mutex> lk(st.mtx);
        return st.ready ? st.lineHeightEm : 0.f;
    }

    bool fontHasMathTable(FontSlot slot)
    {
        auto &st = fontState(slot);
        std::lock_guard<std::mutex> lk(st.mtx);
        return st.ready && st.mathTable.has();
    }

    MathConstants fontMathConstants(FontSlot slot)
    {
        auto &st = fontState(slot);
        std::lock_guard<std::mutex> lk(st.mtx);
        // Returning by value is fine — MathConstants is a small POD. When
        // the slot has no MATH table, we hand back the layout-default-
        // initialised constants so callers can use the result unconditionally.
        return st.ready ? st.mathTable.constants() : MathConstants{};
    }

    bool glyphTopAccentAttachmentEm(uint32_t codepoint,
                                          FontSlot slot,
                                          float &attachmentEm)
    {
        auto &st = fontState(slot);
        std::lock_guard<std::mutex> lk(st.mtx);
        if (!st.ready || !st.mathTable.has() || !st.face) return false;
        FT_UInt gid = FT_Get_Char_Index(st.face, codepoint);
        if (gid == 0) return false;
        return st.mathTable.topAccentAttachmentEm(gid, attachmentEm);
    }

    bool getGlyph(uint32_t codepoint, float curveTolerance,
                        GlyphInfo &out, FontSlot slot, float weight)
    {
        auto &st = fontState(slot);
        std::lock_guard<std::mutex> lk(st.mtx);
        if (!st.ready) return false;
        const float tol = std::max(1e-4f, curveTolerance);
        const GlyphShape *g = getOrBuildGlyph(st, codepoint, tol,
                                              std::max(0.f, weight));
        if (!g) return false;
        out.polygons = g->polygons.empty() ? nullptr : &g->polygons;
        out.advanceEm = g->advanceEm;
        out.lineHeightEm = g->lineHeightEm;
        return true;
    }

    void appendExtrudedPolygons(MeshData &data,
                                      const PolygonSet &polygons,
                                      float x, float y, float em,
                                      float depth,
                                      const glm::vec3 &color)
    {
        extrudePolygonsImpl(data, polygons, x, y, em, depth, color);
    }

    void appendExtrudedRect(MeshData &data,
                                  float x0, float y0,
                                  float x1, float y1,
                                  float depth,
                                  const glm::vec3 &color)
    {
        if (x1 <= x0 || y1 <= y0) return;
        // Build a single-polygon rect in em-space and reuse the extruder with
        // em = 1 (no scaling) so the x/y values map directly to world units.
        PolygonSet polys = {
            { { {x0, y0}, {x1, y0}, {x1, y1}, {x0, y1} } }
        };
        extrudePolygonsImpl(data, polys, 0.f, 0.f, 1.f, depth, color);
    }

    PolygonSet inflatePolygonSet(const PolygonSet &polygons,
                                 float thicknessEm)
    {
        return inflatePolygonSetImpl(polygons, thicknessEm);
    }

    void appendExtrudedPolygonsShifted(MeshData &data,
                                             const PolygonSet &polygons,
                                             float x, float y, float em,
                                             float depth, float zShift,
                                             const glm::vec3 &color)
    {
        extrudePolygonsZ(data, polygons, x, y, em, depth, zShift, color);
    }

    void appendExtrudedRectShifted(MeshData &data,
                                         float x0, float y0,
                                         float x1, float y1,
                                         float depth, float zShift,
                                         const glm::vec3 &color)
    {
        if (x1 <= x0 || y1 <= y0) return;
        PolygonSet polys = {
            { { {x0, y0}, {x1, y0}, {x1, y1}, {x0, y1} } }
        };
        extrudePolygonsZ(data, polys, 0.f, 0.f, 1.f, depth, zShift, color);
    }

    // Outline shell depth separation, in world units. The shell is extruded
    // SHORTER than the main glyph and centered on z=0, so the main glyph's
    // front face sits in front of the shell's front (border reads from the
    // front) AND the main glyph's back face sits in front of the shell's
    // back (border reads from behind too). Before this, the shell was the
    // same depth as the main glyph but z-shifted backward — which worked
    // from the front but made the back of every letter read as the shell's
    // dark back face, not the bright fill.
    //
    // Floor keeps faraway text / subscripts separated even after depth-
    // buffer precision falls off as d². The em-scaled term only kicks in
    // for large glyphs where a thicker separation keeps oblique billboard
    // views clean. Single source of truth — both text3d and latex3d route
    // here.
    static inline float outlineDepthEpsilon(float outlineThickness, float em)
    {
        return std::max(outlineThickness * em * 0.4f, 0.03f);
    }

    void appendGlyphWithOutline(MeshData &data,
                                      const PolygonSet &polygons,
                                      float x, float y, float em,
                                      float depth,
                                      const glm::vec3 &color,
                                      float outlineThickness,
                                      const glm::vec3 &outlineColor)
    {
        if (outlineThickness > 0.f && !polygons.empty())
        {
            PolygonSet shell = inflatePolygonSetImpl(polygons, outlineThickness);
            const float eps = outlineDepthEpsilon(outlineThickness, em);
            const float shellDepth = std::max(depth - 2.f * eps, 0.001f);
            // Centered on z=0 — shell is fully inside main glyph's extrusion.
            extrudePolygonsImpl(data, shell, x, y, em, shellDepth, outlineColor);
        }
        extrudePolygonsImpl(data, polygons, x, y, em, depth, color);
    }

    void appendRectWithOutline(MeshData &data,
                                     float x0, float y0,
                                     float x1, float y1,
                                     float em, float depth,
                                     const glm::vec3 &color,
                                     float outlineThickness,
                                     const glm::vec3 &outlineColor)
    {
        if (x1 <= x0 || y1 <= y0) return;
        if (outlineThickness > 0.f)
        {
            const float pad = outlineThickness * em;
            const float eps = outlineDepthEpsilon(outlineThickness, em);
            const float shellDepth = std::max(depth - 2.f * eps, 0.001f);
            PolygonSet shell = {
                { { {x0 - pad, y0 - pad}, {x1 + pad, y0 - pad},
                    {x1 + pad, y1 + pad}, {x0 - pad, y1 + pad} } }
            };
            extrudePolygonsImpl(data, shell, 0.f, 0.f, 1.f, shellDepth, outlineColor);
        }
        PolygonSet fill = {
            { { {x0, y0}, {x1, y0}, {x1, y1}, {x0, y1} } }
        };
        extrudePolygonsImpl(data, fill, 0.f, 0.f, 1.f, depth, color);
    }

} // namespace latex3d
