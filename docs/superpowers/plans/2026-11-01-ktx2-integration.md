# KTX2 image integration

Goal: Replace the unconnected KTX/Basis placeholder with actual SDK image import, native BC KTX2 encoding and measurable DDS comparisons.

Design: Return an owning DirectXTex ScratchImage into the existing texture upload path. Preserve 2D mip/layer/cube ordering, DXGI/sRGB format and native BC bytes. Use KTX's integrated Basis transcoder for color KTX2, avoiding a second copy of the same transcoder. Reject unsupported dimensions, orientation, swizzle and data-map Basis channel contracts explicitly. Enforce encoded/decoded size bounds before loading pixel data. Keep output unchanged on failure. Do not silently substitute BC7 for unknown VkFormats.

The existing ktxbasis files have no callers and contain noncompiling placeholder API calls. Replace their unused public draft API with tested Load/Encode functions and no exposed SDK ownership pointers. Native KTX2 BC support and color Basis KTX2 are this implementation unit; standalone .basis and general channel swizzles remain separate work.

- [ ] Add failing synthetic tests for native mip/array/cube bytes, formats, sRGB, invalid inputs and Basis color transcoding.
- [ ] Implement bounded owning SDK adapter; compare KTX2+zstd with existing DDS storage using actual cached textures.
- [ ] Connect file and memory imports to TextureManager, preserve downstream upload and compression behavior.
- [ ] Add SDK runtime/license placement, build, review and validate integration. Record size/time/quality boundaries and default decision.
