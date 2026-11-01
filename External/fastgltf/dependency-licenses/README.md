# fastgltf parser dependencies

The upstream fastgltf CMake dependency file pins simdjson 5.0.4 and simdutf 9.2.1, verifying the SHA256 of each downloaded amalgamated header and source. The build script reuses those downloads between configurations, with upstream checking their hashes again.

License files were obtained from the same tags:

- https://raw.githubusercontent.com/simdjson/simdjson/v5.0.4/LICENSE
- https://raw.githubusercontent.com/simdutf/simdutf/v9.2.1/LICENSE-APACHE
- https://raw.githubusercontent.com/simdutf/simdutf/v9.2.1/LICENSE-MIT

fastgltf's own license is `../LICENSE.md`. The product build copies these notices alongside the executable when fastgltf is enabled.
