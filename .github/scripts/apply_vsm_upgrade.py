from pathlib import Path
import re
import sys


def read(path: str) -> str:
    return Path(path).read_text(encoding="utf-8-sig")


def write(path: str, text: str) -> None:
    Path(path).write_text(text, encoding="utf-8", newline="\n")


def replace_once(path: str, old: str, new: str) -> None:
    text = read(path)
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected one exact match, found {count}: {old[:80]!r}")
    write(path, text.replace(old, new, 1))


def replace_regex(path: str, pattern: str, replacement: str) -> None:
    text = read(path)
    updated, count = re.subn(pattern, replacement, text, count=1, flags=re.S)
    if count != 1:
        raise RuntimeError(f"{path}: regex expected one match, found {count}: {pattern[:100]!r}")
    write(path, updated)


# -----------------------------------------------------------------------------
# Renderer limits and constant-buffer layout.
# -----------------------------------------------------------------------------
replace_once(
    "source/rendererstate.h",
    "\tstatic constexpr UINT g_kVIRTUAL_SHADOW_RESIDENT_PAGES_PER_DIMENSION = 2;",
    "\t// Maximum active page window for each directional clipmap level.\n"
    "\t// The physical atlas remains 16x16 pages; only requested pages are rendered.\n"
    "\tstatic constexpr UINT g_kVIRTUAL_SHADOW_RESIDENT_PAGES_PER_DIMENSION = 8;"
)
replace_once(
    "source/rendererstate.h",
    "\t\t10 + g_kMAX_SHADER_LIGHTS * 9 + g_kMAX_VIRTUAL_SHADOW_LEVELS * 5 + 3 +",
    "\t\t10 + g_kMAX_SHADER_LIGHTS * 9 + g_kMAX_VIRTUAL_SHADOW_LEVELS * 6 + 16 + 3 +"
)

# Use a slightly wider first clipmap while retaining centimetre-scale nearby texels.
replace_once(
    "source/renderersettings.h",
    "\t\ts_VirtualFirstLevelRadius = 12.0f;",
    "\t\ts_VirtualFirstLevelRadius = 16.0f;"
)
replace_once(
    "source/renderersettings.h",
    "\t\ts_ContactShadowLength = 0.8f;\n\t\ts_ContactShadowSteps = 8;",
    "\t\ts_ContactShadowLength = 0.65f;\n\t\ts_ContactShadowSteps = 16;"
)
replace_once(
    "source/renderersettings.h",
    "\tinline static float s_VirtualFirstLevelRadius = 12.0f;",
    "\tinline static float s_VirtualFirstLevelRadius = 16.0f;"
)
replace_once(
    "source/renderersettings.h",
    "\tinline static float s_ContactShadowLength = 0.8f;\n\tinline static int s_ContactShadowSteps = 8;",
    "\tinline static float s_ContactShadowLength = 0.65f;\n\tinline static int s_ContactShadowSteps = 16;"
)

# -----------------------------------------------------------------------------
# CPU-side paged clipmap cache.
# -----------------------------------------------------------------------------
replace_once(
    "source/rendererresource.cpp",
    "#include <limits>",
    "#include <limits>\n#include <climits>"
)
replace_once(
    "source/rendererresource.cpp",
    "\t\tbool ClearLayer = true;\n\t\tbool VirtualPage = false;",
    "\t\tbool ClearLayer = true;\n\t\tbool VirtualPage = false;\n\t\tbool NeedsRender = true;"
)
replace_once(
    "source/rendererresource.cpp",
    "\tXMFLOAT4 g_VirtualShadowParams[RendererState::g_kMAX_VIRTUAL_SHADOW_LEVELS]{};\n"
    "\tUINT g_VirtualShadowLevelCount = 0;",
    "\tXMFLOAT4 g_VirtualShadowParams[RendererState::g_kMAX_VIRTUAL_SHADOW_LEVELS]{};\n"
    "\tXMFLOAT4 g_VirtualShadowPageOrigins[RendererState::g_kMAX_VIRTUAL_SHADOW_LEVELS]{};\n"
    "\tuint32_t g_VirtualShadowResidencyRows[RendererState::g_kMAX_VIRTUAL_SHADOW_LEVELS]\n"
    "\t\t[RendererState::g_kVIRTUAL_SHADOW_PAGES_PER_DIMENSION]{};\n"
    "\tstruct VirtualShadowPhysicalPageCache\n"
    "\t{\n"
    "\t\tint GlobalPageX = INT_MIN;\n"
    "\t\tint GlobalPageY = INT_MIN;\n"
    "\t\tuint64_t ContentKey = 0;\n"
    "\t\tbool Valid = false;\n"
    "\t};\n"
    "\tVirtualShadowPhysicalPageCache g_VirtualShadowPageCache\n"
    "\t\t[RendererState::g_kMAX_VIRTUAL_SHADOW_LEVELS]\n"
    "\t\t[RendererState::g_kVIRTUAL_SHADOW_PAGES_PER_DIMENSION]\n"
    "\t\t[RendererState::g_kVIRTUAL_SHADOW_PAGES_PER_DIMENSION]{};\n"
    "\tUINT g_VirtualShadowLevelCount = 0;"
)
replace_once(
    "source/rendererresource.cpp",
    "\t\tXMFLOAT4 VirtualShadowParams[RendererState::g_kMAX_VIRTUAL_SHADOW_LEVELS]{};\n"
    "\t\tXMFLOAT4 VirtualShadowGlobal = { 0.0f, 0.0f, 1.0f, 0.0f };",
    "\t\tXMFLOAT4 VirtualShadowParams[RendererState::g_kMAX_VIRTUAL_SHADOW_LEVELS]{};\n"
    "\t\tXMFLOAT4 VirtualShadowPageOrigins[RendererState::g_kMAX_VIRTUAL_SHADOW_LEVELS]{};\n"
    "\t\tXMUINT4 VirtualShadowResidency[RendererState::g_kMAX_VIRTUAL_SHADOW_LEVELS * 4]{};\n"
    "\t\tXMFLOAT4 VirtualShadowGlobal = { 0.0f, 0.0f, 1.0f, 0.75f };"
)
replace_once(
    "source/rendererresource.cpp",
    "\t\tg_VirtualShadowLevelCount = 0;\n\t\tg_DirectionalShadowMode = 0.0f;",
    "\t\tg_VirtualShadowLevelCount = 0;\n"
    "\t\tmemset(g_VirtualShadowResidencyRows, 0, sizeof(g_VirtualShadowResidencyRows));\n"
    "\t\tg_DirectionalShadowMode = 0.0f;"
)
replace_once(
    "source/rendererresource.cpp",
    "\t\t\tg_VirtualShadowViewProjections[i] = XMMatrixIdentity();\n"
    "\t\t\tg_VirtualShadowParams[i] = XMFLOAT4(-1.0f, 1.0f / RendererState::g_kSHADOW_MAP_SIZE, 0.0f, 0.0f);",
    "\t\t\tg_VirtualShadowViewProjections[i] = XMMatrixIdentity();\n"
    "\t\t\tg_VirtualShadowParams[i] = XMFLOAT4(-1.0f, 1.0f / RendererState::g_kSHADOW_MAP_SIZE, 0.0f, 0.0f);\n"
    "\t\t\tg_VirtualShadowPageOrigins[i] = XMFLOAT4(0.0f, 0.0f,\n"
    "\t\t\t\tstatic_cast<float>(RendererState::g_kVIRTUAL_SHADOW_PAGES_PER_DIMENSION), 0.0f);"
)

