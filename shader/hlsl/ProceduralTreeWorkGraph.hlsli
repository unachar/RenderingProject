// Deterministic depth-first construction keeps the original random stream and topology.
// A separate graph node copies leaf placements in parallel after preparation completes.
struct TreeLeafParameters
{
    uint Shape;
    int Depth;
    int Leaflets;
    float Angle;
    float Scale;
    float Taper;
    float Width;
    float Droop;
    float Fold;
    int Segments;
    float ColorVariation;
    float3 Color;
    float3 TipColor;
};

TreeLeafParameters LoadTreeLeafParameters(uint offset)
{
    TreeLeafParameters leaf;
    leaf.Shape = SourcePixels.Load(offset);
    leaf.Depth = asint(SourcePixels.Load(offset + 4));
    leaf.Leaflets = asint(SourcePixels.Load(offset + 8));
    leaf.Angle = asfloat(SourcePixels.Load(offset + 12));
    leaf.Scale = asfloat(SourcePixels.Load(offset + 16));
    leaf.Taper = asfloat(SourcePixels.Load(offset + 20));
    leaf.Width = asfloat(SourcePixels.Load(offset + 24));
    leaf.Droop = asfloat(SourcePixels.Load(offset + 28));
    leaf.Fold = asfloat(SourcePixels.Load(offset + 32));
    leaf.Segments = asint(SourcePixels.Load(offset + 36));
    leaf.ColorVariation = asfloat(SourcePixels.Load(offset + 40));
    leaf.Color = asfloat(SourcePixels.Load3(offset + 44));
    leaf.TipColor = asfloat(SourcePixels.Load3(offset + 56));
    return leaf;
}

uint TreeCountsOffset()
{
    return Parameters.Load(172) * 44 + Parameters.Load(176) * 4;
}

float TreeRandom(inout uint state)
{
    state = Hash(state + 0x9e3779b9u);
    return Unit(state);
}

float TreeSignedRandom(inout uint state)
{
    return TreeRandom(state) * 2 - 1;
}

float3 TreeNormalize(float3 direction)
{
    float magnitude = sqrt(dot(direction, direction));
    if (magnitude > 1e-12)
    {
        return direction / magnitude;
    }
    return float3(0, 1, 0);
}

float2 TreeTurn(float2 direction, float angle)
{
    float cosine = cos(angle);
    float sine = sin(angle);
    return float2(direction.x * cosine - direction.y * sine, direction.x * sine + direction.y * cosine);
}

float3 TreeRotate(float3 direction, float3 axis, float angle)
{
    float cosine = cos(angle);
    float sine = sin(angle);
    return direction * cosine + cross(axis, direction) * sine + axis * dot(axis, direction) * (1 - cosine);
}

struct TreeMeshCursor
{
    uint VertexBase;
    uint IndexBase;
    uint Vertices;
    uint Indices;
    bool Emit;
    uint IndexLimit;
};

uint TreeAddVertex(inout TreeMeshCursor cursor, float3 position, float3 color, float2 uv)
{
    uint index = cursor.Vertices++;
    if (cursor.Emit)
    {
        if (index >= Parameters.Load(172))
        {
            Output.Store(TreeCountsOffset() + 8, 1);
            return index;
        }
        uint address = cursor.VertexBase + index * 44;
        Output.Store3(address, asuint(position));
        Output.Store3(address + 12, 0);
        Output.Store3(address + 24, asuint(color));
        Output.Store2(address + 36, asuint(uv));
    }
    return index;
}

void TreeAddTriangle(inout TreeMeshCursor cursor, uint3 triangleIndices)
{
    uint index = cursor.Indices;
    cursor.Indices += 3;
    if (cursor.Emit)
    {
        if (index + 2 >= Parameters.Load(176))
        {
            Output.Store(TreeCountsOffset() + 8, 1);
            return;
        }
        Output.Store3(cursor.IndexBase + index * 4, triangleIndices);
    }
}

