#pragma once
#include "rendererstate.h"

class PrimitiveGeometry : protected RendererState
{
public:
	static void CreateSpriteVertex(const VertexResource& vertexstruct);
	static void CreateObjectVertex(const VertexResource& vertexstruct);
	static bool UploadVertices(EntityID entity, const std::vector<Vertex>& vertices);
};
