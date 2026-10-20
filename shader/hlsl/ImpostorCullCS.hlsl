// GPU Driven で描くモデルのインポスター追加（ImpostorRenderer が実行時にコンパイル）。
// 置き換えの判定は GPUOcclusionCS が1か所で行い、結果 w = 4 を書く。ここはそれを読んで
// エンティティ単位（全セクションのどれかが w = 4）でインポスターのインスタンスを追加するだけ。
#include "GpuSceneCommon.hlsli"

StructuredBuffer<GPUInstanceData> SceneInstances : register(t0);
StructuredBuffer<uint4> VisibilityResult : register(t1);
StructuredBuffer<uint4> Candidates : register(t2); // x: スロット一覧の先頭, y: スロット数, z: グループ, w: 出力先頭
StructuredBuffer<uint> CandidateSlots : register(t3);
StructuredBuffer<uint> GroupCapacity : register(t4);
struct ImpostorInstance
{
    row_major float4x4 World; // ImpostorVS と同じ並び（行優先）
};
RWStructuredBuffer<ImpostorInstance> ImpostorInstances : register(u0);
RWByteAddressBuffer ImpostorArgs : register(u1);

cbuffer ImpostorCullConstants : register(b0)
{
    uint CandidateCount;
    uint SlotCount;
    uint2 Padding;
};

[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= CandidateCount)
    {
        return;
    }
    uint4 candidate = Candidates[id.x];
    uint replacedSlot = 0xffffffffu;
    [loop] for (uint i = 0; i < candidate.y; ++i)
    {
        uint slot = CandidateSlots[candidate.x + i];
        if (slot < SlotCount && VisibilityResult[slot].w == 4u)
        {
            replacedSlot = slot;
            break;
        }
    }
    if (replacedSlot == 0xffffffffu)
    {
        return; // 近距離（メッシュで描画）または視錐台・オクルージョンで不可視
    }
    uint index = 0;
    ImpostorArgs.InterlockedAdd(candidate.z * 16u + 4u, 1u, index);
    if (index < GroupCapacity[candidate.z])
    {
        ImpostorInstance instance;
        instance.World = SceneInstances[replacedSlot].World;
        ImpostorInstances[candidate.w + index] = instance;
    }
}
