#include "d3d12_compute_fixture.h"
#include <array>
#include <cmath>
#include <limits>

namespace
{
    // Match animationmodel.h / skinning_cs.hlsl, without pulling in the application PCH.
    struct InputVertex
    {
        float Position[3] = {1,0,0};
        float Normal[3] = {1,0,0};
        float TexCoord[2] = {0.25f,0.75f};
        float Diffuse[4] = {0.2f,0.4f,0.6f,1};
        int BoneIndices[4] = {0,1,-1,-1};
        float BoneWeights[4] = {0.5f,0.5f,0,0};
        int DeformType = 0;
        int Padding[3] = {};
        float SdefC[4] = {};
        float SdefR0[4] = {};
        float SdefR1[4] = {};
    };
    struct OutputVertex { float Position[3], Normal[3], TexCoord[2], Diffuse[4]; };
    struct BoneMatrix { float Value[4][4] = {}; };
    static_assert(sizeof(InputVertex) == 144 && sizeof(OutputVertex) == 48 && sizeof(BoneMatrix) == 64);

    // CPU upload follows animationpose.cpp: Assimp column-vector memory interpreted
    // as a column-major HLSL matrix gives a logical row-vector transform.
    BoneMatrix Bone(float angle, float tx, float scale = 1)
    {
        BoneMatrix m;
        const float c=std::cos(angle), s=std::sin(angle);
        m.Value[0][0]=scale*c; m.Value[0][1]=-scale*s; m.Value[0][3]=tx;
        m.Value[1][0]=scale*s; m.Value[1][1]=scale*c;
        m.Value[2][2]=scale; m.Value[3][3]=1;
        return m;
    }
    void Near(float actual, float expected)
    {
        if (!std::isfinite(actual) || std::abs(actual-expected)>0.0003f)
        {
            std::fprintf(stderr,"Expected %g, got %g\n",expected,actual);
            throw std::runtime_error("GPU skinning result mismatch");
        }
    }
}
int main(int argc, char** argv)
{
    try
    {
        const bool hardware=argc>1 && std::string(argv[1])=="--hardware";
        D3D12Test::Fixture fixture(!hardware);
        auto shader=D3D12Test::Compile(L"shader/hlsl/skinning_cs.hlsl");
        constexpr float angle=1.0471975512f; // +/-60 degrees
        std::array<BoneMatrix,6> bones={Bone(angle,2),Bone(-angle,2),Bone(0,0,2),Bone(0,0,-1),
                                        Bone(angle,-2),Bone(-angle,-2)};
        std::array<InputVertex,11> inputs{};
        inputs[1].BoneWeights[0]=2; inputs[1].BoneWeights[1]=2; // normalization
        inputs[2].BoneIndices[0]=-1; inputs[2].BoneIndices[1]=99; // no valid bones
        inputs[3].BoneIndices[0]=0; inputs[3].BoneIndices[1]=-1; // single influence
        inputs[4].BoneIndices[1]=2; // scaled bone: entire vertex must use LBS
        inputs[5].DeformType=3; // authored SDEF must retain its pivot behavior
        inputs[5].SdefC[0]=1;
        inputs[6].DeformType=4; // PMX QDEF
        inputs[7].BoneWeights[0]=std::numeric_limits<float>::infinity();
        inputs[7].BoneWeights[1]=0; // invalid weights keep the original vertex
        inputs[8].BoneIndices[1]=3; // reflected bone: LBS fallback
        inputs[9].BoneIndices[0]=inputs[9].BoneIndices[1]=1; // translated single rotation
        for (int i=0;i<4;++i)
        {
            inputs[10].BoneIndices[i]=(i<2 ? i : i+2);
            inputs[10].BoneWeights[i]=0.25f;
        }
        auto input=fixture.MakeStructuredBuffer(static_cast<UINT>(inputs.size()),sizeof(InputVertex));
        auto palette=fixture.MakeStructuredBuffer(static_cast<UINT>(bones.size()),sizeof(BoneMatrix));
        auto output=fixture.MakeStructuredBuffer(static_cast<UINT>(inputs.size()),sizeof(OutputVertex));
        fixture.Upload(input,inputs.data(),static_cast<UINT>(sizeof(inputs)));
        fixture.Upload(palette,bones.data(),static_cast<UINT>(sizeof(bones)));
        fixture.DispatchConstants(shader.Get(),{}, {&input,&palette},{&output},1,1);
        const auto bytes=fixture.Readback(output);
        std::array<OutputVertex,11> results{};
        std::memcpy(results.data(),bytes.data(),sizeof(results));
        const float h=std::sqrt(0.75f);
        const float expected[11][3]={{3,0,0},{3,0,0},{1,0,0},{2.5f,h,0},
                                   {2.25f,0.5f*h,0},{2.5f,0,0},{3,0,0},{1,0,0},
                                   {0.75f,0.5f*h,0},{2.5f,-h,0},{1,0,0}};
        for (size_t i=0;i<results.size();++i)
        {
            std::printf("GPU skinning case %zu\n",i);
            for (size_t j=0;j<3;++j) Near(results[i].Position[j],expected[i][j]);
            Near(results[i].TexCoord[0],0.25f); Near(results[i].TexCoord[1],0.75f);
            for (size_t j=0;j<4;++j) Near(results[i].Diffuse[j],inputs[i].Diffuse[j]);
            const auto& n=results[i].Normal;
            Near(n[0]*n[0]+n[1]*n[1]+n[2]*n[2],1);
        }
        Near(results[0].Normal[0],1); Near(results[0].Normal[1],0);
        Near(results[3].Normal[0],0.5f); Near(results[3].Normal[1],h);
        std::puts("DQS compute shader GPU tests: passed");
        return 0;
    }
    catch (const std::exception& error) { std::fprintf(stderr,"%s\n",error.what()); return 1; }
}
