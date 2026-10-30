#include "../source/models/deferredresourcelifetime.h"
#include <cassert>
#include <iostream>

struct Resource
{
    int* Destroyed;

    ~Resource()
    {
        ++*Destroyed;
    }
};

int main()
{
    int destroyed = 0;
    uint64_t pending = 7;
    DeferredResourceLifetime<Resource> lifetime([&] {
        return pending;
    });
    auto scene = lifetime.Own(std::make_unique<Resource>(&destroyed));
    auto undo = scene;
    std::weak_ptr<Resource> registry = scene;
    scene.reset();
    lifetime.Collect(100);
    assert(destroyed == 0 && !registry.expired());
    undo.reset();
    assert(registry.expired() && destroyed == 0);
    lifetime.Collect(6);
    assert(destroyed == 0);
    lifetime.Collect(7);
    assert(destroyed == 1);
    auto last = lifetime.Own(std::make_unique<Resource>(&destroyed));
    lifetime.Shutdown();
    last.reset();
    assert(destroyed == 2);
    std::shared_ptr<Resource> snapshot;
    {
        DeferredResourceLifetime<Resource> registryLifetime([&] {
            return pending;
        });
        snapshot = registryLifetime.Own(std::make_unique<Resource>(&destroyed));
        registryLifetime.Shutdown();
    }
    snapshot.reset();
    assert(destroyed == 3);
    std::cout << "Animation resource ownership and GPU retirement tests passed\n";
}