build_virtual_function = r'''\tvoid BuildVirtualDirectionalShadowViewProjection(const RuntimeLightState& runtimeLight, UINT level, XMMATRIX& outLightViewProjection, XMFLOAT4& outShadowMapParams)
\t{
\t\tXMFLOAT3 direction = runtimeLight.Component.Direction;
\t\tXMVECTOR lightDirection = XMVectorSet(direction.x, direction.y, direction.z, 0.0f);
\t\tif (XMVectorGetX(XMVector3LengthSq(lightDirection)) < 0.000001f)
\t\t{
\t\t\tlightDirection = XMVectorSet(0.25f, 1.0f, -0.25f, 0.0f);
\t\t}
\t\tlightDirection = XMVector3Normalize(lightDirection);

\t\tconst bool conventionalCascade = level >= 4;
\t\tconst UINT actualLevel = conventionalCascade ? level - 4 : level;
\t\tconst UINT cascadeCount = static_cast<UINT>(RendererSettings::GetShadowCascadeCount());
\t\tfloat radius = conventionalCascade
\t\t\t? RendererSettings::GetShadowDistance() /
\t\t\t\tstatic_cast<float>(1u << max(0, static_cast<int>(cascadeCount - 1 - actualLevel)))
\t\t\t: RendererSettings::GetVirtualFirstLevelRadius() * static_cast<float>(1u << actualLevel);

\t\t// The last clipmap deliberately grows faster. With an 8/16 resident window
\t\t// this covers the configured long shadow distance without sacrificing the
\t\t// centimetre-scale texels of levels zero through two.
\t\tif (!conventionalCascade && actualLevel == 3)
\t\t{
\t\t\tradius *= 1.5f;
\t\t}

\t\tXMVECTOR viewForward = XMVectorNegate(lightDirection);
\t\tXMVECTOR upHint = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
\t\tif (fabsf(XMVectorGetX(XMVector3Dot(viewForward, upHint))) > 0.96f)
\t\t{
\t\t\tupHint = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
\t\t}
\t\tconst XMVECTOR right = XMVector3Normalize(XMVector3Cross(upHint, viewForward));
\t\tconst XMVECTOR lightUp = XMVector3Normalize(XMVector3Cross(viewForward, right));
\t\tconst XMVECTOR camera = XMLoadFloat3(&GetActiveCameraPosition());

\t\tfloat planeX = XMVectorGetX(XMVector3Dot(camera, right));
\t\tfloat planeY = XMVectorGetX(XMVector3Dot(camera, lightUp));
\t\tconst float planeZ = XMVectorGetX(XMVector3Dot(camera, lightDirection));
\t\tconst float pageGrid = static_cast<float>(RendererState::g_kVIRTUAL_SHADOW_PAGES_PER_DIMENSION);
\t\tconst float pageWorldSize = (radius * 2.0f) / pageGrid;

\t\tif (RendererSettings::GetStabilizeVirtualClipmaps())
\t\t{
\t\t\tconst float snapSize = conventionalCascade
\t\t\t\t? (radius * 2.0f) / static_cast<float>(RendererState::g_kSHADOW_MAP_SIZE)
\t\t\t\t: pageWorldSize;
\t\t\tplaneX = roundf(planeX / snapSize) * snapSize;
\t\t\tplaneY = roundf(planeY / snapSize) * snapSize;
\t\t}

\t\tXMVECTOR target = XMVectorAdd(
\t\t\tXMVectorAdd(XMVectorScale(right, planeX), XMVectorScale(lightUp, planeY)),
\t\t\tXMVectorScale(lightDirection, planeZ));
\t\ttarget = XMVectorSetW(target, 1.0f);

\t\tif (!conventionalCascade && actualLevel < RendererState::g_kMAX_VIRTUAL_SHADOW_LEVELS)
\t\t{
\t\t\tconst int centerPageX = static_cast<int>(llround(planeX / pageWorldSize));
\t\t\t// Texture V grows in the opposite direction to the light-view Y axis.
\t\t\tconst int centerPageY = static_cast<int>(llround(-planeY / pageWorldSize));
\t\t\tconst int halfGrid = static_cast<int>(RendererState::g_kVIRTUAL_SHADOW_PAGES_PER_DIMENSION / 2);
\t\t\tg_VirtualShadowPageOrigins[actualLevel] = XMFLOAT4(
\t\t\t\tstatic_cast<float>(centerPageX - halfGrid),
\t\t\t\tstatic_cast<float>(centerPageY - halfGrid),
\t\t\t\tpageGrid,
\t\t\t\tpageWorldSize);
\t\t}

\t\tconst XMVECTOR lightPosition = XMVectorAdd(
\t\t\ttarget,
\t\t\tXMVectorScale(lightDirection, max(120.0f, radius * 3.0f)));
\t\tconst XMMATRIX view = XMMatrixLookAtLH(lightPosition, target, lightUp);
\t\tconst XMMATRIX projection = XMMatrixOrthographicLH(
\t\t\tradius * 2.0f,
\t\t\tradius * 2.0f,
\t\t\t0.1f,
\t\t\tmax(300.0f, radius * 8.0f));
\t\toutLightViewProjection = XMMatrixTranspose(view * projection);
\t\toutShadowMapParams = XMFLOAT4(
\t\t\t1.0f / static_cast<float>(RendererState::g_kSHADOW_MAP_SIZE),
\t\t\tRendererSettings::GetShadowDepthBias(),
\t\t\tRendererSettings::GetShadowNormalBias(),
\t\t\t1.0f);
\t}
'''
replace_regex(
    "source/rendererresource.cpp",
    r"\tvoid BuildVirtualDirectionalShadowViewProjection\(.*?\n\t\}\n\n\tint GetShapeSideCount",
    build_virtual_function + "\n\tint GetShapeSideCount"
)

