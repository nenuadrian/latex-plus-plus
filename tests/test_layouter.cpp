// Pin the LaTeX layouter contract: symbol substitution, fraction layout,
// script positioning, group balancing, and fallbacks for unknown commands.
// These tests run without a real font — a fake metrics provider supplies
// monospace advances so we can assert on positions deterministically.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <latex3d/layouter.h>

using Catch::Matchers::WithinAbs;
using namespace latex3d;

namespace
{
    // Mirror of the inter-glyph tracking factor hard-coded in the layouter
    // (`penX += adv * 1.10f` in layouter.cpp). Pen positions and any
    // measurement that derives from sub.penX scale by this; tight content-
    // extent measurements (sub.maxX) do NOT include the trailing tracking.
    // Update both sides if the layouter's tracking changes.
    constexpr float kTracking = 1.10f;

    // Monospace fake: every codepoint advances by `adv` em (default 0.5).
    // Ascender/descender/line-height fixed so tests can reason about y.
    // The MATH-table block defaults to the same values the layouter's
    // built-in defaults use, so tests that don't care about the MATH path
    // see identical numbers to the pre-MATH layouter.
    struct FakeMetrics : FontMetrics
    {
        float adv = 0.5f;
        float asc = 0.8f;
        float desc = -0.2f;
        float lh = 1.2f;

        // MATH-table fields exposed as plain members so tests can mutate
        // them between layouts. Defaults match the layouter's hardcoded
        // pre-MATH numbers so prior tests keep their expectations.
        float subShift = 0.18f;
        float supShift = 0.45f;
        float scriptScale = 0.6f;
        float axisH = 0.25f;
        float fracTh = 0.05f;
        float fracNumShift = 0.4f;
        float fracDenShift = 0.4f;
        float fracNumGap = 0.12f;
        float fracDenGap = 0.12f;
        float overTh = 0.05f;
        float overGap = 0.08f;
        float overExtra = 0.04f;
        float underTh = 0.05f;
        float underGap = 0.08f;
        float underExtra = 0.04f;
        float accentBaseH = 0.7f;
        // When set, every codepoint reports this top-accent attachment
        // (em fraction). When < 0, fall back to the default (advance/2).
        float topAccentOverride = -1.f;

        float advanceEm(uint32_t /*cp*/) const override { return adv; }
        float ascenderEm() const override { return asc; }
        float descenderEm() const override { return desc; }
        float lineHeightEm() const override { return lh; }

        float subscriptShiftDownEm() const override { return subShift; }
        float superscriptShiftUpEm() const override { return supShift; }
        float scriptPercentScaleDown() const override { return scriptScale; }
        float axisHeightEm() const override { return axisH; }
        float fractionRuleThicknessEm() const override { return fracTh; }
        float fractionNumeratorShiftUpEm() const override { return fracNumShift; }
        float fractionDenominatorShiftDownEm() const override { return fracDenShift; }
        float fractionNumeratorGapMinEm() const override { return fracNumGap; }
        float fractionDenominatorGapMinEm() const override { return fracDenGap; }
        float overbarRuleThicknessEm() const override { return overTh; }
        float overbarVerticalGapEm() const override { return overGap; }
        float overbarExtraAscenderEm() const override { return overExtra; }
        float underbarRuleThicknessEm() const override { return underTh; }
        float underbarVerticalGapEm() const override { return underGap; }
        float underbarExtraDescenderEm() const override { return underExtra; }
        float accentBaseHeightEm() const override { return accentBaseH; }
        float topAccentAttachmentEm(uint32_t cp) const override
        {
            return topAccentOverride >= 0.f
                ? topAccentOverride
                : FontMetrics::topAccentAttachmentEm(cp);
        }
    };

    // Metrics with a configurable per-codepoint coverage set — used to test
    // missing-glyph behavior (advance==0 → drop the op).
    struct PartialMetrics : FakeMetrics
    {
        // Codepoints to report as missing.
        std::vector<uint32_t> missing;
        float advanceEm(uint32_t cp) const override
        {
            for (auto m : missing) if (m == cp) return 0.f;
            return adv;
        }
    };

    int countGlyphs(const Layout &l)
    {
        int n = 0;
        for (auto &op : l.ops) if (op.type == LayoutOp::Glyph) ++n;
        return n;
    }
    int countRules(const Layout &l)
    {
        int n = 0;
        for (auto &op : l.ops) if (op.type == LayoutOp::Rule) ++n;
        return n;
    }
    const LayoutOp *firstGlyph(const Layout &l, uint32_t cp)
    {
        for (auto &op : l.ops)
            if (op.type == LayoutOp::Glyph && op.codepoint == cp) return &op;
        return nullptr;
    }
    const LayoutOp *firstRule(const Layout &l)
    {
        for (auto &op : l.ops)
            if (op.type == LayoutOp::Rule) return &op;
        return nullptr;
    }
}

