#include "../source/procedural/proceduralstudio.h"
#include <cmath>
#include <iostream>

namespace
{
    using namespace ProcStudio;

    Float3 Subtract(Float3 a, Float3 b)
    {
        return {a.X - b.X, a.Y - b.Y, a.Z - b.Z};
    }

    Float3 Cross(Float3 a, Float3 b)
    {
        return {a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
    }

    float Dot(Float3 a, Float3 b)
    {
        return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
    }

    bool CheckNormals(const Mesh& mesh)
    {
        if (mesh.Normals.size() != mesh.Positions.size())
        {
            return false;
        }
        std::vector<Float3> areaNormals(mesh.Positions.size());
        for (size_t i = 0; i < mesh.Indices.size(); i += 3)
        {
            const auto a = mesh.Indices[i];
            const auto b = mesh.Indices[i + 1];
            const auto c = mesh.Indices[i + 2];
            const auto normal =
                Cross(Subtract(mesh.Positions[b], mesh.Positions[a]), Subtract(mesh.Positions[c], mesh.Positions[a]));
            for (auto index : {a, b, c})
            {
                areaNormals[index].X += normal.X;
                areaNormals[index].Y += normal.Y;
                areaNormals[index].Z += normal.Z;
            }
        }
        size_t mismatches = 0;
        for (size_t i = 0; i < mesh.Positions.size(); ++i)
        {
            const auto normal = mesh.Normals[i];
            const float length = std::sqrt(Dot(normal, normal));
            const float area = std::sqrt(Dot(areaNormals[i], areaNormals[i]));
            // 一点につぶれた葉の端点には面積がなく、幾何学的な法線を定義できない。
            if (!std::isfinite(length) || std::abs(length - 1) > 1e-4f ||
                (area > 1e-9f && Dot(normal, areaNormals[i]) / area < 0.999f))
            {
                ++mismatches;
            }
        }
        std::cout << "vertices=" << mesh.Positions.size() << " normal mismatches=" << mismatches << '\n';
        return mismatches == 0;
    }
} // 無名名前空間

int main()
{
    using namespace ProcStudio;
    int failures = 0;
    ModelParams params;
    params.Depth = 3;
    params.Branches = 2;
    params.LeafSize = 0.7f;
    params.Leaf.Depth = 1;
    // 表裏の面、折り曲げた葉身、複葉の葉軸、複数の配置方向を検証する。
    for (auto shape : {LeafShape::Card, LeafShape::Simple, LeafShape::Pinnate, LeafShape::Fern, LeafShape::Palmate})
    {
        params.Leaf.Shape = shape;
        for (uint32_t seed : {7u, 42u, 907u})
        {
            params.Seed = seed;
            std::cout << "shape=" << static_cast<int>(shape) << " seed=" << seed << ' ';
            if (!CheckNormals(GenerateModel(params)))
            {
                ++failures;
            }
        }
    }
    params.Leaves = false;
    if (!CheckNormals(GenerateModel(params)))
    {
        ++failures;
    }
    std::cout << "Procedural tree normal failures=" << failures << '\n';
    return failures != 0;
}