paged_pass_generation = r'''
\t\t// Paged clipmap VSM. Virtual pages are cropped from the clipmap projection
\t\t// and stored in ring-mapped physical slots. A per-page content key means a
\t\t// moving camera normally redraws only the entering row/column of pages.
\t\tuint64_t basePageKey = 1469598103934665603ull;
\t\tauto hashPageBytes = [](uint64_t& hash, const void* data, size_t size)
\t\t{
\t\t\tconst auto* bytes = static_cast<const uint8_t*>(data);
\t\t\tfor (size_t byteIndex = 0; byteIndex < size; ++byteIndex)
\t\t\t{
\t\t\t\thash ^= bytes[byteIndex];
\t\t\t\thash *= 1099511628211ull;
\t\t\t}
\t\t};
\t\tconst float virtualSettings[] =
\t\t{
\t\t\tRendererSettings::GetVirtualFirstLevelRadius(),
\t\t\tRendererSettings::GetShadowDepthBias(),
\t\t\tRendererSettings::GetShadowNormalBias(),
\t\t\tstatic_cast<float>(RendererSettings::GetShadowFilterRadius())
\t\t};
\t\thashPageBytes(basePageKey, virtualSettings, sizeof(virtualSettings));
\t\tif (g_CachedDirectionalLight.HasLight)
\t\t{
\t\t\thashPageBytes(basePageKey, &g_CachedDirectionalLight.Component.Direction,
\t\t\t\tsizeof(g_CachedDirectionalLight.Component.Direction));
\t\t}

\t\tg_VirtualShadowCacheHit = g_DirectionalShadowMode == 1.0f;
\t\tfor (UINT layer = 0;
\t\t\tlayer < g_ShadowLightCount && g_ShadowRenderPassCount < RendererState::g_kMAX_SHADOW_PASSES;
\t\t\t++layer)
\t\t{
\t\t\tconst bool virtualLayer =
\t\t\t\tg_DirectionalShadowMode == 1.0f &&
\t\t\t\tg_ShadowLightEntities[layer] == g_VirtualShadowLightEntity;
\t\t\tif (!virtualLayer)
\t\t\t{
\t\t\t\tShadowRenderPass& pass = g_ShadowRenderPasses[g_ShadowRenderPassCount++];
\t\t\t\tpass.ViewProjection = g_ShadowLightViewProjections[layer];
\t\t\t\tpass.Params = g_ShadowMapParams[layer];
\t\t\t\tpass.Layer = layer;
\t\t\t\tpass.X = 0;
\t\t\t\tpass.Y = 0;
\t\t\t\tpass.Size = RendererState::g_kSHADOW_MAP_SIZE;
\t\t\t\tpass.ClearLayer = true;
\t\t\t\tpass.VirtualPage = false;
\t\t\t\tpass.NeedsRender = true;
\t\t\t\tcontinue;
\t\t\t}

\t\t\tUINT virtualLevel = 0;
\t\t\tfor (UINT level = 0; level < g_VirtualShadowLevelCount; ++level)
\t\t\t{
\t\t\t\tif (static_cast<UINT>(max(g_VirtualShadowParams[level].x, 0.0f)) == layer)
\t\t\t\t{
\t\t\t\t\tvirtualLevel = level;
\t\t\t\t\tbreak;
\t\t\t\t}
\t\t\t}

\t\t\tconst UINT pageGrid = RendererState::g_kVIRTUAL_SHADOW_PAGES_PER_DIMENSION;
\t\t\tconst UINT residentGrid = min(
\t\t\t\tRendererState::g_kVIRTUAL_SHADOW_RESIDENT_PAGES_PER_DIMENSION,
\t\t\t\tpageGrid);
\t\t\tconst UINT firstPage = (pageGrid - residentGrid) / 2;
\t\t\tconst int pageOriginX = static_cast<int>(roundf(g_VirtualShadowPageOrigins[virtualLevel].x));
\t\t\tconst int pageOriginY = static_cast<int>(roundf(g_VirtualShadowPageOrigins[virtualLevel].y));
\t\t\tconst float pageWorldSize = max(g_VirtualShadowPageOrigins[virtualLevel].w, 0.001f);
\t\t\tconst XMMATRIX fullViewProjection = XMMatrixTranspose(g_ShadowLightViewProjections[layer]);
\t\t\tauto positiveModulo = [](int value, int modulus)
\t\t\t{
\t\t\t\tconst int result = value % modulus;
\t\t\t\treturn result < 0 ? result + modulus : result;
\t\t\t};

\t\t\tfor (UINT localPageY = 0; localPageY < residentGrid; ++localPageY)
\t\t\t{
\t\t\t\tfor (UINT localPageX = 0; localPageX < residentGrid; ++localPageX)
\t\t\t\t{
\t\t\t\t\tif (g_ShadowRenderPassCount >= RendererState::g_kMAX_SHADOW_PASSES) break;
\t\t\t\t\tconst UINT pageX = firstPage + localPageX;
\t\t\t\t\tconst UINT pageY = firstPage + localPageY;
\t\t\t\t\tg_VirtualShadowResidencyRows[virtualLevel][pageY] |= (1u << pageX);

\t\t\t\t\tconst int globalPageX = pageOriginX + static_cast<int>(pageX);
\t\t\t\t\tconst int globalPageY = pageOriginY + static_cast<int>(pageY);
\t\t\t\t\tconst UINT physicalPageX = static_cast<UINT>(positiveModulo(globalPageX, static_cast<int>(pageGrid)));
\t\t\t\t\tconst UINT physicalPageY = static_cast<UINT>(positiveModulo(globalPageY, static_cast<int>(pageGrid)));

\t\t\t\t\tuint64_t pageContentKey = basePageKey;
\t\t\t\t\thashPageBytes(pageContentKey, &virtualLevel, sizeof(virtualLevel));
\t\t\t\t\thashPageBytes(pageContentKey, &globalPageX, sizeof(globalPageX));
\t\t\t\t\thashPageBytes(pageContentKey, &globalPageY, sizeof(globalPageY));

\t\t\t\t\tfor (EntityID entity : World::GetView<TransformComponent>())
\t\t\t\t\t{
\t\t\t\t\t\tif (!IsShadowBoundsEntity(entity)) continue;
\t\t\t\t\t\tconst auto& transform = ComponentManager::GetComponentUnchecked<TransformComponent>(entity);
\t\t\t\t\t\tXMFLOAT3 center = transform.Position;
\t\t\t\t\t\tfloat objectRadius = max(max(fabsf(transform.Scale.x), fabsf(transform.Scale.y)), fabsf(transform.Scale.z)) * 1.5f;
\t\t\t\t\t\tif (ComponentManager::HasComponent<AABBComponent>(entity))
\t\t\t\t\t\t{
\t\t\t\t\t\t\tconst auto& aabb = ComponentManager::GetComponentUnchecked<AABBComponent>(entity);
\t\t\t\t\t\t\tcenter.x += aabb.Center.x * transform.Scale.x;
\t\t\t\t\t\t\tcenter.y += aabb.Center.y * transform.Scale.y;
\t\t\t\t\t\t\tcenter.z += aabb.Center.z * transform.Scale.z;
\t\t\t\t\t\t\tconst float ex = fabsf(aabb.Extents.x * transform.Scale.x);
\t\t\t\t\t\t\tconst float ey = fabsf(aabb.Extents.y * transform.Scale.y);
\t\t\t\t\t\t\tconst float ez = fabsf(aabb.Extents.z * transform.Scale.z);
\t\t\t\t\t\t\tobjectRadius = sqrtf(ex * ex + ey * ey + ez * ez);
\t\t\t\t\t\t}

\t\t\t\t\t\tXMVECTOR clip = XMVector4Transform(
\t\t\t\t\t\t\tXMVectorSet(center.x, center.y, center.z, 1.0f),
\t\t\t\t\t\t\tfullViewProjection);
\t\t\t\t\t\tconst float clipW = XMVectorGetW(clip);
\t\t\t\t\t\tif (clipW <= 0.000001f) continue;
\t\t\t\t\t\tconst float ndcX = XMVectorGetX(clip) / clipW;
\t\t\t\t\t\tconst float ndcY = XMVectorGetY(clip) / clipW;
\t\t\t\t\t\tconst float pagePositionX = (ndcX * 0.5f + 0.5f) * static_cast<float>(pageGrid);
\t\t\t\t\t\tconst float pagePositionY = (-ndcY * 0.5f + 0.5f) * static_cast<float>(pageGrid);
\t\t\t\t\t\tconst int pageRadius = max(1, static_cast<int>(ceilf(objectRadius / pageWorldSize)) + 1);
\t\t\t\t\t\tif (fabsf(pagePositionX - (static_cast<float>(pageX) + 0.5f)) > static_cast<float>(pageRadius) ||
\t\t\t\t\t\t\tfabsf(pagePositionY - (static_cast<float>(pageY) + 0.5f)) > static_cast<float>(pageRadius))
\t\t\t\t\t\t{
\t\t\t\t\t\t\tcontinue;
\t\t\t\t\t\t}
\t\t\t\t\t\thashPageBytes(pageContentKey, &entity, sizeof(entity));
\t\t\t\t\t\thashPageBytes(pageContentKey, &transform.Position, sizeof(transform.Position));
\t\t\t\t\t\thashPageBytes(pageContentKey, &transform.Rotation, sizeof(transform.Rotation));
\t\t\t\t\t\thashPageBytes(pageContentKey, &transform.Scale, sizeof(transform.Scale));
\t\t\t\t\t}

\t\t\t\t\tauto& cacheEntry = g_VirtualShadowPageCache[virtualLevel][physicalPageY][physicalPageX];
\t\t\t\t\tconst bool cacheEnabled = RendererSettings::GetCacheVirtualShadowPages();
\t\t\t\t\tconst bool needsRender =
\t\t\t\t\t\t!cacheEnabled ||
\t\t\t\t\t\t!cacheEntry.Valid ||
\t\t\t\t\t\tcacheEntry.GlobalPageX != globalPageX ||
\t\t\t\t\t\tcacheEntry.GlobalPageY != globalPageY ||
\t\t\t\t\t\tcacheEntry.ContentKey != pageContentKey;

\t\t\t\t\tconst float centerX = -1.0f +
\t\t\t\t\t\t(2.0f * static_cast<float>(pageX) + 1.0f) / static_cast<float>(pageGrid);
\t\t\t\t\tconst float centerY = 1.0f -
\t\t\t\t\t\t(2.0f * static_cast<float>(pageY) + 1.0f) / static_cast<float>(pageGrid);
\t\t\t\t\tconst XMMATRIX crop =
\t\t\t\t\t\tXMMatrixTranslation(-centerX, -centerY, 0.0f) *
\t\t\t\t\t\tXMMatrixScaling(static_cast<float>(pageGrid), static_cast<float>(pageGrid), 1.0f);

\t\t\t\t\tShadowRenderPass& pass = g_ShadowRenderPasses[g_ShadowRenderPassCount++];
\t\t\t\t\tpass.ViewProjection = XMMatrixTranspose(fullViewProjection * crop);
\t\t\t\t\tpass.Params = g_ShadowMapParams[layer];
\t\t\t\t\tpass.Layer = layer;
\t\t\t\t\tpass.X = physicalPageX * RendererState::g_kVIRTUAL_SHADOW_PAGE_SIZE;
\t\t\t\t\tpass.Y = physicalPageY * RendererState::g_kVIRTUAL_SHADOW_PAGE_SIZE;
\t\t\t\t\tpass.Size = RendererState::g_kVIRTUAL_SHADOW_PAGE_SIZE;
\t\t\t\t\tpass.ClearLayer = false;
\t\t\t\t\tpass.VirtualPage = true;
\t\t\t\t\tpass.NeedsRender = needsRender;

\t\t\t\t\tif (needsRender)
\t\t\t\t\t{
\t\t\t\t\t\tg_VirtualShadowCacheHit = false;
\t\t\t\t\t\tcacheEntry.GlobalPageX = globalPageX;
\t\t\t\t\t\tcacheEntry.GlobalPageY = globalPageY;
\t\t\t\t\t\tcacheEntry.ContentKey = pageContentKey;
\t\t\t\t\t\tcacheEntry.Valid = true;
\t\t\t\t\t}
\t\t\t\t}
\t\t\t}
\t\t}
\t\tg_LightCacheValid = true;'''
replace_regex(
    "source/rendererresource.cpp",
    r"\n\t\tfor \(UINT layer = 0; layer < g_ShadowLightCount.*?\n\t\tg_LightCacheValid = true;",
    paged_pass_generation
)

