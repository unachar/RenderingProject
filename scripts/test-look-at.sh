#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
validation_dir="$(mktemp -d)"
trap 'rm -rf "$validation_dir"' EXIT
json_root=External/RTXNS/external/donut/thirdparty/jsoncpp
flags=(-std=c++20 -Wall -Wextra -Werror -I source -I assimp -I "$json_root/include")
"${CXX:-g++}" "${flags[@]}" tests/look_at_tests.cpp -o "$validation_dir/solver"
"$validation_dir/solver"
"${CXX:-g++}" "${flags[@]}" tests/look_at_fixture_tests.cpp \
    "$json_root/src/lib_json/json_reader.cpp" "$json_root/src/lib_json/json_value.cpp" \
    "$json_root/src/lib_json/json_writer.cpp" -o "$validation_dir/fixture"
"$validation_dir/fixture"
