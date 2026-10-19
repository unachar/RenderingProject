#pragma once
#include "ecs.h"
#include "materialasset.h"
#include "rendertypes.h"
#include "light.h"
#include "animationplayback.h"
#include "physicscomponent.h"
#include "timelinecomponent.h"
#include "proceduralcomponents.h"
#include "visibility/visibilitytypes.h"
#include <cassert>
#include <array>
#include <cstdlib>
#include <limits>
#include <typeindex>
#include <type_traits>
#include <memory>

enum class PostProcessType;

struct TransformComponent
{
    XMFLOAT3 Position = {0.0f, 0.0f, 0.0f};
    XMFLOAT3 Scale = {1.0f, 1.0f, 1.0f};
    XMFLOAT3 Rotation = {0.0f, 0.0f, 0.0f};
    XMFLOAT4X4 WorldMatrix{};
    XMFLOAT4X4 PreviousWorldMatrix{};
    bool HasPreviousWorld = false;
    bool IsDirty = true;
};

struct StaticMeshData;

struct MeshComponent
{
    void* MeshData = nullptr;
    ComPtr<ID3D12Resource> VertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW VertexBufferView{};
    UINT VertexCount = 0;
    uint64_t GeometryHash = 0;
    XMFLOAT3 LocalBoundsCenter{};
    XMFLOAT3 LocalBoundsExtents{};
    bool HasLocalBounds = false;
    bool IsEnvironment = false;
    shared_ptr<StaticMeshData> NaniteMesh;
    // シーン保存用：プリミティブから作られたメッシュの再生成情報（0 = プリミティブ以外）
    int PrimitiveType = 0;  // オブジェクト種別（ObjectType）
    int PrimitiveColor = 0; // 色（Color）
    float PrimitiveRadius = 0.5f;
    float PrimitiveHeight = 1.0f;
    float PrimitiveWidth = 1.0f;
    float PrimitiveDepth = 1.0f;
    int PrimitiveSegments = 32;
    int PrimitiveRings = 16;
    int PrimitiveFaceDivisions = 1;
};

struct InputComponent
{
    bool MoveLeft = false;
    bool MoveRight = false;
    bool MoveForward = false;
    bool MoveBackward = false;
    bool MoveUp = false;
    bool MoveDown = false;
    bool RotateLeft = false;
    bool RotateRight = false;
    bool RotateUp = false;
    bool RotateDown = false;
    bool PostProcessNone = false;
    bool PostProcessBlur = false;
    bool PostProcessSepia = false;
    bool PostProcessGrayscale = false;
    bool PostProcessInvert = false;
    bool IntensityUp = false;
    bool IntensityDown = false;
    bool IsActive = false;
};

struct AABBComponent
{
    XMFLOAT3 Min = {0.0f, 0.0f, 0.0f};
    XMFLOAT3 Max = {0.0f, 0.0f, 0.0f};
    XMFLOAT3 Center = {0.0f, 0.0f, 0.0f};
    XMFLOAT3 Extents = {0.0f, 0.0f, 0.0f};
    bool DrawDebug = false;
};

struct OBBComponent
{
    XMFLOAT3 Center = {0.0f, 0.0f, 0.0f};
    XMFLOAT3 Extents = {0.0f, 0.0f, 0.0f};
    XMFLOAT3 Axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    bool DrawDebug = false;
};

struct CameraComponent
{
    XMFLOAT3 Position = {0.0f, 0.0f, 0.0f};
    XMFLOAT3 Target = {0.0f, 0.0f, 0.0f};
    float Fov = XM_PIDIV4;
    float NearClip = 0.1f;
    float FarClip = 100.0f;
    bool EnablePostProcess = false;
    bool IsGameCamera = false;
    bool IsMainGameCamera = false;
    bool AllowUserControl = true;
    int Priority = 0;
    EntityID LockOnTarget = g_kINVALID_ENTITY;
    XMFLOAT3 LockOnOffset = {0.0f, 0.0f, 0.0f};
    XMFLOAT4X4 ViewMatrix{};
    XMFLOAT4X4 ProjectionMatrix{};
};

