#define SHADER_GPU_DRIVEN
// GPU 駆動のルートシグネチャはライト定数（b1）を持つ。POM セルフシャドウの主光源に使う。
#define POM_LIGHT_CBV
#include "DeferredPS.hlsl"

