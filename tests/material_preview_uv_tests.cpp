#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <vector>

using namespace DirectX;
using namespace std;

struct StaticModelVertex
{
    XMFLOAT3 Position;
    XMFLOAT3 Normal;
    XMFLOAT2 TexCoord;
    XMFLOAT4 Color;
};

// Compile the actual preview geometry builder without initializing the renderer.
#include "material-preview-geometry.inc"

namespace SceneGeometry
{
struct Vertex
{
    XMFLOAT3 Pos;
    XMFLOAT3 Normal;
    XMFLOAT2 TexCoord;
    XMFLOAT4 Color;
};

enum class Color { White };
XMFLOAT4 GetVertexColor(Color)
{
    return {1, 1, 1, 1};
}

#include "scene-plane-geometry.inc"
}

int main()
{
    int failures = 0;
    for (int shape : {1, 2})
    {
        const auto vertices = MakeGeometry(shape);
        const size_t expectedCount = shape == 1 ? 36 : 6;
        if (vertices.size() != expectedCount)
        {
            ++failures;
        }
        for (const auto& vertex : vertices)
        {
            const auto& normal = vertex.Normal;
            XMFLOAT3 right;
            XMFLOAT3 up;
            if (abs(normal.y) > 0.5f)
            {
                right = {1, 0, 0};
                up = {0, 0, normal.y};
            }
            else
            {
                right = {-normal.z, 0, normal.x};
                up = {0, 1, 0};
            }
            const auto position = XMLoadFloat3(&vertex.Position);
            const float horizontal = XMVectorGetX(XMVector3Dot(position, XMLoadFloat3(&right)));
            const float vertical = XMVectorGetX(XMVector3Dot(position, XMLoadFloat3(&up)));
            const float expectedU = (horizontal + 1) * 0.5f;
            const float expectedV = (1 - vertical) * 0.5f;
            if (abs(vertex.TexCoord.x - expectedU) > 0.0001f ||
                abs(vertex.TexCoord.y - expectedV) > 0.0001f)
            {
                cerr << "Shape " << shape << " face normal " << normal.x << ',' << normal.y << ',' << normal.z
                     << ": UV does not preserve image right/top\n";
                ++failures;
            }
        }
    }
    const auto plane = SceneGeometry::CreatePlaneVertices(SceneGeometry::Color::White);
    if (plane.size() != 6)
    {
        ++failures;
    }
    for (const auto& vertex : plane)
    {
        // Viewed from +Z above the plane: screen right is -X, image top is -Z.
        const float expectedU = 0.5f - vertex.Pos.x;
        const float expectedV = vertex.Pos.z + 0.5f;
        if (abs(vertex.TexCoord.x - expectedU) > 0.0001f ||
            abs(vertex.TexCoord.y - expectedV) > 0.0001f)
        {
            cerr << "Scene plane mirrors the source image\n";
            ++failures;
        }
    }
    if (failures != 0)
    {
        return 1;
    }
    cout << "UV orientation passed for preview cube/plane and scene plane.\n";
}