struct AnimationModelComponent
{
    int ModelId = -1;
    string ModelPath;
    bool IsConvert = true;
    vector<string> AnimationPaths{};
    vector<string> Animations{};
    vector<AnimationPlaybackLayer> ActiveAnimationLayers{};
    string CurrentAnimation{};
    string NextAnimation{};
    float CurrentTime = 0.0f;
    float NextTime = 0.0f;
    float BlendRate = 0.0f;
    float Speed = 1.0f;
    bool IsPlaying = true;
};

struct ShaderComponent
{
    string VsPath;
    string PsPath;
    ComPtr<ID3D12PipelineState> Pso = nullptr;
};

struct StaticModelComponent
{
    int ModelId = -1;
    string ModelPath;
    bool IsConvert = true;
};

enum class MaterialMode : int
{
    Auto = 0,
    Manual = 1,
};

enum class ShaderClass : int
{
    Transparent = 0,
    Hair = 1,
    Cloth = 2,
    Skin = 3,
    Toon = 4,
    Shadow = 5,
    Metallic = 6,
    SelfShadow = 7,
    Lit = 8,
    Eye = 9,
    Unlit = 10,
    PBR = 11,
    BRDF = 12,
    BTDF = 13,
    BSDF = 14,
};

enum class ToonOutlineMode : int
{
    Extrude = 0,
    TEO = 1,
    Mix = 2,
};

enum class ToonOutlineWidthMode : int
{
    World = 0,
    ScreenPixels = 1,
};

enum class ToonTeoMode : int
{
    Balanced = 0,
    Boundary = 1,
    HardEdge = 2,
    Clean = 3,
};

enum class MeshOutlineOverride : int
{
    Auto = 0,
    ForceOn = 1,
    ForceOff = 2,
};

enum class MeshShadingOverride : int
{
    Auto = -1,
    Transparent = 0,
    Hair = 1,
    Cloth = 2,
    Skin = 3,
    Toon = 4,
    Shadow = 5,
    Metallic = 6,
    SelfShadow = 7,
    Lit = 8,
    Eye = 9,
    Unlit = 10,
    PBR = 11,
    BRDF = 12,
    BTDF = 13,
    BSDF = 14,
};

static constexpr int kMaterialPartParamCount = 15;

struct MaterialPartParams
{
    float Metallic = 0.0f;
    float Roughness = 0.5f;
    float Fresnel = 0.04f;
    float NormalBlend = 0.0f;
    float NormalBias = 0.0f;
    float BaseSaturation = 1.2f;
    float BaseBrightness = 1.0f;
    float ShadowThreshold = 0.44f;
    float ShadowSoftness = 0.045f;
    float ShadowStrength = 1.0f;
    float MidStrength = 1.0f;
    float LitStrength = 1.0f;
    float RimStrength = 0.45f;
    float RimThreshold = 0.70f;
    float RimSoftness = 0.055f;
    float RimPower = 1.0f;
    XMFLOAT3 RimColor = {0.38f, 0.48f, 0.80f};
    float RimAlbedoBlend = 0.20f;
    float RimLightBlend = 0.35f;
    float SpecularStrength = 0.35f;
    float SpecularThreshold = 0.35f;
    float KawaiiBlend = 1.0f;
    float SkinScatterStrength = 1.0f;
    float SkinScatterWrap = 0.42f;
    float SkinBacklightStrength = 1.0f;
    float SkinRimScatterStrength = 1.0f;
    float SkinOilSpecularStrength = 1.0f;
    float SkinShadowScatter = 0.54f;
    float CastShadowThreshold = 0.28f;
    float CastShadowSoftness = 0.10f;
};