void TreeBlade(inout TreeMeshCursor cursor, TreeLeafParameters leaf, float2 origin, float2 direction, float bladeLength,
               float halfWidth, float tint)
{
    int segments = clamp(leaf.Segments, 2, 16);
    if (!cursor.Emit)
    {
        cursor.Vertices += 6 * (segments + 1);
        cursor.Indices += 24 * segments;
        return;
    }
    float2 side = float2(-direction.y, direction.x);
    for (uint face = 0; face < 2; ++face)
    {
        uint first = cursor.Vertices;
        for (int row = 0; row <= segments; ++row)
        {
            float along = float(row) / segments;
            float outline = pow(max(sin(kPi * pow(along, 0.75)), 0), 0.9);
            for (int column = -1; column <= 1; ++column)
            {
                float2 position = origin + direction * bladeLength * along + side * (halfWidth * outline) * column;
                float3 color = lerp(leaf.Color, leaf.TipColor, along * 0.8);
                float veinTint = 1;
                if (column == 0)
                {
                    veinTint = 0.85;
                }
                TreeAddVertex(cursor, float3(position, 0), color * (tint * veinTint),
                              float2(0.5 + 0.5 * column * outline, along));
            }
        }
        for (int row = 0; row < segments; ++row)
        {
            for (uint column = 0; column < 2; ++column)
            {
                uint firstCorner = first + row * 3 + column;
                if (face == 0)
                {
                    TreeAddTriangle(cursor, uint3(firstCorner, firstCorner + 4, firstCorner + 1));
                    TreeAddTriangle(cursor, uint3(firstCorner, firstCorner + 3, firstCorner + 4));
                }
                else
                {
                    TreeAddTriangle(cursor, uint3(firstCorner, firstCorner + 1, firstCorner + 4));
                    TreeAddTriangle(cursor, uint3(firstCorner, firstCorner + 4, firstCorner + 3));
                }
            }
        }
    }
}

void TreeRachis(inout TreeMeshCursor cursor, TreeLeafParameters leaf, float2 origin, float2 direction, float axisLength,
                float tint)
{
    if (!cursor.Emit)
    {
        cursor.Vertices += 8;
        cursor.Indices += 12;
        return;
    }
    float halfWidth = axisLength * 0.012 + 0.002;
    float2 side = float2(-direction.y, direction.x);
    float2 tip = origin + direction * axisLength;
    float3 color = leaf.Color * float3(0.8, 0.75, 0.6) * tint;
    float3 corners[4] = {float3(origin - side * halfWidth, 0), float3(origin + side * halfWidth, 0),
                         float3(tip + side * halfWidth * 0.4, 0), float3(tip - side * halfWidth * 0.4, 0)};
    for (uint face = 0; face < 2; ++face)
    {
        uint first = cursor.Vertices;
        for (uint corner = 0; corner < 4; ++corner)
        {
            float along = 1;
            if (corner < 2)
            {
                along = 0;
            }
            TreeAddVertex(cursor, corners[corner], color, float2(0.5, along));
        }
        if (face == 0)
        {
            TreeAddTriangle(cursor, uint3(first, first + 2, first + 1));
            TreeAddTriangle(cursor, uint3(first, first + 3, first + 2));
        }
        else
        {
            TreeAddTriangle(cursor, uint3(first, first + 1, first + 2));
            TreeAddTriangle(cursor, uint3(first, first + 2, first + 3));
        }
    }
}

struct LeafGrowthFrame
{
    float2 Origin;
    float2 Direction;
    float Length;
    int Depth;
    uint Child;
};

LeafGrowthFrame NewLeafFrame(float2 origin, float2 direction, float axisLength, int depth)
{
    LeafGrowthFrame frame;
    frame.Origin = origin;
    frame.Direction = direction;
    frame.Length = axisLength;
    frame.Depth = depth;
    frame.Child = 0xffffffff;
    return frame;
}