replace_once(
    "source/rendererresource.cpp",
    "\t\tconstants.VirtualShadowViewProjections[i] = g_VirtualShadowViewProjections[i];\n"
    "\t\tconstants.VirtualShadowParams[i] = g_VirtualShadowParams[i];",
    "\t\tconstants.VirtualShadowViewProjections[i] = g_VirtualShadowViewProjections[i];\n"
    "\t\tconstants.VirtualShadowParams[i] = g_VirtualShadowParams[i];\n"
    "\t\tconstants.VirtualShadowPageOrigins[i] = g_VirtualShadowPageOrigins[i];"
)
replace_once(
    "source/rendererresource.cpp",
    "\t}\n\tconstants.VirtualShadowGlobal = XMFLOAT4(\n"
    "\t\tg_DirectionalShadowMode,",
    "\t}\n"
    "\tfor (UINT level = 0; level < RendererState::g_kMAX_VIRTUAL_SHADOW_LEVELS; ++level)\n"
    "\t{\n"
    "\t\tfor (UINT group = 0; group < 4; ++group)\n"
    "\t\t{\n"
    "\t\t\tconst UINT row = group * 4;\n"
    "\t\t\tconstants.VirtualShadowResidency[level * 4 + group] = XMUINT4(\n"
    "\t\t\t\tg_VirtualShadowResidencyRows[level][row + 0],\n"
    "\t\t\t\tg_VirtualShadowResidencyRows[level][row + 1],\n"
    "\t\t\t\tg_VirtualShadowResidencyRows[level][row + 2],\n"
    "\t\t\t\tg_VirtualShadowResidencyRows[level][row + 3]);\n"
    "\t\t}\n"
    "\t}\n"
    "\tconstants.VirtualShadowGlobal = XMFLOAT4(\n"
    "\t\tg_DirectionalShadowMode,"
)
replace_once(
    "source/rendererresource.cpp",
    "\t\tstatic_cast<float>(RendererSettings::GetShadowFilterRadius()),\n\t\t0.06f);",
    "\t\tstatic_cast<float>(RendererSettings::GetShadowFilterRadius()),\n\t\t0.85f);"
)
replace_regex(
    "source/rendererresource.cpp",
    r"bool RendererResource::ShouldRenderShadowPass\(UINT shadowIndex\)\n\{.*?\n\}",
    "bool RendererResource::ShouldRenderShadowPass(UINT shadowIndex)\n"
    "{\n"
    "\tif (!g_LightCacheValid) RebuildLightCache();\n"
    "\tif (shadowIndex >= g_ShadowRenderPassCount) return false;\n"
    "\tconst ShadowRenderPass& pass = g_ShadowRenderPasses[shadowIndex];\n"
    "\treturn !pass.VirtualPage || pass.NeedsRender;\n"
    "}"
)

