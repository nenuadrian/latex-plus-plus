#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

// Forward-declare the FreeType handles we expose in this header, so
// downstream consumers of <latex3d/text3d.h> (which transitively includes
// this) don't need FreeType on their include path. The full FT headers are
// pulled in by math_table.cpp where load() is implemented.
struct FT_FaceRec_;
typedef struct FT_FaceRec_ *FT_Face;
typedef unsigned int FT_UInt;

namespace latex3d
{

    // ── MathConstants ──────────────────────────────────────────────────────────
    // Subset of the OpenType MATH table's `MathConstants` block that the LaTeX
    // layouter consumes. All linear values are in em fractions (divided by
    // units-per-em); percentages are unitless 0..1 fractions.
    //
    // When the loaded face has no MATH table, every field stays at its
    // sensible LaTeX-Computer-Modern-ish default so the layouter still
    // produces a reasonable layout — these defaults match the old hardcoded
    // numbers in the stub.
    struct MathConstants
    {
        // Sub/superscript scaling (LaTeX `script` style is ~70% of `text`).
        float scriptPercentScaleDown = 0.6f;
        float scriptScriptPercentScaleDown = 0.5f;

        // Sub/superscript baseline shifts.
        float subscriptShiftDownEm = 0.18f;
        float superscriptShiftUpEm = 0.45f;

        // Vertical centerline of inline math (≈ x-height/2). Used as the
        // anchor for fraction bars and stretchy delimiters.
        float axisHeightEm = 0.25f;

        // Accent positioning.
        float accentBaseHeightEm = 0.7f;
        float flattenedAccentBaseHeightEm = 0.7f;

        // Fractions.
        float fractionNumeratorShiftUpEm = 0.4f;
        float fractionDenominatorShiftDownEm = 0.4f;
        float fractionNumeratorGapMinEm = 0.12f;
        float fractionDenominatorGapMinEm = 0.12f;
        float fractionRuleThicknessEm = 0.05f;

        // Overline / underline rules.
        float overbarVerticalGapEm = 0.08f;
        float overbarRuleThicknessEm = 0.05f;
        float overbarExtraAscenderEm = 0.04f;
        float underbarVerticalGapEm = 0.08f;
        float underbarRuleThicknessEm = 0.05f;
        float underbarExtraDescenderEm = 0.04f;

        // Radical (\sqrt). Not yet plumbed but parsed for future use.
        float radicalRuleThicknessEm = 0.05f;
        float radicalVerticalGapEm = 0.08f;
        float radicalExtraAscenderEm = 0.05f;
    };

    // ── MathTable ──────────────────────────────────────────────────────────────
    // Parsed view of the OpenType MATH table for a single FT_Face. Construct,
    // call load(face), then query. After load() returns false, has() is false
    // and every accessor returns the layout-defaults from MathConstants{}.
    //
    // Per-glyph data is keyed by glyph index (FT_UInt), not codepoint —
    // converting from codepoint is the caller's responsibility (use
    // FT_Get_Char_Index). Top-accent attachment is the only per-glyph field
    // we read today; the rest of MathGlyphInfo (italic correction, kerning,
    // extended shape coverage) is skipped intentionally.
    class MathTable
    {
    public:
        // Parse the MATH table from `face`. Returns true on success and a
        // valid MathConstants is populated. Returns false (and leaves
        // constants at their built-in defaults) when the face has no MATH
        // table or it can't be parsed — never throws, never logs.
        bool load(FT_Face face);

        bool has() const { return mLoaded; }
        const MathConstants &constants() const { return mConstants; }

        // Top-accent attachment x-coordinate (em fraction) for `glyphIndex`.
        // Returns NaN-sentinel via the bool out-param so callers can fall
        // back to "use the glyph's advance/2" without a magic value collision
        // with a legitimate 0.0 attachment (some glyphs really do anchor at
        // x=0). attachmentEm is undefined when the bool is false.
        bool topAccentAttachmentEm(FT_UInt glyphIndex,
                                   float &attachmentEm) const;

    private:
        bool mLoaded = false;
        float mUnitsPerEm = 1000.f;
        MathConstants mConstants;
        // glyphIndex → top-accent attachment in em fractions.
        std::unordered_map<FT_UInt, float> mTopAccent;
    };

} // namespace latex3d
