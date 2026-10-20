void WriteModelVertex(uint index, float3 position, float3 normal, float3 color, float2 uv)
{
    uint offset = index * 44;
    Output.Store3(offset, asuint(position));
    Output.Store3(offset + 12, asuint(normal));
    Output.Store3(offset + 24, asuint(color));
    Output.Store2(offset + 36, asuint(uv));
}

#if PROCEDURAL_SPECIALIZED_MODE == -1 || PROCEDURAL_SPECIALIZED_MODE == 8 || PROCEDURAL_SPECIALIZED_MODE == 9 || PROCEDURAL_SPECIALIZED_MODE == 10 || PROCEDURAL_SPECIALIZED_MODE == 11
float3 SpherePosition(uint index)
{
    float u = float(index % 65) / 64;
    float v = float(index / 65) / 32;
    return float3(sin(v * kPi) * cos(u * 2 * kPi), cos(v * kPi), sin(v * kPi) * sin(u * 2 * kPi));
}

void GenerateAnalyticVertex(uint index, uint mode)
{
    uint vertexCount = Parameters.Load(172);
    uint indexCount = Parameters.Load(176);
    uint level = Parameters.Load(180);
    if (mode == 8)
    {
        if (index < indexCount)
        {
            uint cell = index / 6;
            uint a = (cell / 64) * 65 + cell % 64;
            uint indices[6] = {a, a + 1, a + 65, a + 1, a + 66, a + 65};
            Output.Store(vertexCount * 44 + index * 4, indices[index % 6]);
        }
        if (index >= vertexCount)
        {
            return;
        }
        float3 normal = 0;
        int2 cell = int2(index % 65, index / 65);
        for (int row = max(cell.y - 1, 0); row <= min(cell.y, 31); ++row)
        {
            for (int column = max(cell.x - 1, 0); column <= min(cell.x, 63); ++column)
            {
                uint a = row * 65 + column;
                uint indices[6] = {a, a + 1, a + 65, a + 1, a + 66, a + 65};
                for (uint faceIndex = 0; faceIndex < 2; ++faceIndex)
                {
                    uint3 vertices =
                        uint3(indices[faceIndex * 3], indices[faceIndex * 3 + 1], indices[faceIndex * 3 + 2]);
                    if (any(vertices == index))
                    {
                        normal += cross(SpherePosition(vertices.y) - SpherePosition(vertices.x),
                                        SpherePosition(vertices.z) - SpherePosition(vertices.x));
                    }
                }
            }
        }
        if (length(normal) > 1e-12)
        {
            normal = normalize(normal);
        }
        else
        {
            normal = float3(0, 1, 0);
        }
        WriteModelVertex(index, SpherePosition(index), normal, 1, float2(cell) / float2(64, 32));
        return;
    }
    if (mode == 9 || mode == 10)
    {
        if (index < indexCount)
        {
            uint corners[6] = {0, 1, 2, 0, 2, 3};
            Output.Store(vertexCount * 44 + index * 4, (index / 6) * 4 + corners[index % 6]);
        }
        if (index >= vertexCount)
        {
            return;
        }
        uint cube = index / 24;
        uint face = (index % 24) / 4;
        uint corner = index % 4;
        float3 center = 0;
        float halfSize = 0.8;
        float3 color = 1;
        if (mode == 10)
        {
            halfSize = 1;
            color = float3(0.8, 0.78, 0.72);
            uint divisor = 1;
            for (uint depth = 1; depth < level; ++depth)
            {
                divisor *= 20;
            }
            for (uint depth = 0; depth < level; ++depth)
            {
                uint child = (cube / divisor) % 20;
                uint ordinal = 0;
                int3 chosen = 0;
                for (int z = -1; z <= 1; ++z)
                {
                    for (int y = -1; y <= 1; ++y)
                    {
                        for (int x = -1; x <= 1; ++x)
                        {
                            if (int(x == 0) + int(y == 0) + int(z == 0) >= 2)
                            {
                                continue;
                            }
                            if (ordinal == child)
                            {
                                chosen = int3(x, y, z);
                            }
                            ++ordinal;
                        }
                    }
                }
                halfSize /= 3;
                center += float3(chosen) * halfSize * 2;
                if (depth + 1 == level)
                {
                    color *= float3(0.5 + 0.125 * (chosen.x + 1), 0.55 + 0.1 * (chosen.y + 1),
                                    0.7 + 0.075 * (chosen.z + 1)) *
                             1.3;
                }
                divisor = max(divisor / 20, 1u);
            }
        }
        float3 normals[6] = {float3(1, 0, 0),  float3(-1, 0, 0), float3(0, 1, 0),
                             float3(0, -1, 0), float3(0, 0, 1),  float3(0, 0, -1)};
        float3 normal = normals[face];
        float3 up = float3(0, 1, 0);
        if (abs(normal.y) > 0.5)
        {
            up = float3(0, 0, 1);
        }
        float su[4] = {-1, 1, 1, -1};
        float sv[4] = {-1, -1, 1, 1};
        float3 position =
            center + normal * halfSize + cross(up, normal) * su[corner] * halfSize + up * sv[corner] * halfSize;
        float2 uv = float2(su[corner] * 0.5 + 0.5, 0.5 - sv[corner] * 0.5);
        if (mode == 10)
        {
            uv = float2(position.x + position.z, position.y + position.z);
        }
        WriteModelVertex(index, position, normal, color, uv);
        return;
    }
    if (index >= vertexCount)
    {
        return;
    }
    Output.Store(vertexCount * 44 + index * 4, index);
    uint tetrahedron = index / 12;
    float size = 1.2;
    float3 a = float3(-size, -size * 0.7, -size * 0.58);
    float3 b = float3(size, -size * 0.7, -size * 0.58);
    float3 c = float3(0, -size * 0.7, size * 1.15);
    float3 d = float3(0, size, 0);
    float3 color = float3(0.85, 0.65, 0.3);
    uint divisor = 1u << (2 * max(int(level) - 1, 0));
    for (uint depth = 0; depth < level; ++depth)
    {
        uint child = (tetrahedron / divisor) % 4;
        float3 ab = (a + b) * 0.5;
        float3 ac = (a + c) * 0.5;
        float3 ad = (a + d) * 0.5;
        float3 bc = (b + c) * 0.5;
        float3 bd = (b + d) * 0.5;
        float3 cd = (c + d) * 0.5;
        if (child == 0)
        {
            b = ab;
            c = ac;
            d = ad;
        }
        else if (child == 1)
        {
            a = ab;
            c = bc;
            d = bd;
            color *= float3(1.05, 0.95, 0.9);
        }
        else if (child == 2)
        {
            a = ac;
            b = bc;
            d = cd;
        }
        else
        {
            a = ad;
            b = bd;
            c = cd;
            color *= float3(1.05, 0.95, 0.9);
        }
        divisor = max(divisor / 4, 1u);
    }
    float3 vertices[12] = {a, c, b, a, b, d, b, c, d, c, a, d};
    uint face = (index % 12) / 3;
    uint corner = index % 3;
    float3 first = vertices[face * 3];
    float3 second = vertices[face * 3 + 1];
    float3 third = vertices[face * 3 + 2];
    float3 normal = cross(second - first, third - first);
    if (dot(normal, first - (a + b + c + d) * 0.25) < 0)
    {
        float3 saved = second;
        second = third;
        third = saved;
        normal = -normal;
    }
    float3 position = first;
    if (corner == 1)
    {
        position = second;
    }
    else if (corner == 2)
    {
        position = third;
    }
    WriteModelVertex(index, position, normalize(normal), color, 0);
}

#endif
