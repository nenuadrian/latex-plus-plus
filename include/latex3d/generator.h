#pragma once

#include <latex3d/mesh.h>
#include <glm/glm.hpp>
#include <string>

namespace latex3d
{

    enum class ExtrusionMode
    {
        // Solid extruded 3D formula (depth > 0). Casts shadows; matches the
        // look of 3D world-text signs.
        Extruded,
        // Flat single-sided front face. Use for 2D signs / pictogram panels
        // where depth would be visually noisy.
        Panel,
    };

    struct LatexOptions
    {
        // Height of a single em (baseline ascent height) in world units.
        float charHeight = 1.0f;
        // Z extrusion in world units (front at +depth/2, back at -depth/2).
        // Ignored when mode == Panel.
        float depth = 0.18f;
        // Curve flattening tolerance in em units (forwarded to the font
        // outline rasteriser).
        float curveTolerance = 0.01f;
        // Stroke weight added to every glyph outline, in em fractions. 0
        // keeps the LaTeX font's native weight; positive values thicken every
        // stroke and rule. Forwarded to getGlyph (see Text3DOptions).
        float weight = 0.f;
        // Baked vertex color — rendered through the standard mesh pipeline
        // so toon shading + shadows apply.
        glm::vec3 color{1.f, 1.f, 1.f};
        // Stylised dark border baked into the mesh as an inflated shell
        // around each glyph AND each rule (fraction bars etc.). Same
        // semantics as Text3DOptions::outlineThickness / outlineColor — see
        // that comment for rationale. Rules are inflated in world units
        // equivalent to the same em fraction.
        float outlineThickness = 0.05f;
        glm::vec3 outlineColor{0.04f, 0.03f, 0.03f};
        // Re-center the compiled formula's bounding box around the origin
        // on each axis (useful for placing one formula cleanly at a world
        // point regardless of its visual extent).
        bool centerX = true;
        bool centerY = true;
        // Solid extruded vs flat panel.
        ExtrusionMode mode = ExtrusionMode::Extruded;
    };

    // Compile a LaTeX math expression into an extruded 3D mesh using the
    // font currently loaded by loadFont(). The font must be ready
    // before calling this — returns an empty mesh otherwise.
    //
    // Current backend is a lightweight stub parser that handles a useful
    // subset of math-mode commands: Greek letters (\alpha..\Omega), common
    // operators (\sum, \int, \cdot, \leq, ...), \frac{a}{b}, \sqrt{a},
    // superscripts (x^2, x^{ab}) and subscripts (a_i). See the MICROTEX
    // block in the implementation for the insertion point of a full LaTeX
    // layout engine.
    MeshData generateLatexMesh(const std::string &latex,
                                           const LatexOptions &opt = {});

} // namespace latex3d
