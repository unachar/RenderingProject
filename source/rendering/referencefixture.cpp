#include "pch.h"
#include "referencefixture.h"

#include <cmath>

namespace ReferenceFixture
{
    namespace
    {
        // Row-vector 3x4 affine compose: world = local * (S * R * T).
        struct Transform
        {
            float Rows[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
            float Translation[3] = {0, 0, 0};
        };

        Transform Compose(float sx, float sy, float sz, float rotXRadians, float rotYRadians, float tx, float ty,
                          float tz)
        {
            // R = Rx * Ry (row-vector order: X rotation applied first).
            const float cx = std::cos(rotXRadians), sxn = std::sin(rotXRadians);
            const float cy = std::cos(rotYRadians), syn = std::sin(rotYRadians);
            // Rx rows: [1 0 0; 0 cx sxn; 0 -sxn cx], Ry rows: [cy 0 -syn; 0 1 0; syn 0 cy]
            // R = Rx * Ry (row-vector product).
            float r[3][3];
            r[0][0] = cy;
            r[0][1] = 0.0f;
            r[0][2] = -syn;
            r[1][0] = sxn * syn;
            r[1][1] = cx;
            r[1][2] = sxn * cy;
            r[2][0] = cx * syn;
            r[2][1] = -sxn;
            r[2][2] = cx * cy;
            Transform t{};
            const float scale[3] = {sx, sy, sz};
            for (int i = 0; i < 3; ++i)
            {
                for (int j = 0; j < 3; ++j)
                {
                    t.Rows[i][j] = scale[i] * r[i][j];
                }
                t.Rows[i][3] = 0.0f;
            }
            t.Translation[0] = tx;
            t.Translation[1] = ty;
            t.Translation[2] = tz;
            return t;
        }

        struct Builder
        {
            SceneData Data;
            uint32_t AddGeometry(const float* verts, uint32_t vertCount, const uint32_t* indices, uint32_t indexCount)
            {
                const uint32_t geometryIndex = static_cast<uint32_t>(Data.GeometryInfo.size() / 4);
                Data.GeometryInfo.push_back(static_cast<uint32_t>(Data.Indices.size()));
                Data.GeometryInfo.push_back(static_cast<uint32_t>(Data.Positions.size() / 3));
                Data.GeometryInfo.push_back(indexCount);
                Data.GeometryInfo.push_back(0);
                Data.Positions.insert(Data.Positions.end(), verts, verts + vertCount * 3);
                Data.Indices.insert(Data.Indices.end(), indices, indices + indexCount);
                return geometryIndex;
            }
            uint32_t AddMaterial(float r, float g, float b, float metallic, float er, float eg, float eb,
                                 float roughness)
            {
                const uint32_t index = static_cast<uint32_t>(Data.MaterialData.size() / 12);
                const float values[12] = {r, g, b, metallic, er, eg, eb, roughness, 0, 0, 0, 0};
                Data.MaterialData.insert(Data.MaterialData.end(), values, values + 12);
                return index;
            }
            void AddInstance(uint32_t geometry, uint32_t material, const Transform& t)
            {
                Data.InstanceTransforms0.insert(Data.InstanceTransforms0.end(), t.Rows[0], t.Rows[0] + 4);
                Data.InstanceTransforms0.insert(Data.InstanceTransforms0.end(), t.Rows[1], t.Rows[1] + 4);
                Data.InstanceTransforms1.insert(Data.InstanceTransforms1.end(), t.Rows[2], t.Rows[2] + 4);
                const float translation[4] = {t.Translation[0], t.Translation[1], t.Translation[2], 1.0f};
                Data.InstanceTransforms1.insert(Data.InstanceTransforms1.end(), translation, translation + 4);
                const uint32_t info[4] = {geometry, material, 0, 0};
                Data.InstanceInfo.insert(Data.InstanceInfo.end(), info, info + 4);
            }
            void AddPointLight(float x, float y, float z, float r, float g, float b, float intensity)
            {
                const float values[8] = {x, y, z, 1.0f, r, g, b, intensity};
                Data.Lights.insert(Data.Lights.end(), values, values + 8);
            }
        };