void BuildTreeLeaf(inout TreeMeshCursor cursor, TreeLeafParameters leaf, uint seed)
{
    uint random = Hash(seed ^ 0x1eafu);
    if (leaf.Shape == 0)
    {
        TreeBlade(cursor, leaf, 0, float2(0, 1), 1, 0.5, 1);
        if (cursor.Emit && cursor.Vertices <= Parameters.Load(172))
        {
            for (uint vertex = 0; vertex < cursor.Vertices; ++vertex)
            {
                uint address = cursor.VertexBase + vertex * 44;
                int column = int(vertex % 3);
                Output.Store(address, asuint((1 - column) * 0.3));
                Output.Store(address + 36, asuint(0.5 - (1 - column) * 0.5));
            }
        }
        return;
    }
    LeafGrowthFrame frames[5];
    frames[0] = NewLeafFrame(0, float2(0, 1), 1, clamp(leaf.Depth, 0, 4));
    int stack = 0;
    while (stack >= 0)
    {
        if (!cursor.Emit && cursor.Indices > cursor.IndexLimit)
        {
            return;
        }
        LeafGrowthFrame frame = frames[stack];
        if (frame.Child == 0xffffffff)
        {
            float tint = 1 + TreeSignedRandom(random) * leaf.ColorVariation;
            if (frame.Depth <= 0 || leaf.Shape == 1 || frame.Length < 1e-3)
            {
                TreeBlade(cursor, leaf, frame.Origin, frame.Direction, frame.Length, frame.Length * leaf.Width * 0.5,
                          tint);
                --stack;
                continue;
            }
            if (leaf.Shape != 4)
            {
                TreeRachis(cursor, leaf, frame.Origin, frame.Direction, frame.Length, tint);
            }
            frame.Child = 0;
        }
        int count = clamp(leaf.Leaflets, 1, 12);
        float angle = leaf.Angle * kPi / 180;
        if (leaf.Shape == 4)
        {
            int lobes = count | 1;
            if (frame.Child >= uint(lobes))
            {
                --stack;
                continue;
            }
            float across = 0;
            if (lobes != 1)
            {
                across = float(frame.Child) / (lobes - 1) * 2 - 1;
            }
            float childLength = frame.Length * (1 - leaf.Taper * 0.6 * abs(across));
            if (frame.Depth > 1)
            {
                childLength *= min(leaf.Scale + 0.35, 1);
            }
            float2 childDirection = TreeTurn(frame.Direction, across * angle * 1.4 + TreeSignedRandom(random) * 0.05);
            ++frame.Child;
            frames[stack] = frame;
            frames[++stack] = NewLeafFrame(frame.Origin, childDirection, childLength, frame.Depth - 1);
            continue;
        }
        bool fern = leaf.Shape == 3;
        int pairs = count;
        if (fern)
        {
            pairs *= 2;
        }
        if (frame.Child < uint(pairs * 2))
        {
            uint pair = frame.Child / 2;
            int sign = -1;
            if ((frame.Child & 1) != 0)
            {
                sign = 1;
            }
            float along = 0.12 + 0.78 * (float(pair) + 0.5) / pairs;
            float scale = leaf.Scale * (1 - leaf.Taper * along);
            if (fern)
            {
                scale *= 1.2;
            }
            float2 childOrigin = frame.Origin + frame.Direction * frame.Length * along;
            float childAngle = sign * angle;
            bool alternating = ((pair & 1) != 0 && sign < 0) || ((pair & 1) == 0 && sign > 0);
            if (fern && alternating && frame.Depth == 1)
            {
                childOrigin += frame.Direction * frame.Length * 0.5 / pairs;
            }
            else
            {
                childAngle += TreeSignedRandom(random) * 0.04;
            }
            ++frame.Child;
            frames[stack] = frame;
            frames[++stack] =
                NewLeafFrame(childOrigin, TreeTurn(frame.Direction, childAngle), frame.Length * scale, frame.Depth - 1);
            continue;
        }
        if (frame.Child == uint(pairs * 2))
        {
            float terminalScale = 1;
            if (fern)
            {
                terminalScale = 0.9;
            }
            float terminal = frame.Length * terminalScale * leaf.Scale;
            float2 childOrigin = frame.Origin + frame.Direction * (frame.Length - terminal * 0.2);
            ++frame.Child;
            frames[stack] = frame;
            frames[++stack] = NewLeafFrame(childOrigin, frame.Direction, terminal, frame.Depth - 1);
            continue;
        }
        --stack;
    }
}

