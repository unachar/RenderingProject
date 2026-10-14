#pragma once

enum class RenderPass
{
	ShadowMap,
	PrimaryScene,
	OcclusionPhase2,
	Velocity,
	OverlayScene,
    TransparentScene,
    TransmittanceShadow
};

class SystemBase
{
public:
	virtual void Init() {}
	virtual void Uninit() {}
	virtual void Update() {}
	virtual void Draw(RenderPass renderPass, bool receivingPostProcessOnly)
	{
		(void)renderPass;
		(void)receivingPostProcessOnly;
	}
};