TEST_CASE("Empty input produces an empty layout",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("", m);
    REQUIRE(l.ops.empty());
    REQUIRE(l.empty());
}

TEST_CASE("Plain ASCII lays out left-to-right at constant y=0",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("abc", m);
    REQUIRE(l.ops.size() == 3);
    for (size_t i = 0; i < 3; ++i)
    {
        REQUIRE(l.ops[i].type == LayoutOp::Glyph);
        REQUIRE(l.ops[i].codepoint == static_cast<uint32_t>('a' + i));
        REQUIRE_THAT(l.ops[i].x,
                     WithinAbs(static_cast<float>(i) * m.adv * kTracking, 1e-5f));
        REQUIRE_THAT(l.ops[i].y, WithinAbs(0.f, 1e-5f));
        REQUIRE_THAT(l.ops[i].scale, WithinAbs(1.f, 1e-5f));
    }
}

TEST_CASE("\\alpha substitutes to U+03B1",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\alpha", m);
    REQUIRE(l.ops.size() == 1);
    REQUIRE(l.ops[0].type == LayoutOp::Glyph);
    REQUIRE(l.ops[0].codepoint == 0x03B1u);
}

TEST_CASE("Greek table covers the canonical letters",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    struct Case { const char *src; uint32_t cp; };
    const Case cases[] = {
        {"\\beta", 0x03B2}, {"\\pi", 0x03C0}, {"\\theta", 0x03B8},
        {"\\Gamma", 0x0393}, {"\\Omega", 0x03A9}, {"\\Sigma", 0x03A3},
    };
    for (auto &c : cases)
    {
        auto l = layout(c.src, m);
        REQUIRE(l.ops.size() == 1);
        REQUIRE(l.ops[0].codepoint == c.cp);
    }
}

TEST_CASE("Operator macros substitute to the right glyph",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    REQUIRE(layout("\\sum", m).ops[0].codepoint == 0x2211u);
    REQUIRE(layout("\\int", m).ops[0].codepoint == 0x222Bu);
    REQUIRE(layout("\\leq", m).ops[0].codepoint == 0x2264u);
    REQUIRE(layout("\\le",  m).ops[0].codepoint == 0x2264u);
    REQUIRE(layout("\\to",  m).ops[0].codepoint == 0x2192u);
    REQUIRE(layout("\\infty", m).ops[0].codepoint == 0x221Eu);
}

TEST_CASE("Unknown command falls back to literal letters",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\bogus", m);
    REQUIRE(l.ops.size() == 5);
    const char *expected = "bogus";
    for (size_t i = 0; i < 5; ++i)
        REQUIRE(l.ops[i].codepoint == static_cast<uint32_t>(expected[i]));
}

TEST_CASE("Named operators (\\sin, \\cos, \\log) render as their letters",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\sin", m);
    REQUIRE(l.ops.size() == 3);
    REQUIRE(l.ops[0].codepoint == 's');
    REQUIRE(l.ops[1].codepoint == 'i');
    REQUIRE(l.ops[2].codepoint == 'n');

    auto lc = layout("\\cos", m);
    REQUIRE(lc.ops.size() == 3);
    REQUIRE(lc.ops[0].codepoint == 'c');
}

TEST_CASE("Superscript shrinks scale and raises baseline",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("x^2", m);
    REQUIRE(l.ops.size() == 2);
    // Base
    REQUIRE(l.ops[0].codepoint == 'x');
    REQUIRE_THAT(l.ops[0].scale, WithinAbs(1.f, 1e-5f));
    REQUIRE_THAT(l.ops[0].y, WithinAbs(0.f, 1e-5f));
    // Exponent
    REQUIRE(l.ops[1].codepoint == '2');
    REQUIRE_THAT(l.ops[1].scale, WithinAbs(0.6f, 1e-5f));
    REQUIRE_THAT(l.ops[1].y, WithinAbs(0.45f, 1e-5f));
    REQUIRE(l.ops[1].x > l.ops[0].x);
}

TEST_CASE("Subscript shrinks scale and lowers baseline",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("a_i", m);
    REQUIRE(l.ops.size() == 2);
    REQUIRE_THAT(l.ops[1].scale, WithinAbs(0.6f, 1e-5f));
    REQUIRE_THAT(l.ops[1].y, WithinAbs(-0.18f, 1e-5f));
}

