#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
validation_dir="$(mktemp -d)"
trap 'rm -rf "$validation_dir"' EXIT
json_root=External/RTXNS/external/donut/thirdparty/jsoncpp
flags=(-std=c++20 -Wall -Wextra -Werror -I source -I assimp -I "$json_root/include")
for part in reader value writer; do
    g++ "${flags[@]}" -c "$json_root/src/lib_json/json_$part.cpp" -o "$validation_dir/json_$part.o"
done
for test in model_resource_map model_map_import model_map_json vmd_retarget vmd_ik vmd_ik_fixture; do
    g++ "${flags[@]}" "tests/${test}_tests.cpp" "$validation_dir"/json_*.o -o "$validation_dir/$test"
    "$validation_dir/$test"
done

g++ "${flags[@]}" -I External/ImGUI tests/model_mesh_table_tests.cpp \
    External/ImGUI/imgui.cpp External/ImGUI/imgui_draw.cpp \
    External/ImGUI/imgui_tables.cpp External/ImGUI/imgui_widgets.cpp -o "$validation_dir/model_mesh_table"
"$validation_dir/model_mesh_table"
