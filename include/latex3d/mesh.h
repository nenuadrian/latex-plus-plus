#pragma once

#include <cstdint>
#include <vector>

namespace latex3d
{

    // Renderer-agnostic vertex POD. The library never depends on a host
    // engine's vertex layout — consumers convert at the boundary. Triangle
    // winding is CCW (front face); flip indices on import for left-handed
    // renderers.
    struct Vertex
    {
        float position[3];
        float color[3];
        float normal[3];
    };

    struct MeshData
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
    };

} // namespace latex3d
