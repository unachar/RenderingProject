# Stage lighting
Keep existing Physical/Decal/EmissionOnly routing separate from attenuation. Add a PhysicalBased checkbox, default true for old scenes. Off means unit brightness inside the influence volume, with a fixed one-metre smooth edge (clamped to range), independent of source area. Preserve angular spot masks and shadow settings. Physical mode preserves legacy appearance; it does not claim new photometric calibration.

Existing point, spot, volume and rect lights remain. Add Capsule (enum 5), using the nearest point on a centred segment along Direction, with length and radius. Capsule defaults to stage attenuation, no shadows, and uses the existing tiled path (not MegaLights stochastic shape sampling). Conservative culling includes half-length and radius. Rect remains enum 4 and only that type may use LTC/texture sampling.

Approaches: a new render route would duplicate routing and break Forward; a separate flag shares all lighting paths and is selected here. Pack the flag into Extra.z's sign, encoding stage density as -(density+1), preserving buffer layouts. Shared HLSL decodes it; positive legacy values remain unchanged. All CPU packers share one helper. Fixed edge width avoids additional GPU layout changes.

Reference: https://speakerdeck.com/cyberagentdevelopers/xue-yuan-aidorumasutano-aidoruwoyorihui-kaseru-raiteingushou-fa (decal point/area/capsule). This is an approximation, not the game's implementation or render-texture-driven decal system.