        constexpr float kHalfPi = 1.57079632679f;
        constexpr float kPi = 3.14159265359f;
    } // namespace

    SceneData BuildCornellBox()
    {
        Builder builder;
        // Unit quad in the XY plane, front face +Z.
        const float quadVerts[12] = {-0.5f, -0.5f, 0, 0.5f, -0.5f, 0, 0.5f, 0.5f, 0, -0.5f, 0.5f, 0};
        const uint32_t quadIndices[6] = {0, 1, 2, 0, 2, 3};
        const uint32_t quad = builder.AddGeometry(quadVerts, 4, quadIndices, 6);
        // Unit cube, 8 corners, outward faces.
        const float cubeVerts[24] = {-0.5f, -0.5f, -0.5f, 0.5f, -0.5f, -0.5f, 0.5f, 0.5f, -0.5f, -0.5f, 0.5f, -0.5f,
                                     -0.5f, -0.5f, 0.5f,  0.5f, -0.5f, 0.5f,  0.5f, 0.5f, 0.5f,  -0.5f, 0.5f, 0.5f};
        const uint32_t cubeIndices[36] = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
                                          3, 6, 2, 3, 7, 6, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};
        const uint32_t cube = builder.AddGeometry(cubeVerts, 8, cubeIndices, 36);

        const uint32_t white = builder.AddMaterial(0.73f, 0.73f, 0.73f, 0.0f, 0, 0, 0, 0.9f);
        const uint32_t red = builder.AddMaterial(0.63f, 0.065f, 0.05f, 0.0f, 0, 0, 0, 0.9f);
        const uint32_t green = builder.AddMaterial(0.14f, 0.45f, 0.09f, 0.0f, 0, 0, 0, 0.9f);
        const uint32_t lightMat = builder.AddMaterial(0.0f, 0.0f, 0.0f, 0.0f, 18.0f, 18.0f, 18.0f, 1.0f);

        // Room: 2 x 2 x 2, camera looks toward +Z from -Z.
        builder.AddInstance(quad, white, Compose(2, 2, 1, -kHalfPi, 0, 0, -1, 0));      // floor, normal +Y
        builder.AddInstance(quad, white, Compose(2, 2, 1, kHalfPi, 0, 0, 1, 0));        // ceiling, normal -Y
        builder.AddInstance(quad, white, Compose(2, 2, 1, 0, kPi, 0, 0, 1));            // back, normal -Z
        builder.AddInstance(quad, red, Compose(2, 2, 1, 0, kHalfPi, -1, 0, 0));         // left, normal +X
        builder.AddInstance(quad, green, Compose(2, 2, 1, 0, -kHalfPi, 1, 0, 0));       // right, normal -X
        builder.AddInstance(quad, lightMat, Compose(0.6f, 0.6f, 1, kHalfPi, 0, 0, 0.99f, 0)); // ceiling light
        // Boxes resting on the floor.
        builder.AddInstance(cube, white, Compose(0.6f, 1.2f, 0.6f, 0, 0.35f, -0.35f, -0.4f, 0.25f));
        builder.AddInstance(cube, white, Compose(0.55f, 0.55f, 0.55f, 0, -0.26f, 0.35f, -0.725f, -0.2f));

        builder.AddPointLight(0.0f, 0.85f, 0.0f, 1.0f, 1.0f, 1.0f, 8.0f);

        builder.Data.CameraPosition[0] = 0.0f;
        builder.Data.CameraPosition[1] = 0.0f;
        builder.Data.CameraPosition[2] = -3.4f;
        builder.Data.CameraTarget[0] = 0.0f;
        builder.Data.CameraTarget[1] = 0.0f;
        builder.Data.CameraTarget[2] = 0.0f;
        builder.Data.CameraFovYRadians = 0.9f;
        return builder.Data;
    }
} // namespace ReferenceFixture