TEST_CASE("Braced script atoms group correctly",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("x^{ab}", m);
    REQUIRE(countGlyphs(l) == 3);
    // Both 'a' and 'b' must share the script scale + y-shift; 'b' must sit
    // to the right of 'a' at the same scale.
    REQUIRE_THAT(l.ops[1].scale, WithinAbs(0.6f, 1e-5f));
    REQUIRE_THAT(l.ops[2].scale, WithinAbs(0.6f, 1e-5f));
    REQUIRE_THAT(l.ops[1].y, WithinAbs(0.45f, 1e-5f));
    REQUIRE_THAT(l.ops[2].y, WithinAbs(0.45f, 1e-5f));
    REQUIRE(l.ops[2].x > l.ops[1].x);
}

TEST_CASE("\\frac{1}{2} emits two glyphs centered around one rule",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\frac{1}{2}", m);

    REQUIRE(countGlyphs(l) == 2);
    REQUIRE(countRules(l) == 1);

    const LayoutOp *num = firstGlyph(l, '1');
    const LayoutOp *den = firstGlyph(l, '2');
    const LayoutOp *bar = firstRule(l);
    REQUIRE(num);
    REQUIRE(den);
    REQUIRE(bar);

    // Numerator/denominator render at script size (matches the default
    // scriptPercentScaleDown — overridden per-font from MathConstants).
    const float innerScale = 0.6f;
    REQUIRE_THAT(num->scale, WithinAbs(innerScale, 1e-5f));
    REQUIRE_THAT(den->scale, WithinAbs(innerScale, 1e-5f));

    // Numerator sits above the bar, denominator below.
    REQUIRE(num->y > bar->y1);
    REQUIRE(den->y < bar->y0);

    // Bar centerline sits at the math-axis (axisHeightEm — defaults to
    // 0.25 em above pen, override-able per font from the MATH table).
    const float barCenter = 0.5f * (bar->y0 + bar->y1);
    REQUIRE_THAT(barCenter, WithinAbs(0.25f, 1e-5f));

    // Numerator and denominator x-centers match (both single-digit at the
    // same shrunk scale, so equal widths and equal centering).
    const float numCenter = num->x + 0.5f * m.adv * innerScale;
    const float denCenter = den->x + 0.5f * m.adv * innerScale;
    REQUIRE_THAT(numCenter, WithinAbs(denCenter, 1e-4f));
}

TEST_CASE("\\frac wider denominator widens the rule to the wider side",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\frac{1}{abcd}", m);
    const LayoutOp *bar = firstRule(l);
    REQUIRE(bar);
    const float ruleWidth = bar->x1 - bar->x0;
    // Denominator is 4 chars at scriptPercentScaleDown (0.6 default).
    // Width of N glyphs with tracking T = (N-1)*adv*T + adv = adv*((N-1)*T + 1).
    REQUIRE_THAT(ruleWidth,
                 WithinAbs(0.6f * m.adv * (3.f * kTracking + 1.f), 1e-4f));
}

TEST_CASE("Nested \\frac doesn't truncate inner groups",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\frac{\\frac{a}{b}}{c}", m);
    // 3 glyphs (a, b, c) + 2 rules (inner + outer).
    REQUIRE(countGlyphs(l) == 3);
    REQUIRE(countRules(l) == 2);
    REQUIRE(firstGlyph(l, 'a'));
    REQUIRE(firstGlyph(l, 'b'));
    REQUIRE(firstGlyph(l, 'c'));
}

TEST_CASE("Balanced braces in groups don't terminate early",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    // {a{b}c} is one group with a, b, c — not "a{b" then "c}".
    auto l = layout("{a{b}c}", m);
    REQUIRE(countGlyphs(l) == 3);
}

TEST_CASE("\\sqrt{x} emits radical glyph then its argument",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\sqrt{x}", m);
    REQUIRE(countGlyphs(l) == 2);
    REQUIRE(l.ops[0].codepoint == 0x221Au);
    REQUIRE(l.ops[1].codepoint == 'x');
    REQUIRE(l.ops[1].x > l.ops[0].x);
}

TEST_CASE("\\mathrm{abc} renders inner text as plain glyphs",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\mathrm{abc}", m);
    REQUIRE(countGlyphs(l) == 3);
    REQUIRE(l.ops[0].codepoint == 'a');
    REQUIRE(l.ops[1].codepoint == 'b');
    REQUIRE(l.ops[2].codepoint == 'c');
}

TEST_CASE("\\left( and \\right) prefixes are stripped, paren remains",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\left(x\\right)", m);
    REQUIRE(countGlyphs(l) == 3);
    REQUIRE(l.ops[0].codepoint == '(');
    REQUIRE(l.ops[1].codepoint == 'x');
    REQUIRE(l.ops[2].codepoint == ')');
}

