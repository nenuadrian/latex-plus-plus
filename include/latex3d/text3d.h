#pragma once

#include <latex3d/math_table.h>
#include <latex3d/mesh.h>
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace latex3d
{

    struct Text3DOptions
    {
        // Height of a single line of text (cap/em height), in world units.
        float charHeight = 1.0f;
        // Extrusion depth along local Z (front face at +depth/2, back at -depth/2).
        // If <= 0, the mesh is a flat single-sided front face ("panel" mode)
        // useful for in-world signs / 2D formulas.
        float depth = 0.18f;
        // Vertical distance between baselines, as a multiple of the font's
        // native line height. 1.0 = native metrics; raise for looser leading.
        float lineSpacing = 1.0f;
        // Extra inter-glyph spacing in em-fractions (added to the font's
        // advance width). 0 keeps native kerning; small positive values give
        // more breathing room for carved/sign look.
        float tracking = 0.f;
        // Chord tolerance when flattening Bezier curves into polylines, as a
        // fraction of em height. Lower = smoother curves, more triangles.
        float curveTolerance = 0.01f;
        // Stroke weight added to every glyph outline, in em fractions. 0 keeps
        // the font's native weight; positive values thicken every stroke (the
        // "leg of A" gets a wider surface, not a deeper extrusion). Internally
        // applied via FT_Outline_Embolden, so counters/holes shrink correctly.
        // Negative values thin the strokes — usually undesirable.
        float weight = 0.f;
        // Baked vertex color. The mesh is rendered through the standard mesh
        // pipeline, so toon-shading + shadows apply automatically.
        glm::vec3 color{1.f, 1.f, 1.f};
        // Stylised dark border baked into the mesh as an inflated shell
        // around each glyph (polygon offset of the glyph contours, extruded
        // and colored `outlineColor`). Expressed in em fractions — 0 disables
        // the border entirely; typical pronounced values are 0.03..0.06.
        // Unlike the post-process Sobel outline, this is view-independent
        // and reads cleanly at any distance.
        float outlineThickness = 0.035f;
        glm::vec3 outlineColor{0.04f, 0.03f, 0.03f};
        // If true, each line is horizontally centered around x=0; otherwise
        // the first glyph's origin sits at x=0 and text grows in +X.
        bool centerX = true;
        // If true, the whole block is vertically centered around y=0;
        // otherwise the first line's baseline sits at y=0 and subsequent
        // lines fall into -Y.
        bool centerY = true;
    };

    // Two independent font slots. Display is the aesthetic face used by
    // in-world text3d signs (Cormorant Garamond in this project). Latex is a
    // separate face, loaded independently, that must have broad Unicode
    // coverage (Greek, math operators) because the LaTeX pipeline routes
    // everything through it — regardless of the decorative Display font,
    // formulas must always render. We deliberately keep this orthogonal so
    // swapping the decorative font never breaks a formula.
    enum class FontSlot
    {
        Display = 0,
        Latex = 1,
    };

    // Initialise a 3D-text font slot against a TrueType/OpenType file. Safe
    // to call multiple times — re-init switches that slot to the new font.
    // Returns false if the file can't be loaded. Cleanup is automatic at
    // program exit.
    bool loadFont(const std::string &ttfPath,
                        FontSlot slot = FontSlot::Display);

    // Release font resources held by one slot. Safe to call even if init was
    // never called for that slot.
    void unloadFont(FontSlot slot = FontSlot::Display);

    // Generate an extruded 3D mesh for (possibly multi-line) text using the
    // Display font slot. Unknown glyphs advance as spaces. Returns an empty
    // mesh if the Display font isn't initialised.
    MeshData generateTextMesh(const std::string &text,
                                          const Text3DOptions &opt = {});

    // ── Shared building blocks (also used by the 3D LaTeX pipeline) ─────────
    // The latex3d generator reuses the font loaded by loadFont() plus the
    // outline extraction and extrusion code below. The types/functions are
    // deliberately low-level so a higher-level layout engine (e.g. MicroTeX)
    // can position individual glyphs and rules without going through the
    // flat-string path.

    // A polygon set in em-space (y-up, baseline at y=0). Each polygon is a list
    // of rings where ring[0] is an outer contour (CCW) and ring[1..] are holes
    // (CW). Matches mapbox::earcut's expected input.
    using PolygonSet =
        std::vector<std::vector<std::vector<std::array<float, 2>>>>;

    struct GlyphInfo
    {
        // Non-owning pointer; valid until unloadFont(). May be null
        // if the glyph has no geometry (e.g. whitespace).
        const PolygonSet *polygons = nullptr;
        float advanceEm = 0.f;
        float lineHeightEm = 1.2f;
    };

    // Returns true if the given 3D-text font slot has been successfully
    // initialised.
    bool fontReady(FontSlot slot = FontSlot::Display);

    // Native font metrics in em units (from the face loaded in the given
    // slot). All three are zero if that slot isn't ready.
    float fontAscenderEm(FontSlot slot = FontSlot::Display);
    float fontDescenderEm(FontSlot slot = FontSlot::Display);
    float fontLineHeightEm(FontSlot slot = FontSlot::Display);

    // OpenType MATH-table accessors. has() returns true only when the slot
    // is ready and the loaded face actually carries a MATH table (Fira Math,
    // STIX Two Math, Latin Modern Math, …). When false, the LaTeX layouter
    // should fall back to its baked-in stub constants.
    bool fontHasMathTable(FontSlot slot = FontSlot::Latex);

    // Always returns a valid MathConstants — populated from the MATH table
    // if present, or default-initialised (LaTeX-CM-ish defaults) otherwise.
    // Cheap to call (small POD copy); no caching required at the call site.
    MathConstants fontMathConstants(FontSlot slot = FontSlot::Latex);

    // Per-glyph top-accent attachment x-coordinate in em fractions for the
    // glyph that maps to `codepoint` in the slot's font. Returns false if
    // the slot has no MATH table, the glyph is unmapped, or the glyph has
    // no entry in the MathTopAccentAttachment subtable — callers should
    // fall back to the glyph's advance/2.
    bool glyphTopAccentAttachmentEm(uint32_t codepoint,
                                          FontSlot slot,
                                          float &attachmentEm);

    // Look up (and lazily build) the outline for a Unicode codepoint in a
    // given font slot. Returns false if that slot isn't ready. Missing glyphs
    // still return true but with `polygons == nullptr` and a reasonable
    // advance so callers can treat them as spaces. `weight` is the stroke
    // emboldening offset in em fractions (see Text3DOptions::weight); 0 = the
    // font's native outline.
    bool getGlyph(uint32_t codepoint, float curveTolerance,
                        GlyphInfo &out,
                        FontSlot slot = FontSlot::Display,
                        float weight = 0.f);

    // Append an extruded solid to `data` from a polygon set in em-space.
    //   x, y   — world-space offset of the baseline origin (em-space 0,0 maps
    //            to this point).
    //   em     — em → world-unit scale (i.e. final glyph height).
    //   depth  — total Z extrusion. If <= 0 only a flat single-sided front
    //            face is emitted (panel mode); otherwise the front face sits
    //            at +depth/2, the back at -depth/2, with side walls between.
    void appendExtrudedPolygons(MeshData &data,
                                      const PolygonSet &polygons,
                                      float x, float y, float em,
                                      float depth,
                                      const glm::vec3 &color);

    // Append an axis-aligned rectangle [x0,y0]×[x1,y1] in world space,
    // extruded along Z with the same depth semantics as above. Handy for
    // fraction bars, \overline, and radical strokes.
    void appendExtrudedRect(MeshData &data,
                                  float x0, float y0,
                                  float x1, float y1,
                                  float depth,
                                  const glm::vec3 &color);

    // ── Stylised dark-border shell helpers ──────────────────────────────────
    // Used by generateTextMesh and the LaTeX pipeline to bake a dark
    // outline behind every glyph/rule. The shell is an inflated copy of the
    // same geometry, pushed slightly back in Z so the main mesh is never
    // occluded from the front. Exposed publicly so higher-level layouters
    // (e.g. latex3d) can stack identical outlines onto their own glyph and
    // rule ops without duplicating the offset/extrude code.

    // Grow every ring of `polygons` outward by `thicknessEm` em. For outer
    // rings this enlarges the shape; for holes (normalised CW) it shrinks
    // them, so counters stay topologically correct. Uses a miter join with
    // a clamped max-length so sharp serifs don't sprout spikes.
    PolygonSet inflatePolygonSet(const PolygonSet &polygons,
                                             float thicknessEm);

    // Extrude polygons and rect with an extra Z-center shift applied to
    // every emitted vertex — usually a small negative value to push an
    // outline shell just behind the main glyph's coplanar front face.
    void appendExtrudedPolygonsShifted(MeshData &data,
                                             const PolygonSet &polygons,
                                             float x, float y, float em,
                                             float depth, float zShift,
                                             const glm::vec3 &color);
    void appendExtrudedRectShifted(MeshData &data,
                                         float x0, float y0,
                                         float x1, float y1,
                                         float depth, float zShift,
                                         const glm::vec3 &color);

    // Emit a glyph (or any polygon set) with a baked dark outline shell
    // behind it. Wraps the common pattern used by the text3d and latex3d
    // generators: inflate polygons in em-space by `outlineThickness` (also
    // em-fractions), push the shell back in Z by a depth-precision-safe
    // offset that scales with `em`, extrude both. Outline is skipped when
    // `outlineThickness <= 0` or `polygons` is empty.
    void appendGlyphWithOutline(MeshData &data,
                                      const PolygonSet &polygons,
                                      float x, float y, float em,
                                      float depth,
                                      const glm::vec3 &color,
                                      float outlineThickness,
                                      const glm::vec3 &outlineColor);

    // Emit an axis-aligned world-space rectangle with the same dark outline
    // treatment used by the LaTeX rule (fraction bar, overline, radical
    // stroke). `em` converts the em-fraction outline thickness into the
    // world-unit pad applied around the rect. Outline is skipped when
    // `outlineThickness <= 0`.
    void appendRectWithOutline(MeshData &data,
                                     float x0, float y0,
                                     float x1, float y1,
                                     float em, float depth,
                                     const glm::vec3 &color,
                                     float outlineThickness,
                                     const glm::vec3 &outlineColor);

} // namespace latex3d