struct MaterialComponent
{
    vector<AssetHandle> MaterialSlots;
    AssetHandle MaterialSceneIdentity = 0;
    AssetHandle RuntimeHandle = 0;
    XMFLOAT4 BaseColor{1, 1, 1, 1};
    float NormalScale = 1.0f;
    float AOStrength = 1.0f;
    XMFLOAT3 EmissiveColor{};
    float EmissiveIntensity = 0.0f;
    int MetallicRoughnessID = -1;
    int AOTextureID = -1;
    int EmissiveTextureID = -1;
    int HeightTextureID = -1;
    float ParallaxDepth = 0.0f;
    float ParallaxSteps = 32.0f;
    MaterialSurface AssetSurface = MaterialSurface::Opaque;
    uint32_t ShadingModel = 0; // MaterialAssetData::ShadingModel を参照。
    float ShadingStrength = 1.0f;
    MaterialCull AssetCull = MaterialCull::Back;
    bool AssetDepthWrite = true;
    float AlphaCutoff = 0.5f;
    int TextureID = -1;
    string TexturePath;
    int NormalMapID = -1;
    string NormalMapPath;
    float Metallic = 0.0f;
    float Roughness = 0.5f;
    float Fresnel = 0.04f;
    float NormalBlend = 0.0f;
    float NormalBias = 0.0f;
    float BaseSaturation = 1.2f;
    float BaseBrightness = 1.0f;
    float ShadowThreshold = 0.44f;
    float ShadowSoftness = 0.045f;
    float ShadowStrength = 1.0f;
    float MidStrength = 1.0f;
    float LitStrength = 1.0f;
    float RimStrength = 0.45f;
    float RimThreshold = 0.70f;
    float RimSoftness = 0.055f;
    float RimPower = 1.0f;
    XMFLOAT3 RimColor = {0.38f, 0.48f, 0.80f};
    float RimAlbedoBlend = 0.20f;
    float RimLightBlend = 0.35f;
    float SpecularStrength = 0.35f;
    float SpecularThreshold = 0.35f;
    float KawaiiBlend = 1.0f;
    float SkinScatterStrength = 1.0f;
    float SkinScatterWrap = 0.42f;
    float SkinBacklightStrength = 1.0f;
    float SkinRimScatterStrength = 1.0f;
    float SkinOilSpecularStrength = 1.0f;
    float SkinShadowScatter = 0.54f;
    float CastShadowThreshold = 0.28f;
    float CastShadowSoftness = 0.10f;
    MaterialPartParams PartParams[kMaterialPartParamCount]{};
    MaterialMode ShaderClassMode = MaterialMode::Auto;
    ShaderClass ShaderClass = ShaderClass::Unlit;
    float ToonOutlineWidth = 0.035f;
    float ToonOutlineScreenWidth = 3.0f;
    ToonOutlineMode ToonOutlineRenderMode = ToonOutlineMode::Extrude;
    ToonOutlineWidthMode ToonOutlineWidthModeSetting = ToonOutlineWidthMode::World;
    ToonTeoMode ToonTeoRenderMode = ToonTeoMode::Balanced;
    float ToonOutlineTeoWidthScale = 1.0f;
    vector<MeshOutlineOverride> ToonMeshOutlineOverrides{};
    vector<float> ToonMeshOutlineWidthScales{};
    vector<MeshShadingOverride> MeshShadingOverrides{};
    float Alpha = 1.0f;
    MaterialShadowMode ShadowMode = MaterialShadowMode::Auto;
    float ShadowCutoff = 0.5f;

    float IOR = 1.50f;
    float Transmission = 0.0f;
    float TransmissionRoughness = 0.02f;
    float RefractionStrength = 0.035f;
    float Thickness = 0.10f;
    XMFLOAT3 AbsorptionCoefficient = {0.02f, 0.01f, 0.005f};
    bool IsTransparent = false;
    bool DoubleSided = false; // 静的不透明 Mesh Shader のラスタライズとコーン判定の方針。
    bool ReceivingPostProcess = true;
    bool UseTexture = false;
};

struct PostProcessComponent
{
    PostProcessType Type = PostProcessType::NONE;
    float Intensity = 1.0f;
    float BloomThreshold = 2.0f;
    float BloomSoftKnee = 0.5f;
    float BloomRadius = 1.0f;
};

struct NameComponent
{
    string Name = "EntityName";
};

struct MoveComponent
{
    XMFLOAT3 Direction = {0.0f, 0.0f, 0.0f};
    float Speed = 0.0f;
    bool UseCameraRelativeMovement = false;
    bool CanMove = true;
    float RotationSpeed = 0.0f;
};

