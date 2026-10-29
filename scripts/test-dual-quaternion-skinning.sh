#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
validation_dir="$(mktemp -d)"
trap 'rm -rf "$validation_dir"' EXIT
"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror tests/dual_quaternion_skinning_tests.cpp -o "$validation_dir/dqs"
"$validation_dir/dqs"
