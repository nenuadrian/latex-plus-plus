#include <latex3d/layouter.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace latex3d
{
    namespace
    {
        // ── Curly-brace polygon construction ─────────────────────────────────
        // Used by \underbrace and \overbrace. The brace is built as a smoothed
        // polyline skeleton (sharp tips and peak, quadratic-Bezier-rounded
        // shoulders), then offset perpendicular by the stroke half-thickness
        // to produce a CCW outer ring. The geometry pass extrudes this ring
        // like any other polygon — no curve primitive is needed.
        using BracePt = std::array<float, 2>;

        BracePt bAdd(BracePt a, BracePt b) { return {a[0] + b[0], a[1] + b[1]}; }
        BracePt bSub(BracePt a, BracePt b) { return {a[0] - b[0], a[1] - b[1]}; }
        BracePt bMul(BracePt a, float s)   { return {a[0] * s,    a[1] * s   }; }
        float   bDot(BracePt a, BracePt b) { return a[0] * b[0] + a[1] * b[1]; }
        float   bLen(BracePt v)            { return std::sqrt(bDot(v, v)); }
        BracePt bNorm(BracePt v)
        {
            float l = bLen(v);
            return l > 1e-6f ? BracePt{v[0] / l, v[1] / l} : BracePt{0.f, 0.f};
        }
        BracePt bPerp(BracePt v) { return {-v[1], v[0]}; } // 90° CCW rotation

        void sampleQuadBezier(BracePt p0, BracePt p1, BracePt p2,
                              int N, std::vector<BracePt> &out)
        {
            // Skip t=0 (caller already pushed the curve's starting anchor) so
            // adjacent segments don't double up at shared endpoints.
            for (int k = 1; k <= N; ++k)
            {
                float t = static_cast<float>(k) / static_cast<float>(N);
                float u = 1.f - t;
                BracePt p = bAdd(bAdd(bMul(p0, u * u),
                                      bMul(p1, 2.f * u * t)),
                                 bMul(p2, t * t));
                out.push_back(p);
            }
        }

        // Skeleton for a horizontal curly brace centered between x0 and x1.
        // `downward`: true → underbrace (peak below baseline y, hooks curl up
        // toward the base content); false → overbrace (mirrored).
        std::vector<BracePt> buildBraceSkeleton(
            float x0, float x1, float y,
            float curl, float peakH, float peakW,
            bool downward, int corner_samples)
        {
            const float sgn = downward ? -1.f : 1.f;
            const float mid = 0.5f * (x0 + x1);

            // 7 control points. Tips (0, 6) and peak (3) stay sharp; the
            // four shoulders (1, 2, 4, 5) are smoothed with quadratic
            // Beziers passing through the corner with anchors at the
            // adjacent segment midpoints.
            const std::array<BracePt, 7> raw = {{
                {{x0,            y                       }},
                {{x0 + curl,     y + sgn * curl          }},
                {{mid - peakW,   y + sgn * curl          }},
                {{mid,           y + sgn * (curl + peakH)}},
                {{mid + peakW,   y + sgn * curl          }},
                {{x1 - curl,     y + sgn * curl          }},
                {{x1,            y                       }},
            }};
            auto midOf = [](BracePt a, BracePt b) {
                return BracePt{0.5f * (a[0] + b[0]), 0.5f * (a[1] + b[1])};
            };

            std::vector<BracePt> skel;
            skel.reserve(7 + 4 * corner_samples);
            skel.push_back(raw[0]);
            // Shoulder at raw[1]: curve from mid(0,1) through raw[1] to mid(1,2).
            sampleQuadBezier(midOf(raw[0], raw[1]), raw[1],
                             midOf(raw[1], raw[2]), corner_samples, skel);
            // Shoulder at raw[2]: curve from mid(1,2) through raw[2] to mid(2,3).
            sampleQuadBezier(midOf(raw[1], raw[2]), raw[2],
                             midOf(raw[2], raw[3]), corner_samples, skel);
            skel.push_back(raw[3]);
            // Shoulder at raw[4]: curve from mid(3,4) through raw[4] to mid(4,5).
            sampleQuadBezier(midOf(raw[3], raw[4]), raw[4],
                             midOf(raw[4], raw[5]), corner_samples, skel);
            // Shoulder at raw[5]: curve from mid(4,5) through raw[5] to mid(5,6).
            sampleQuadBezier(midOf(raw[4], raw[5]), raw[5],
                             midOf(raw[5], raw[6]), corner_samples, skel);
            skel.push_back(raw[6]);
            return skel;
        }

        // ── Wide-accent skeletons (\widetilde, \widehat) ─────────────────────
        // Drawn procedurally rather than via a font glyph so they (1) always
        // render even if the math font lacks U+02DC / U+02C6 and (2) span the
        // base's full width like a real stretchy accent instead of staying at
        // a single fixed glyph size.
        constexpr float kBracePi = 3.14159265358979323846f;

        std::vector<BracePt> buildWideTildeSkeleton(
            float x0, float x1, float y, float peakH, int samples)
        {
            // One sine cycle: 0 → +peak → 0 → −peak → 0. Matches the
            // "stretched S" silhouette of LaTeX's \widetilde.
            std::vector<BracePt> s;
            s.reserve(samples + 1);
            const float W = x1 - x0;
            for (int k = 0; k <= samples; ++k)
            {
                float t = static_cast<float>(k)
                          / static_cast<float>(samples);
                float xx = x0 + t * W;
                float yy = y + peakH * std::sin(t * 2.f * kBracePi);
                s.push_back({xx, yy});
            }
            return s;
        }

        std::vector<BracePt> buildWideHatSkeleton(
            float x0, float x1, float y, float peakH, int samples)
        {
            // Quadratic Bezier from (x0, y) through (mid, y+peakH*1.4) to
            // (x1, y). The 1.4 factor makes the visual peak land near peakH
            // (Bezier midpoint sits halfway up the control point), giving
            // the same intent as a hand-drawn ^ accent.
            const float mid = 0.5f * (x0 + x1);
            const float ctrlY = y + peakH * 1.4f;
            std::vector<BracePt> s;
            s.reserve(samples + 1);
            for (int k = 0; k <= samples; ++k)
            {
                float t = static_cast<float>(k)
                          / static_cast<float>(samples);
                float u = 1.f - t;
                float xx = u * u * x0 + 2.f * u * t * mid + t * t * x1;
                float yy = u * u * y + 2.f * u * t * ctrlY + t * t * y;
                s.push_back({xx, yy});
            }
            return s;
        }

        // Offset a polyline by ±halfThickness perpendicular to local segments
        // and stitch the two parallel edges into a closed CCW ring.
        std::vector<BracePt> offsetSkeletonToRing(
            const std::vector<BracePt> &skel, float thickness)
        {
            const size_t n = skel.size();
            if (n < 2) return {};
            const float t = 0.5f * thickness;

            std::vector<BracePt> upper(n), lower(n);
            for (size_t i = 0; i < n; ++i)
            {
                BracePt nrm;
                if (i == 0)
                {
                    nrm = bPerp(bNorm(bSub(skel[1], skel[0])));
                }
                else if (i == n - 1)
                {
                    nrm = bPerp(bNorm(bSub(skel[n - 1], skel[n - 2])));
                }
                else
                {
                    BracePt dIn  = bNorm(bSub(skel[i],     skel[i - 1]));
                    BracePt dOut = bNorm(bSub(skel[i + 1], skel[i]    ));
                    BracePt nIn  = bPerp(dIn);
                    BracePt nOut = bPerp(dOut);
                    BracePt mid  = bMul(bAdd(nIn, nOut), 0.5f);
                    float ml = bLen(mid);
                    if (ml > 1e-6f)
                    {
                        nrm = bMul(mid, 1.f / ml);
                        // Miter compensation: scale the bisector so the
                        // *perpendicular* offset distance stays at `t` even
                        // through sharp turns. Clamp so very acute corners
                        // (peak tip) don't blow up to infinite length.
                        float d = bDot(nrm, nIn);
                        if (d > 0.2f) nrm = bMul(nrm, 1.f / d);
                    }
                    else
                    {
                        nrm = nIn;
                    }
                }
                upper[i] = bAdd(skel[i], bMul(nrm, t));
                lower[i] = bSub(skel[i], bMul(nrm, t));
            }

            // CCW outer ring: lower edge L→R, then upper edge R→L. With +y-up
            // math conventions this matches the (0,-1)→(W,-1)→(W,1)→(0,1)
            // traversal of a horizontal rectangle, which earcut treats as CCW.
            std::vector<BracePt> ring;
            ring.reserve(2 * n);
            for (size_t i = 0; i < n; ++i)         ring.push_back(lower[i]);
            for (size_t i = n; i-- > 0; )          ring.push_back(upper[i]);
            return ring;
        }

        // ── Symbol substitution table ───────────────────────────────────────
        // Maps a LaTeX command name (sans leading backslash) to a Unicode
        // codepoint. Whether the loaded TTF actually contains these glyphs is
        // font-dependent — missing glyphs fall back to a blank advance, so
        // the formula still lays out without crashing.
        const std::unordered_map<std::string, uint32_t> &latexSymbols()
        {
            static const std::unordered_map<std::string, uint32_t> m = {
                // Lowercase Greek
                {"alpha", 0x03B1}, {"beta", 0x03B2}, {"gamma", 0x03B3},
                {"delta", 0x03B4}, {"epsilon", 0x03B5}, {"varepsilon", 0x03B5},
                {"zeta", 0x03B6}, {"eta", 0x03B7}, {"theta", 0x03B8},
                {"vartheta", 0x03D1}, {"iota", 0x03B9}, {"kappa", 0x03BA},
                {"lambda", 0x03BB}, {"mu", 0x03BC}, {"nu", 0x03BD},
                {"xi", 0x03BE}, {"omicron", 0x03BF}, {"pi", 0x03C0},
                {"varpi", 0x03D6}, {"rho", 0x03C1}, {"varrho", 0x03F1},
                {"sigma", 0x03C3}, {"varsigma", 0x03C2}, {"tau", 0x03C4},
                {"upsilon", 0x03C5}, {"phi", 0x03C6}, {"varphi", 0x03D5},
                {"chi", 0x03C7}, {"psi", 0x03C8}, {"omega", 0x03C9},
                // Uppercase Greek
                {"Gamma", 0x0393}, {"Delta", 0x0394}, {"Theta", 0x0398},
                {"Lambda", 0x039B}, {"Xi", 0x039E}, {"Pi", 0x03A0},
                {"Sigma", 0x03A3}, {"Upsilon", 0x03A5}, {"Phi", 0x03A6},
                {"Psi", 0x03A8}, {"Omega", 0x03A9},
                // Binary operators & relations
                {"times", 0x00D7}, {"div", 0x00F7}, {"pm", 0x00B1},
                {"mp", 0x2213}, {"cdot", 0x22C5}, {"ast", 0x2217},
                {"star", 0x22C6}, {"circ", 0x2218}, {"bullet", 0x2219},
                {"leq", 0x2264}, {"le", 0x2264}, {"geq", 0x2265}, {"ge", 0x2265},
                {"neq", 0x2260}, {"ne", 0x2260}, {"approx", 0x2248},
                {"equiv", 0x2261}, {"sim", 0x223C}, {"simeq", 0x2243},
                {"cong", 0x2245}, {"propto", 0x221D},
                {"ll", 0x226A}, {"gg", 0x226B},
                // Arrows
                {"rightarrow", 0x2192}, {"to", 0x2192},
                {"leftarrow", 0x2190}, {"gets", 0x2190},
                {"leftrightarrow", 0x2194}, {"mapsto", 0x21A6},
                {"Rightarrow", 0x21D2}, {"Leftarrow", 0x21D0},
                {"Leftrightarrow", 0x21D4}, {"iff", 0x21D4},
                {"longrightarrow", 0x27F6}, {"longleftarrow", 0x27F5},
                {"longleftrightarrow", 0x27F7}, {"longmapsto", 0x27FC},
                {"Longrightarrow", 0x27F9}, {"Longleftarrow", 0x27F8},
                {"Longleftrightarrow", 0x27FA},
                {"implies", 0x27F9}, {"impliedby", 0x27F8},
                // Sets
                {"in", 0x2208}, {"notin", 0x2209},
                {"subset", 0x2282}, {"supset", 0x2283},
                {"subseteq", 0x2286}, {"supseteq", 0x2287},
                {"cup", 0x222A}, {"cap", 0x2229},
                {"emptyset", 0x2205}, {"varnothing", 0x2205},
                // Logic
                {"forall", 0x2200}, {"exists", 0x2203}, {"neg", 0x00AC},
                {"land", 0x2227}, {"wedge", 0x2227},
                {"lor", 0x2228}, {"vee", 0x2228},
                // Big operators
                {"sum", 0x2211}, {"prod", 0x220F}, {"coprod", 0x2210},
                {"int", 0x222B}, {"oint", 0x222E}, {"iint", 0x222C},
                {"iiint", 0x222D},
                // Calculus
                {"partial", 0x2202}, {"nabla", 0x2207}, {"infty", 0x221E},
                // Misc symbols
                {"angle", 0x2220}, {"triangle", 0x25B3}, {"square", 0x25A1},
                {"diamond", 0x25C7}, {"cdots", 0x22EF}, {"ldots", 0x2026},
                {"dots", 0x2026},
                {"vdots", 0x22EE}, {"ddots", 0x22F1},
                {"hbar", 0x210F}, {"ell", 0x2113}, {"Re", 0x211C},
                {"Im", 0x2111}, {"aleph", 0x2135},
                {"colon", ':'},
                // Delimiters (just the glyph; no auto-sizing yet)
                {"langle", 0x27E8}, {"rangle", 0x27E9},
                {"lceil", 0x2308}, {"rceil", 0x2309},
                {"lfloor", 0x230A}, {"rfloor", 0x230B},
                // Space commands. Most collapse to a thin space; the
                // negative-space \! is handled specially in run() as a
                // negative pen-advance — it isn't in this table.
                {"quad", ' '}, {"qquad", ' '},
                {",", ' '}, {":", ' '}, {";", ' '}, {" ", ' '},
                // Common relations that don't have dedicated glyphs
                {"mid", '|'},
                {"|", 0x2225}, {"parallel", 0x2225},     // ∥ — norm / parallel
                // \top would normally be U+22A4 (⊤), but Fira Math (our
                // primary math font) ships without that glyph; render it
                // as a plain capital T, which is the conventional
                // transpose mark in most papers anyway.
                {"top", 'T'},
                {"bot", 0x22A5}, {"perp", 0x22A5},        // ⊥ — bottom / perp
                // Sqrt falls back to the radical glyph if used as a bare
                // symbol; \sqrt{...} is handled specially.
                {"sqrt", 0x221A},
            };
            return m;
        }

        // ── Layouter ────────────────────────────────────────────────────────
        // Walks the source once, recursively, emitting ops at the current pen
        // position. State (pen, scale) is saved/restored for script and
        // sub-expression contexts.
        struct Layouter
        {
            const std::string &src;
            const FontMetrics &metrics;
            size_t i = 0;
            float penX = 0.f;
            float penY = 0.f;
            float scale = 1.f; // current glyph-size relative to run em
            float weight = 0.f;
            std::vector<LayoutOp> ops;
            float minX = 1e9f, maxX = -1e9f;
            float minY = 1e9f, maxY = -1e9f;

            Layouter(const std::string &s, const FontMetrics &m, float w)
                : src(s), metrics(m), weight(w) {}

            static bool isCmdChar(char c)
            {
                return std::isalpha(static_cast<unsigned char>(c)) != 0;
            }

            void emitGlyph(uint32_t cp)
            {
                const float advNative = metrics.advanceEm(cp);
                // Missing glyphs (advance==0) are dropped entirely — matches
                // the original behavior where text3DGetGlyph returning false
                // skipped both geometry and advance.
                if (advNative <= 0.f) return;

                LayoutOp op;
                op.type = LayoutOp::Glyph;
                op.codepoint = cp;
                op.x = penX;
                op.y = penY;
                op.scale = scale;
                ops.push_back(op);

                const float asc = metrics.ascenderEm() * scale;
                const float desc = metrics.descenderEm() * scale;
                const float adv = advNative * scale;

                minX = std::min(minX, penX);
                maxX = std::max(maxX, penX + adv);
                minY = std::min(minY, penY + desc);
                maxY = std::max(maxY, penY + asc);
                // Tracking: 10% extra inter-glyph advance over the font's
                // native advance for a slightly looser look.
                penX += adv * 1.10f;
            }

            std::string readGroup()
            {
                if (i >= src.size() || src[i] != '{') return {};
                ++i;
                int depth = 1;
                std::string inner;
                while (i < src.size() && depth > 0)
                {
                    char c = src[i];
                    if (c == '{') { depth++; inner += c; ++i; }
                    else if (c == '}') { depth--; if (depth) inner += c; ++i; }
                    else { inner += c; ++i; }
                }
                return inner;
            }

            std::string readAtom()
            {
                if (i >= src.size()) return {};
                if (src[i] == '{') return readGroup();
                if (src[i] == '\\' && i + 1 < src.size()
                    && isCmdChar(src[i + 1]))
                {
                    size_t start = i;
                    ++i;
                    while (i < src.size() && isCmdChar(src[i])) ++i;
                    return src.substr(start, i - start);
                }
                char c = src[i++];
                return std::string(1, c);
            }

            std::string readCommand()
            {
                std::string name;
                while (i < src.size() && isCmdChar(src[i]))
                    name += src[i++];
                if (name.empty() && i < src.size())
                    name = std::string(1, src[i++]);
                return name;
            }

            float layoutSubstring(const std::string &expr)
            {
                Layouter sub(expr, metrics, weight);
                sub.penX = penX;
                sub.penY = penY;
                sub.scale = scale;
                sub.run();
                for (auto &op : sub.ops) ops.push_back(op);
                if (sub.maxX > sub.minX)
                {
                    minX = std::min(minX, sub.minX);
                    maxX = std::max(maxX, sub.maxX);
                    minY = std::min(minY, sub.minY);
                    maxY = std::max(maxY, sub.maxY);
                }
                float adv = sub.penX - penX;
                penX = sub.penX;
                return adv;
            }

            void handleFrac()
            {
                std::string num = readGroup();
                std::string den = readGroup();

                // Inner scale: numerator/denominator render at script size
                // (matches LaTeX inline-math behaviour: \frac in textstyle
                // uses the same shrink as a superscript).
                const float innerScale =
                    scale * metrics.scriptPercentScaleDown();
                Layouter ln(num, metrics, weight);
                ln.scale = innerScale;
                ln.run();
                Layouter ld(den, metrics, weight);
                ld.scale = innerScale;
                ld.run();

                const float wn = std::max(0.f, ln.maxX - ln.minX);
                const float wd = std::max(0.f, ld.maxX - ld.minX);
                const float w = std::max(wn, wd);
                // Bar centerline = math-axis. Numerator/denominator clear
                // it by at least the MATH-table-specified gap, with a
                // minimum baseline shift the type designer recommends.
                const float barY = penY + metrics.axisHeightEm() * scale;
                const float barHalfTh =
                    0.5f * metrics.fractionRuleThicknessEm() * scale;
                const float numGap =
                    metrics.fractionNumeratorGapMinEm() * scale;
                const float denGap =
                    metrics.fractionDenominatorGapMinEm() * scale;

                const float numShiftX =
                    penX + (w - wn) * 0.5f - (ln.maxX > ln.minX ? ln.minX : 0.f);
                // Numerator: take the larger of (a) lift the numerator
                // baseline to fractionNumeratorShiftUp above pen — the
                // type designer's preferred home, and (b) lift it just
                // enough to clear the bar by numGap. Real fonts
                // typically satisfy (a) with room to spare, but tall
                // numerators force (b) to dominate.
                const float numShiftY = std::max(
                    (barY + barHalfTh + numGap) - ln.minY,
                    penY + metrics.fractionNumeratorShiftUpEm() * scale);
                for (auto op : ln.ops)
                {
                    if (op.type == LayoutOp::Glyph)
                    { op.x += numShiftX; op.y += numShiftY; }
                    else
                    {
                        op.x0 += numShiftX; op.x1 += numShiftX;
                        op.y0 += numShiftY; op.y1 += numShiftY;
                    }
                    ops.push_back(op);
                }

                const float denShiftX =
                    penX + (w - wd) * 0.5f - (ld.maxX > ld.minX ? ld.minX : 0.f);
                // Denominator: drop so its top clears the bar by denGap,
                // or to the type designer's recommended drop, whichever
                // pushes it lower (more negative).
                const float denShiftY = std::min(
                    (barY - barHalfTh - denGap) - ld.maxY,
                    penY - metrics.fractionDenominatorShiftDownEm() * scale);
                for (auto op : ld.ops)
                {
                    if (op.type == LayoutOp::Glyph)
                    { op.x += denShiftX; op.y += denShiftY; }
                    else
                    {
                        op.x0 += denShiftX; op.x1 += denShiftX;
                        op.y0 += denShiftY; op.y1 += denShiftY;
                    }
                    ops.push_back(op);
                }

                LayoutOp bar;
                bar.type = LayoutOp::Rule;
                bar.x0 = penX;
                bar.x1 = penX + w;
                bar.y0 = barY - barHalfTh;
                bar.y1 = barY + barHalfTh;
                ops.push_back(bar);

                minX = std::min(minX, penX);
                maxX = std::max(maxX, penX + w);
                if (ln.maxY > ln.minY)
                {
                    minY = std::min(minY, numShiftY + ln.minY);
                    maxY = std::max(maxY, numShiftY + ln.maxY);
                }
                if (ld.maxY > ld.minY)
                {
                    minY = std::min(minY, denShiftY + ld.minY);
                    maxY = std::max(maxY, denShiftY + ld.maxY);
                }

                penX += w + 0.08f * scale;
            }

            // \phantom{...} reserves the inner expression's bbox without
            // emitting any geometry. \hphantom keeps only the horizontal
            // contribution (advance pen, ignore vertical extent); \vphantom
            // keeps only the vertical contribution (don't advance the pen).
            void handlePhantom(bool horiz, bool vert)
            {
                if (i >= src.size() || src[i] != '{') return;
                std::string inner = readGroup();
                if (inner.empty()) return;

                Layouter sub(inner, metrics, weight);
                sub.scale = scale;
                sub.run();

                const float w = (sub.maxX > sub.minX)
                                    ? (sub.maxX - sub.minX) : 0.f;

                if (horiz)
                {
                    minX = std::min(minX, penX);
                    maxX = std::max(maxX, penX + w);
                    penX += w;
                }
                if (vert && sub.maxY > sub.minY)
                {
                    minY = std::min(minY, penY + sub.minY);
                    maxY = std::max(maxY, penY + sub.maxY);
                }
            }

            // \overline{...} (over=true) draws a horizontal rule above the
            // argument's ascender. \underline{...} (over=false) draws below
            // the argument's descender. The rule reuses the same op type as
            // the fraction bar so the geometry pass needs no changes.
            void handleOverUnderLine(bool over)
            {
                if (i >= src.size() || src[i] != '{') return;
                std::string inner = readGroup();
                if (inner.empty()) return;

                const float startX = penX;
                Layouter sub(inner, metrics, weight);
                sub.penX = penX;
                sub.penY = penY;
                sub.scale = scale;
                sub.run();
                for (auto &op : sub.ops) ops.push_back(op);

                // Rule thickness, vertical gap, and the extra ascender/
                // descender padding all come from the font's MATH table
                // (with stub defaults for non-math fonts).
                const float halfTh = 0.5f * (over
                    ? metrics.overbarRuleThicknessEm()
                    : metrics.underbarRuleThicknessEm()) * scale;
                const float gap = (over
                    ? metrics.overbarVerticalGapEm()
                    : metrics.underbarVerticalGapEm()) * scale;
                const float extra = (over
                    ? metrics.overbarExtraAscenderEm()
                    : metrics.underbarExtraDescenderEm()) * scale;
                float y;
                if (over)
                    y = std::max(sub.maxY,
                                 penY + metrics.ascenderEm() * scale)
                        + gap + extra;
                else
                    y = std::min(sub.minY,
                                 penY + metrics.descenderEm() * scale)
                        - gap - extra;

                LayoutOp bar;
                bar.type = LayoutOp::Rule;
                bar.x0 = startX;
                bar.x1 = sub.penX;
                bar.y0 = y - halfTh;
                bar.y1 = y + halfTh;
                ops.push_back(bar);

                if (sub.maxX > sub.minX)
                {
                    minX = std::min(minX, sub.minX);
                    maxX = std::max(maxX, sub.maxX);
                    minY = std::min(minY, std::min(sub.minY, y - halfTh));
                    maxY = std::max(maxY, std::max(sub.maxY, y + halfTh));
                }
                penX = sub.penX;
            }

            // \binom{n}{k} stacks two args like \frac but without the bar,
            // wrapped in parentheses. Uses the same MATH-table constants
            // as the fraction so binom and frac stay visually consistent.
            void handleBinom()
            {
                std::string num = readGroup();
                std::string den = readGroup();

                emitGlyph('(');

                const float innerScale =
                    scale * metrics.scriptPercentScaleDown();
                Layouter ln(num, metrics, weight);
                ln.scale = innerScale;
                ln.run();
                Layouter ld(den, metrics, weight);
                ld.scale = innerScale;
                ld.run();

                const float wn = std::max(0.f, ln.maxX - ln.minX);
                const float wd = std::max(0.f, ld.maxX - ld.minX);
                const float w = std::max(wn, wd);
                const float numGap =
                    metrics.fractionNumeratorGapMinEm() * scale;
                const float denGap =
                    metrics.fractionDenominatorGapMinEm() * scale;
                const float midY = penY + metrics.axisHeightEm() * scale;

                const float numShiftX = penX + (w - wn) * 0.5f
                    - (ln.maxX > ln.minX ? ln.minX : 0.f);
                const float numShiftY = (midY + numGap) - ln.minY;
                for (auto op : ln.ops)
                {
                    if (op.type == LayoutOp::Glyph)
                    { op.x += numShiftX; op.y += numShiftY; }
                    else
                    {
                        op.x0 += numShiftX; op.x1 += numShiftX;
                        op.y0 += numShiftY; op.y1 += numShiftY;
                    }
                    ops.push_back(op);
                }

                const float denShiftX = penX + (w - wd) * 0.5f
                    - (ld.maxX > ld.minX ? ld.minX : 0.f);
                const float denShiftY = (midY - denGap) - ld.maxY;
                for (auto op : ld.ops)
                {
                    if (op.type == LayoutOp::Glyph)
                    { op.x += denShiftX; op.y += denShiftY; }
                    else
                    {
                        op.x0 += denShiftX; op.x1 += denShiftX;
                        op.y0 += denShiftY; op.y1 += denShiftY;
                    }
                    ops.push_back(op);
                }

                minX = std::min(minX, penX);
                maxX = std::max(maxX, penX + w);
                if (ln.maxY > ln.minY)
                {
                    minY = std::min(minY, numShiftY + ln.minY);
                    maxY = std::max(maxY, numShiftY + ln.maxY);
                }
                if (ld.maxY > ld.minY)
                {
                    minY = std::min(minY, denShiftY + ld.minY);
                    maxY = std::max(maxY, denShiftY + ld.maxY);
                }

                penX += w + 0.08f * scale;
                emitGlyph(')');
            }

            // \overset{a}{b} places `a` (small, centered) above `b` at the
            // current pen. \underset{a}{b} places it below. Both consume two
            // braced arguments and advance by the wider of the two. Mirrors
            // the layout pattern used by handleFrac: sub-layout in local
            // coords, then shift into place.
            void handleOverUnderset(bool above)
            {
                std::string upper = readGroup();
                std::string lower = readGroup();

                Layouter base(lower, metrics, weight);
                base.scale = scale;
                base.run();
                Layouter sm(upper, metrics, weight);
                sm.scale = scale * metrics.scriptPercentScaleDown();
                sm.run();

                const float baseW = std::max(0.f, base.maxX - base.minX);
                const float smW = std::max(0.f, sm.maxX - sm.minX);
                const float w = std::max(baseW, smW);

                // Sub-layouters were laid out at local (0,0); shift them so
                // the base sits at the parent's (penX, penY) and the script
                // hovers above/below it. Mirrors how handleFrac folds penY
                // into the bar-y offset.
                const float baseShiftX = penX + (w - baseW) * 0.5f
                    - (base.maxX > base.minX ? base.minX : 0.f);
                for (auto op : base.ops)
                {
                    if (op.type == LayoutOp::Glyph)
                    { op.x += baseShiftX; op.y += penY; }
                    else
                    {
                        op.x0 += baseShiftX; op.x1 += baseShiftX;
                        op.y0 += penY; op.y1 += penY;
                    }
                    ops.push_back(op);
                }

                const float gap = 0.08f * scale;
                const float smShiftX = penX + (w - smW) * 0.5f
                    - (sm.maxX > sm.minX ? sm.minX : 0.f);
                const float baseTop = (base.maxY > base.minY)
                    ? base.maxY : (metrics.ascenderEm() * scale);
                const float baseBot = (base.minY < base.maxY)
                    ? base.minY : (metrics.descenderEm() * scale);
                const float smShiftY = above
                    ? (penY + baseTop + gap - sm.minY)
                    : (penY + baseBot - gap - sm.maxY);
                for (auto op : sm.ops)
                {
                    if (op.type == LayoutOp::Glyph)
                    { op.x += smShiftX; op.y += smShiftY; }
                    else
                    {
                        op.x0 += smShiftX; op.x1 += smShiftX;
                        op.y0 += smShiftY; op.y1 += smShiftY;
                    }
                    ops.push_back(op);
                }

                minX = std::min(minX, penX);
                maxX = std::max(maxX, penX + w);
                if (base.maxY > base.minY)
                {
                    minY = std::min(minY, penY + base.minY);
                    maxY = std::max(maxY, penY + base.maxY);
                }
                if (sm.maxY > sm.minY)
                {
                    minY = std::min(minY, smShiftY + sm.minY);
                    maxY = std::max(maxY, smShiftY + sm.maxY);
                }
                penX += w;
            }

            // \underbrace{base}_{label} (downward=true) and the mirrored
            // \overbrace{base}^{label} (downward=false). The brace itself is
            // a smooth-shouldered curly shape generated as a CCW polygon ring
            // and emitted as a Polygon op — the geometry pass extrudes it
            // alongside the regular Glyph/Rule ops. The optional subscript
            // (or superscript) is consumed eagerly so it lands centered below
            // (above) the brace's peak rather than at the right edge of the
            // base content, which is where the default `_` / `^` handler
            // would put it.
            void handleBrace(bool downward)
            {
                if (i >= src.size() || src[i] != '{') return;
                std::string inner = readGroup();
                if (inner.empty()) return;

                const float startX = penX;
                Layouter sub(inner, metrics, weight);
                sub.penX = penX;
                sub.penY = penY;
                sub.scale = scale;
                sub.run();
                for (auto &op : sub.ops) ops.push_back(op);

                // Stroke + curl + peak proportions in em fractions. Tuned to
                // feel close to LaTeX Computer Modern's underbrace — a slim
                // hairline brace that doesn't crowd the formula above it.
                const float thickness = 0.05f * scale;
                const float curl      = 0.13f * scale;
                const float peakH     = 0.18f * scale;
                const float peakW     = 0.10f * scale;
                const float gap       = 0.10f * scale;
                const int   samples   = 4;

                const float baseLeft  = startX;
                const float baseRight = sub.penX;

                // Anchor the brace just outside the base content's bbox so
                // it never overlaps glyphs that sit below the baseline
                // (descenders) or above the cap height (overbar accents).
                float braceY;
                if (downward)
                {
                    braceY = std::min(sub.minY,
                                      penY + metrics.descenderEm() * scale)
                             - gap;
                }
                else
                {
                    braceY = std::max(sub.maxY,
                                      penY + metrics.ascenderEm() * scale)
                             + gap;
                }

                std::vector<BracePt> skel = buildBraceSkeleton(
                    baseLeft, baseRight, braceY,
                    curl, peakH, peakW, downward, samples);
                std::vector<BracePt> ring = offsetSkeletonToRing(skel, thickness);

                if (!ring.empty())
                {
                    LayoutOp braceOp;
                    braceOp.type = LayoutOp::Polygon;
                    braceOp.polygon = ring;
                    ops.push_back(braceOp);
                }

                const float peakY = downward
                    ? braceY - curl - peakH - 0.5f * thickness
                    : braceY + curl + peakH + 0.5f * thickness;

                if (sub.maxX > sub.minX)
                {
                    minX = std::min(minX, sub.minX);
                    maxX = std::max(maxX, sub.maxX);
                    if (downward)
                        minY = std::min(minY, std::min(sub.minY, peakY));
                    else
                        maxY = std::max(maxY, std::max(sub.maxY, peakY));
                }
                penX = sub.penX;

                // Eagerly consume the matching script after the brace so the
                // label sits centered under (over) the peak. Underbrace pairs
                // with `_`, overbrace with `^`. The other script falls through
                // to the regular handler in run(), which attaches it to the
                // last glyph as if the brace weren't there — that mirrors how
                // the LaTeX engine treats `\underbrace{X}^{a}` (superscript
                // on the underbrace as a math operator).
                const char wantedScript = downward ? '_' : '^';
                if (i < src.size() && src[i] == wantedScript)
                {
                    ++i;
                    std::string label = readAtom();
                    if (!label.empty())
                    {
                        Layouter lbl(label, metrics, weight);
                        lbl.scale = scale * metrics.scriptPercentScaleDown();
                        lbl.run();

                        const float midX = 0.5f * (baseLeft + baseRight);
                        const float lblW = std::max(0.f, lbl.maxX - lbl.minX);
                        const float lblShiftX = midX - 0.5f * lblW
                            - (lbl.maxX > lbl.minX ? lbl.minX : 0.f);
                        const float labelGap = 0.06f * scale;
                        const float lblShiftY = downward
                            ? peakY - labelGap
                              - (lbl.maxY > lbl.minY ? lbl.maxY : 0.f)
                            : peakY + labelGap
                              - (lbl.minY < lbl.maxY ? lbl.minY : 0.f);

                        for (auto op : lbl.ops)
                        {
                            if (op.type == LayoutOp::Glyph)
                            {
                                op.x += lblShiftX;
                                op.y += lblShiftY;
                            }
                            else if (op.type == LayoutOp::Rule)
                            {
                                op.x0 += lblShiftX; op.x1 += lblShiftX;
                                op.y0 += lblShiftY; op.y1 += lblShiftY;
                            }
                            else if (op.type == LayoutOp::Polygon)
                            {
                                for (auto &v : op.polygon)
                                {
                                    v[0] += lblShiftX;
                                    v[1] += lblShiftY;
                                }
                            }
                            ops.push_back(op);
                        }

                        if (lbl.maxX > lbl.minX)
                        {
                            if (downward)
                                minY = std::min(minY, lblShiftY + lbl.minY);
                            else
                                maxY = std::max(maxY, lblShiftY + lbl.maxY);
                        }
                    }
                }
            }

            // Place an accent glyph above an inner expression. Uses the
            // MATH table's TopAccentAttachment per-glyph anchor when the
            // base is a single glyph, otherwise the geometric midpoint of
            // the base's bbox. The accent's vertical position is the max
            // of the base's actual top + a small gap and the type
            // designer's AccentBaseHeight constant — so accents on
            // ascender-tall bases (Latin caps, integrals) sit just above
            // the base, while accents on shorter bases (lowercase x, π)
            // line up at a uniform height.
            //
            // wide=true is for \widehat / \widetilde — a future wide-
            // variant lookup will pick a stretched accent glyph from the
            // MATH MathVariants table; today we share the single accent
            // glyph but center it over the entire base width, which still
            // looks correct for typical 1–3 char arguments.
            void handleAccent(uint32_t accentCp, bool wide)
            {
                if (i >= src.size() || src[i] != '{')
                {
                    // \hat with no argument — fall back to a bare accent.
                    emitGlyph(accentCp);
                    return;
                }
                std::string inner = readGroup();
                if (inner.empty()) return;

                // Layout the base in local coords, then shift into place.
                Layouter sub(inner, metrics, weight);
                sub.scale = scale;
                sub.run();
                for (auto op : sub.ops)
                {
                    if (op.type == LayoutOp::Glyph)
                    { op.x += penX; op.y += penY; }
                    else
                    {
                        op.x0 += penX; op.x1 += penX;
                        op.y0 += penY; op.y1 += penY;
                    }
                    ops.push_back(op);
                }

                // Determine the base's accent attachment x in global coords.
                float baseAttachX;
                const bool singleGlyph =
                    (sub.ops.size() == 1
                     && sub.ops[0].type == LayoutOp::Glyph);
                if (singleGlyph && !wide)
                {
                    const uint32_t baseCp = sub.ops[0].codepoint;
                    const float baseAttach =
                        metrics.topAccentAttachmentEm(baseCp) * scale;
                    baseAttachX = penX + sub.ops[0].x + baseAttach;
                }
                else if (sub.maxX > sub.minX)
                {
                    const float baseW = sub.maxX - sub.minX;
                    baseAttachX = penX + sub.minX + 0.5f * baseW;
                }
                else
                {
                    // Empty/whitespace-only base — anchor accent at pen.
                    baseAttachX = penX;
                }

                // Center the accent's own attachment point on baseAttachX.
                const float accentAttach =
                    metrics.topAccentAttachmentEm(accentCp) * scale;
                const float accentX = baseAttachX - accentAttach;

                // Vertical placement: sit above the base's actual top, but
                // never below AccentBaseHeight (gives stable placement
                // across short and tall bases).
                const float baseTop = (sub.maxY > sub.minY)
                    ? sub.maxY
                    : (penY + metrics.ascenderEm() * scale);
                const float accentY = std::max(
                    baseTop,
                    penY + metrics.accentBaseHeightEm() * scale);

                if (wide && (accentCp == 0x02DC || accentCp == 0x02C6))
                {
                    // Procedural wide accent. The font's small spacing
                    // diacritic at U+02DC / U+02C6 is fixed-size and (a)
                    // not always present in the math font's coverage, (b)
                    // not designed to span a wide base. Drawing a polygon
                    // ourselves dodges both problems and looks closer to
                    // what LaTeX's \widetilde / \widehat actually emit
                    // (which would otherwise need MathVariants assembly).
                    const float baseW = (sub.maxX > sub.minX)
                        ? (sub.maxX - sub.minX) : (0.5f * scale);
                    const float accentX0 = penX + sub.minX;
                    const float accentX1 = accentX0 + baseW;
                    const float gap       = 0.04f * scale;
                    const float thickness = 0.05f * scale;
                    const float peakH     = (accentCp == 0x02DC)
                        ? 0.07f * scale   // tilde peak amplitude
                        : 0.10f * scale;  // hat peak height
                    const float anchorY = accentY + gap;

                    std::vector<BracePt> skel = (accentCp == 0x02DC)
                        ? buildWideTildeSkeleton(accentX0, accentX1,
                                                 anchorY, peakH, 16)
                        : buildWideHatSkeleton(accentX0, accentX1,
                                               anchorY, peakH, 8);
                    std::vector<BracePt> ring = offsetSkeletonToRing(
                        skel, thickness);

                    if (!ring.empty())
                    {
                        LayoutOp accPoly;
                        accPoly.type = LayoutOp::Polygon;
                        accPoly.polygon = ring;
                        ops.push_back(accPoly);
                    }
                }
                else
                {
                    LayoutOp acc;
                    acc.type = LayoutOp::Glyph;
                    acc.codepoint = accentCp;
                    acc.x = accentX;
                    acc.y = accentY;
                    acc.scale = scale;
                    ops.push_back(acc);
                }

                // Bbox: include the base's extent and the accent's vertical
                // contribution. The accent's exact top depends on its
                // outline — use ascender as a safe bound.
                if (sub.maxX > sub.minX)
                {
                    minX = std::min(minX, sub.minX + penX);
                    maxX = std::max(maxX, sub.maxX + penX);
                    minY = std::min(minY, sub.minY + penY);
                }
                maxY = std::max(maxY,
                                accentY + metrics.ascenderEm() * scale);
                // sub was laid out from x=0; its local penX equals its
                // width. Advance our pen by that width — accents
                // themselves are non-spacing in our model.
                penX += sub.penX;
            }

            void run()
            {
                while (i < src.size())
                {
                    char c = src[i];

                    if (c == '\\')
                    {
                        ++i;
                        std::string name = readCommand();
                        if (name == "frac" || name == "tfrac" || name == "dfrac"
                            || name == "cfrac")
                        {
                            handleFrac();
                        }
                        else if (name == "phantom")
                        {
                            handlePhantom(true, true);
                        }
                        else if (name == "hphantom")
                        {
                            handlePhantom(true, false);
                        }
                        else if (name == "vphantom")
                        {
                            handlePhantom(false, true);
                        }
                        else if (name == "overline")
                        {
                            handleOverUnderLine(true);
                        }
                        else if (name == "underline")
                        {
                            handleOverUnderLine(false);
                        }
                        else if (name == "binom" || name == "dbinom"
                                 || name == "tbinom")
                        {
                            handleBinom();
                        }
                        else if (name == "overset")
                        {
                            handleOverUnderset(true);
                        }
                        else if (name == "underset")
                        {
                            handleOverUnderset(false);
                        }
                        else if (name == "underbrace")
                        {
                            handleBrace(true);
                        }
                        else if (name == "overbrace")
                        {
                            handleBrace(false);
                        }
                        else if (name == "!")
                        {
                            // Negative thin space — pull the pen back by
                            // ~3/18 em (the LaTeX convention).
                            penX -= 0.16f * scale;
                        }
                        else if (name == "bmod")
                        {
                            // Binary modulo operator: a small space, the
                            // letters "mod", then another small space.
                            penX += 0.18f * scale;
                            for (char ch : std::string("mod"))
                                emitGlyph(static_cast<unsigned char>(ch));
                            penX += 0.18f * scale;
                        }
                        else if (name == "pmod")
                        {
                            // Parenthesised modulo: " (mod <arg>)".
                            penX += 0.28f * scale;
                            emitGlyph('(');
                            for (char ch : std::string("mod "))
                                emitGlyph(static_cast<unsigned char>(ch));
                            if (i < src.size() && src[i] == '{')
                            {
                                std::string inner = readGroup();
                                if (!inner.empty()) layoutSubstring(inner);
                            }
                            emitGlyph(')');
                        }
                        else if (name == "displaystyle" || name == "textstyle"
                                 || name == "scriptstyle"
                                 || name == "scriptscriptstyle"
                                 || name == "limits" || name == "nolimits")
                        {
                            // Math-style directives — no argument, no-op in
                            // this stub (we don't switch inline/display sizes).
                        }
                        else if (name == "left" || name == "right"
                                 || name == "bigl" || name == "bigr"
                                 || name == "Bigl" || name == "Bigr"
                                 || name == "biggl" || name == "biggr"
                                 || name == "Biggl" || name == "Biggr"
                                 || name == "big" || name == "Big"
                                 || name == "bigg" || name == "Bigg")
                        {
                            // Delimiter-size annotations — ignore the prefix.
                        }
                        else if (name == "hat")    { handleAccent(0x02C6, false); }
                        else if (name == "widehat"){ handleAccent(0x02C6, true); }
                        else if (name == "tilde")  { handleAccent(0x02DC, false); }
                        else if (name == "widetilde"){ handleAccent(0x02DC, true); }
                        else if (name == "bar")    { handleAccent(0x00AF, false); }
                        else if (name == "vec")    { handleAccent(0x20D7, false); }
                        else if (name == "dot")    { handleAccent(0x02D9, false); }
                        else if (name == "ddot")   { handleAccent(0x00A8, false); }
                        else if (name == "check")  { handleAccent(0x02C7, false); }
                        else if (name == "breve")  { handleAccent(0x02D8, false); }
                        else if (name == "acute")  { handleAccent(0x00B4, false); }
                        else if (name == "grave")  { handleAccent(0x0060, false); }
                        else if (name == "mathrm" || name == "mathbf"
                                 || name == "mathit" || name == "mathsf"
                                 || name == "mathtt" || name == "mathcal"
                                 || name == "mathbb" || name == "mathfrak"
                                 || name == "mathscr"
                                 || name == "boldsymbol" || name == "bm"
                                 || name == "text" || name == "textrm"
                                 || name == "textbf" || name == "textit"
                                 || name == "operatorname")
                        {
                            // Font-style / text wrappers — lay out the
                            // inner argument as plain math. We don't have
                            // distinct outline variants per math style, so
                            // these reduce to "render the contents".
                            if (i < src.size() && src[i] == '{')
                            {
                                std::string inner = readGroup();
                                if (!inner.empty()) layoutSubstring(inner);
                            }
                        }
                        else if (name == "arg" || name == "max" || name == "min"
                                 || name == "sup" || name == "inf"
                                 || name == "lim" || name == "liminf" || name == "limsup"
                                 || name == "log" || name == "ln" || name == "exp"
                                 || name == "sin" || name == "cos" || name == "tan"
                                 || name == "sec" || name == "csc" || name == "cot"
                                 || name == "sinh" || name == "cosh" || name == "tanh"
                                 || name == "det" || name == "dim" || name == "ker"
                                 || name == "gcd" || name == "mod")
                        {
                            for (char ch : name)
                                emitGlyph(static_cast<unsigned char>(ch));
                        }
                        else if (name == "sqrt")
                        {
                            emitGlyph(0x221A);
                            if (i < src.size() && src[i] == '{')
                            {
                                std::string inner = readGroup();
                                if (!inner.empty()) layoutSubstring(inner);
                            }
                        }
                        else
                        {
                            const auto &table = latexSymbols();
                            auto it = table.find(name);
                            if (it != table.end())
                            {
                                emitGlyph(it->second);
                            }
                            else
                            {
                                for (char ch : name)
                                    emitGlyph(static_cast<unsigned char>(ch));
                            }
                        }
                    }
                    else if (c == '^' || c == '_')
                    {
                        bool sup = (c == '^');
                        ++i;
                        std::string atom = readAtom();
                        if (!atom.empty())
                        {
                            float savedScale = scale;
                            float savedPenY = penY;
                            scale *= metrics.scriptPercentScaleDown();
                            penY += sup
                                ? metrics.superscriptShiftUpEm() * savedScale
                                : -metrics.subscriptShiftDownEm() * savedScale;
                            layoutSubstring(atom);
                            scale = savedScale;
                            penY = savedPenY;
                        }
                    }
                    else if (c == '{')
                    {
                        std::string inner = readGroup();
                        if (!inner.empty()) layoutSubstring(inner);
                    }
                    else if (c == '}')
                    {
                        ++i;
                    }
                    else if (c == '(' || c == '[')
                    {
                        // Math brackets sit tight against their content but
                        // the 10% letter-spacing alone isn't enough breathing
                        // room from the preceding token (operator letters,
                        // subscripts, \mathbb{E}\!, …). A small lead-in keeps
                        // them visually separated.
                        if (!ops.empty()) penX += 0.10f * scale;
                        emitGlyph(static_cast<unsigned char>(c));
                        ++i;
                    }
                    else if (c == ')' || c == ']')
                    {
                        emitGlyph(static_cast<unsigned char>(c));
                        penX += 0.10f * scale;
                        ++i;
                    }
                    else if (c == ' ' || c == '\t')
                    {
                        penX += 0.28f * scale;
                        ++i;
                    }
                    else if (c == '\n')
                    {
                        const float lh = metrics.lineHeightEm();
                        penY -= (lh > 0.f ? lh : 1.2f) * scale;
                        penX = 0.f;
                        ++i;
                    }
                    else
                    {
                        emitGlyph(static_cast<unsigned char>(c));
                        ++i;
                    }
                }
            }
        };
    } // namespace

    Layout layout(const std::string &latex,
                  const FontMetrics &metrics,
                                float weight)
    {
        Layouter L(latex, metrics, weight);
        L.run();

        Layout out;
        out.ops = std::move(L.ops);
        out.minX = L.minX;
        out.maxX = L.maxX;
        out.minY = L.minY;
        out.maxY = L.maxY;
        return out;
    }

} // namespace latex3d
