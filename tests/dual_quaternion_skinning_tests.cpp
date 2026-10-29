#include "skinning_hlsl_math_shim.h"
#include "../shader/hlsl/DualQuaternionSkinning.hlsli"
#include <cassert>
#include <cstdio>
#include <limits>

static void Near(float3 a, float3 b)
{
    if (std::abs(a.x-b.x)>=0.0002f || std::abs(a.y-b.y)>=0.0002f || std::abs(a.z-b.z)>=0.0002f)
        std::fprintf(stderr,"actual (%g,%g,%g) expected (%g,%g,%g)\n",a.x,a.y,a.z,b.x,b.y,b.z);
    assert(std::abs(a.x-b.x)<0.0002f);
    assert(std::abs(a.y-b.y)<0.0002f);
    assert(std::abs(a.z-b.z)<0.0002f);
}
static float4x4 Matrix(float4 q, float3 t = {0,0,0})
{
    const auto r = MatrixFromQuaternion(q);
    float4x4 m;
    for (int i=0;i<3;++i) for (int j=0;j<3;++j) m[i][j]=r[i][j];
    m[3][0]=t.x; m[3][1]=t.y; m[3][2]=t.z;
    return m;
}
int main()
{
    // Matrix conversion uses row-vector rotations and row-3 translation.
    const float s=std::sqrt(0.5f);
    const float4 rotations[]={{0,0,0,1},{s,0,0,s},{0,s,0,s},{0,0,s,s},
                              {1,0,0,0},{0,1,0,0},{0,0,1,0}};
    for (auto q : rotations)
    {
        const auto m=Matrix(q,{2,-3,4});
        assert(IsRigidSkinningMatrix(m));
        const auto dq=NormalizeDualQuaternion(DualQuaternionFromMatrix(m));
        Near(TransformDualQuaternionPosition(dq,{1,2,3}),mul({1,2,3},(float3x3)m)+float3(2,-3,4));
        Near(RotateDualQuaternionVector(dq,{0,1,0}),mul({0,1,0},(float3x3)m));
    }
    // Independent expected direction catches a transpose/sign error shared by conversion helpers.
    Near(TransformDualQuaternionPosition(DualQuaternionFromMatrix(Matrix({0,0,s,s})),{1,0,0}),{0,1,0});
    const float c=std::sqrt(0.75f);
    auto a=DualQuaternionFromMatrix(Matrix({0,0,0.5f,c},{2,0,0}));
    auto b=DualQuaternionFromMatrix(Matrix({0,0,-0.5f,c},{2,0,0}));
    auto zero=ZeroDualQuaternion();
    auto blend=AccumulateDualQuaternion(zero,a,0.5f,a.Real);
    blend=AccumulateDualQuaternion(blend,b,0.5f,a.Real);
    blend=NormalizeDualQuaternion(blend);
    // +/-60-degree twist must retain radius (LBS halves this point's radius).
    // Avoid a 180-degree relative-rotation tie, which has two equally short paths.
    Near(TransformDualQuaternionPosition(blend,{1,0,0}),{3,0,0});
    Near(RotateDualQuaternionVector(blend,{1,0,0}),{1,0,0});
    b=a; b.Real=-b.Real; b.Dual=-b.Dual;
    blend=AccumulateDualQuaternion(AccumulateDualQuaternion(zero,a,2,a.Real),b,3,a.Real);
    blend=NormalizeDualQuaternion(blend);
    Near(TransformDualQuaternionPosition(blend,{1,0,0}),{2.5f,std::sqrt(0.75f),0});
    assert(std::abs(dot(blend.Real,blend.Real)-1)<0.00001f);
    assert(std::abs(dot(blend.Real,blend.Dual))<0.00001f);
    Near(TransformDualQuaternionPosition(NormalizeDualQuaternion(zero),{1,2,3}),{1,2,3});
    // Four active influences with one rotation preserve the weighted translation.
    auto four=ZeroDualQuaternion();
    const float3 translations[]={{1,2,3},{-2,5,1},{3,-1,4},{6,0,-2}};
    const float influences[]={0.1f,0.2f,0.3f,0.4f};
    float3 mean(0,0,0);
    for (int i=0;i<4;++i)
    {
        auto value=DualQuaternionFromMatrix(Matrix({0,0,s,s},translations[i]));
        four=AccumulateDualQuaternion(four,value,influences[i],float4(0,0,s,s));
        mean=mean+translations[i]*influences[i];
    }
    four=NormalizeDualQuaternion(four);
    Near(TransformDualQuaternionPosition(four,{1,0,0}),mean+float3(0,1,0));
    float4x4 bad;
    bad[0][0]=2; assert(!IsRigidSkinningMatrix(bad)); // uniform/nonuniform scale
    bad=float4x4{}; bad[0][0]=bad[1][1]=bad[2][2]=2; assert(!IsRigidSkinningMatrix(bad));
    bad=float4x4{}; bad[0][1]=0.2f; assert(!IsRigidSkinningMatrix(bad)); // shear
    bad=float4x4{}; bad[0][0]=-1; assert(!IsRigidSkinningMatrix(bad)); // reflection
    bad=float4x4{}; bad[0][3]=1; assert(!IsRigidSkinningMatrix(bad)); // projective
    bad=float4x4{}; bad[3][3]=2; assert(!IsRigidSkinningMatrix(bad));
    bad=float4x4{}; bad[3][0]=std::numeric_limits<float>::infinity(); assert(!IsRigidSkinningMatrix(bad));
    bad=float4x4{}; bad[1][1]=std::numeric_limits<float>::quiet_NaN(); assert(!IsRigidSkinningMatrix(bad));
    puts("Dual quaternion shader math: passed");
}
