"""Check the imported mesh texture mapping used by the thumbnail renderer."""

from pathlib import Path
import re
import unittest
import sys


ROOT = Path(__file__).resolve().parents[1]


class ThumbnailMaterialContracts(unittest.TestCase):
    def test_preview_pipeline_is_prepared_before_ui_thread_generation(self):
        renderer = (ROOT / "source/rendering/thumbnailrenderer.cpp").read_text(encoding="utf-8")
        generate = renderer.split("ThumbnailRenderer::Generate(", 1)[1].split(
            "bool ThumbnailRenderer::BeginReadback", 1
        )[0]
        self.assertNotIn("->Initialize(", generate,
                         "GPU preview initialization must run in asynchronous preparation")
        manager = (ROOT / "source/editor/assetthumbnailmanager.cpp").read_text(encoding="utf-8")
        self.assertRegex(manager, r"std::async\([\s\S]+ThumbnailRenderer::PreparePreview\(")

    def test_every_material_slot_has_an_import_type(self):
        material = (ROOT / "source/assets/materialasset.h").read_text(encoding="utf-8")
        slots = re.search(r"enum class MaterialTextureSlot[^\{]*\{([^}]+)", material)[1]
        slot_names = [name.strip() for name in slots.split(",") if name.strip() != "Count"]
        renderer = (ROOT / "source/rendering/thumbnailrenderer.cpp").read_text(encoding="utf-8")
        mapping = re.search(r"const aiTextureType types\[\] = \{([^}]+)", renderer)[1]
        import_types = [name.strip() for name in mapping.split(",")]
        self.assertEqual(
            len(import_types), len(slot_names),
            "ReadMeshMaterial indexes the import table once for every material slot",
        )
        self.assertEqual(import_types, [
            "aiTextureType_BASE_COLOR", "aiTextureType_NORMALS", "aiTextureType_UNKNOWN",
            "aiTextureType_AMBIENT_OCCLUSION", "aiTextureType_EMISSIVE", "aiTextureType_HEIGHT",
        ])


if __name__ == "__main__":
    if "--emit-dispatch" in sys.argv:
        manager = (ROOT / "source/editor/assetthumbnailmanager.cpp").read_text(encoding="utf-8")
        start = manager.index("            if (prepared->Error.empty() && !prepared->Cached")
        end = manager.index("            ThumbnailPrepared input =", start)
        output = ROOT / "build/thumbnail-tests/preview-dispatch.inc"
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(manager[start:end], encoding="utf-8")
        sys.exit(0)
    unittest.main()
