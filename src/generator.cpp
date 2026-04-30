#include <latex3d/generator.h>
#include <latex3d/layouter.h>
#include <latex3d/text3d.h>

#include <algorithm>

namespace latex3d
{
    namespace
    {
        // Adapter that forwards FontMetrics queries to the global
        // FreeType-backed text3d helpers, using the Latex font slot. When
        // the loaded font has an OpenType MATH table (Fira Math / STIX Two
        // Math), we route the typographic constants through it so accents,
        // sub/superscripts and fractions land where the type designer
        // intended; otherwise the base-class defaults take over.
        struct Text3DLatexMetrics : FontMetrics
        {
            float curveTolerance;
            float weight;
            // Cached snapshot — fontMathConstants() takes the slot's
            // mutex internally, so we don't want to call it on every
            // sub/sup or fraction. Per-construction snapshot is fine
            // because font swaps re-create the layout call.
            MathConstants mc;
            bool hasMath;

            Text3DLatexMetrics(float tol, float w)
                : curveTolerance(tol), weight(w),
                  mc(fontMathConstants(FontSlot::Latex)),
                  hasMath(fontHasMathTable(FontSlot::Latex)) {}

            float advanceEm(uint32_t cp) const override
            {
                GlyphInfo info;
                if (!getGlyph(cp, curveTolerance, info,
                                    FontSlot::Latex, weight))
                    return 0.f;
                return info.advanceEm;
            }
            float ascenderEm() const override
            { return fontAscenderEm(FontSlot::Latex); }
            float descenderEm() const override
            { return fontDescenderEm(FontSlot::Latex); }
            float lineHeightEm() const override
            { return fontLineHeightEm(FontSlot::Latex); }

            // ── MATH-table overrides ────────────────────────────────────
            float subscriptShiftDownEm() const override
            { return mc.subscriptShiftDownEm; }
            float superscriptShiftUpEm() const override
            { return mc.superscriptShiftUpEm; }
            float scriptPercentScaleDown() const override
            { return mc.scriptPercentScaleDown; }
            float axisHeightEm() const override
            { return mc.axisHeightEm; }
            float fractionRuleThicknessEm() const override
            { return mc.fractionRuleThicknessEm; }
            float fractionNumeratorShiftUpEm() const override
            { return mc.fractionNumeratorShiftUpEm; }
            float fractionDenominatorShiftDownEm() const override
            { return mc.fractionDenominatorShiftDownEm; }
            float fractionNumeratorGapMinEm() const override
            { return mc.fractionNumeratorGapMinEm; }
            float fractionDenominatorGapMinEm() const override
            { return mc.fractionDenominatorGapMinEm; }
            float overbarRuleThicknessEm() const override
            { return mc.overbarRuleThicknessEm; }
            float overbarVerticalGapEm() const override
            { return mc.overbarVerticalGapEm; }
            float overbarExtraAscenderEm() const override
            { return mc.overbarExtraAscenderEm; }
            float underbarRuleThicknessEm() const override
            { return mc.underbarRuleThicknessEm; }
            float underbarVerticalGapEm() const override
            { return mc.underbarVerticalGapEm; }
            float underbarExtraDescenderEm() const override
            { return mc.underbarExtraDescenderEm; }
            float accentBaseHeightEm() const override
            { return mc.accentBaseHeightEm; }

            float topAccentAttachmentEm(uint32_t cp) const override
            {
                if (hasMath)
                {
                    float att;
                    if (glyphTopAccentAttachmentEm(cp, FontSlot::Latex,
                                                         att))
                        return att;
                }
                // Fall back to the geometric midpoint — works well for
                // upright bases (digits, Greek, Latin caps) which is most
                // of what carries an accent in practice.
                return advanceEm(cp) * 0.5f;
            }
        };
    } // namespace

    MeshData generateLatexMesh(const std::string &latex,
                                           const LatexOptions &opt)
    {
        MeshData data;
        if (!fontReady(FontSlot::Latex)) return data;
        if (latex.empty()) return data;

        // The layouter walks the source once and emits glyph + rule ops
        // in em-space; this generator extrudes them into 3D meshes through
        // appendGlyphWithOutline / appendRectWithOutline. The
        // metrics adapter below pulls per-font typographic constants from
        // the loaded math font's OpenType MATH table when present (Fira
        // Math, STIX Two Math), so accents, sub/superscripts and fractions
        // land at type-designer-specified positions instead of stub
        // approximations. Stretchy delimiters (\left(...\right)) and
        // \begin{...}\end{...} environments are not yet wired — those need
        // MathVariants/MathGlyphAssembly parsing and a layout-twice pass,
        // tracked separately.

        Text3DLatexMetrics metrics(opt.curveTolerance, opt.weight);
        Layout L = layout(latex, metrics, opt.weight);
        if (L.empty()) return data;

        const float em = opt.charHeight;
        const float depth = (opt.mode == ExtrusionMode::Panel) ? 0.f : opt.depth;

        const float cx = opt.centerX ? -0.5f * (L.minX + L.maxX) : 0.f;
        const float cy = opt.centerY ? -0.5f * (L.minY + L.maxY) : 0.f;

        const float outlineT = std::max(0.f, opt.outlineThickness);

        for (const auto &op : L.ops)
        {
            if (op.type == LayoutOp::Glyph)
            {
                GlyphInfo info;
                if (!getGlyph(op.codepoint, opt.curveTolerance, info,
                                    FontSlot::Latex, opt.weight))
                    continue;
                if (!info.polygons) continue;

                const float gx = (cx + op.x) * em;
                const float gy = (cy + op.y) * em;
                const float gem = op.scale * em;

                appendGlyphWithOutline(data, *info.polygons,
                                             gx, gy, gem, depth,
                                             opt.color, outlineT,
                                             opt.outlineColor);
            }
            else if (op.type == LayoutOp::Rule)
            {
                const float rx0 = (cx + op.x0) * em;
                const float ry0 = (cy + op.y0) * em;
                const float rx1 = (cx + op.x1) * em;
                const float ry1 = (cy + op.y1) * em;

                appendRectWithOutline(data, rx0, ry0, rx1, ry1,
                                            em, depth, opt.color,
                                            outlineT, opt.outlineColor);
            }
            else // Polygon — used by \underbrace / \overbrace. The ring is
                 // an em-space CCW outer contour. We pass it through the
                 // same outline-shell helper that glyphs use so braces
                 // inherit the dark border treatment for free; the helper
                 // inflates in em-space and only multiplies by em when
                 // emitting world vertices, which keeps the outline
                 // proportional to the text scale instead of fixed-size.
            {
                if (op.polygon.size() < 3) continue;

                PolygonSet poly(1);
                poly[0].emplace_back();
                auto &ring = poly[0][0];
                ring.reserve(op.polygon.size());
                for (const auto &v : op.polygon)
                    ring.push_back({v[0], v[1]});

                appendGlyphWithOutline(data, poly,
                                             cx * em, cy * em, em, depth,
                                             opt.color, outlineT,
                                             opt.outlineColor);
            }
        }

        return data;
    }

} // namespace latex3d
