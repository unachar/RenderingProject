#include <draco/compression/encode.h>
#include <draco/mesh/triangle_soup_mesh_builder.h>
#include <array>
#include <fstream>
#include <iostream>

int main()
{
    draco::TriangleSoupMeshBuilder builder;
    builder.Start(2);
    const int position = builder.AddAttribute(draco::GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);
    const int normal = builder.AddAttribute(draco::GeometryAttribute::NORMAL, 3, draco::DT_FLOAT32);
    const int uv = builder.AddAttribute(draco::GeometryAttribute::TEX_COORD, 2, draco::DT_FLOAT32);
    const int color = builder.AddAttribute(draco::GeometryAttribute::COLOR, 4, draco::DT_UINT8, true);
    const int joints = builder.AddAttribute(draco::GeometryAttribute::GENERIC, 4, draco::DT_UINT8);
    const int weights = builder.AddAttribute(draco::GeometryAttribute::GENERIC, 4, draco::DT_UINT8, true);
    const int attributes[]{position, normal, uv, color, joints, weights};
    const std::uint32_t ids[]{13, 7, 22, 42, 55, 66};
    for (int i = 0; i < 6; ++i)
    {
        builder.SetAttributeUniqueId(attributes[i], ids[i]);
    }
    const float positions[][3]{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
    const float normals[3]{0, 0, 1};
    const float texcoords[][2]{{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    const std::uint8_t colors[][4]{{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}};
    const std::uint8_t joint[4]{0, 0, 0, 0}, weight[4]{255, 0, 0, 0};
    const int faces[][3]{{0, 1, 2}, {1, 3, 2}};
    for (int face = 0; face < 2; ++face)
    {
        const auto index = draco::FaceIndex(face);
        const auto& f = faces[face];
        builder.SetAttributeValuesForFace(position, index, positions[f[0]], positions[f[1]], positions[f[2]]);
        builder.SetAttributeValuesForFace(normal, index, normals, normals, normals);
        builder.SetAttributeValuesForFace(uv, index, texcoords[f[0]], texcoords[f[1]], texcoords[f[2]]);
        builder.SetAttributeValuesForFace(color, index, colors[f[0]], colors[f[1]], colors[f[2]]);
        builder.SetAttributeValuesForFace(joints, index, joint, joint, joint);
        builder.SetAttributeValuesForFace(weights, index, weight, weight, weight);
    }
    auto mesh = builder.Finalize();
    if (!mesh)
    {
        return 1;
    }
    draco::Encoder encoder;
    encoder.SetSpeedOptions(5, 5);
    encoder.SetAttributeQuantization(draco::GeometryAttribute::POSITION, 0);
    encoder.SetAttributeQuantization(draco::GeometryAttribute::NORMAL, 0);
    encoder.SetAttributeQuantization(draco::GeometryAttribute::TEX_COORD, 0);
    draco::EncoderBuffer buffer;
    const auto status = encoder.EncodeMeshToBuffer(*mesh, &buffer);
    if (!status.ok())
    {
        std::cerr << status.error_msg() << '\n';
        return 2;
    }
    std::ofstream encoded("bin/validation/draco/fixture.drc", std::ios::binary);
    encoded.write(buffer.data(), buffer.size());
    std::ofstream metadata("bin/validation/draco/fixture.json");
    metadata << "{\"points\":" << mesh->num_points() << ",\"bytes\":" << buffer.size() << '}';
    return encoded && metadata ? 0 : 3;
}
