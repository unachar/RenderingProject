#include "rendering/renderfeature.h"
#include <iostream>
#include <limits>
#include <stdexcept>

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

int main()
{
    using namespace RenderFeature;
    try
    {
        const Requirements requirements{true, true, true, true};
        const Budget budget{1024, 256, 2.0};
        const Usage usage{1024, 256, 2.0};
        Check(Evaluate(false, {}, requirements, budget, usage) == Status::Disabled, "disabled takes precedence");
        Check(Evaluate(true, {}, requirements, budget, usage) == Status::UnsupportedHardware, "missing hardware");
        const Capabilities caps{true, true, true, true};
        Check(Evaluate(true, caps, requirements, budget, usage) == Status::Ready, "exact budget fits");
        Check(Evaluate(true, caps, requirements, budget, {1025, 0, 0}) == Status::BudgetExceeded, "memory budget");
        Check(Evaluate(true, caps, requirements, budget, {0, 257, 0}) == Status::BudgetExceeded, "upload budget");
        Check(Evaluate(true, caps, requirements, budget, {0, 0, 2.1}) == Status::BudgetExceeded, "time budget");
        Check(Evaluate(true, caps, requirements, {}, usage) == Status::Ready, "zero limits are unlimited");
        Check(Evaluate(true, caps, requirements, budget, {0, 0, -1}) == Status::InvalidMeasurement, "negative timing");
        Check(Evaluate(true, caps, requirements, budget, {0, 0, std::numeric_limits<double>::quiet_NaN()}) ==
                  Status::InvalidMeasurement,
              "NaN timing");
        Check(Evaluate(true, caps, requirements, budget, {0, 0, std::numeric_limits<double>::infinity()}) ==
                  Status::InvalidMeasurement,
              "infinite timing");
        Check(Evaluate(true, {}, {}, {}, {}) == Status::Ready, "CPU features need no GPU capabilities");
        Check(Evaluate(true, {false, true, true, true}, requirements, {}, {}) == Status::UnsupportedHardware,
              "mesh capability is independently required");
        Check(Evaluate(true, {true, false, true, true}, requirements, {}, {}) == Status::UnsupportedHardware,
              "DXR capability is independently required");
        Check(Evaluate(true, {true, true, false, true}, requirements, {}, {}) == Status::UnsupportedHardware,
              "feedback capability is independently required");
        Check(Evaluate(true, {true, true, true, false}, requirements, {}, {}) == Status::UnsupportedHardware,
              "reserved resource capability is independently required");
        Check(Evaluate(true, caps, {}, {0, 0, -1}, {}) == Status::InvalidMeasurement, "negative time limit");
        Check(Evaluate(true, caps, {}, {0, 0, std::numeric_limits<double>::quiet_NaN()}, {}) ==
                  Status::InvalidMeasurement,
              "NaN time limit");
        const uint64_t maximum = std::numeric_limits<uint64_t>::max();
        Check(Evaluate(true, caps, {}, {maximum, maximum, 0}, {maximum, maximum, 0}) == Status::Ready,
              "64-bit byte limits compare without overflow");
        std::cout << "Render feature tests passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