struct SpriteComponent
{
    bool Is3D = false;
    bool UseUvTransform = false;
    XMFLOAT2 UvOffset = {0.0f, 0.0f};
    XMFLOAT2 UvScale = {1.0f, 1.0f};

    ComPtr<ID3D12Resource> VertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW VertexBufferView{};
    UINT VertexCount = 0;
    uint64_t GeometryHash = 0;
    XMFLOAT3 LocalBoundsCenter{};
    XMFLOAT3 LocalBoundsExtents{};
    bool HasLocalBounds = false;
    bool UsePostProcess = false;
    // シーン保存用：スプライト形状の再生成情報（0 = 不明）
    int SourceShape = 0; // 形状種別（ShapeType）
    float SourceRadius = 0.5f;
    int SourceColor = 0; // 色（Color）
};

struct InstancingComponent
{
    bool UseInstancing = false;
    bool EnableFrustumCulling = true;

    uint32_t GroupId = 0;
};

struct LODComponent
{
    bool UseLOD = false;
    float Lod1Distance = 12.0f;
    float Lod2Distance = 28.0f;
    Visibility::LODMetadata ScreenLOD{};
    bool UseAssetSettings = true;
};

struct LightComponent
{
    XMFLOAT3 Position = {0.0f, 0.0f, 0.0f};
    XMFLOAT4 Color = {1.0f, 1.0f, 1.0f, 1.0f};
    float Intensity = 1.0f;
    float Range = 1.0f;
    XMFLOAT3 Direction = {0.0f, 1.0f, -1.0f};
    float OuterAngle = 0.0f;
    float InnerAngle = 0.0f;
    float VolumeDensity = 0.0f;
    int VolumeShape = 0;
    float AreaWidth = 1.0f; // 矩形ライトの寸法（メートル）。
    float AreaHeight = 1.0f;
    bool IsActive = true;
    bool DrawDebug = false;
    bool AffectsOpaque = true;
    bool AffectsForward = true;
    bool AffectsVolumetrics = false;
    bool CastShadow = false;
    bool AllowMegaLights = true; // ディファード時、MegaLights のクラスターグリッドで評価してよい（Point / Spot / Rect）
    float SourceRadius = 0.0f;   // Point / Spot の発光球の半径（m）。MegaLights の影レイでソフトシャドウになる
    string AreaTexturePath;      // Rect の発光面のテクスチャ（MegaLights M7。空 = 単色）
    string IesProfilePath;       // IES 配光。空の場合は従来の配光を使う。
    float IesRotationDegrees = 0.0f;
    float Priority = 0.0f;
    LightRenderMode RenderMode = LightRenderMode::Physical;

    LightType Type = LightType::Directional;
};

struct SunComponent
{
    XMFLOAT3 Target = {0.0f, 0.0f, 0.0f};
    float VisualRadius = 1.5f;
    bool SyncDirectionalLight = true;
};

template <typename T> struct ComponentStorage
{
    static constexpr uint32_t kInvalidIndex = numeric_limits<uint32_t>::max();

    static vector<T>& DenseData()
    {
        static vector<T> data;
        return data;
    }

    static vector<EntityID>& DenseEntities()
    {
        static vector<EntityID> entities;
        return entities;
    }

    static array<uint32_t, g_kMAX_ENTITIES>& SparseIndices()
    {
        static array<uint32_t, g_kMAX_ENTITIES> indices = [] {
            array<uint32_t, g_kMAX_ENTITIES> value{};
            value.fill(kInvalidIndex);
            return value;
        }();
        return indices;
    }

    static void CreateEntity(EntityID entity)
    {
        if (entity >= g_kMAX_ENTITIES || SparseIndices()[entity] != kInvalidIndex)
        {
            return;
        }

        auto& data = DenseData();
        auto& entities = DenseEntities();
        entities.push_back(entity);
        try
        {
            data.emplace_back();
        }
        catch (...)
        {
            entities.pop_back();
            throw;
        }
        SparseIndices()[entity] = static_cast<uint32_t>(data.size() - 1);
    }

    static bool Contains(EntityID entity)
    {
        return entity < g_kMAX_ENTITIES && SparseIndices()[entity] != kInvalidIndex;
    }

    static T& Get(EntityID entity)
    {
        return DenseData()[SparseIndices()[entity]];
    }

    static void ClearEntity(EntityID entity)
    {
        if (!Contains(entity))
        {
            return;
        }

        auto& data = DenseData();
        auto& entities = DenseEntities();
        auto& sparse = SparseIndices();
        const uint32_t index = sparse[entity];
        const uint32_t lastIndex = static_cast<uint32_t>(data.size() - 1);
        if (index != lastIndex)
        {
            data[index] = move(data[lastIndex]);
            const EntityID movedEntity = entities[lastIndex];
            entities[index] = movedEntity;
            sparse[movedEntity] = index;
        }
        data.pop_back();
        entities.pop_back();
        sparse[entity] = kInvalidIndex;
    }

    static void Reset()
    {
        DenseData().clear();
        DenseEntities().clear();
        SparseIndices().fill(kInvalidIndex);
    }
};