TEST_CASE("Style directives (\\displaystyle, \\limits) consume nothing",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\displaystyle x", m);
    // Just 'x' — \displaystyle is a no-op, the space after it advances pen.
    REQUIRE(countGlyphs(l) == 1);
    REQUIRE(l.ops[0].codepoint == 'x');
    REQUIRE(l.ops[0].x > 0.f);  // pushed by the space
}

TEST_CASE("Newline resets x to 0 and drops y by line height * scale",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("a\nb", m);
    REQUIRE(countGlyphs(l) == 2);
    REQUIRE_THAT(l.ops[0].x, WithinAbs(0.f, 1e-5f));
    REQUIRE_THAT(l.ops[0].y, WithinAbs(0.f, 1e-5f));
    REQUIRE_THAT(l.ops[1].x, WithinAbs(0.f, 1e-5f));
    REQUIRE_THAT(l.ops[1].y, WithinAbs(-m.lh, 1e-5f));
}

TEST_CASE("Spaces advance pen by ~0.28 em without emitting a glyph",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("a b", m);
    REQUIRE(countGlyphs(l) == 2);
    // 'b' must be at: a's tracked advance + space (0.28).
    const float expected = m.adv * kTracking + 0.28f;
    REQUIRE_THAT(l.ops[1].x, WithinAbs(expected, 1e-5f));
}

TEST_CASE("Missing glyph (advance==0) is dropped, layout continues",
          "[latex3d][layouter]")
{
    PartialMetrics m;
    m.missing.push_back('b');
    auto l = layout("abc", m);
    // 'b' has no advance — it gets dropped entirely. 'c' sits where 'b'
    // would have been (since the pen never moved past 'a').
    REQUIRE(countGlyphs(l) == 2);
    REQUIRE(l.ops[0].codepoint == 'a');
    REQUIRE(l.ops[1].codepoint == 'c');
    REQUIRE_THAT(l.ops[1].x, WithinAbs(m.adv * kTracking, 1e-5f));
}

TEST_CASE("Bounding box covers all emitted ops",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("abc", m);
    REQUIRE_FALSE(l.empty());
    REQUIRE_THAT(l.minX, WithinAbs(0.f, 1e-5f));
    // Bbox right edge = last glyph's right edge = penX_after_2 + adv
    // = 2*adv*tracking + adv = adv * (2*tracking + 1).
    REQUIRE_THAT(l.maxX, WithinAbs(m.adv * (2.f * kTracking + 1.f), 1e-5f));
    REQUIRE_THAT(l.minY, WithinAbs(m.desc, 1e-5f));
    REQUIRE_THAT(l.maxY, WithinAbs(m.asc, 1e-5f));
}

TEST_CASE("Stray closing brace is skipped, not a parse error",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("a}b", m);
    REQUIRE(countGlyphs(l) == 2);
    REQUIRE(l.ops[0].codepoint == 'a');
    REQUIRE(l.ops[1].codepoint == 'b');
}

// ── New command coverage (tier-1 fixes for content found in worlds) ──────────

TEST_CASE("\\dots aliases \\ldots and substitutes to U+2026",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\dots", m);
    REQUIRE(l.ops.size() == 1);
    REQUIRE(l.ops[0].codepoint == 0x2026u);
}

TEST_CASE("\\implies, \\ll, and long arrows substitute to the right glyphs",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    REQUIRE(layout("\\implies", m).ops[0].codepoint == 0x27F9u);
    REQUIRE(layout("\\impliedby", m).ops[0].codepoint == 0x27F8u);
    REQUIRE(layout("\\ll", m).ops[0].codepoint == 0x226Au);
    REQUIRE(layout("\\gg", m).ops[0].codepoint == 0x226Bu);
    REQUIRE(layout("\\longrightarrow", m).ops[0].codepoint == 0x27F6u);
    REQUIRE(layout("\\longleftarrow",  m).ops[0].codepoint == 0x27F5u);
    REQUIRE(layout("\\longmapsto",     m).ops[0].codepoint == 0x27FCu);
    REQUIRE(layout("\\Longrightarrow", m).ops[0].codepoint == 0x27F9u);
}

TEST_CASE("\\mathscr{H} renders just the inner glyph, not the macro letters",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\mathscr{H}", m);
    REQUIRE(countGlyphs(l) == 1);
    REQUIRE(l.ops[0].codepoint == 'H');
    // No "m/a/t/h/s/c/r" letter spew.
    REQUIRE(firstGlyph(l, 'm') == nullptr);
}

