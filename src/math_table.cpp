#include <latex3d/math_table.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

namespace latex3d
{
    namespace
    {
        // ── Big-endian SFNT readers ─────────────────────────────────────────
        // OpenType tables are big-endian; FreeType hands us the raw bytes.
        // Bounds-check on every read so a malformed table bails cleanly
        // rather than reading past the buffer.
        struct Cursor
        {
            const uint8_t *data;
            size_t size;
            size_t pos;

            bool ok(size_t n) const { return pos + n <= size; }

            uint16_t u16()
            {
                if (!ok(2)) { pos = size; return 0; }
                uint16_t v = (uint16_t(data[pos]) << 8) | uint16_t(data[pos + 1]);
                pos += 2;
                return v;
            }
            int16_t i16()
            {
                return static_cast<int16_t>(u16());
            }
            uint32_t u32()
            {
                if (!ok(4)) { pos = size; return 0; }
                uint32_t v = (uint32_t(data[pos]) << 24)
                           | (uint32_t(data[pos + 1]) << 16)
                           | (uint32_t(data[pos + 2]) << 8)
                           | uint32_t(data[pos + 3]);
                pos += 4;
                return v;
            }
            void seek(size_t p) { pos = (p > size) ? size : p; }
            void skip(size_t n) { pos = (pos + n > size) ? size : pos + n; }
        };

        // MathValueRecord = int16 value (FUnits) + Offset16 deviceTable.
        // We only use the value; the device table refines the value at small
        // ppem sizes for hinted rasterisation, irrelevant to a 3D mesh.
        struct ReadValue
        {
            int16_t value;
            void operator()(Cursor &c)
            {
                value = c.i16();
                c.skip(2);  // device-table offset
            }
        };

        // Position of each MathValueRecord we care about within the
        // MathConstants block, counted *after* the four leading int16/uint16
        // fields. Indices come from the OpenType spec table ordering.
        constexpr int kMVR_AxisHeight                    = 1;
        constexpr int kMVR_AccentBaseHeight              = 2;
        constexpr int kMVR_FlattenedAccentBaseHeight     = 3;
        constexpr int kMVR_SubscriptShiftDown            = 4;
        constexpr int kMVR_SuperscriptShiftUp            = 7;
        constexpr int kMVR_FractionNumeratorShiftUp      = 26;
        constexpr int kMVR_FractionDenominatorShiftDown  = 28;
        constexpr int kMVR_FractionNumeratorGapMin       = 30;
        constexpr int kMVR_FractionRuleThickness         = 32;
        constexpr int kMVR_FractionDenominatorGapMin     = 33;
        constexpr int kMVR_OverbarVerticalGap            = 37;
        constexpr int kMVR_OverbarRuleThickness          = 38;
        constexpr int kMVR_OverbarExtraAscender          = 39;
        constexpr int kMVR_UnderbarVerticalGap           = 40;
        constexpr int kMVR_UnderbarRuleThickness         = 41;
        constexpr int kMVR_UnderbarExtraDescender        = 42;
        constexpr int kMVR_RadicalVerticalGap            = 43;
        constexpr int kMVR_RadicalRuleThickness          = 45;
        constexpr int kMVR_RadicalExtraAscender          = 46;
        // Highest index we read; defines the minimum number of records the
        // MathConstants subtable must provide for us to use it (older font
        // versions truncate the trailing radical-kern fields).
        constexpr int kMVR_MaxIndex                      = 46;

        // Read a coverage table (format 1 or 2) and return a flat list of
        // glyph indices in coverage-order. Empty list on parse failure.
        std::vector<FT_UInt> readCoverage(Cursor c)
        {
            std::vector<FT_UInt> out;
            uint16_t format = c.u16();
            if (format == 1)
            {
                uint16_t count = c.u16();
                out.reserve(count);
                for (uint16_t i = 0; i < count; ++i)
                    out.push_back(c.u16());
            }
            else if (format == 2)
            {
                uint16_t rangeCount = c.u16();
                for (uint16_t r = 0; r < rangeCount; ++r)
                {
                    uint16_t start = c.u16();
                    uint16_t end   = c.u16();
                    uint16_t startCovIdx = c.u16();
                    if (end < start) continue;
                    // startCovIdx is the position of `start` in the implied
                    // coverage list; intermediate glyphs follow contiguously.
                    if (out.size() < size_t(startCovIdx) + (end - start + 1))
                        out.resize(size_t(startCovIdx) + (end - start + 1));
                    for (uint16_t g = start; g <= end; ++g)
                        out[startCovIdx + (g - start)] = g;
                }
            }
            return out;
        }
    } // namespace