template <typename T> inline ComponentType ComponentTypeRegistry::GetType()
{
    type_index key(typeid(T));
    auto& typeIds = TypeIds();
    auto it = typeIds.find(key);
    if (it != typeIds.end())
    {
        return ComponentType(it->second);
    }

    auto& nextTypeId = NextTypeId();
    assert(nextTypeId < g_kMAX_COMPONENTS && "Component type limit reached");
    ComponentTypeID id = nextTypeId++;
    typeIds.emplace(key, id);
    ComponentTypeRegistry::CreateCallbacks().push_back(&ComponentStorage<T>::CreateEntity);
    ClearCallbacks().push_back(&ComponentStorage<T>::ClearEntity);
    ComponentTypeRegistry::ResetCallbacks().push_back(&ComponentStorage<T>::Reset);
    return ComponentType(id);
}

template <typename T> struct ComponentTypeTraits
{
    // 型ごとに1回だけ登録表を引き、以降はキャッシュ値を返す（HasComponent等で毎回ハッシュ検索しない）。
    static ComponentType value()
    {
        static const ComponentType type = ComponentTypeRegistry::GetType<T>();
        return type;
    }
};

inline const ComponentType ComponentType::TRANSFORM = ComponentTypeRegistry::GetType<TransformComponent>();
inline const ComponentType ComponentType::MESH = ComponentTypeRegistry::GetType<MeshComponent>();
inline const ComponentType ComponentType::INPUT_CTRL = ComponentTypeRegistry::GetType<InputComponent>();
inline const ComponentType ComponentType::AABB = ComponentTypeRegistry::GetType<AABBComponent>();
inline const ComponentType ComponentType::OBB = ComponentTypeRegistry::GetType<OBBComponent>();
inline const ComponentType ComponentType::CAMERA = ComponentTypeRegistry::GetType<CameraComponent>();
inline const ComponentType ComponentType::ANIMATION_MODEL = ComponentTypeRegistry::GetType<AnimationModelComponent>();
inline const ComponentType ComponentType::SHADER = ComponentTypeRegistry::GetType<ShaderComponent>();
inline const ComponentType ComponentType::STATIC_MODEL = ComponentTypeRegistry::GetType<StaticModelComponent>();
inline const ComponentType ComponentType::MATERIAL = ComponentTypeRegistry::GetType<MaterialComponent>();
inline const ComponentType ComponentType::PHYSICS = ComponentTypeRegistry::GetType<PhysicsComponent>();
inline const ComponentType ComponentType::POST_PROCESS = ComponentTypeRegistry::GetType<PostProcessComponent>();
inline const ComponentType ComponentType::NAME = ComponentTypeRegistry::GetType<NameComponent>();
inline const ComponentType ComponentType::MOVE = ComponentTypeRegistry::GetType<MoveComponent>();
inline const ComponentType ComponentType::SPRITE = ComponentTypeRegistry::GetType<SpriteComponent>();
inline const ComponentType ComponentType::LIGHT = ComponentTypeRegistry::GetType<LightComponent>();
inline const ComponentType ComponentType::SUN = ComponentTypeRegistry::GetType<SunComponent>();
inline const ComponentType ComponentType::INSTANCING = ComponentTypeRegistry::GetType<InstancingComponent>();
inline const ComponentType ComponentType::LOD = ComponentTypeRegistry::GetType<LODComponent>();
inline const ComponentType ComponentType::TIMELINE = ComponentTypeRegistry::GetType<TimelineComponent>();
inline const ComponentType ComponentType::PROCEDURAL_TERRAIN =
    ComponentTypeRegistry::GetType<ProceduralTerrainComponent>();