TEST_CASE("\\phantom{abc} emits zero ops but advances pen by the inner width",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    // Trailing 'x' lets us probe where the pen ended up.
    auto l = layout("\\phantom{abc}x", m);
    REQUIRE(countGlyphs(l) == 1);
    REQUIRE(l.ops[0].codepoint == 'x');
    // 'x' must sit at exactly the width of "abc" = adv*(2*tracking + 1).
    REQUIRE_THAT(l.ops[0].x,
                 WithinAbs(m.adv * (2.f * kTracking + 1.f), 1e-5f));
}

TEST_CASE("\\hphantom advances the pen but \\vphantom does not",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto lh = layout("\\hphantom{abc}x", m);
    REQUIRE_THAT(lh.ops[0].x,
                 WithinAbs(m.adv * (2.f * kTracking + 1.f), 1e-5f));

    auto lv = layout("\\vphantom{abc}x", m);
    // \vphantom must NOT advance the pen — 'x' sits at x=0.
    REQUIRE_THAT(lv.ops[0].x, WithinAbs(0.f, 1e-5f));
}

TEST_CASE("\\! pulls the pen back by a thin negative space",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("a\\!b", m);
    REQUIRE(countGlyphs(l) == 2);
    // 'a' at 0, then \! pulls back by 0.16, then 'b' lands at
    // tracked-adv - 0.16.
    REQUIRE_THAT(l.ops[1].x, WithinAbs(m.adv * kTracking - 0.16f, 1e-5f));
}

// ── Tier-2 structural macros ─────────────────────────────────────────────────

TEST_CASE("\\overline{x} emits the inner glyph and a rule above it",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\overline{x}", m);
    REQUIRE(countGlyphs(l) == 1);
    REQUIRE(countRules(l) == 1);
    REQUIRE(firstGlyph(l, 'x'));
    const LayoutOp *bar = firstRule(l);
    REQUIRE(bar);
    // Rule sits above the ascender.
    REQUIRE(bar->y0 > m.asc);
    // Rule spans the glyph's full tracked advance (uses sub.penX).
    REQUIRE_THAT(bar->x1 - bar->x0, WithinAbs(m.adv * kTracking, 1e-4f));
}

TEST_CASE("\\underline{x} emits the inner glyph and a rule below it",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\underline{x}", m);
    REQUIRE(countGlyphs(l) == 1);
    REQUIRE(countRules(l) == 1);
    const LayoutOp *bar = firstRule(l);
    REQUIRE(bar);
    // Rule sits below the descender (which is negative).
    REQUIRE(bar->y1 < m.desc);
}

TEST_CASE("\\binom{n}{k} emits two parens and two stacked digits, no rule",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\binom{n}{k}", m);
    REQUIRE(countGlyphs(l) == 4);
    REQUIRE(countRules(l) == 0);
    REQUIRE(firstGlyph(l, '('));
    REQUIRE(firstGlyph(l, ')'));
    const LayoutOp *n = firstGlyph(l, 'n');
    const LayoutOp *k = firstGlyph(l, 'k');
    REQUIRE(n);
    REQUIRE(k);
    // n above the centerline, k below.
    REQUIRE(n->y > k->y);
    // Both at the same script size as \frac uses.
    REQUIRE_THAT(n->scale, WithinAbs(0.6f, 1e-5f));
    REQUIRE_THAT(k->scale, WithinAbs(0.6f, 1e-5f));
}

TEST_CASE("\\overset{a}{b} stacks 'a' above 'b' at smaller scale",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\overset{a}{b}", m);
    REQUIRE(countGlyphs(l) == 2);
    const LayoutOp *base = firstGlyph(l, 'b');
    const LayoutOp *upper = firstGlyph(l, 'a');
    REQUIRE(base);
    REQUIRE(upper);
    // Base at scale 1, upper at scale 0.6.
    REQUIRE_THAT(base->scale, WithinAbs(1.f, 1e-5f));
    REQUIRE_THAT(upper->scale, WithinAbs(0.6f, 1e-5f));
    // Upper sits above base.
    REQUIRE(upper->y > base->y);
}

TEST_CASE("\\underset{a}{b} stacks 'a' below 'b' at smaller scale",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\underset{a}{b}", m);
    REQUIRE(countGlyphs(l) == 2);
    const LayoutOp *base = firstGlyph(l, 'b');
    const LayoutOp *lower = firstGlyph(l, 'a');
    REQUIRE(base);
    REQUIRE(lower);
    REQUIRE_THAT(lower->scale, WithinAbs(0.6f, 1e-5f));
    REQUIRE(lower->y < base->y);
}

