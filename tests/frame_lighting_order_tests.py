"""Guard scene-loading order against stale shadow-light entity references."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class FrameLightingOrderTests(unittest.TestCase):
    def test_scene_edits_finish_before_light_cache_and_atmosphere_update(self):
        source = (ROOT / "source/rendering/renderframe.cpp").read_text(encoding="utf-8")
        begin_draw = source.split("void RenderFrame::BeginDraw()", 1)[1].split(
            "void RenderFrame::EndDraw()", 1
        )[0]
        self.assertEqual(begin_draw.count("LightingResources::BeginFrame();"), 1)
        self.assertLess(
            begin_draw.index("ImGuiManager::Update();"),
            begin_draw.index("LightingResources::BeginFrame();"),
            "UI scene loading can destroy and reuse the entities in the light cache",
        )
        self.assertLess(
            begin_draw.index("LightingResources::BeginFrame();"),
            begin_draw.index("AtmosphereLut::Update("),
            "Atmosphere lighting must use the cache for the newly loaded scene",
        )


if __name__ == "__main__":
    unittest.main()