void NormalizeTreeLeaf(TreeMeshCursor cursor, TreeLeafParameters leaf)
{
    float lower = 3.402823466e+38;
    float upper = -3.402823466e+38;
    for (uint vertex = 0; vertex < cursor.Vertices; ++vertex)
    {
        float height = asfloat(Output.Load(cursor.VertexBase + vertex * 44 + 4));
        lower = min(lower, height);
        upper = max(upper, height);
    }
    float extent = max(upper - lower, 1e-4);
    for (uint vertex = 0; vertex < cursor.Vertices; ++vertex)
    {
        uint address = cursor.VertexBase + vertex * 44;
        float3 position = asfloat(Output.Load3(address)) / extent;
        float along = saturate(position.y);
        position.z -= leaf.Droop * along * along;
        position.z += leaf.Fold * abs(position.x);
        Output.Store3(address, asuint(position));
    }
}

void ComputeTreeMeshNormals(TreeMeshCursor cursor)
{
    for (uint triangleOffset = 0; triangleOffset + 2 < cursor.Indices; triangleOffset += 3)
    {
        uint3 indices = Output.Load3(cursor.IndexBase + triangleOffset * 4);
        float3 first = asfloat(Output.Load3(cursor.VertexBase + indices.x * 44));
        float3 second = asfloat(Output.Load3(cursor.VertexBase + indices.y * 44));
        float3 third = asfloat(Output.Load3(cursor.VertexBase + indices.z * 44));
        float3 normal = cross(second - first, third - first);
        for (uint corner = 0; corner < 3; ++corner)
        {
            uint address = cursor.VertexBase + indices[corner] * 44 + 12;
            float3 accumulated = asfloat(Output.Load3(address));
            Output.Store3(address, asuint(accumulated + normal));
        }
    }
    for (uint vertex = 0; vertex < cursor.Vertices; ++vertex)
    {
        uint address = cursor.VertexBase + vertex * 44 + 12;
        Output.Store3(address, asuint(TreeNormalize(asfloat(Output.Load3(address)))));
    }
}

struct TreeGrowthFrame
{
    float3 Origin;
    float3 Direction;
    float3 End;
    float3 Side;
    float Length;
    float Radius;
    float EndRadius;
    float Phase;
    int Depth;
    int Child;
};

TreeGrowthFrame NewTreeFrame(float3 origin, float3 direction, float axisLength, float radius, int depth)
{
    TreeGrowthFrame frame = (TreeGrowthFrame)0;
    frame.Origin = origin;
    frame.Direction = direction;
    frame.Length = axisLength;
    frame.Radius = radius;
    frame.Depth = depth;
    frame.Child = -1;
    return frame;
}

void TreeBranch(inout TreeMeshCursor cursor, TreeGrowthFrame frame, int segments, float3 color)
{
    float3 axis = TreeNormalize(frame.End - frame.Origin);
    float3 helper = float3(1, 0, 0);
    if (abs(axis.y) < 0.95)
    {
        helper = float3(0, 1, 0);
    }
    float3 side = TreeNormalize(cross(helper, axis));
    float3 up = cross(axis, side);
    uint first = cursor.Vertices;
    for (int segment = 0; segment <= segments; ++segment)
    {
        float angle = float(segment) / segments * 2 * kPi;
        float3 direction = side * cos(angle) + up * sin(angle);
        TreeAddVertex(cursor, frame.Origin + direction * frame.Radius, color, float2(float(segment) / segments, 0));
        TreeAddVertex(cursor, frame.End + direction * frame.EndRadius, color,
                      float2(float(segment) / segments, frame.Length));
    }
    for (int segment = 0; segment < segments; ++segment)
    {
        uint firstCorner = first + segment * 2;
        TreeAddTriangle(cursor, uint3(firstCorner, firstCorner + 2, firstCorner + 1));
        TreeAddTriangle(cursor, uint3(firstCorner + 2, firstCorner + 3, firstCorner + 1));
    }
}

