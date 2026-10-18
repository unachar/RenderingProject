#include "rendering/temporalhistory.h"
#include "rendering/temporalhistorycontext.h"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
void Check(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void TestHistory()
{
    using namespace TemporalHistory;
    State previous;
    previous.Serial = 10;
    previous.CameraId = 4;
    previous.InputWidth = 600;
    previous.InputHeight = 300;
    previous.OutputWidth = 2000;
    previous.OutputHeight = 1000;
    State current = previous;
    current.Serial = 11;
    Check(Evaluate(previous, current, true).CanReuse, "continuous camera movement has no reset flag");
    Check(Evaluate(previous, current, false).Reasons == FirstFrame, "invalid previous history");
    const auto expectReason = [&](State value, uint32_t reason, const char* message) {
        const auto decision = Evaluate(previous, value, true);
        Check(!decision.CanReuse && (decision.Reasons & reason) != 0, message);
    };
    State changed = current;
    changed.Serial = 12;
    expectReason(changed, FrameGap, "skipped frame");
    changed = current;
    changed.CameraId = 5;
    expectReason(changed, CameraChanged, "camera switch");
    changed = current;
    changed.CameraCut = true;
    expectReason(changed, CameraChanged, "camera cut");
    changed = current;
    changed.InputWidth = 601;
    expectReason(changed, ResolutionChanged, "internal resolution change");
    changed = current;
    changed.OutputHeight = 1001;
    expectReason(changed, ResolutionChanged, "output resolution change");
    changed = current;
    changed.InputWidth = 0;
    expectReason(changed, ResolutionChanged, "zero extent is invalid");
    changed = current;
    ++changed.SceneGeneration;
    expectReason(changed, SceneChanged, "scene generation");
    changed = current;
    ++changed.LightingGeneration;
    expectReason(changed, LightingChanged, "lighting generation");
    changed = current;
    ++changed.LodGeneration;
    expectReason(changed, LodChanged, "LOD generation");
    changed = current;
    changed.Exposure = 2.0;
    expectReason(changed, ExposureChanged, "uncompensated exposure change");
    changed.Exposure = std::numeric_limits<double>::quiet_NaN();
    expectReason(changed, ExposureChanged, "nonfinite exposure");
    changed.Exposure = 0.0;
    expectReason(changed, ExposureChanged, "zero exposure");
    changed = current;
    ++changed.ConfigurationKey;
    expectReason(changed, ConfigurationChanged, "denoiser or signal format switch");
    previous.Serial = (std::numeric_limits<uint64_t>::max)();
    current.Serial = 0;
    Check(!Evaluate(previous, current, true).CanReuse, "serial overflow does not revive history");
    previous = changed;
    current = changed;
    ++current.Serial;
    Check(Evaluate(previous, current, true).CanReuse, "changed state converges next frame");
}

void TestSharedContext()
{
    using namespace TemporalHistory;
    Context::BeginFrame(2000, 1000);
    const auto previous = Context::Capture(4, 600, 300, 1);
    Context::BeginFrame(2000, 1000);
    auto current = Context::Capture(4, 600, 300, 1);
    Check(Evaluate(previous, current, true).CanReuse, "frame context is independent of profiler enablement");
    Context::NotifySceneChanged();
    current = Context::Capture(4, 600, 300, 1);
    Check((Evaluate(previous, current, true).Reasons & SceneChanged) != 0, "scene reset notification");
    const auto beforeCut = current;
    Context::NotifyCameraCut();
    current = Context::Capture(4, 600, 300, 1);
    Check((Evaluate(beforeCut, current, true).Reasons & CameraChanged) != 0, "same-camera cut generation");
    Check(Context::Capture(4, 600, 300, 1).CameraCutGeneration == current.CameraCutGeneration,
          "notification is not consumed by the first effect");
    Context::NotifyLightingChanged();
    Context::NotifyLodChanged();
    Context::BeginFrame(2001, 1000);
    current = Context::Capture(4, 600, 300, 1);
    const auto reasons = Evaluate(beforeCut, current, true).Reasons;
    Check((reasons & LightingChanged) && (reasons & LodChanged) && (reasons & ResolutionChanged),
          "independent generations and output extent reach all effects");
}
} // namespace

int main()
{
    try
    {
        TestHistory();
        TestSharedContext();
        std::cout << "temporalhistory tests: PASS\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
