#pragma once

// Umbrella header — pulls in the entire public surface of the latex3d
// library. Consumers can pick the individual headers below if they want to
// minimise includes; this header exists for the common "I just want LaTeX
// 3D meshes" case.
//
//   latex3d/mesh.h         — renderer-agnostic Vertex POD + MeshData
//   latex3d/log.h          — log-callback hook for diagnostics
//   latex3d/math_table.h   — OpenType MATH-table parser (used by the
//                            generator; exposes MathConstants for embedders
//                            who want to query font math metrics directly)
//   latex3d/text3d.h       — font registry, glyph extraction, plain 3D text
//                            rendering, low-level extrusion primitives
//   latex3d/layouter.h     — font-agnostic LaTeX → em-space op stream
//   latex3d/generator.h    — high-level LaTeX → MeshData using the loaded
//                            Latex font slot

#include <latex3d/mesh.h>
#include <latex3d/log.h>
#include <latex3d/math_table.h>
#include <latex3d/text3d.h>
#include <latex3d/layouter.h>
#include <latex3d/generator.h>
