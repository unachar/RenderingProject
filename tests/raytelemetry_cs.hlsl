#define ENABLE_RAY_TELEMETRY 1
#include "../shader/hlsl/RayTelemetry.hlsli"

[numthreads(64, 1, 1)] void main(uint3 thread : SV_GroupThreadID) {
    if (thread.x == 0)
    {
        RayTelemetryCounters.Store(0, 0xffffffe0u);
        RayTelemetryCounters.Store(4, 0);
    }
    DeviceMemoryBarrierWithGroupSync();
    CountTelemetryRay(RAY_TELEMETRY_REFLECTION);
    CountTelemetryRay(RAY_TELEMETRY_GI);
    CountTelemetryRay(RAY_TELEMETRY_SHADOW);
    CountTelemetryRay(RAY_TELEMETRY_AO);
}