uint BuildProceduralTree(inout TreeMeshCursor cursor, uint placementBase)
{
    int originalDepth = asint(SourcePixels.Load(60));
    int branches = max(asint(SourcePixels.Load(64)), 0);
    float lengthRatio = asfloat(SourcePixels.Load(76));
    float radiusRatio = asfloat(SourcePixels.Load(80));
    float spreadAngle = asfloat(SourcePixels.Load(84));
    float tropism = asfloat(SourcePixels.Load(88));
    float jitter = asfloat(SourcePixels.Load(92));
    bool hasLeaves = (SourcePixels.Load(96) & 255) != 0;
    float leafSize = asfloat(SourcePixels.Load(100));
    int leavesPerTip = clamp(asint(SourcePixels.Load(104)), 1, 12);
    int radialSegments = asint(SourcePixels.Load(180));
    uint random = Hash(SourcePixels.Load(40));
    uint placements = 0;
    TreeGrowthFrame frames[9];
    frames[0] = NewTreeFrame(0, float3(0, 1, 0), asfloat(SourcePixels.Load(68)), asfloat(SourcePixels.Load(72)),
                             clamp(originalDepth, 1, 9));
    int stack = 0;
    while (stack >= 0)
    {
        if (Output.Load(TreeCountsOffset() + 8) != 0)
        {
            return placements;
        }
        TreeGrowthFrame frame = frames[stack];
        if (frame.Child < 0)
        {
            // Name each draw to keep the CPU generator's left-to-right random ordering.
            float jitterX = TreeSignedRandom(random) * jitter * 0.3;
            float jitterZ = TreeSignedRandom(random) * jitter * 0.3;
            frame.Direction = TreeNormalize(frame.Direction + float3(jitterX, tropism, jitterZ));
            frame.End = frame.Origin + frame.Direction * frame.Length;
            float endRadiusRatio = radiusRatio + (1 - radiusRatio) * 0.25;
            if (frame.Depth <= 1)
            {
                endRadiusRatio = 0.35;
            }
            frame.EndRadius = frame.Radius * endRadiusRatio;
            float bark = 0.6 + 0.4 * TreeRandom(random);
            TreeBranch(cursor, frame, max(3, radialSegments - (originalDepth - frame.Depth)),
                       float3(0.36, 0.25, 0.16) * bark);
            if (frame.Depth <= 1)
            {
                if (hasLeaves)
                {
                    for (int leaf = 0; leaf < leavesPerTip; ++leaf)
                    {
                        float3 offset;
                        offset.x = TreeSignedRandom(random);
                        offset.y = TreeSignedRandom(random) * 0.5;
                        offset.z = TreeSignedRandom(random);
                        float3 forward = TreeNormalize(frame.Direction + offset * 1.2);
                        float3 normalJitter;
                        normalJitter.x = TreeSignedRandom(random);
                        normalJitter.y = 0;
                        normalJitter.z = TreeSignedRandom(random);
                        float3 normal = TreeNormalize(float3(0, 1, 0) + normalJitter * 0.6);
                        float tint = 0.75 + 0.25 * TreeRandom(random);
                        if (placements >= Parameters.Load(180))
                        {
                            Output.Store(TreeCountsOffset() + 8, 1);
                            return placements;
                        }
                        uint address = placementBase + placements * 40;
                        Output.Store3(address, asuint(frame.End + offset * (leafSize * 0.3)));
                        Output.Store3(address + 12, asuint(forward));
                        Output.Store3(address + 24, asuint(normal));
                        Output.Store(address + 36, asuint(tint));
                        ++placements;
                    }
                }
                --stack;
                continue;
            }
            float3 helper = float3(1, 0, 0);
            if (abs(frame.Direction.y) < 0.95)
            {
                helper = float3(0, 1, 0);
            }
            frame.Side = TreeNormalize(cross(helper, frame.Direction));
            frame.Phase = TreeRandom(random) * 2 * kPi;
            frame.Child = 0;
        }
        if (frame.Child < branches)
        {
            float spread = (spreadAngle + TreeSignedRandom(random) * spreadAngle * jitter) * kPi / 180;
            float3 direction = TreeRotate(frame.Direction, frame.Side, spread);
            direction = TreeRotate(direction, frame.Direction, frame.Phase + 2.39996323 * frame.Child);
            float scale = lengthRatio * (1 + TreeSignedRandom(random) * jitter * 0.3);
            ++frame.Child;
            frames[stack] = frame;
            frames[++stack] =
                NewTreeFrame(frame.End, direction, frame.Length * scale, frame.EndRadius, frame.Depth - 1);
            continue;
        }
        if (frame.Child == branches && frame.Depth > 2)
        {
            ++frame.Child;
            frames[stack] = frame;
            frames[++stack] = NewTreeFrame(frame.End, frame.Direction, frame.Length * lengthRatio * 0.9,
                                           frame.EndRadius * 0.95, frame.Depth - 2);
            continue;
        }
        --stack;
    }
    return placements;
}

