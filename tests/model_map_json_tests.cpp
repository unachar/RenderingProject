#include "models/modelmapjson.h"
#include <cassert>
#include <iostream>
int main()
{
    ModelResourceMap source;
    source.Add(ModelResourceMap::Bones, "hips", 19);
    source.Add(ModelResourceMap::Materials, "unused", 6);
    source.Add(ModelResourceMap::Meshes, "Body", 1);
    auto snapshot = ModelMapJson::Write(source);
    ModelResourceMap derived;
    assert(ModelMapJson::Read(snapshot, derived));
    assert(derived.Categories() == source.Categories());
    snapshot["data"] = "oops";
    assert(!ModelMapJson::Read(snapshot, derived));
    assert(derived.Categories() == source.Categories());
    snapshot = ModelMapJson::Write(source);
    snapshot["fingerprint"] = "invalid";
    assert(!ModelMapJson::Read(snapshot, derived));
    std::cout << "Derived model map snapshot: PASS\n";
}