TEST_CASE("\\colon substitutes to ':' so it can flow next to operators",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\colon", m);
    REQUIRE(l.ops.size() == 1);
    REQUIRE(l.ops[0].codepoint == ':');
}

namespace
{
    int countPolygons(const Layout &l)
    {
        int n = 0;
        for (auto &op : l.ops) if (op.type == LayoutOp::Polygon) ++n;
        return n;
    }
    const LayoutOp *firstPolygon(const Layout &l)
    {
        for (auto &op : l.ops)
            if (op.type == LayoutOp::Polygon) return &op;
        return nullptr;
    }
}

TEST_CASE("\\underbrace lays out base content, emits one Polygon op below it",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\underbrace{abc}", m);
    // Base content rendered: a, b, c.
    REQUIRE(countGlyphs(l) == 3);
    REQUIRE(countPolygons(l) == 1);

    const LayoutOp *poly = firstPolygon(l);
    REQUIRE(poly);
    // Smoothed skeleton: 7 raw points, 4 shoulders × 4 samples each, then
    // ±half-thickness offset stitched into a closed ring. Exact count is
    // an implementation detail; we just want a non-degenerate polygon.
    REQUIRE(poly->polygon.size() >= 14);

    // All ring vertices should sit at or below the baseline (y=0) since
    // \underbrace points DOWN — the brace lives below the descender line.
    float maxRingY = -1e9f;
    for (const auto &v : poly->polygon) maxRingY = std::max(maxRingY, v[1]);
    REQUIRE(maxRingY < 0.f);
}

TEST_CASE("\\overbrace mirrors \\underbrace above the base content",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\overbrace{abc}", m);
    REQUIRE(countGlyphs(l) == 3);
    REQUIRE(countPolygons(l) == 1);

    const LayoutOp *poly = firstPolygon(l);
    REQUIRE(poly);

    // \overbrace ring sits above the ascender line.
    float minRingY = 1e9f;
    for (const auto &v : poly->polygon) minRingY = std::min(minRingY, v[1]);
    REQUIRE(minRingY > 0.f);
}

TEST_CASE("\\underbrace{X}_{a} consumes the trailing subscript and centers it under the peak",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\underbrace{XYZ}_{a}", m);
    // Three base glyphs + one label glyph.
    REQUIRE(countGlyphs(l) == 4);
    REQUIRE(countPolygons(l) == 1);

    const LayoutOp *label = firstGlyph(l, 'a');
    REQUIRE(label);
    // Label rendered at script scale, not 1.0 — confirms it went through
    // the eager-subscript path inside handleBrace rather than as plain text.
    REQUIRE_THAT(label->scale, WithinAbs(0.6f, 1e-5f));

    // Label x is centered around the midpoint of the base. baseRight =
    // sub.penX after 3 glyphs = 3 * adv * tracking; midX = baseRight / 2.
    // Label glyph at scale 0.6 has width = adv * 0.6, shift = midX -
    // 0.5 * label-width.
    const float midX = 1.5f * m.adv * kTracking;
    const float labelW = m.adv * m.scriptScale;
    REQUIRE_THAT(label->x, WithinAbs(midX - 0.5f * labelW, 0.05f));

    // Label y is below the brace's peak, which is below the base baseline.
    const LayoutOp *base = firstGlyph(l, 'X');
    REQUIRE(base);
    REQUIRE(label->y < base->y);
}

TEST_CASE("\\overbrace{X}^{a} mirrors the eager-superscript behavior above",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\overbrace{XYZ}^{a}", m);
    REQUIRE(countGlyphs(l) == 4);
    REQUIRE(countPolygons(l) == 1);

    const LayoutOp *label = firstGlyph(l, 'a');
    REQUIRE(label);
    REQUIRE_THAT(label->scale, WithinAbs(0.6f, 1e-5f));

    const LayoutOp *base = firstGlyph(l, 'X');
    REQUIRE(base);
    REQUIRE(label->y > base->y);
}

TEST_CASE("\\underbrace bare (no following subscript) leaves the pen at the base's right edge",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\underbrace{ab}c", m);
    // a, b inside the brace, then c continues at the base's right edge.
    REQUIRE(countGlyphs(l) == 3);
    REQUIRE(countPolygons(l) == 1);

    const LayoutOp *a = firstGlyph(l, 'a');
    const LayoutOp *b = firstGlyph(l, 'b');
    const LayoutOp *c = firstGlyph(l, 'c');
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(c);
    // c sits to the right of b (the brace doesn't push the pen back).
    REQUIRE(c->x > b->x);
    // a, b, c all on the baseline — no script shift on the trailing 'c'.
    REQUIRE_THAT(c->y, WithinAbs(a->y, 1e-5f));
}