uint PrepareProceduralTree(uint mode)
{
    uint vertexCapacity = Parameters.Load(172);
    uint indexCapacity = Parameters.Load(176);
    uint counts = TreeCountsOffset();
    // Initialize the header before any capacity check can set its failure flag.
    for (uint field = 0; field < 16; ++field)
    {
        Output.Store(counts + field * 4, 0);
    }
    uint placementBase = counts + 64;
    uint templateVertices = placementBase + Parameters.Load(180) * 40;
    uint templateIndices = templateVertices + vertexCapacity * 44;
    TreeMeshCursor mesh = (TreeMeshCursor)0;
    mesh.IndexBase = vertexCapacity * 44;
    mesh.Emit = true;
    mesh.IndexLimit = 0xffffffff;
    if (mode == 14)
    {
        TreeLeafParameters leaf = LoadTreeLeafParameters(0);
        BuildTreeLeaf(mesh, leaf, SourcePixels.Load(68));
        if (Output.Load(counts + 8) != 0)
        {
            return 0;
        }
        NormalizeTreeLeaf(mesh, leaf);
        ComputeTreeMeshNormals(mesh);
        Output.Store2(counts, uint2(mesh.Vertices, mesh.Indices));
        if (asfloat(Parameters.Load(184)) != 0)
        {
            return mesh.Vertices;
        }
        return 0;
    }
    uint placementCount = BuildProceduralTree(mesh, placementBase);
    if (Output.Load(counts + 8) != 0)
    {
        return 0;
    }
    ComputeTreeMeshNormals(mesh);
    uint barkVertices = mesh.Vertices;
    uint barkIndices = mesh.Indices;
    Output.Store2(counts + 12, uint2(barkVertices, barkIndices));
    if (placementCount == 0)
    {
        Output.Store2(counts, uint2(barkVertices, barkIndices));
        return 0;
    }
    TreeLeafParameters leaf = LoadTreeLeafParameters(112);
    int originalSegments = leaf.Segments;
    uint seed = SourcePixels.Load(40);
    uint triangleBudget = uint(max(asint(SourcePixels.Load(108)), 1000));
    uint trianglesPerPlacement = triangleBudget / placementCount;
    TreeMeshCursor leafMesh;
    while (true)
    {
        leafMesh = (TreeMeshCursor)0;
        leafMesh.IndexLimit = trianglesPerPlacement * 3;
        BuildTreeLeaf(leafMesh, leaf, seed);
        if (leafMesh.Indices / 3 <= trianglesPerPlacement || (leaf.Segments <= 3 && leaf.Depth <= 0))
        {
            break;
        }
        if (leaf.Segments > 3)
        {
            --leaf.Segments;
        }
        else
        {
            --leaf.Depth;
            leaf.Segments = max(originalSegments, 3);
        }
    }
    leafMesh = (TreeMeshCursor)0;
    leafMesh.VertexBase = templateVertices;
    leafMesh.IndexBase = templateIndices;
    leafMesh.Emit = true;
    leafMesh.IndexLimit = 0xffffffff;
    BuildTreeLeaf(leafMesh, leaf, seed);
    if (Output.Load(counts + 8) != 0)
    {
        return 0;
    }
    NormalizeTreeLeaf(leafMesh, leaf);
    ComputeTreeMeshNormals(leafMesh);
    uint64_t finalVertices = uint64_t(barkVertices) + uint64_t(leafMesh.Vertices) * placementCount;
    uint64_t finalIndices = uint64_t(barkIndices) + uint64_t(leafMesh.Indices) * placementCount;
    if (finalVertices > vertexCapacity || finalIndices > indexCapacity)
    {
        Output.Store(counts + 8, 1);
        return 0;
    }
    Output.Store2(counts, uint2(uint(finalVertices), uint(finalIndices)));
    Output.Store3(counts + 20, uint3(leafMesh.Vertices, leafMesh.Indices, placementCount));
    Output.Store3(counts + 32, uint3(templateVertices, templateIndices, placementBase));
    return max(leafMesh.Vertices, leafMesh.Indices) * placementCount;
}

