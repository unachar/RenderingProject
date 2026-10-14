#include "assets/thumbnailcache.h"
#include "rendering/editordescriptorpool.h"
#include "assets/thumbnailgeometry.h"
#include <cassert>
#include <iostream>

int main()
{
    ThumbnailCache cache;
    cache.BeginFrame(1);
    cache.Request(10, ThumbnailPriority::Visible);
    cache.Request(10, ThumbnailPriority::Visible);
    cache.Request(20, ThumbnailPriority::Selected);
    auto selected = cache.TakeNext();
    assert(selected && selected->Handle == 20);
    assert(cache.Complete(*selected, true));
    auto visible = cache.TakeNext();
    assert(visible && visible->Handle == 10);
    cache.MarkDirty(10);
    assert(!cache.Complete(*visible, true));
    auto newer = cache.TakeNext();
    assert(newer && newer->Revision > visible->Revision);
    assert(cache.Complete(*newer, true));
    assert(!cache.TakeNext());
    cache.Request(30);
    cache.Remove(30);
    assert(!cache.TakeNext());
    cache.Request(40);
    auto removed = cache.TakeNext();
    cache.Remove(40);
    cache.Request(40);
    assert(!cache.Complete(*removed, true));
    auto recreated = cache.TakeNext();
    assert(cache.Complete(*recreated, false));
    cache.Request(40);
    assert(!cache.TakeNext());
    cache.MarkDirty(40);
    assert(cache.TakeNext());
    auto wide = FitThumbnail(1920, 1080, 128);
    assert(wide.Width == 128 && wide.Height == 72);
    auto square = FitThumbnail(512, 512, 128);
    assert(square.Width == 128 && square.Height == 128);
    assert(FitThumbnail(0, 1080, 128).Width == 0);
    EditorDescriptorPool descriptors(2);
    auto first = descriptors.Allocate();
    auto second = descriptors.Allocate();
    assert(first && second && *first != *second && !descriptors.Allocate());
    assert(descriptors.Retire(*first, 10));
    assert(!descriptors.Retire(*first, 10));
    descriptors.Collect(9);
    assert(!descriptors.Allocate());
    descriptors.Collect(10);
    assert(descriptors.Allocate() == first);
    assert(!descriptors.Retire(99, 0));
    assert(descriptors.Retire(*first, 20) && descriptors.Retire(*second, 20));
    descriptors.Collect(20);
    for (uint64_t cycle = 0; cycle < 1000; ++cycle)
    {
        const auto left = descriptors.Allocate(), right = descriptors.Allocate();
        assert(left && right && descriptors.Available() == 0);
        assert(descriptors.Retire(*left, cycle + 22) && descriptors.Retire(*right, cycle + 22));
        descriptors.Collect(cycle + 21);
        assert(descriptors.Available() == 0);
        descriptors.Collect(cycle + 22);
        assert(descriptors.Available() == 2);
    }
    for (float radius : {0.00001f, 1.0f, 100000.0f})
    {
        const auto camera = FitThumbnailCamera(radius);
        assert(camera.Distance > radius && camera.Near > 0 && camera.Far > camera.Distance + radius);
    }
    assert(!FitThumbnailCamera(-1).Valid);
    assert(ThumbnailCacheFileName(0x1234) == "0000000000001234.dds");
    ThumbnailCache capacity;
    capacity.BeginFrame(1);
    capacity.Request(1, ThumbnailPriority::Selected);
    auto blocked = capacity.TakeNext();
    assert(capacity.Defer(*blocked, 2));
    capacity.Request(2);
    assert(capacity.TakeNext()->Handle == 2);
    assert(!capacity.TakeNext());
    capacity.BeginFrame(3);
    assert(capacity.TakeNext()->Handle == 1);
    std::cout << "Thumbnail cache tests passed\n";
}
