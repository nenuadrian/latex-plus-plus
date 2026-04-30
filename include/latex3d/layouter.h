#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace latex3d
{

    // ── LayoutOp ───────────────────────────────────────────────────────────────
    // Flat draw-op produced by the LaTeX layouter. Coordinates are in em-space
    // (y-up, baseline at y=0); the geometry pass in generator.cpp scales and
    // translates these into world units.
    struct LayoutOp
    {
        enum Type { Glyph, Rule, Polygon };
        Type type = Glyph;
        // Glyph fields
        uint32_t codepoint = 0;
        float x = 0.f;       // em-space position of glyph origin
        float y = 0.f;
        float scale = 1.f;   // relative em scale (1.0 = current run size)
        // Rule fields (axis-aligned rectangle in em-space)
        float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
        // Polygon fields. Outer ring of [x,y] vertices in em-space, CCW
        // (matches mapbox-earcut / latex3d::PolygonSet conventions). Empty for
        // non-Polygon ops. Used by \underbrace and \overbrace, which need a
        // shape that no glyph or axis-aligned rect can express.
        std::vector<std::array<float, 2>> polygon;
    };

    // ── FontMetrics ────────────────────────────────────────────────────────────
    // Pluggable font-metrics interface. Production wires this to the FreeType
    // text3d helpers; tests pass a fake with deterministic advances so the
    // layouter's positioning logic can be checked without a real font load.
    //
    // The basic block (advance/ascender/descender/lineHeight) is required.
    // The OpenType-MATH block below is optional — every method has a sensible
    // LaTeX-Computer-Modern-ish default, so legacy or test subclasses don't
    // need to override them. The production adapter overrides them with
    // values from the loaded math font's MATH table.
    struct FontMetrics
    {
        virtual ~FontMetrics() = default;
        // Em-fraction advance width for `cp`. Return 0 to mean "missing
        // glyph — skip emitting geometry but still advance the pen by 0".
        // Callers always advance the pen by this value, so a non-zero
        // fallback (e.g. 0.5) keeps unknown glyphs from collapsing the line.
        virtual float advanceEm(uint32_t cp) const = 0;
        virtual float ascenderEm() const = 0;
        virtual float descenderEm() const = 0;     // negative
        virtual float lineHeightEm() const = 0;
        // True if the font has a real outline for `cp`. Layouter still emits
        // the op (positions are font-independent at the metric level), but
        // this lets callers query coverage in tests.
        virtual bool hasGlyph(uint32_t cp) const { return advanceEm(cp) > 0.f; }

        // ── OpenType MATH-table block (optional) ────────────────────────────
        // Sub/superscript baseline shifts (em fractions, both positive
        // magnitudes — the layouter applies the sign).
        virtual float subscriptShiftDownEm() const { return 0.18f; }
        virtual float superscriptShiftUpEm() const { return 0.45f; }
        // Sub/superscript scale, fraction of the surrounding text size.
        virtual float scriptPercentScaleDown() const { return 0.6f; }

        // Vertical math-axis (≈ x-height/2) in em fractions; the conceptual
        // centerline a fraction bar sits on.
        virtual float axisHeightEm() const { return 0.25f; }

        // Fractions.
        virtual float fractionRuleThicknessEm() const { return 0.05f; }
        virtual float fractionNumeratorShiftUpEm() const { return 0.4f; }
        virtual float fractionDenominatorShiftDownEm() const { return 0.4f; }
        virtual float fractionNumeratorGapMinEm() const { return 0.12f; }
        virtual float fractionDenominatorGapMinEm() const { return 0.12f; }

        // Overline / underline.
        virtual float overbarRuleThicknessEm() const { return 0.05f; }
        virtual float overbarVerticalGapEm() const { return 0.08f; }
        virtual float overbarExtraAscenderEm() const { return 0.04f; }
        virtual float underbarRuleThicknessEm() const { return 0.05f; }
        virtual float underbarVerticalGapEm() const { return 0.08f; }
        virtual float underbarExtraDescenderEm() const { return 0.04f; }

        // Accents. accentBaseHeightEm is the y at which the accent's
        // baseline ought to sit when the base glyph is "tall" (Latin caps
        // and most Greek). For shorter bases the accent should hug the
        // glyph's actual top — the layouter handles that choice.
        virtual float accentBaseHeightEm() const { return 0.7f; }

        // Top-accent attachment x in em-fractions for `cp`. The OpenType
        // MATH table stores this per-glyph (the optical center accounting
        // for italics and asymmetric shapes); the default falls back to
        // the geometric midpoint, which is good enough for upright bases.
        virtual float topAccentAttachmentEm(uint32_t cp) const
        {
            return advanceEm(cp) * 0.5f;
        }
    };

    // ── Result ─────────────────────────────────────────────────────────────────
    struct Layout
    {
        std::vector<LayoutOp> ops;
        // Em-space bounding box of all emitted ops; if no ops were emitted,
        // maxX < minX (sentinel).
        float minX = 1e9f, maxX = -1e9f;
        float minY = 1e9f, maxY = -1e9f;

        bool empty() const { return ops.empty() || maxX <= minX; }
    };

    // Lay out a LaTeX math expression against the supplied metrics. The result
    // is font-independent: same metrics → same op stream, regardless of any
    // global state. `weight` is forwarded as a hint to glyph lookups (the
    // layouter itself doesn't use it, but tests may want to exercise it).
    Layout layout(const std::string &latex,
                  const FontMetrics &metrics,
                  float weight = 0.f);

} // namespace latex3d