TEST_CASE("\\bmod renders as the letters 'mod' with surrounding space",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("a\\bmod b", m);
    // a, m, o, d, b — five glyphs.
    REQUIRE(countGlyphs(l) == 5);
    REQUIRE(l.ops[0].codepoint == 'a');
    REQUIRE(l.ops[1].codepoint == 'm');
    REQUIRE(l.ops[2].codepoint == 'o');
    REQUIRE(l.ops[3].codepoint == 'd');
    REQUIRE(l.ops[4].codepoint == 'b');
    // The 'm' must sit further right than just adv (extra \bmod padding).
    REQUIRE(l.ops[1].x > m.adv);
}

TEST_CASE("\\pmod{p} renders as ' (mod p)'",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\pmod{p}", m);
    // ( m o d <space=no glyph> p )  → 6 glyphs, no rule.
    REQUIRE(countRules(l) == 0);
    REQUIRE(firstGlyph(l, '('));
    REQUIRE(firstGlyph(l, ')'));
    REQUIRE(firstGlyph(l, 'm'));
    REQUIRE(firstGlyph(l, 'p'));
}

// ── MATH-table-driven typography ─────────────────────────────────────────────
// Every constant the layouter consumes from MathConstants is overridable on
// the metrics; these tests verify that the layouter actually reads them
// rather than ignoring the override and using a baked constant.

TEST_CASE("Superscript shift respects metrics.superscriptShiftUpEm()",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    m.supShift = 0.65f;     // taller-than-default lift
    m.scriptScale = 0.7f;   // and a bigger script size
    auto l = layout("x^2", m);
    REQUIRE(l.ops.size() == 2);
    REQUIRE_THAT(l.ops[1].scale, WithinAbs(0.7f, 1e-5f));
    REQUIRE_THAT(l.ops[1].y, WithinAbs(0.65f, 1e-5f));
}

TEST_CASE("Subscript shift respects metrics.subscriptShiftDownEm()",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    m.subShift = 0.30f;
    auto l = layout("a_i", m);
    REQUIRE(l.ops.size() == 2);
    REQUIRE_THAT(l.ops[1].y, WithinAbs(-0.30f, 1e-5f));
}

TEST_CASE("\\frac uses metrics.scriptPercentScaleDown for inner content",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    m.scriptScale = 0.75f;
    auto l = layout("\\frac{1}{2}", m);
    const LayoutOp *num = firstGlyph(l, '1');
    const LayoutOp *den = firstGlyph(l, '2');
    REQUIRE(num);
    REQUIRE(den);
    REQUIRE_THAT(num->scale, WithinAbs(0.75f, 1e-5f));
    REQUIRE_THAT(den->scale, WithinAbs(0.75f, 1e-5f));
}

TEST_CASE("\\frac bar centerline matches metrics.axisHeightEm()",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    m.axisH = 0.40f;
    auto l = layout("\\frac{1}{2}", m);
    const LayoutOp *bar = firstRule(l);
    REQUIRE(bar);
    const float center = 0.5f * (bar->y0 + bar->y1);
    REQUIRE_THAT(center, WithinAbs(0.40f, 1e-5f));
}

TEST_CASE("\\frac bar thickness matches metrics.fractionRuleThicknessEm()",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    m.fracTh = 0.10f;
    auto l = layout("\\frac{1}{2}", m);
    const LayoutOp *bar = firstRule(l);
    REQUIRE(bar);
    REQUIRE_THAT(bar->y1 - bar->y0, WithinAbs(0.10f, 1e-5f));
}

TEST_CASE("\\overline rule uses metrics.overbarRuleThicknessEm",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    m.overTh = 0.08f;
    auto l = layout("\\overline{x}", m);
    const LayoutOp *bar = firstRule(l);
    REQUIRE(bar);
    REQUIRE_THAT(bar->y1 - bar->y0, WithinAbs(0.08f, 1e-5f));
}

// ── Real accent rendering ────────────────────────────────────────────────────
// \hat / \bar / \tilde / \vec / \dot / \ddot / \check / \breve / \acute /
// \grave used to silently render their argument and drop the accent. They
// now emit the inner expression PLUS a positioned accent glyph above it,
// using the per-glyph TopAccentAttachment from the MATH table when present
// and the geometric midpoint otherwise.

TEST_CASE("\\hat{x} emits the base plus a circumflex accent above",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\hat{x}", m);
    REQUIRE(countGlyphs(l) == 2);
    const LayoutOp *base = firstGlyph(l, 'x');
    const LayoutOp *acc  = firstGlyph(l, 0x02C6);     // ˆ
    REQUIRE(base);
    REQUIRE(acc);
    REQUIRE(acc->y > base->y);
    // Accent must sit at least at AccentBaseHeight.
    REQUIRE(acc->y >= m.accentBaseH - 1e-5f);
}

