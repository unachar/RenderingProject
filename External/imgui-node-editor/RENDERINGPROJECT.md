# RenderingProject dependency pin

- Upstream: https://github.com/thedmd/imgui-node-editor
- Commit: `1e0e494f1a86885eb332ca518543108ccd88d13e`
- Header version: 0.9.4 (904)
- Retrieved: 2026-11-02
- License: MIT; see the unmodified `LICENSE` file and source notices.
- Local modifications to the downloaded root sources: none.
- Built sources: `crude_json.cpp`, `imgui_canvas.cpp`, `imgui_node_editor.cpp`,
  `imgui_node_editor_api.cpp`, with the project's existing Dear ImGui.
- Do not build the ImGui copy below `external/imgui` or the example backends.

The root library sources were missing from the pre-existing vendor directory.
Only the required root files and license were retrieved at this revision;
pre-existing example/documentation files have not been replaced and should not
be used as a version pin.

## Selection

Compared with https://github.com/Nelarius/imnodes (official README):

| Criterion | imgui-node-editor | imnodes |
| --- | --- | --- |
| Existing ImGui compatibility | Pinned upstream README reports testing through 1.93 WIP; local product uses 1.92.8 WIP | ImGui dependency; exact local compatibility not compiled |
| Custom content and node images | Arbitrary ImGui widgets/draw content inside nodes | Supports ImGui widgets inside nodes |
| Interaction | Selection, grouping, movement, pan/zoom, context queries, shortcut hooks | Simpler immediate-mode nodes/pins/links, application owns state |
| Build | Four source files and root headers; no new backend | Three core source/header files |
| Maintenance | Pin upstream compatibility changes; validate against bundled ImGui | Smaller surface, still needs ImGui compatibility review on upgrades |
| Draw cost | No comparative GPU/CPU measurement performed | No comparative GPU/CPU measurement performed |

imgui-node-editor was selected for the requested UE-style interaction and groups,
while graph model, type rules, serialization and undo remain application-owned.
Both are MIT licensed. imnodes is not added as a duplicate dependency.

Sources consulted:
https://github.com/thedmd/imgui-node-editor/blob/1e0e494f1a86885eb332ca518543108ccd88d13e/docs/README.md
https://github.com/Nelarius/imnodes/blob/master/README.md
