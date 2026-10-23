#include "editor/prefabformat.h"
#include <cassert>
#include <iostream>

int main()
{
    Json::Value prefab;
    prefab["format"] = "DX12Prefab";
    prefab["version"] = 1;
    prefab["root"] = 10;
    Json::Value parent;
    parent["id"] = 10;
    parent["transform"]["parent"] = -1;
    Json::Value child;
    child["id"] = 20;
    child["transform"]["parent"] = 10;
    prefab["entities"].append(child); // Root need not be first.
    prefab["entities"].append(parent);
    std::string error;
    assert(PrefabFormat::Validate(prefab, error));
    prefab["entities"][0]["id"] = 10;
    assert(!PrefabFormat::Validate(prefab, error));
    prefab["entities"][0]["id"] = 20;
    prefab["entities"][0]["transform"]["parent"] = 30;
    assert(!PrefabFormat::Validate(prefab, error));
    prefab["entities"][0]["transform"]["parent"] = 20;
    assert(!PrefabFormat::Validate(prefab, error));
    prefab["entities"][0]["transform"]["parent"] = 10;
    prefab["root"] = 99;
    assert(!PrefabFormat::Validate(prefab, error));
    prefab["root"] = 10;
    prefab["version"] = 99;
    assert(!PrefabFormat::Validate(prefab, error));
    std::cout << "Prefab format tests: PASS\n";
}