# Clear only the physical page rectangle instead of destroying the cached layer.
replace_once(
    "source/rendererdraw.cpp",
    "\tif (clearShadowLayer)\n\t{\n"
    "\t\tm_CommandList->ClearDepthStencilView(shadowDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);\n"
    "\t}",
    "\tif (clearShadowLayer)\n\t{\n"
    "\t\tm_CommandList->ClearDepthStencilView(shadowDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);\n"
    "\t}\n\telse\n\t{\n"
    "\t\t// Virtual pages share a layer. Clear only the physical page slot so\n"
    "\t\t// cached neighbours survive camera movement.\n"
    "\t\tm_CommandList->ClearDepthStencilView(\n"
    "\t\t\tshadowDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 1, &shadowScissor);\n"
    "\t}"
)

# -----------------------------------------------------------------------------
# Shared shader page table and physical ring mapping.
# -----------------------------------------------------------------------------
replace_once(
    "shader/hlsl/common.hlsl",
    "    float4 VirtualShadowParams[4]; // x: physical layer, y: texel size, z: depth bias, w: normal bias\n"
    "    float4 VirtualShadowGlobal; // x: enabled, y: level count, z: PCF radius, w: clipmap guard band",
    "    float4 VirtualShadowParams[4]; // x: physical layer, y: texel size, z: depth bias, w: normal bias\n"
    "    float4 VirtualShadowPageOrigins[4]; // xy: global page origin, z: page grid, w: page world size\n"
    "    uint4 VirtualShadowResidency[16]; // four packed 16-bit row masks per level\n"
    "    float4 VirtualShadowGlobal; // x: mode, y: level count, z: PCF radius, w: level transition pages"
)

