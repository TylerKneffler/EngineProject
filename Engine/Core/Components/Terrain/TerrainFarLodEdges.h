#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace Engine::Components
{
// Coarse tile interiors remain coarse, but every tile boundary retains the
// height grid's cell vertices. Adjacent tiles therefore share the same edge
// polyline even when their interior strides differ.
template<class Vertex, class MakeVertex>
void BuildFarLodIndices(std::vector<Vertex>& vertices,
    std::vector<uint32_t>& indices, int width, int depth, int startX,
    int startY, int endX, int endY, int stride, MakeVertex makeVertex)
{
    for (int row = 0; row < depth - 1; ++row)
        for (int col = 0; col < width - 1; ++col)
        {
            const uint32_t a = row * width + col, b = a + width;
            if (row != 0 && row != depth - 2 && col != 0 && col != width - 2)
            {
                indices.insert(indices.end(), {a, b, a + 1, a + 1, b, b + 1});
                continue;
            }
            const int x0 = std::min(startX + col * stride, endX);
            const int x1 = std::min(x0 + stride, endX);
            const int y0 = std::min(startY + row * stride, endY);
            const int y1 = std::min(y0 + stride, endY);
            Vertex center = makeVertex((x0 + x1) / 2, (y0 + y1) / 2);
            for (int axis = 0; axis < 3; ++axis)
                center.pos[axis] = (vertices[a].pos[axis] + vertices[a + 1].pos[axis] +
                    vertices[b].pos[axis] + vertices[b + 1].pos[axis]) * .25f;
            const uint32_t middle = static_cast<uint32_t>(vertices.size());
            vertices.push_back(center);
            std::vector<uint32_t> boundary;
            boundary.push_back(a);
            const auto add = [&](int x, int y) {
                boundary.push_back(static_cast<uint32_t>(vertices.size()));
                vertices.push_back(makeVertex(x, y));
            };
            if (col == 0) for (int y = y0 + 1; y < y1; ++y) add(x0, y);
            boundary.push_back(b);
            if (row == depth - 2) for (int x = x0 + 1; x < x1; ++x) add(x, y1);
            boundary.push_back(b + 1);
            if (col == width - 2) for (int y = y1 - 1; y > y0; --y) add(x1, y);
            boundary.push_back(a + 1);
            if (row == 0) for (int x = x1 - 1; x > x0; --x) add(x, y0);
            for (size_t i = 0; i < boundary.size(); ++i)
                indices.insert(indices.end(), {middle, boundary[i], boundary[(i + 1) % boundary.size()]});
        }
}
}