    bool MathTable::load(FT_Face face)
    {
        mLoaded = false;
        mConstants = MathConstants{};
        mTopAccent.clear();
        if (!face) return false;
        mUnitsPerEm = (face->units_per_EM > 0)
            ? static_cast<float>(face->units_per_EM) : 1000.f;
        const float upem = mUnitsPerEm;

        // Pull the raw MATH table bytes. FT_Load_Sfnt_Table with a null
        // buffer reports the size; we then allocate and read it in one go.
        const FT_ULong tag = FT_MAKE_TAG('M', 'A', 'T', 'H');
        FT_ULong tableLen = 0;
        if (FT_Load_Sfnt_Table(face, tag, 0, nullptr, &tableLen) != 0
            || tableLen < 10)
        {
            return false;
        }
        std::vector<uint8_t> buf(tableLen);
        if (FT_Load_Sfnt_Table(face, tag, 0, buf.data(), &tableLen) != 0)
            return false;

        Cursor head{buf.data(), buf.size(), 0};
        const uint32_t version = head.u32();
        if (version != 0x00010000u) return false;
        const uint16_t constantsOff = head.u16();
        const uint16_t glyphInfoOff = head.u16();
        // const uint16_t variantsOff = head.u16();  // for stretchy delims

        // ── MathConstants ──────────────────────────────────────────────────
        if (constantsOff != 0 && constantsOff + 8 < buf.size())
        {
            Cursor c{buf.data(), buf.size(), constantsOff};
            const int16_t scriptPct       = c.i16();
            const int16_t scriptScriptPct = c.i16();
            c.skip(4);  // DelimitedSubFormulaMinHeight + DisplayOperatorMinHeight

            // Read MathValueRecords up to kMVR_MaxIndex. Older fonts may
            // truncate; if we run out, stop and keep whatever we got.
            std::vector<int16_t> mvr;
            mvr.reserve(kMVR_MaxIndex + 1);
            for (int i = 0; i <= kMVR_MaxIndex; ++i)
            {
                if (!c.ok(4)) break;
                mvr.push_back(c.i16());
                c.skip(2);  // device-table offset
            }

            auto pick = [&](int idx, float &dst)
            {
                if (idx < static_cast<int>(mvr.size()))
                    dst = static_cast<float>(mvr[idx]) / upem;
            };

            mConstants.scriptPercentScaleDown =
                (scriptPct > 0) ? scriptPct / 100.f : 0.6f;
            mConstants.scriptScriptPercentScaleDown =
                (scriptScriptPct > 0) ? scriptScriptPct / 100.f : 0.5f;

            pick(kMVR_AxisHeight,                   mConstants.axisHeightEm);
            pick(kMVR_AccentBaseHeight,             mConstants.accentBaseHeightEm);
            pick(kMVR_FlattenedAccentBaseHeight,    mConstants.flattenedAccentBaseHeightEm);
            pick(kMVR_SubscriptShiftDown,           mConstants.subscriptShiftDownEm);
            pick(kMVR_SuperscriptShiftUp,           mConstants.superscriptShiftUpEm);
            pick(kMVR_FractionNumeratorShiftUp,     mConstants.fractionNumeratorShiftUpEm);
            pick(kMVR_FractionDenominatorShiftDown, mConstants.fractionDenominatorShiftDownEm);
            pick(kMVR_FractionNumeratorGapMin,      mConstants.fractionNumeratorGapMinEm);
            pick(kMVR_FractionRuleThickness,        mConstants.fractionRuleThicknessEm);
            pick(kMVR_FractionDenominatorGapMin,    mConstants.fractionDenominatorGapMinEm);
            pick(kMVR_OverbarVerticalGap,           mConstants.overbarVerticalGapEm);
            pick(kMVR_OverbarRuleThickness,         mConstants.overbarRuleThicknessEm);
            pick(kMVR_OverbarExtraAscender,         mConstants.overbarExtraAscenderEm);
            pick(kMVR_UnderbarVerticalGap,          mConstants.underbarVerticalGapEm);
            pick(kMVR_UnderbarRuleThickness,        mConstants.underbarRuleThicknessEm);
            pick(kMVR_UnderbarExtraDescender,       mConstants.underbarExtraDescenderEm);
            pick(kMVR_RadicalVerticalGap,           mConstants.radicalVerticalGapEm);
            pick(kMVR_RadicalRuleThickness,         mConstants.radicalRuleThicknessEm);
            pick(kMVR_RadicalExtraAscender,         mConstants.radicalExtraAscenderEm);

            // Subscript shift is conventionally stored as a positive
            // FUnits value meaning "shift down by this much"; LaTeX's
            // descend convention wants a negative number for the y-shift,
            // but our layouter handles the sign at the call site, so keep
            // the magnitude as the spec writes it.
            //
            // Same goes for fractionDenominatorShiftDown and the underbar
            // family — they're magnitudes; the layouter applies the sign.
        }

        // ── MathGlyphInfo → MathTopAccentAttachment ────────────────────────
        if (glyphInfoOff != 0 && glyphInfoOff + 8 < buf.size())
        {
            Cursor gi{buf.data(), buf.size(), glyphInfoOff};
            gi.u16();   // italics-correction offset (unused)
            const uint16_t topAccOff = gi.u16();
            // gi.u16();  // extended-shape coverage offset (unused)
            // gi.u16();  // math-kern info offset (unused)

            if (topAccOff != 0
                && size_t(glyphInfoOff) + topAccOff + 4 < buf.size())
            {
                const size_t base = size_t(glyphInfoOff) + topAccOff;
                Cursor ta{buf.data(), buf.size(), base};
                const uint16_t covOff = ta.u16();
                const uint16_t count  = ta.u16();
                if (covOff != 0 && base + covOff < buf.size() && count > 0)
                {
                    Cursor cov{buf.data(), buf.size(), base + covOff};
                    auto glyphs = readCoverage(cov);
                    // Read `count` MathValueRecords; pair them with glyphs.
                    std::vector<int16_t> values;
                    values.reserve(count);
                    for (uint16_t k = 0; k < count && ta.ok(4); ++k)
                    {
                        values.push_back(ta.i16());
                        ta.skip(2);
                    }
                    const size_t pairs = std::min(glyphs.size(), values.size());
                    mTopAccent.reserve(pairs);
                    for (size_t k = 0; k < pairs; ++k)
                    {
                        mTopAccent.emplace(
                            glyphs[k],
                            static_cast<float>(values[k]) / upem);
                    }
                }
            }
        }

        mLoaded = true;
        return true;
    }

    bool MathTable::topAccentAttachmentEm(FT_UInt glyphIndex,
                                         float &attachmentEm) const
    {
        auto it = mTopAccent.find(glyphIndex);
        if (it == mTopAccent.end()) return false;
        attachmentEm = it->second;
        return true;
    }

} // namespace latex3d
