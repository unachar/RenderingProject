#pragma once

enum class RenderPass
{
	ShadowMap,
	PrimaryScene,
	Velocity,
	OverlayScene
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