virtual_helpers = r'''

uint VirtualShadowResidencyRowCommon(int level, int row)
{
    int packedIndex = level * 4 + row / 4;
    uint4 packedRows = VirtualShadowResidency[packedIndex];
    return packedRows[row & 3];
}

bool VirtualShadowPageResidentCommon(int level, int2 localPage)
{
    int pageGrid = max((int)round(ShadowDebugGlobal.w), 1);
    if (level < 0 || level >= 4 ||
        localPage.x < 0 || localPage.x >= pageGrid ||
        localPage.y < 0 || localPage.y >= pageGrid)
    {
        return false;
    }
    uint rowMask = VirtualShadowResidencyRowCommon(level, localPage.y);
    return (rowMask & (1u << localPage.x)) != 0u;
}

int VirtualShadowPositiveModuloCommon(int value, int modulus)
{
    int result = value % modulus;
    return result < 0 ? result + modulus : result;
}

float2 VirtualShadowMapUvCommon(
    int level,
    float2 virtualUv,
    out bool resident,
    out int2 localPage)
{
    int pageGrid = max((int)round(VirtualShadowPageOrigins[level].z), 1);
    float2 virtualPagePosition = virtualUv * (float)pageGrid;
    localPage = (int2)floor(virtualPagePosition);
    resident =
        all(virtualUv >= 0.0f) &&
        all(virtualUv < 1.0f) &&
        VirtualShadowPageResidentCommon(level, localPage);
    if (!resident)
    {
        return float2(0.0f, 0.0f);
    }

    int2 globalPage =
        (int2)round(VirtualShadowPageOrigins[level].xy) + localPage;
    int2 physicalPage = int2(
        VirtualShadowPositiveModuloCommon(globalPage.x, pageGrid),
        VirtualShadowPositiveModuloCommon(globalPage.y, pageGrid));
    float2 inPageUv = frac(virtualPagePosition);
    return ((float2)physicalPage + inPageUv) / (float)pageGrid;
}
'''
replace_once(
    "shader/hlsl/common.hlsl",
    "};\n\nfloat3 ApplyLocalHeightFogCommon",
    "};" + virtual_helpers + "\nfloat3 ApplyLocalHeightFogCommon"
)

virtual_atmosphere = r'''

float SampleVirtualAtmosphereShadowMapCommon(
    float3 worldPos,
    float3 lightDir,
    Texture2DArray<float> shadowMap,
    SamplerState shadowSampler)
{
    int levelCount = clamp((int)round(VirtualShadowGlobal.y), 1, 4);
    [unroll]
    for (int level = 0; level < 4; ++level)
    {
        if (level >= levelCount) break;
        float4 clip = mul(float4(worldPos, 1.0f), VirtualShadowViewProjections[level]);
        if (clip.w <= 0.000001f) continue;
        float3 ndc = clip.xyz / clip.w;
        float2 virtualUv = float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
        if (ndc.z < 0.0f || ndc.z > 1.0f) continue;

        bool resident;
        int2 localPage;
        float2 physicalUv = VirtualShadowMapUvCommon(level, virtualUv, resident, localPage);
        if (!resident) continue;

        float4 params = VirtualShadowParams[level];
        float currentDepth = ndc.z - max(params.z, params.w) * (1.0f + level * 0.45f);
        float closestDepth = shadowMap.SampleLevel(
            shadowSampler,
            float3(physicalUv, max(params.x, 0.0f)),
            0);
        return currentDepth <= closestDepth ? 1.0f : 0.0f;
    }
    return 1.0f;
}
'''
replace_once(
    "shader/hlsl/common.hlsl",
    "\n\nfloat3 WorldToScreenUV(float3 worldPos);",
    virtual_atmosphere + "\n\nfloat3 WorldToScreenUV(float3 worldPos);"
)

