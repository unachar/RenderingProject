#pragma once
#include <memory>
#include <string>

struct NiagaraSceneInstance;

struct NiagaraInstanceSettings
{
    std::string AssetPath;
    bool Enabled = true;
    bool AutoPlay = true;
    bool PreviewInEditor = false;
    float PlaybackRate = 1.0f;
    // -1 follows the asset; 0 plays once; 1 loops.
    int LoopOverride = -1;
};

struct NiagaraComponent : NiagaraInstanceSettings
{
    std::shared_ptr<NiagaraSceneInstance> Instance;

    NiagaraComponent() = default;
    // Scene snapshots and clones copy settings, never a running simulation.
    NiagaraComponent(const NiagaraComponent& other) : NiagaraInstanceSettings(other) {}
    NiagaraComponent& operator=(const NiagaraComponent& other)
    {
        if (this != &other)
        {
            NiagaraInstanceSettings::operator=(other);
            Instance.reset();
        }
        return *this;
    }
    NiagaraComponent(NiagaraComponent&&) noexcept = default;
    NiagaraComponent& operator=(NiagaraComponent&&) noexcept = default;
};
