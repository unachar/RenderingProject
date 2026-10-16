#pragma once
#include "rendererstate.h"

struct PrimitiveSettings
{
	float Radius = 0.5f;
	float Height = 1.0f;
	float Width = 1.0f;
	float Depth = 1.0f;
	int Segments = 32;
	int Rings = 16;
	int FaceDivisions = 1;
};

class PrimitiveGeometry : protected RendererState
{
public:
	static void CreateSpriteVertex(const VertexResource& vertexstruct);
	static void CreateObjectVertex(const VertexResource& vertexstruct, const PrimitiveSettings& settings = {});
	static bool UploadVertices(EntityID entity, const vector<Vertex>& vertices);
};
