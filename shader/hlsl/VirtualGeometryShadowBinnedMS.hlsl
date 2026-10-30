// Stage 1C shadow material binning: the shadow mesh shader with
// material-bin addressing (NativeBinOffset/NativeBinCount from the
// indirect command), used for both shadow bins - with the alpha
// pixel shader for the alpha-tested bin and with no pixel shader
// for the opaque bin.
#define VIRTUAL_GEOMETRY_SHADOW
#define VIRTUAL_GEOMETRY_BINNED
#include "VirtualGeometryMS.hlsl"