TEST_CASE("Each accent macro emits the spec'd Unicode codepoint",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    struct Case { const char *src; uint32_t cp; };
    const Case cases[] = {
        {"\\bar{x}",   0x00AF},   // ¯
        {"\\tilde{x}", 0x02DC},   // ˜
        {"\\vec{x}",   0x20D7},   // ⃗
        {"\\dot{x}",   0x02D9},   // ˙
        {"\\ddot{x}",  0x00A8},   // ¨
        {"\\check{x}", 0x02C7},   // ˇ
        {"\\breve{x}", 0x02D8},   // ˘
        {"\\acute{x}", 0x00B4},   // ´
        {"\\grave{x}", 0x0060},   // `
    };
    for (auto &c : cases)
    {
        auto l = layout(c.src, m);
        REQUIRE(countGlyphs(l) == 2);
        REQUIRE(firstGlyph(l, 'x'));
        REQUIRE(firstGlyph(l, c.cp));
    }
}

TEST_CASE("Accent x-position uses the base glyph's top-accent attachment",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    // Force a non-trivial attachment point — well off-center.
    m.topAccentOverride = 0.10f;
    auto l = layout("\\hat{x}", m);
    const LayoutOp *base = firstGlyph(l, 'x');
    const LayoutOp *acc  = firstGlyph(l, 0x02C6);
    REQUIRE(base);
    REQUIRE(acc);
    // Both attachments are 0.10 (override applies to base AND accent), so
    // they cancel — accent's left edge sits at the base's left edge.
    REQUIRE_THAT(acc->x, WithinAbs(base->x, 1e-5f));
}

TEST_CASE("\\widehat{abc} draws a procedural polygon spanning the base width",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\widehat{abc}", m);
    // Three base glyphs + one accent polygon (procedural ^ arch). The
    // accent is no longer a font glyph, so the U+02C6 lookup is gone.
    REQUIRE(countGlyphs(l) == 3);
    REQUIRE(countPolygons(l) == 1);
    REQUIRE(firstGlyph(l, 0x02C6) == nullptr);

    const LayoutOp *poly = firstPolygon(l);
    REQUIRE(poly);

    // Polygon's horizontal extent should roughly match the base bbox
    // (x = [0, 3*adv]). Allow some slack for thickness offset miters.
    float minRingX = 1e9f, maxRingX = -1e9f;
    float minRingY = 1e9f;
    for (const auto &v : poly->polygon)
    {
        minRingX = std::min(minRingX, v[0]);
        maxRingX = std::max(maxRingX, v[0]);
        minRingY = std::min(minRingY, v[1]);
    }
    REQUIRE(minRingX < 0.1f * m.adv);
    REQUIRE(maxRingX > 2.9f * m.adv);
    // Sits above the accent base height — never overlapping the glyphs.
    REQUIRE(minRingY >= m.accentBaseH - 0.05f);
}

TEST_CASE("\\widetilde{xyz} also draws a procedural polygon (sine wave)",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\widetilde{xyz}", m);
    REQUIRE(countGlyphs(l) == 3);
    REQUIRE(countPolygons(l) == 1);
    // The U+02DC font glyph must NOT have been emitted — the procedural
    // path is the whole point: avoid font-coverage gaps.
    REQUIRE(firstGlyph(l, 0x02DC) == nullptr);

    const LayoutOp *poly = firstPolygon(l);
    REQUIRE(poly);
    // Sine skeleton at samples=16 produces ~17 skeleton points → ~34 ring
    // vertices after both-sided offset. Accept anything reasonably dense.
    REQUIRE(poly->polygon.size() >= 20);
}

TEST_CASE("Bare \\hat with no argument emits the accent as a standalone glyph",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\hat", m);
    REQUIRE(countGlyphs(l) == 1);
    REQUIRE(l.ops[0].codepoint == 0x02C6u);
}

TEST_CASE("Accent advances the pen by the base's width, not the accent's",
          "[latex3d][layouter]")
{
    FakeMetrics m;
    auto l = layout("\\hat{x}y", m);
    REQUIRE(countGlyphs(l) == 3);
    const LayoutOp *y = firstGlyph(l, 'y');
    REQUIRE(y);
    // 'y' should land right after 'x' at the tracked advance (accent is
    // non-spacing, so pen advances only by the base glyph's tracked width).
    REQUIRE_THAT(y->x, WithinAbs(m.adv * kTracking, 1e-5f));
}