void FinalizeProceduralTree(uint index)
{
    uint counts = TreeCountsOffset();
    if (Parameters.Load(0) == 14)
    {
        if (index < Output.Load(counts) && asfloat(Parameters.Load(184)) != 0)
        {
            uint address = index * 44;
            float3 position = asfloat(Output.Load3(address));
            float3 normal = asfloat(Output.Load3(address + 12));
            Output.Store3(address, asuint(float3(position.x, position.z, -position.y)));
            Output.Store3(address + 12, asuint(float3(normal.x, normal.z, -normal.y)));
        }
        return;
    }
    uint barkVertices = Output.Load(counts + 12);
    uint barkIndices = Output.Load(counts + 16);
    uint templateVertexCount = Output.Load(counts + 20);
    uint templateIndexCount = Output.Load(counts + 24);
    uint placementCount = Output.Load(counts + 28);
    uint templateVertices = Output.Load(counts + 32);
    uint templateIndices = Output.Load(counts + 36);
    uint placementBase = Output.Load(counts + 40);
    uint vertices = templateVertexCount * placementCount;
    uint indices = templateIndexCount * placementCount;
    if (index < vertices)
    {
        uint placement = index / templateVertexCount;
        uint vertex = index % templateVertexCount;
        uint placementAddress = placementBase + placement * 40;
        float3 origin = asfloat(Output.Load3(placementAddress));
        float3 forward = TreeNormalize(asfloat(Output.Load3(placementAddress + 12)));
        float3 placementNormal = asfloat(Output.Load3(placementAddress + 24));
        float tint = asfloat(Output.Load(placementAddress + 36));
        float3 side = cross(forward, placementNormal);
        if (dot(side, side) < 1e-8)
        {
            float3 helper = float3(1, 0, 0);
            if (abs(forward.y) < 0.95)
            {
                helper = float3(0, 1, 0);
            }
            side = cross(forward, helper);
        }
        side = TreeNormalize(side);
        float3 normal = cross(side, forward);
        uint source = templateVertices + vertex * 44;
        float3 localPosition = asfloat(Output.Load3(source));
        float3 localNormal = asfloat(Output.Load3(source + 12));
        float3 color = asfloat(Output.Load3(source + 24));
        float2 uv = asfloat(Output.Load2(source + 36));
        float leafSize = asfloat(SourcePixels.Load(100));
        float3 worldPosition =
            origin + (side * localPosition.x + forward * localPosition.y + normal * localPosition.z) * leafSize;
        float3 worldNormal = TreeNormalize(side * localNormal.x + forward * localNormal.y + normal * localNormal.z);
        WriteModelVertex(barkVertices + index, worldPosition, worldNormal, color * tint, uv);
    }
    if (index < indices)
    {
        uint placement = index / templateIndexCount;
        uint triangleIndex = index % templateIndexCount;
        uint vertex = Output.Load(templateIndices + triangleIndex * 4);
        uint indexBase = Parameters.Load(172) * 44;
        Output.Store(indexBase + (barkIndices + index) * 4, barkVertices + placement * templateVertexCount + vertex);
    }
}
