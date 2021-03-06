#pragma once

#include "core/types.h"
#include "core/vector.h"
#include "math/aabb.h"
#include "gpu/resources.h"
#include "graphics/shader.h"
#include "common.h"
#include "render_packets.h"

class CompressedModel
{
public:
	CompressedModel(const char* fileName);
	~CompressedModel();

	void DrawClusters(DrawContext& drawCtx, ObjectConstants object);

	struct Mesh
	{
		Math::AABB bounds_;
		i32 baseVertex_ = 0;
		i32 baseIndex_ = 0;
		i32 numIndices_ = 0;
	};

	Core::Vector<Mesh> meshes_;


	Core::Vector<GPU::Handle> paramsBuffer_;
	Core::Vector<GPU::Handle> positionTex_;
	Core::Vector<GPU::Handle> normalTex_;
	Core::Vector<GPU::Handle> colorTex_;

	GPU::Format positionFmt_;
	GPU::Format normalFmt_;
	GPU::Format colorFmt_;

	GPU::BufferDesc vertexDesc_;
	GPU::BufferDesc indexDesc_;

	Core::Vector<GPU::VertexElement> elements_;

	GPU::Handle vertexBuffer_;
	GPU::Handle indexBuffer_;

	GPU::Handle dbs_;

	Core::Vector<Graphics::MaterialRef> materials_;
	Core::Vector<Graphics::MaterialRef> compressedMaterials_;

	Graphics::ShaderBindingSet objectBindings_;
	Graphics::ShaderBindingSet geometryBindings_;

	Graphics::ShaderTechniqueDesc techDesc_;
	Graphics::ShaderTechniqueDesc compressedTechDesc_;
	Core::Vector<ShaderTechniques> techs_;
	Core::Vector<ShaderTechniques> compressedTechs_;

	bool useCompressed_ = true;

	bool enableCulling_ = true;
};