# -----------------------------------------------------------------------------
# Deferred VSM sampling, level blending, page-safe PCF and contact-shadow fix.
# -----------------------------------------------------------------------------
vsm_deferred_function = r'''float ShadowNoiseHash(float3 value)
{
    return frac(sin(dot(value, float3(12.9898f, 78.233f, 37.719f))) * 43758.5453f);
}

bool ProjectVirtualShadowLevel(
    int level,
    float3 worldPos,
    out float3 lightNdc,
    out float2 virtualUv,
    out float levelInterior)
{
    float4 clip = mul(float4(worldPos, 1.0f), VirtualShadowViewProjections[level]);
    if (clip.w <= 0.000001f)
    {
        lightNdc = 0.0f;
        virtualUv = 0.0f;
        levelInterior = 0.0f;
        return false;
    }

    lightNdc = clip.xyz / clip.w;
    virtualUv = float2(lightNdc.x * 0.5f + 0.5f, -lightNdc.y * 0.5f + 0.5f);
    bool resident;
    int2 localPage;
    VirtualShadowMapUvCommon(level, virtualUv, resident, localPage);
    if (!resident || lightNdc.z < 0.0f || lightNdc.z > 1.0f)
    {
        levelInterior = 0.0f;
        return false;
    }

    float pageGrid = max(VirtualShadowPageOrigins[level].z, 1.0f);
    float residentGrid = max(ShadowDebugGlobal.z, 1.0f);
    float firstResident = (pageGrid - residentGrid) * 0.5f;
    float2 pagePosition = virtualUv * pageGrid;
    float2 edgeDistance = min(
        pagePosition - firstResident,
        firstResident + residentGrid - pagePosition);
    float closestEdge = min(edgeDistance.x, edgeDistance.y);
    levelInterior = smoothstep(
        0.12f,
        max(VirtualShadowGlobal.w, 0.25f),
        closestEdge);
    return true;
}

bool SampleVirtualShadowLevel(
    int level,
    float3 worldPos,
    float3 normal,
    float3 lightDir,
    out float visibility,
    out float levelInterior)
{
    float3 lightNdc;
    float2 virtualUv;
    if (!ProjectVirtualShadowLevel(level, worldPos, lightNdc, virtualUv, levelInterior))
    {
        visibility = 1.0f;
        return false;
    }

    float4 params = VirtualShadowParams[level];
    float3 n = SafeNormalizeCommon(normal, float3(0.0f, 1.0f, 0.0f));
    float3 l = SafeNormalizeCommon(lightDir, float3(0.0f, 1.0f, 0.0f));
    float nDotL = saturate(dot(n, l));
    float levelBiasScale = 1.0f + (float)level * 0.55f;
    float bias = max(params.z * (1.0f - nDotL), params.w) * levelBiasScale;
    float currentDepth = lightNdc.z - bias;

    const float2 poisson[9] =
    {
        float2(0.0f, 0.0f),
        float2(-0.613392f, 0.617481f),
        float2(0.170019f, -0.040254f),
        float2(-0.299417f, 0.791925f),
        float2(0.645680f, 0.493210f),
        float2(-0.651784f, -0.717887f),
        float2(0.421003f, 0.027070f),
        float2(-0.817194f, -0.271096f),
        float2(-0.705374f, -0.668203f)
    };

    int filterRadius = clamp((int)round(VirtualShadowGlobal.z), 0, 3);
    int tapCount = filterRadius <= 0 ? 1 : 9;
    float angle = ShadowNoiseHash(worldPos * 17.0f + (float)level) * 6.28318530718f;
    float sineValue;
    float cosineValue;
    sincos(angle, sineValue, cosineValue);
    float2x2 rotation = float2x2(cosineValue, -sineValue, sineValue, cosineValue);

    float result = 0.0f;
    float sampleCount = 0.0f;
    [unroll]
    for (int tap = 0; tap < 9; ++tap)
    {
        if (tap >= tapCount) break;
        float2 rotatedOffset = mul(poisson[tap], rotation) * (float)max(filterRadius, 1);
        float2 tapVirtualUv = virtualUv + rotatedOffset * params.y;
        bool tapResident;
        int2 tapPage;
        float2 physicalUv = VirtualShadowMapUvCommon(
            level,
            tapVirtualUv,
            tapResident,
            tapPage);
        if (!tapResident) continue;

        float closestDepth = ShadowMapTexture.SampleLevel(
            ShadowSampler,
            float3(physicalUv, max(params.x, 0.0f)),
            0);
        result += currentDepth <= closestDepth ? 1.0f : 0.0f;
        sampleCount += 1.0f;
    }

    if (sampleCount <= 0.0f)
    {
        visibility = 1.0f;
        return false;
    }
    visibility = result / sampleCount;
    return true;
}

float SampleDeferredShadowMap(int lightIndex, float3 worldPos, float3 normal, float3 lightDir)
{
    float shadowLayer = LightShadowData[lightIndex].x;
    [branch]
    if (shadowLayer < -0.5f)
    {
        return 1.0f;
    }

    const bool virtualShadow =
        VirtualShadowGlobal.x > 0.5f &&
        LightShadowData[lightIndex].w < 0.0f;
    if (virtualShadow)
    {
        int levelCount = clamp((int)round(VirtualShadowGlobal.y), 1, 4);
        float fineVisibility = 1.0f;
        float fineInterior = 1.0f;
        bool hasFine = false;

        [unroll]
        for (int level = 0; level < 4; ++level)
        {
            if (level >= levelCount) break;
            float levelVisibility;
            float levelInterior;
            if (!SampleVirtualShadowLevel(
                    level,
                    worldPos,
                    normal,
                    lightDir,
                    levelVisibility,
                    levelInterior))
            {
                continue;
            }

            if (!hasFine)
            {
                fineVisibility = levelVisibility;
                fineInterior = levelInterior;
                hasFine = true;
                if (fineInterior >= 0.999f)
                {
                    return fineVisibility;
                }
                continue;
            }

            // Blend to the next resident coarser level near a page-window edge.
            // This removes the straight clipmap seam without blurring the centre.
            return lerp(levelVisibility, fineVisibility, fineInterior);
        }
        return hasFine ? fineVisibility : 1.0f;
    }

    float3 n = SafeNormalizeCommon(normal, float3(0.0f, 1.0f, 0.0f));
    float3 l = SafeNormalizeCommon(lightDir, float3(0.0f, 1.0f, 0.0f));
    float4 lightClip = mul(float4(worldPos, 1.0f), LightViewProjections[lightIndex]);
    float3 lightNdc = lightClip.xyz / max(lightClip.w, 0.000001f);
    float2 shadowUv = float2(lightNdc.x * 0.5f + 0.5f, -lightNdc.y * 0.5f + 0.5f);
    bool inBounds =
        lightClip.w > 0.0f &&
        all(shadowUv >= 0.0f) && all(shadowUv <= 1.0f) &&
        lightNdc.z >= 0.0f && lightNdc.z <= 1.0f;
    if (!inBounds) return 1.0f;

    float texelSize = LightShadowData[lightIndex].y;
    float bias = max(
        LightShadowData[lightIndex].z * (1.0f - saturate(dot(n, l))),
        abs(LightShadowData[lightIndex].w));
    float currentDepth = lightNdc.z - bias;
    float visibility = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float closestDepth = ShadowMapTexture.SampleLevel(
                ShadowSampler,
                float3(shadowUv + float2(x, y) * texelSize, shadowLayer),
                0);
            visibility += currentDepth <= closestDepth ? 1.0f : 0.0f;
        }
    }
    return visibility / 9.0f;
}

'''
replace_regex(
    "shader/hlsl/DeferredLightingPS.hlsl",
    r"float SampleDeferredShadowMap\(.*?\n\}\n\nfloat SampleContactShadow",
    vsm_deferred_function + "float SampleContactShadow"
)