inline const ComponentType ComponentType::PROCEDURAL_MOTION =
    ComponentTypeRegistry::GetType<ProceduralMotionComponent>();

class ComponentManager
{
  private:
    friend class Registry;

  public:
    static void Init();
    static void Uninit();
    static void ClearEntity(EntityID entity);

    template <typename T> static T& GetComponent(EntityID entity)
    {
        if (entity >= g_kMAX_ENTITIES)
        {
            ReportMissingComponentError(entity, "Invalid Entity");
            assert(false && "GetComponent called on invalid entity id");
            abort();
        }

        if (!Registry::HasComponent(entity, ComponentTypeTraits<T>::value()))
        {
            ReportMissingComponentError(entity, typeid(T).name());
            assert(false && "GetComponent called for a missing component");
            abort();
        }

        if constexpr (is_same_v<T, MeshComponent> || is_same_v<T, StaticModelComponent> ||
                      is_same_v<T, AnimationModelComponent> || is_same_v<T, MaterialComponent> ||
                      is_same_v<T, SpriteComponent> || is_same_v<T, AABBComponent> || is_same_v<T, LODComponent> ||
                      is_same_v<T, InstancingComponent> || is_same_v<T, ShaderComponent> ||
                      is_same_v<T, NameComponent> || is_same_v<T, ProceduralMotionComponent>)
        {
            GpuSceneChanges::Notify(entity);
        }
        return ComponentStorage<T>::Get(entity);
    }

    template <typename T> static T& GetComponentUnchecked(EntityID entity)
    {
        assert(entity < g_kMAX_ENTITIES && Registry::IsAlive(entity));
        assert(ComponentStorage<T>::Contains(entity));
        return ComponentStorage<T>::Get(entity);
    }

    template <typename T> static const vector<T>& GetDenseComponents()
    {
        return ComponentStorage<T>::DenseData();
    }

    template <typename T> static const vector<EntityID>& GetDenseEntities()
    {
        return ComponentStorage<T>::DenseEntities();
    }

    template <typename T, typename Function> static void ForEachComponent(Function&& function)
    {
        auto& data = ComponentStorage<T>::DenseData();
        const auto& entities = ComponentStorage<T>::DenseEntities();
        const size_t count = data.size();
        for (size_t i = 0; i < count; ++i)
        {
            function(entities[i], data[i]);
        }
    }

    template <typename Primary, typename... Required, typename Function> static void ForEach(Function&& function)
    {
        auto& primaryData = ComponentStorage<Primary>::DenseData();
        const auto& entities = ComponentStorage<Primary>::DenseEntities();
        const size_t count = primaryData.size();
        for (size_t i = 0; i < count; ++i)
        {
            const EntityID entity = entities[i];
            if ((ComponentStorage<Required>::Contains(entity) && ...))
            {
                function(entity, primaryData[i], ComponentStorage<Required>::Get(entity)...);
            }
        }
    }

    static void ReportMissingComponentError(EntityID entity, const char* componentName);

    template <typename T> static bool HasComponent(EntityID entity)
    {
        return Registry::HasComponent(entity, ComponentTypeTraits<T>::value());
    }

    static bool HasComponent(EntityID entity, ComponentType type);

    template <typename... ComponentTypes> static void AddComponent(EntityID entity, ComponentTypes... types)
    {
        Registry::AddComponent(entity, types...);
    }

    template <typename... ComponentTypes> static void RemoveComponent(EntityID entity, ComponentTypes... types)
    {
        Registry::RemoveComponent(entity, types...);
    }
};