contact_function = r'''float3 ReconstructContactWorldPosition(float2 uv, float depth)
{
    float2 ndc = uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
    float4 world = mul(float4(ndc, depth, 1.0f), PPInvViewProjection);
    return world.xyz / max(abs(world.w), 0.000001f);
}

float SampleContactShadow(float3 worldPos, float3 normal, float3 lightDir)
{
    if (ShadowRuntimeGlobal.x < 0.5f) return 1.0f;

    const int stepCount = clamp((int)round(ShadowRuntimeGlobal.z), 6, 24);
    const float rayLength = max(ShadowRuntimeGlobal.y, 0.02f);
    const float3 safeNormal = SafeNormalizeCommon(normal, float3(0.0f, 1.0f, 0.0f));
    const float3 safeLightDir = SafeNormalizeCommon(lightDir, float3(0.0f, 1.0f, 0.0f));
    const float stepLength = rayLength / (float)stepCount;
    const float3 origin = worldPos + safeNormal * max(0.018f, stepLength * 0.55f);
    const float jitter = ShadowNoiseHash(worldPos * 31.0f + safeNormal * 7.0f);

    float occlusion = 0.0f;
    [loop]
    for (int stepIndex = 0; stepIndex < stepCount; ++stepIndex)
    {
        float t = ((float)stepIndex + 0.25f + jitter * 0.65f) / (float)stepCount;
        float3 rayPosition = origin + safeLightDir * rayLength * t;
        float3 projected = WorldToScreenUV(rayPosition);
        if (projected.x <= 0.001f || projected.x >= 0.999f ||
            projected.y <= 0.001f || projected.y >= 0.999f ||
            projected.z <= 0.0f || projected.z >= 1.0f)
        {
            break;
        }

        float sceneDepth = DepthTexture.SampleLevel(TextureSampler, projected.xy, 0).r;
        if (sceneDepth >= 0.9999f) continue;
        float3 sceneWorld = ReconstructContactWorldPosition(projected.xy, sceneDepth);
        float3 cameraToRay = rayPosition - PPCameraPos.xyz;
        float rayDistance = length(cameraToRay);
        float3 viewDirection = cameraToRay / max(rayDistance, 0.000001f);
        float sceneDistance = dot(sceneWorld - PPCameraPos.xyz, viewDirection);
        float depthDelta = rayDistance - sceneDistance;

        // World-space thickness remains stable with camera distance. A smooth
        // band-pass replaces the old binary first-hit return that caused stripes.
        float thickness =
            0.012f +
            stepLength * 0.75f +
            rayDistance * 0.0015f;
        float hit =
            smoothstep(0.0015f, thickness * 0.35f, depthDelta) *
            (1.0f - smoothstep(thickness * 0.72f, thickness, depthDelta));
        occlusion = max(occlusion, hit * (1.0f - t * 0.65f));
    }

    float cameraDistance = length(worldPos - PPCameraPos.xyz);
    float distanceFade = 1.0f - smoothstep(24.0f, 55.0f, cameraDistance);
    return lerp(1.0f, 0.42f, saturate(occlusion * distanceFade));
}'''
replace_regex(
    "shader/hlsl/DeferredLightingPS.hlsl",
    r"float SampleContactShadow\(.*?\n\}",
    contact_function
)
replace_once(
    "shader/hlsl/DeferredLightingPS.hlsl",
    "        shadowVisibility *= SampleContactShadow(worldPos, normal, singleDir);",
    "        float contactVisibility = SampleContactShadow(worldPos, normal, singleDir);\n"
    "        shadowVisibility *= lerp(1.0f, contactVisibility, 0.45f);"
)

# Keep atmospheric shadow lookups compatible with ring-mapped VSM pages.
replace_once(
    "shader/hlsl/AtmosphereGBufferPS.hlsl",
    "    if (LightShadowData[lightIndex].x < -0.5f)\n"
    "    {\n"
    "        return 1.0f;\n"
    "    }\n\n"
    "    float4 perLightShadowParams = float4(",
    "    if (LightShadowData[lightIndex].x < -0.5f)\n"
    "    {\n"
    "        return 1.0f;\n"
    "    }\n\n"
    "    if (VirtualShadowGlobal.x > 0.5f &&\n"
    "        LightPositionTypes[lightIndex].w < 0.5f &&\n"
    "        LightShadowData[lightIndex].w < 0.0f)\n"
    "    {\n"
    "        return SampleVirtualAtmosphereShadowMapCommon(\n"
    "            samplePos, lightDir, ShadowMapTexture, ShadowSampler);\n"
    "    }\n\n"
    "    float4 perLightShadowParams = float4("
)

# Lightweight structural checks before the workflow commits anything.
for shader_path in [
    "shader/hlsl/common.hlsl",
    "shader/hlsl/DeferredLightingPS.hlsl",
    "shader/hlsl/AtmosphereGBufferPS.hlsl",
]:
    text = read(shader_path)
    if text.count("{") != text.count("}"):
        raise RuntimeError(f"{shader_path}: brace count mismatch")

print("VSM upgrade patch applied successfully")
