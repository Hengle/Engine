#include "compressed_model.h"
#include "common.h"

#include "core/concurrency.h"
#include "core/debug.h"
#include "core/file.h"
#include "core/half.h"
#include "core/library.h"
#include "core/misc.h"
#include "core/pair.h"
#include "core/string.h"
#include "core/type_conversion.h"
#include "gpu/enum.h"
#include "gpu/manager.h"
#include "gpu/utils.h"
#include "graphics/converters/import_model.h"
#include "image/image.h"
#include "image/process.h"
#include "image/save.h"
#include "job/concurrency.h"
#include "job/function_job.h"
#include "job/manager.h"
#include "math/aabb.h"
#include "math/mat44.h"
#include "resource/converter.h"
#include "serialization/serializer.h"

#include "assimp/config.h"
#include "assimp/cimport.h"
#include "assimp/scene.h"
#include "assimp/mesh.h"
#include "assimp/postprocess.h"

#if ENABLE_SIMPLYGON
#include "SimplygonSDK.h"
#endif

#include <cstring>
#include <regex>
#include <limits>

#if defined(COMPILER_MSVC)
#pragma optimize("", on)
#endif

struct GeometryParams
{
	Math::Vec4 posScale;
	Math::Vec4 posOffset;
};

// Utility code pulled from model converter.
namespace
{
	class BinaryStream
	{
	public:
		BinaryStream() {}

		~BinaryStream() {}

		const i32 GROW_ALIGNMENT = 1024 * 1024;

		void GrowAmount(i32 amount)
		{
			i32 minSize = Core::PotRoundUp(offset_ + amount, GROW_ALIGNMENT);
			if(minSize > data_.size())
				data_.resize(minSize * 2);
		}

		void Write(const void* data, i32 bytes)
		{
			GrowAmount(bytes);
			DBG_ASSERT(offset_ + bytes <= data_.size());
			memcpy(data_.data() + offset_, data, bytes);
			offset_ += bytes;
		}

		template<typename TYPE>
		void Write(const TYPE& data)
		{
			Write(&data, sizeof(data));
		}

		const void* Data() const { return data_.data(); }
		i32 Size() const { return offset_; }

	private:
		i32 offset_ = 0;
		Core::Vector<u8> data_;
	};

	bool GetInStreamDesc(Core::StreamDesc& outDesc, GPU::VertexUsage usage)
	{
		switch(usage)
		{
		case GPU::VertexUsage::POSITION:
		case GPU::VertexUsage::NORMAL:
		case GPU::VertexUsage::TEXCOORD:
		case GPU::VertexUsage::TANGENT:
		case GPU::VertexUsage::BINORMAL:
			outDesc.dataType_ = Core::DataType::FLOAT;
			outDesc.numBits_ = 32;
			outDesc.stride_ = 3 * sizeof(f32);
			break;
		case GPU::VertexUsage::BLENDWEIGHTS:
		case GPU::VertexUsage::BLENDINDICES:
		case GPU::VertexUsage::COLOR:
			outDesc.dataType_ = Core::DataType::FLOAT;
			outDesc.numBits_ = 32;
			outDesc.stride_ = 4 * sizeof(f32);
			break;
		default:
			return false;
		}
		return outDesc.numBits_ > 0;
	}

	bool GetOutStreamDesc(Core::StreamDesc& outDesc, GPU::Format format)
	{
		auto formatInfo = GPU::GetFormatInfo(format);
		outDesc.dataType_ = formatInfo.rgbaFormat_;
		outDesc.numBits_ = formatInfo.rBits_;
		outDesc.stride_ = formatInfo.blockBits_ >> 3;
		return outDesc.numBits_ > 0;
	}

	Core::Mutex assimpMutex_;

	/**
	 * Assimp logging function.
	 */
	void AssimpLogStream(const char* message, char* user)
	{
		if(strstr(message, "Error") != nullptr || strstr(message, "Warning") != nullptr)
		{
			DBG_LOG("ASSIMP: %s", message);
		}
	}

	/**
	 * Determine material name.
	 */
	Core::String AssimpGetMaterialName(aiMaterial* material)
	{
		aiString aiName("default");
		// Try material name.
		if(material->Get(AI_MATKEY_NAME, aiName) == aiReturn_SUCCESS)
		{
		}
		// Try diffuse texture.
		else if(material->Get(AI_MATKEY_TEXTURE(aiTextureType_DIFFUSE, 0), aiName) == aiReturn_SUCCESS)
		{
		}
		return aiName.C_Str();
	}

	/**
	 * Fill next element that is less than zero.
	 * Will check elements until first one less than 0.0 is found and overwrite it.
	 */
	i32 FillNextElementLessThanZero(f32 value, f32* elements, i32 noofElements)
	{
		for(i32 idx = 0; idx < noofElements; ++idx)
		{
			if(elements[idx] < 0.0f)
			{
				elements[idx] = value;
				return idx;
			}
		}

		return -1;
	}

	/**
	 * Fill all elements less than zero with specific value.
	 */
	void FillAllElementsLessThanZero(f32 value, f32* elements, i32 noofElements)
	{
		for(i32 idx = 0; idx < noofElements; ++idx)
		{
			if(elements[idx] < 0.0f)
			{
				elements[idx] = value;
			}
		}
	}

#if ENABLE_SIMPLYGON
	SimplygonSDK::ISimplygonSDK* GetSimplygon()
	{
		SimplygonSDK::ISimplygonSDK* sdk = nullptr;
		auto sgLib = Core::LibraryOpen("SimplygonSDKRuntimeReleasex64.dll");
		if(sgLib)
		{
			typedef void (*GetInterfaceVersionSimplygonSDKPtr)(char*);
			GetInterfaceVersionSimplygonSDKPtr GetInterfaceVersionSimplygonSDK =
			    (GetInterfaceVersionSimplygonSDKPtr)Core::LibrarySymbol(sgLib, "GetInterfaceVersionSimplygonSDK");

			typedef int (*InitializeSimplygonSDKPtr)(
			    const char* licenseData, SimplygonSDK::ISimplygonSDK** outInterfacePtr);
			InitializeSimplygonSDKPtr InitializeSimplygonSDK =
			    (InitializeSimplygonSDKPtr)Core::LibrarySymbol(sgLib, "InitializeSimplygonSDK");

			if(GetInterfaceVersionSimplygonSDK == nullptr || InitializeSimplygonSDK == nullptr)
				return nullptr;

			char versionHash[200];
			GetInterfaceVersionSimplygonSDK(versionHash);
			if(strcmp(versionHash, SimplygonSDK::GetInterfaceVersionHash()) != 0)
			{
				DBG_LOG(
				    "Library version mismatch. Header=%s Lib=%s", SimplygonSDK::GetInterfaceVersionHash(), versionHash);
				return nullptr;
			}

			const char* licenseData = nullptr;
			Core::Vector<u8> licenseFile;
			if(auto file = Core::File("../../../../res/simplygon_license.xml", Core::FileFlags::READ))
			{
				licenseFile.resize((i32)file.Size());
				file.Read(licenseFile.data(), file.Size());
				licenseData = (const char*)licenseFile.data();
			}


			i32 result = InitializeSimplygonSDK(licenseData, &sdk);
			if(result != SimplygonSDK::SG_ERROR_NOERROR && result != SimplygonSDK::SG_ERROR_ALREADYINITIALIZED)
			{
				DBG_LOG("Failed to initialize Simplygon. Error: %d.", result);
				sdk = nullptr;
			}

			return sdk;
		}
		return nullptr;
	}
#endif

	Graphics::MaterialRef GetMaterial(Core::String sourceFile, aiMaterial* material)
	{
		Core::UUID retVal;

		// Grab material name.
		auto materialName = AssimpGetMaterialName(material);

		// Find material file name.
		Core::Array<char, Core::MAX_PATH_LENGTH> materialPath = {0};
		Core::Array<char, Core::MAX_PATH_LENGTH> sourceName = {0};
		Core::Array<char, Core::MAX_PATH_LENGTH> sourceExt = {0};
		Core::Array<char, Core::MAX_PATH_LENGTH> origMaterialPath = {0};
		Core::FileSplitPath(sourceFile.c_str(), materialPath.data(), materialPath.size(), sourceName.data(),
		    sourceName.size(), sourceExt.data(), sourceExt.size());
		strcat_s(materialPath.data(), materialPath.size(), "/materials/");
		Core::FileCreateDir(materialPath.data());

		strcat_s(materialPath.data(), materialPath.size(), sourceName.data());
		strcat_s(materialPath.data(), materialPath.size(), ".");
		strcat_s(materialPath.data(), materialPath.size(), sourceExt.data());
		strcat_s(materialPath.data(), materialPath.size(), ".");
		strcat_s(materialPath.data(), materialPath.size(), materialName.c_str());
		strcat_s(materialPath.data(), materialPath.size(), ".material");

		return materialPath.data();
	}

} // namespace

namespace MeshTools
{
	// http://www.forceflow.be/2013/10/07/morton-encodingdecoding-through-bit-interleaving-implementations/
	// method to seperate bits from a given integer 3 positions apart
	inline uint64_t splitBy3(unsigned int a)
	{
		uint64_t x = a & 0x1fffff; // we only look at the first 21 bits
		x = (x | x << 32) &
		    0x1f00000000ffff; // shift left 32 bits, OR with self, and 00011111000000000000000000000000000000001111111111111111
		x = (x | x << 16) &
		    0x1f0000ff0000ff; // shift left 32 bits, OR with self, and 00011111000000000000000011111111000000000000000011111111
		x = (x | x << 8) &
		    0x100f00f00f00f00f; // shift left 32 bits, OR with self, and 0001000000001111000000001111000000001111000000001111000000000000
		x = (x | x << 4) &
		    0x10c30c30c30c30c3; // shift left 32 bits, OR with self, and 0001000011000011000011000011000011000011000011000011000100000000
		x = (x | x << 2) & 0x1249249249249249;
		return x;
	}

	inline uint64_t MortonEncode(unsigned int x, unsigned int y, unsigned int z)
	{
		uint64_t answer = 0;
		answer |= splitBy3(x) | splitBy3(y) << 1 | splitBy3(z) << 2;
		return answer;
	}

	struct Vertex
	{
		Math::Vec3 position_;
		Math::Vec3 normal_;
		Math::Vec3 tangent_;
		Math::Vec2 texcoord_;
		Math::Vec4 color_;

		u32 hash_ = 0;
		void Initialize()
		{
			hash_ = 0;
			hash_ = Core::HashCRC32(hash_, &position_, sizeof(position_));
			hash_ = Core::HashCRC32(hash_, &normal_, sizeof(normal_));
			hash_ = Core::HashCRC32(hash_, &tangent_, sizeof(tangent_));
			hash_ = Core::HashCRC32(hash_, &texcoord_, sizeof(texcoord_));
			hash_ = Core::HashCRC32(hash_, &color_, sizeof(color_));
		}

		bool operator==(const Vertex& other) const
		{
			if(hash_ != other.hash_)
				return false;
			if(position_ != other.position_)
				return false;
			if(normal_ != other.normal_)
				return false;
			if(tangent_ != other.tangent_)
				return false;
			if(texcoord_ != other.texcoord_)
				return false;
			if(color_ != other.color_)
				return false;
			return true;
		}

		u64 SortKey(Math::AABB bounds) const
		{
			Math::Vec3 position = position_;
			position = (position - bounds.Minimum()) / bounds.Dimensions();
			f32 scaleFactor = 0x1fffff; // 21 bits x 3 = 63 bits.
			u32 x = (u32)(position.x * scaleFactor);
			u32 y = (u32)(position.y * scaleFactor);
			u32 z = (u32)(position.z * scaleFactor);
			//u64 s = (u64)((triBounds.Diameter() / bounds.Diameter()) * 4);

			return MortonEncode(x, y, z);
		}
	};

	struct Triangle
	{
		Triangle() = default;

		Triangle(i32 a, i32 b, i32 c)
		{
			idx_[0] = a;
			idx_[1] = b;
			idx_[2] = c;
		}

		u64 SortKey(Core::ArrayView<Vertex> vertices, Math::AABB bounds) const
		{
			auto a = vertices[idx_[0]];
			auto b = vertices[idx_[1]];
			auto c = vertices[idx_[2]];

			Math::AABB triBounds;
			triBounds.ExpandBy(a.position_);
			triBounds.ExpandBy(b.position_);
			triBounds.ExpandBy(c.position_);

			Math::Vec3 position = (a.position_ + b.position_ + c.position_) / 3.0f;
			position = (position - bounds.Minimum()) / bounds.Dimensions();
			f32 scaleFactor = 0xff; //0x1fffff; // 21 bits x 3 = 63 bits.
			u32 x = (u32)(position.x * scaleFactor);
			u32 y = (u32)(position.y * scaleFactor);
			u32 z = (u32)(position.z * scaleFactor);
			//u64 s = (u64)((triBounds.Diameter() / bounds.Diameter()) * 4);

			return MortonEncode(x, y, z);
		}

		i32 idx_[3];
	};


	static constexpr i32 BLOCK_SIZE = 4;
	static constexpr i32 BLOCK_TEXELS = BLOCK_SIZE * BLOCK_SIZE;

	struct OctTree
	{
		struct NodeLinks
		{
			i32 c_ = -1;
			bool IsLeaf() const { return c_ == -1; }
		};

		struct NodeData
		{
			Math::AABB bounds_;

			Core::Array<Math::Vec3, BLOCK_TEXELS> points_;
			Core::Array<i32, BLOCK_TEXELS> indices_;
			i32 numPoints_ = 0;

			bool AddPoint(Math::Vec3 point)
			{
				if(numPoints_ < points_.size())
				{
					points_[numPoints_++] = point;
					return true;
				}
				return false;
			}
		};

		Core::Vector<NodeLinks> nodeLinks_;
		Core::Vector<NodeData> nodeDatas_;
		i32 numNodes_ = 0;

		i32 NewNodes(i32 numNodes)
		{
			i32 idx = numNodes_;
			numNodes_ += numNodes;

			// Double size.
			if(numNodes_ > nodeDatas_.size())
			{
				nodeDatas_.resize(numNodes_ * 2);
				nodeLinks_.resize(numNodes_ * 2);
			}

			return idx;
		}

		void CreateRoot(Math::AABB bounds)
		{
			i32 idx = NewNodes(1);
			auto& data = nodeDatas_[idx];
			data.bounds_ = bounds;
		}

		void Subdivide(i32 idx)
		{
			i32 baseIdx = NewNodes(8);

			auto& data = nodeDatas_[idx];
			auto& links = nodeLinks_[idx];

			Math::Vec3 center = data.bounds_.Centre();

			links.c_ = baseIdx;

			i32 pointsMoved = 0;
			u64 pointBits = 0xffffffffffffffffULL;
			for(i32 i = 0; i < 8; ++i)
			{
				auto& cData = nodeDatas_[i + baseIdx];
				Math::AABB bounds;
				bounds.ExpandBy(center);
				bounds.ExpandBy(data.bounds_.Corner(i));
				cData.bounds_ = bounds;
				cData.numPoints_ = 0;

				for(i32 j = 0; j < data.numPoints_; ++j)
				{
					auto point = data.points_[j];
					u64 pointBit = (1ULL << j);
					if((pointBit & pointBits) != 0)
					{
						if(bounds.Classify(point) == Math::AABB::INSIDE)
						{
							bool success = cData.AddPoint(point);
							DBG_ASSERT(success);
							pointsMoved++;

							pointBits &= ~pointBit;
						}
					}
				}
			}

			DBG_ASSERT(pointsMoved == data.numPoints_);
			data.numPoints_ = 0;
		}

		i32 FindNode(Math::Vec3 point) const
		{
			i32 idx = 0;
			i32 oldIdx = 0;
			while(nodeLinks_[idx].IsLeaf() == false)
			{
				oldIdx = idx;
				for(i32 i = 0; i < 8; ++i)
				{
					const i32 cIdx = i + nodeLinks_[idx].c_;
					const auto& data = nodeDatas_[cIdx];
					if(data.bounds_.Classify(point) == Math::AABB::INSIDE)
					{
						idx = cIdx;
						break;
					}
				}

				DBG_ASSERT(oldIdx != idx);
			}
			return idx;
		}

		i32 FindIndex(Math::Vec3 point) const
		{
			i32 nodeIdx = FindNode(point);
			const auto& data = nodeDatas_[nodeIdx];

			i32 nearestIdx = 0;
			f32 diff = 1e6f;
			for(i32 i = 0; i < data.numPoints_; ++i)
			{
				f32 calcDiff = (point - data.points_[i]).Magnitude();
				if(calcDiff < diff)
				{
					nearestIdx = i;
					diff = calcDiff;
				}
			}
			return data.indices_[nearestIdx];
		}

		void AddPoint(Math::Vec3 point)
		{
			bool added = false;
			i32 tries = 0;
			do
			{
				DBG_ASSERT(tries < 16);
				i32 idx = FindNode(point);
				added = nodeDatas_[idx].AddPoint(point);
				if(!added)
					Subdivide(idx);

				++tries;
			} while(!added);
		}
	};


	struct Mesh
	{
		Mesh() = default;

		Core::Vector<Vertex> vertices_;
		Core::Vector<u32> vertexHashes_;
		Core::Vector<Triangle> triangles_;
		Math::AABB bounds_;

		void AddFace(Vertex a, Vertex b, Vertex c)
		{
			auto AddVertex = [this](Vertex a) -> i32 {
				i32 idx = vertices_.size();
				auto it = std::find_if(vertexHashes_.begin(), vertexHashes_.end(), [&a, this](const u32& b) {
					if(a.hash_ == b)
					{
						i32 idx = (i32)(&b - vertexHashes_.begin());
						return a == vertices_[idx];
					}
					return false;
				});

				if(it != vertexHashes_.end())
					return (i32)(it - vertexHashes_.begin());
				vertices_.push_back(a);
				vertexHashes_.push_back(a.hash_);
				return idx;
			};

			bounds_.ExpandBy(a.position_);
			bounds_.ExpandBy(b.position_);
			bounds_.ExpandBy(c.position_);
			auto ia = AddVertex(a);
			auto ib = AddVertex(b);
			auto ic = AddVertex(c);
			triangles_.emplace_back(ia, ib, ic);
		}

		void ImportAssimpMesh(const aiMesh* mesh)
		{
			vertices_.reserve(mesh->mNumFaces * 3);
			vertexHashes_.reserve(mesh->mNumFaces * 3);
			triangles_.reserve(mesh->mNumFaces);

			for(unsigned int i = 0; i < mesh->mNumVertices; ++i)
			{
				auto GetVertex = [mesh](int idx) {
					Vertex v = {};
					v.position_ = &mesh->mVertices[idx].x;
					if(mesh->mNormals)
						v.normal_ = &mesh->mNormals[idx].x;
					if(mesh->mTangents)
						v.tangent_ = &mesh->mTangents[idx].x;
					if(mesh->mTextureCoords[0])
						v.texcoord_ = &mesh->mTextureCoords[0][idx].x;
					if(mesh->mColors[0])
						v.color_ = &mesh->mColors[0][idx].r;
					else
						v.color_ = Math::Vec4(1.0f, 1.0f, 1.0f, 1.0f);
					v.Initialize();
					return v;
				};

				Vertex v = GetVertex(i);
				bounds_.ExpandBy(v.position_);

				vertices_.push_back(v);
				vertexHashes_.push_back(v.hash_);
			}

			for(unsigned int i = 0; i < mesh->mNumFaces; ++i)
			{
				// Skip anything that isn't a triangle.
				if(mesh->mFaces[i].mNumIndices == 3)
				{
					int ia = mesh->mFaces[i].mIndices[0];
					int ib = mesh->mFaces[i].mIndices[1];
					int ic = mesh->mFaces[i].mIndices[2];

					triangles_.emplace_back(ia, ib, ic);
				}
			}
		}

		void SortTriangles()
		{
			std::sort(triangles_.begin(), triangles_.end(), [this](const Triangle& a, const Triangle& b) {
				auto aKey = a.SortKey(Core::ArrayView<Vertex>(vertices_.begin(), vertices_.end()), bounds_);
				auto bKey = b.SortKey(Core::ArrayView<Vertex>(vertices_.begin(), vertices_.end()), bounds_);
				return aKey < bKey;
			});
		}

		void ReorderIndices()
		{
			Core::Vector<Vertex> oldVertices;
			Core::Vector<Triangle> oldTriangles;
			std::swap(oldVertices, vertices_);
			std::swap(oldTriangles, triangles_);

			std::sort(triangles_.begin(), triangles_.end(), [this](const Triangle& a, const Triangle& b) {
				auto aKey = a.SortKey(Core::ArrayView<Vertex>(vertices_.begin(), vertices_.end()), bounds_);
				auto bKey = b.SortKey(Core::ArrayView<Vertex>(vertices_.begin(), vertices_.end()), bounds_);
				return aKey < bKey;
			});

			struct VtxIdx
			{
				Vertex vtx;
				u32 idx;
			};

			Core::Vector<VtxIdx> vtxIdx;
			for(u32 idx = 0; idx < (u32)oldVertices.size(); ++idx)
			{
				VtxIdx pair = {oldVertices[idx], idx};
				vtxIdx.push_back(pair);
			}

			std::sort(vtxIdx.begin(), vtxIdx.end(), [&](const VtxIdx& a, const VtxIdx& b) {
				auto aKey = a.vtx.SortKey(bounds_);
				auto bKey = b.vtx.SortKey(bounds_);
				return aKey < bKey;
			});


			Core::Map<u32, u32> remap;
			u32 newIdx = 0;
			for(const auto& vi : vtxIdx)
			{
				remap.insert(vi.idx, newIdx++);
			}

			// Readd all the vertices.
			for(const auto& vi : vtxIdx)
			{
				vertices_.push_back(vi.vtx);
			}

			for(const auto& tri : oldTriangles)
			{
				auto newTri = tri;
				newTri.idx_[0] = remap[newTri.idx_[0]];
				newTri.idx_[1] = remap[newTri.idx_[1]];
				newTri.idx_[2] = remap[newTri.idx_[2]];
				triangles_.push_back(newTri);
			}
		}

		void ReorderIndices(const OctTree& octtree, i32 numVertices)
		{
			Core::Vector<Vertex> oldVertices;
			Core::Vector<Triangle> oldTriangles;
			std::swap(oldVertices, vertices_);
			std::swap(oldTriangles, triangles_);

			vertices_.resize(numVertices);

			for(const auto& tri : oldTriangles)
			{
				const auto& a = oldVertices[tri.idx_[0]];
				const auto& b = oldVertices[tri.idx_[1]];
				const auto& c = oldVertices[tri.idx_[2]];

				auto ia = octtree.FindIndex(a.position_);
				auto ib = octtree.FindIndex(b.position_);
				auto ic = octtree.FindIndex(c.position_);

				vertices_[ia] = oldVertices[tri.idx_[0]];
				vertices_[ib] = oldVertices[tri.idx_[1]];
				vertices_[ic] = oldVertices[tri.idx_[2]];

				Triangle newTri;
				newTri.idx_[0] = ia;
				newTri.idx_[1] = ib;
				newTri.idx_[2] = ic;

				triangles_.push_back(newTri);
			}
		}

		void ImportMeshCluster(Mesh* mesh, i32 firstTri, i32 numTris)
		{
			if(firstTri >= mesh->triangles_.size())
				DBG_ASSERT(false);

			for(i32 i = firstTri; i < (firstTri + numTris); ++i)
			{
				if(i < mesh->triangles_.size())
				{
					auto tri = mesh->triangles_[i];
					AddFace(mesh->vertices_[tri.idx_[0]], mesh->vertices_[tri.idx_[1]], mesh->vertices_[tri.idx_[2]]);
				}
				else
				{
					// Patch up with degenerates.
					AddFace(vertices_[0], vertices_[0], vertices_[0]);
				}
			}
		}
	};

	struct Texture
	{
		Core::Vector<Math::Vec4> texels_;
		i32 width_ = 0;
		i32 height_ = 0;

		void Initialize(i32 numIndices)
		{
			i32 minSize = (i32)ceil(sqrt((f32)numIndices));
			width_ = Core::PotRoundUp(minSize, 4);
			height_ = Core::PotRoundUp(minSize, 4);
			texels_.resize(width_ * height_);
		}

		GPU::Handle Create(GPU::Format format, Core::StreamDesc outStream, const char* debugName)
		{
			GPU::TextureDesc desc = {};
			desc.type_ = GPU::TextureType::TEX2D;
			desc.bindFlags_ = GPU::BindFlags::SHADER_RESOURCE;
			desc.width_ = width_;
			desc.height_ = height_;
			desc.format_ = format;

			auto size = GPU::GetTextureSize(format, width_, height_, 1, 1, 1);
			auto footprint = GPU::GetTextureFootprint(format, width_, height_);
			auto formatInfo = GPU::GetFormatInfo(format);

			Core::Vector<u8> uploadData((i32)size);

			Core::StreamDesc inStream(texels_.data(), Core::DataType::FLOAT, 32, sizeof(Math::Vec4));
			outStream.data_ = uploadData.data();
			Core::Convert(outStream, inStream, texels_.size(), outStream.stride_ / (outStream.numBits_ / 8));

			GPU::ConstTextureSubResourceData subRscData;
			subRscData.data_ = uploadData.data();
			subRscData.rowPitch_ = footprint.rowPitch_;
			subRscData.slicePitch_ = footprint.slicePitch_;

			return GPU::Manager::CreateTexture(desc, &subRscData, debugName);
		}

		GPU::Handle Create(GPU::Format format, const char* debugName)
		{
			GPU::TextureDesc desc = {};
			desc.type_ = GPU::TextureType::TEX2D;
			desc.bindFlags_ = GPU::BindFlags::SHADER_RESOURCE;
			desc.width_ = width_;
			desc.height_ = height_;
			desc.format_ = format;

			bool retVal = false;
			Image::Image inputImage(GPU::TextureType::TEX2D, GPU::Format::R32G32B32A32_FLOAT, width_, height_, 1, 1,
			    (u8*)texels_.data(), [](u8*) {});

			Image::Image intImage;
			retVal = Image::Convert(intImage, inputImage, Image::ImageFormat::R8G8B8A8_UNORM);
			DBG_ASSERT(retVal);

			Image::Image outImage;
			retVal = Image::Convert(outImage, intImage, format, Image::ConvertQuality::VERY_HIGH);
			DBG_ASSERT(retVal);

			const auto footprint = GPU::GetTextureFootprint(format, width_, height_);

			GPU::ConstTextureSubResourceData subRscData;
			subRscData.data_ = outImage.GetMipData<u8>(0);
			subRscData.rowPitch_ = footprint.rowPitch_;
			subRscData.slicePitch_ = footprint.slicePitch_;

			return GPU::Manager::CreateTexture(desc, &subRscData, debugName);
		}

		void Set(i32 idx, Math::Vec4 v) { texels_[idx] = v; }

		Math::Vec4 Get(i32 idx) const { return texels_[idx]; }
	};

	// Normal encoding methods grabbed from https://aras-p.info/texts/CompactNormalStorage.html
	Math::Vec2 EncodeSpherical(Math::Vec3 n)
	{
		Math::Vec2 o(0.0f, 0.0f);
		o.x = std::atan2(n.y, n.x) / Core::F32_PI;
		o.y = n.z;
		return (o + Math::Vec2(1.0f, 1.0f)) * 0.5f;
	}

	Math::Vec3 DecodeSpherical(Math::Vec2 enc)
	{
		Math::Vec2 ang = enc * 2.0f - Math::Vec2(1.0f, 1.0f);
		Math::Vec2 scth;
		scth.x = std::sin(ang.x * Core::F32_PI);
		scth.y = std::cos(ang.y * Core::F32_PI);
		Math::Vec2 scphi = Math::Vec2(std::sqrt(1.0f - ang.y * ang.y), ang.y);
		return Math::Vec3(scth.y * scphi.x, scth.x * scphi.x, scphi.y);
	}

	Math::Vec2 EncodeSMT(Math::Vec3 n)
	{
		Math::Vec2 n2(n.x, n.y);
		Math::Vec2 enc = n2.Normal() * std::sqrt(-n.z * 0.5f + 0.5f);
		enc = enc * 0.5f + Math::Vec2(0.5f, 0.5f);
		enc.x = Core::Clamp(enc.x, 0.0f, 1.0f);
		enc.y = Core::Clamp(enc.y, 0.0f, 1.0f);
		return enc;
	}

	Math::Vec3 DecodeSMT(Math::Vec2 enc)
	{
		Math::Vec4 nn = Math::Vec4(enc) * Math::Vec4(2, 2, 0, 0) + Math::Vec4(-1.0f, -1.0f, 1.0f, -1.0f);
		Math::Vec3 n1(nn.x, nn.y, nn.z);
		Math::Vec3 n2(-nn.x, -nn.y, -nn.w);
		f32 l = n1.Dot(n2);
		nn.z = l;
		nn.x *= std::sqrt(l);
		nn.y *= std::sqrt(l);
		return Math::Vec3(nn.x, nn.y, nn.z) * 2.0f + Math::Vec3(0.0f, 0.0f, -1.0f);
	}

	Math::Vec3 EncodeYCoCg(Math::Vec3 rgb)
	{
		return Math::Vec3(rgb.Dot(Math::Vec3(0.25f, 0.5f, 0.25f)), rgb.Dot(Math::Vec3(0.5f, 0.0f, -0.5f)),
		    rgb.Dot(Math::Vec3(-0.25f, 0.5f, -0.25f)));
	}

	Math::Vec3 DecodeYCoCg(Math::Vec3 ycocg)
	{
		return Math::Vec3(ycocg.x + ycocg.y - ycocg.z, ycocg.x + ycocg.z, ycocg.x - ycocg.y - ycocg.z);
	}

	Math::Vec4 EncodeYCoCg(Math::Vec4 rgba)
	{
		Math::Vec3 rgb(rgba.x, rgba.y, rgba.z);
		return Math::Vec4(rgb.Dot(Math::Vec3(0.25f, 0.5f, 0.25f)), rgb.Dot(Math::Vec3(0.5f, 0.0f, -0.5f)),
		    rgb.Dot(Math::Vec3(-0.25f, 0.5f, -0.25f)), rgba.w);
	}

	Math::Vec4 DecodeYCoCg(Math::Vec4 ycocg)
	{
		return Math::Vec4(ycocg.x + ycocg.y - ycocg.z, ycocg.x + ycocg.z, ycocg.x - ycocg.y - ycocg.z, ycocg.w);
	}

#if ENABLE_SIMPLYGON
	SimplygonSDK::spGeometryData CreateSGGeometry(SimplygonSDK::ISimplygonSDK* sg, const Mesh* mesh)
	{
		using namespace SimplygonSDK;

		spGeometryData geom = sg->CreateGeometryData();

		geom->SetVertexCount(mesh->vertices_.size());
		geom->SetTriangleCount(mesh->triangles_.size());
		geom->AddMaterialIds();
		geom->AddNormals();
		geom->AddTangents(0);
		geom->AddTexCoords(0);
		geom->AddColors(0);

		auto positions = geom->GetCoords();
		auto normals = geom->GetNormals();
		auto tangents = geom->GetTangents(0);
		auto texcoords = geom->GetTexCoords(0);
		auto colors = geom->GetColors(0);
		auto vertexIds = geom->GetVertexIds();
		auto materialIds = geom->GetMaterialIds();

		DBG_ASSERT(positions->GetTupleSize() == 3);
		DBG_ASSERT(normals->GetTupleSize() == 3);
		DBG_ASSERT(tangents->GetTupleSize() == 3);
		DBG_ASSERT(texcoords->GetTupleSize() == 2);
		DBG_ASSERT(colors->GetTupleSize() == 4);
		DBG_ASSERT(vertexIds->GetTupleSize() == 1);
		DBG_ASSERT(materialIds->GetTupleSize() == 1);

		for(i32 idx = 0; idx < mesh->vertices_.size(); ++idx)
		{
			auto vertex = mesh->vertices_[idx];
			positions->SetTuple(idx, (const f32*)&vertex.position_);
			normals->SetTuple(idx, (const f32*)&vertex.normal_);
			tangents->SetTuple(idx, (const f32*)&vertex.tangent_);
			texcoords->SetTuple(idx, (const f32*)&vertex.texcoord_);
			colors->SetTuple(idx, (const f32*)&vertex.color_);
		}

		for(i32 idx = 0; idx < mesh->triangles_.size(); ++idx)
		{
			vertexIds->SetTuple(idx * 3 + 0, &mesh->triangles_[idx].idx_[0]);
			vertexIds->SetTuple(idx * 3 + 1, &mesh->triangles_[idx].idx_[1]);
			vertexIds->SetTuple(idx * 3 + 2, &mesh->triangles_[idx].idx_[2]);
		}

		for(i32 idx = 0; idx < mesh->triangles_.size(); ++idx)
		{
			materialIds->SetItem(idx, 0);
		}

		return geom;
	}

	SimplygonSDK::spScene CreateSGScene(SimplygonSDK::ISimplygonSDK* sg, Core::ArrayView<Mesh*> meshes)
	{
		using namespace SimplygonSDK;
		spScene scene = sg->CreateScene();

		for(const auto* mesh : meshes)
		{
			spSceneMesh sceneMesh = sg->CreateSceneMesh();

			sceneMesh->SetGeometry(CreateSGGeometry(sg, mesh));

			scene->GetRootNode()->AddChild(sceneMesh);
		}

		return scene;
	}

	Mesh* CreateMesh(SimplygonSDK::ISimplygonSDK* sg, SimplygonSDK::spSceneMesh sceneMesh)
	{
		using namespace SimplygonSDK;

		Mesh* mesh = new Mesh();
		spGeometryData geom = sceneMesh->GetGeometry();

		auto positions = geom->GetCoords();
		auto normals = geom->GetNormals();
		auto tangents = geom->GetTangents(0);
		auto texcoords = geom->GetTexCoords(0);
		auto colors = geom->GetColors(0);
		auto vertexIds = geom->GetVertexIds();
		auto materialIds = geom->GetMaterialIds();

		mesh->vertices_.resize(geom->GetVertexCount());
		mesh->triangles_.resize(geom->GetTriangleCount());

		auto GetVec2 = [](
		    spRealArray arr, i32 idx) { return Math::Vec2(arr->GetItem(idx * 2), arr->GetItem(idx * 2 + 1)); };

		auto GetVec3 = [](spRealArray arr, i32 idx) {
			return Math::Vec3(arr->GetItem(idx * 3), arr->GetItem(idx * 3 + 1), arr->GetItem(idx * 3 + 2));
		};

		auto GetVec4 = [](spRealArray arr, i32 idx) {
			return Math::Vec4(
			    arr->GetItem(idx * 4), arr->GetItem(idx * 4 + 1), arr->GetItem(idx * 4 + 2), arr->GetItem(idx * 4 + 3));
		};

		for(i32 idx = 0; idx < mesh->vertices_.size(); ++idx)
		{
			auto& vertex = mesh->vertices_[idx];

			vertex.position_ = GetVec3(positions, idx);
			vertex.normal_ = GetVec3(normals, idx);
			vertex.tangent_ = GetVec3(tangents, idx);
			vertex.texcoord_ = GetVec2(texcoords, idx);
			vertex.color_ = GetVec4(colors, idx);

			mesh->bounds_.ExpandBy(vertex.position_);
		}

		for(i32 idx = 0; idx < mesh->vertices_.size(); ++idx)
		{
			auto& vertex = mesh->vertices_[idx];
			vertex.Initialize();
		}

		for(i32 idx = 0; idx < mesh->triangles_.size(); ++idx)
		{
			auto& triangle = mesh->triangles_[idx];

			triangle.idx_[0] = vertexIds->GetItem(idx * 3 + 0);
			triangle.idx_[1] = vertexIds->GetItem(idx * 3 + 1);
			triangle.idx_[2] = vertexIds->GetItem(idx * 3 + 2);
		}

		return mesh;
	}

	Mesh* ReduceMesh(SimplygonSDK::ISimplygonSDK* sg, Mesh* mesh, f32 ratio)
	{
		auto sgScene = MeshTools::CreateSGScene(sg, mesh);
		auto rp = sg->CreateReductionProcessor();
		auto settings = rp->GetReductionSettings();

		settings->SetTriangleRatio(ratio);
		rp->SetScene(sgScene);

		rp->RunProcessing();

		for(i32 idx = 0; idx < (i32)sgScene->GetRootNode()->GetChildCount(); ++idx)
		{
			auto childNode = sgScene->GetRootNode()->GetChild(idx);
			if(auto* meshNode = SimplygonSDK::ISceneMesh::SafeCast(childNode))
			{
				return MeshTools::CreateMesh(sg, meshNode);
			}
		}
		return nullptr;
	}
#endif

} // namespace MeshTools

CompressedModel::CompressedModel(const char* sourceFile)
{
	const aiScene* scene = nullptr;

	Core::String fileName = "../../../../res/";
	fileName.Append(sourceFile);

	auto propertyStore = aiCreatePropertyStore();
	aiLogStream assimpLogger = {AssimpLogStream, (char*)this};
	{
		Core::ScopedMutex lock(assimpMutex_);
		aiAttachLogStream(&assimpLogger);

		int flags = aiProcess_Triangulate | aiProcess_GenUVCoords | aiProcess_FindDegenerates | aiProcess_SortByPType |
		            aiProcess_FindInvalidData | aiProcess_RemoveRedundantMaterials | aiProcess_SplitLargeMeshes |
		            aiProcess_GenSmoothNormals | aiProcess_ValidateDataStructure | aiProcess_SplitByBoneCount |
		            aiProcess_LimitBoneWeights | aiProcess_MakeLeftHanded | aiProcess_FlipUVs |
		            aiProcess_FlipWindingOrder | aiProcess_OptimizeGraph | aiProcess_OptimizeMeshes |
		            aiProcess_RemoveComponent;
		aiSetImportPropertyInteger(
		    propertyStore, AI_CONFIG_PP_RVC_FLAGS, aiComponent_ANIMATIONS | aiComponent_LIGHTS | aiComponent_CAMERAS);
		aiSetImportPropertyInteger(propertyStore, AI_CONFIG_PP_SLM_VERTEX_LIMIT, 256 * 1024);

		scene = aiImportFileExWithProperties(fileName.c_str(), flags, nullptr, propertyStore);

		aiReleasePropertyStore(propertyStore);
		aiDetachLogStream(&assimpLogger);
	}

	if(scene)
	{
		Core::Vector<MeshTools::Mesh*> meshes;
		i32 numVertices = 0;
		i32 numIndices = 0;

		// Create meshes.
		for(unsigned int i = 0; i < scene->mNumMeshes; ++i)
		{
			auto* mesh = new MeshTools::Mesh();
			meshes.push_back(mesh);
		}

		// Spin up jobs for all meshes to perform importing.
		Job::FunctionJob importJob("cluster_model_import",
		    [&scene, &meshes](i32 param) { meshes[param]->ImportAssimpMesh(scene->mMeshes[param]); });
		Job::Counter* counter = nullptr;
		importJob.RunMultiple(Job::Priority::LOW, 0, meshes.size() - 1, &counter);
		Job::Manager::WaitForCounter(counter, 0);

#if 0
		Job::FunctionJob sortJob("cluster_model_sort", [&meshes](i32 param) { meshes[param]->SortTriangles(); });
		sortJob.RunMultiple(0, meshes.size() - 1, &counter);
		Job::Manager::WaitForCounter(counter, 0);
#endif

		for(i32 i = 0; i < meshes.size(); ++i)
		{
			auto* mesh = meshes[i];

			Core::Vector<Math::Vec3> positions;
			Math::AABB bounds;
			for(const auto& vtx : mesh->vertices_)
			{
				Math::Vec3 scaledPos = vtx.position_;
				//scaledPos -= mesh->bounds_.Minimum();
				//scaledPos = scaledPos / (mesh->bounds_.Maximum() - mesh->bounds_.Minimum());

				bounds.ExpandBy(scaledPos);
				positions.push_back(scaledPos);
			}

			MeshTools::OctTree posTree;
			posTree.CreateRoot(bounds);
			for(const auto& pos : positions)
			{
				posTree.AddPoint(pos);
			}

			Core::Vector<i32> leafNodes;
			for(i32 idx = 0; idx < posTree.numNodes_; ++idx)
			{
				const auto& link = posTree.nodeLinks_[idx];
				const auto& data = posTree.nodeDatas_[idx];
				if(link.IsLeaf() && data.numPoints_ > 0)
				{
					leafNodes.push_back(idx);
				}
			}

			i32 minSize = (i32)ceil(sqrt((f32)leafNodes.size())) * MeshTools::BLOCK_SIZE;

			auto outImage = Image::Image(
			    GPU::TextureType::TEX2D, GPU::Format::R32G32B32A32_FLOAT, minSize, minSize, 1, 1, nullptr, nullptr);

			auto* outData = outImage.GetMipData<Math::Vec4>(0);
			for(i32 idx = 0; idx < leafNodes.size(); ++idx)
			{
				i32 blockX = idx % (minSize / MeshTools::BLOCK_SIZE);
				i32 blockY = idx / (minSize / MeshTools::BLOCK_SIZE);

				i32 nodeIdx = leafNodes[idx];
				const auto& link = posTree.nodeLinks_[nodeIdx];
				auto& data = posTree.nodeDatas_[nodeIdx];
				if(link.IsLeaf() && data.numPoints_ > 0)
				{
					for(i32 j = 0; j < MeshTools::BLOCK_TEXELS; ++j)
					{
						const i32 pIdx = Core::Min(j, data.numPoints_ - 1);
						const i32 x = (j % MeshTools::BLOCK_SIZE) + (blockX * MeshTools::BLOCK_SIZE);
						const i32 y = (j / MeshTools::BLOCK_SIZE) + (blockY * MeshTools::BLOCK_SIZE);

						const i32 texIdx = (y * minSize) + x;
						data.indices_[pIdx] = texIdx;
						auto point = data.points_[pIdx];

						outData[texIdx] = Math::Vec4(point, 1.0f);
					}
				}
			}

#if 0
			Image::Image fileImage;

			Image::Convert(fileImage, outImage, Image::ImageFormat::R8G8B8A8_UNORM);

			if(auto file = Core::File("packed_positions.png", Core::FileFlags::DEFAULT_WRITE))
				Image::Save(file, fileImage, Image::FileType::PNG);
#endif

			mesh->ReorderIndices(posTree, minSize * minSize);

			//auto material = GetMaterial(sourceFile, scene->mMaterials[scene->mMeshes[i]->mMaterialIndex]);
			//if(!material)
			Graphics::MaterialRef material = "default.material";
			materials_.emplace_back(std::move(material));

			material = "default_compressed.material";
			compressedMaterials_.emplace_back(std::move(material));

			MeshTools::Texture packedPositionTex;
			MeshTools::Texture packedNormalTex;
			MeshTools::Texture packedColorTex;
			packedPositionTex.Initialize(mesh->vertices_.size());
			packedNormalTex.Initialize(mesh->vertices_.size());
			packedColorTex.Initialize(mesh->vertices_.size());

			for(i32 idx = 0; idx < mesh->vertices_.size(); ++idx)
			{
				const auto& vtx = mesh->vertices_[idx];

				auto pos = outData[idx];
				Math::Vec3 scaledPos = pos.xyz();
				scaledPos -= mesh->bounds_.Minimum();
				scaledPos = scaledPos / (mesh->bounds_.Maximum() - mesh->bounds_.Minimum());

				// Alpha channel could encode a shared exponent, but that would break the ability to
				// interpolate.

				packedPositionTex.Set(idx, Math::Vec4(scaledPos, 1.0f));
				packedNormalTex.Set(idx, MeshTools::EncodeSMT(vtx.normal_));
				packedColorTex.Set(idx, MeshTools::EncodeYCoCg(vtx.color_));
			}

			bool useCompression = true;
			if(useCompression)
			{
				positionFmt_ = GPU::Format::BC7_UNORM;
				positionTex_.push_back(packedPositionTex.Create(positionFmt_, "PackedPositionTex"));
			}
			else
			{
				positionFmt_ = GPU::Format::R8G8B8A8_UNORM;
				Core::StreamDesc outStream(nullptr, Core::DataType::UNORM, 8, 4 * sizeof(u8));
				positionTex_.push_back(packedPositionTex.Create(positionFmt_, outStream, "PackedPositionTex"));
			}

			if(useCompression)
			{
				normalFmt_ = GPU::Format::BC5_UNORM;
				normalTex_.push_back(packedNormalTex.Create(normalFmt_, "PackedNormalTex"));
			}
			else
			{
				normalFmt_ = GPU::Format::R8G8_UNORM;
				Core::StreamDesc outStream(nullptr, Core::DataType::UNORM, 8, 2 * sizeof(u8));
				normalTex_.push_back(packedNormalTex.Create(normalFmt_, outStream, "PackedNormalTex"));
			}

			{
				GeometryParams params;
				params.posScale = Math::Vec4((mesh->bounds_.Maximum() - mesh->bounds_.Minimum()), 0.0f);
				params.posOffset = Math::Vec4(mesh->bounds_.Minimum(), 0.0f);

				GPU::BufferDesc desc;
				desc.bindFlags_ = GPU::BindFlags::CONSTANT_BUFFER;
				desc.size_ = sizeof(GeometryParams);
				paramsBuffer_.push_back(GPU::Manager::CreateBuffer(desc, &params, "GeometryParams"));
			}


			numIndices += mesh->triangles_.size() * 3;
			numVertices += mesh->vertices_.size();
		}

		// Setup vertex declaration.
		Core::Array<GPU::VertexElement, GPU::MAX_VERTEX_ELEMENTS> elements;
		i32 numElements = 0;
		i32 currStream = 0;

		// Vertex format.
		elements[numElements++] =
		    GPU::VertexElement(currStream, 0, GPU::Format::R32G32B32_FLOAT, GPU::VertexUsage::POSITION, 0);
		currStream++;

		elements[numElements++] =
		    GPU::VertexElement(currStream, 0, GPU::Format::R8G8B8A8_SNORM, GPU::VertexUsage::NORMAL, 0);

		elements[numElements++] =
		    GPU::VertexElement(currStream, 0, GPU::Format::R16G16_FLOAT, GPU::VertexUsage::TEXCOORD, 0);
		currStream++;

		elements[numElements++] =
		    GPU::VertexElement(currStream, 0, GPU::Format::R8G8B8A8_UNORM, GPU::VertexUsage::COLOR, 0);
		currStream++;

		// Calculate offsets per-stream.
		Core::Array<i32, GPU::MAX_VERTEX_STREAMS> offsets;
		offsets.fill(0);
		for(i32 elementIdx = 0; elementIdx < numElements; ++elementIdx)
		{
			auto& element(elements[elementIdx]);
			i32 size = GPU::GetFormatInfo(element.format_).blockBits_ / 8;
			element.offset_ = offsets[element.streamIdx_];
			offsets[element.streamIdx_] += size;
		}

		elements_.insert(elements.begin(), elements.begin() + numElements);

		Core::Array<BinaryStream, GPU::MAX_VERTEX_STREAMS> streams;
		BinaryStream idxStream;

		i32 indexOffset = 0;
		i32 vertexOffset = 0;
		for(const auto& mesh : meshes)
		{
			Mesh outMesh;
			outMesh.bounds_ = mesh->bounds_;
			outMesh.baseIndex_ = indexOffset;
			outMesh.baseVertex_ = vertexOffset;
			outMesh.numIndices_ = mesh->triangles_.size() * 3;
			meshes_.push_back(outMesh);

			for(i32 triIdx = 0; triIdx < mesh->triangles_.size(); ++triIdx)
			{
				auto tri = mesh->triangles_[triIdx];
				idxStream.Write(tri.idx_[0] + indexOffset);
				idxStream.Write(tri.idx_[1] + indexOffset);
				idxStream.Write(tri.idx_[2] + indexOffset);
			}

			for(i32 vtxStreamIdx = 0; vtxStreamIdx < GPU::MAX_VERTEX_STREAMS; ++vtxStreamIdx)
			{
				// Setup stream descs.
				const i32 stride = GPU::GetStride(elements.data(), numElements, vtxStreamIdx);
				if(stride > 0)
				{
					Core::Vector<u8> vertexData(stride * mesh->vertices_.size(), 0);
					Core::Vector<Core::StreamDesc> inStreamDescs;
					Core::Vector<Core::StreamDesc> outStreamDescs;
					Core::Vector<i32> numComponents;
					for(i32 elementIdx = 0; elementIdx < numElements; ++elementIdx)
					{
						const auto& element(elements[elementIdx]);
						if(element.streamIdx_ == vtxStreamIdx)
						{
							Core::StreamDesc inStreamDesc;
							if(GetInStreamDesc(inStreamDesc, element.usage_))
							{
								inStreamDesc.stride_ = sizeof(MeshTools::Vertex);
								switch(element.usage_)
								{
								case GPU::VertexUsage::POSITION:
									inStreamDesc.data_ = &mesh->vertices_.data()->position_;
									break;
								case GPU::VertexUsage::NORMAL:
									inStreamDesc.data_ = &mesh->vertices_.data()->normal_;
									break;
								case GPU::VertexUsage::TEXCOORD:
									inStreamDesc.data_ = &mesh->vertices_.data()->texcoord_;
									break;
								case GPU::VertexUsage::TANGENT:
									inStreamDesc.data_ = &mesh->vertices_.data()->tangent_;
									break;
								case GPU::VertexUsage::COLOR:
									inStreamDesc.data_ = &mesh->vertices_.data()->color_;
									break;
								default:
									DBG_ASSERT(false);
								}

								DBG_ASSERT(inStreamDesc.data_);

								Core::StreamDesc outStreamDesc;
								if(GetOutStreamDesc(outStreamDesc, element.format_))
								{
									outStreamDesc.data_ = vertexData.data() + element.offset_;

									numComponents.push_back(
									    Core::Min(inStreamDesc.stride_ / (inStreamDesc.numBits_ >> 3),
									        outStreamDesc.stride_ / (outStreamDesc.numBits_ >> 3)));

									outStreamDesc.stride_ = stride;

									inStreamDescs.push_back(inStreamDesc);
									outStreamDescs.push_back(outStreamDesc);
								}
							}
						}
					}

					for(i32 elementStreamIdx = 0; elementStreamIdx < inStreamDescs.size(); ++elementStreamIdx)
					{
						auto inStreamDesc = inStreamDescs[elementStreamIdx];
						auto outStreamDesc = outStreamDescs[elementStreamIdx];

						DBG_ASSERT(vertexData.size() >= (outStreamDesc.stride_ * (i32)mesh->vertices_.size()));
						auto retVal = Core::Convert(
						    outStreamDesc, inStreamDesc, mesh->vertices_.size(), numComponents[elementStreamIdx]);
						DBG_ASSERT_MSG(retVal, "Unable to convert stream.");
					}

					streams[vtxStreamIdx].Write(vertexData.data(), vertexData.size());
				}
			}

			indexOffset += mesh->triangles_.size() * 3;
		}

		BinaryStream vtxStream;

		// Create buffers.
		vertexDesc_.bindFlags_ = GPU::BindFlags::VERTEX_BUFFER;
		vertexDesc_.size_ = 0;
		for(i32 i = 0; i < currStream; ++i)
		{
			vertexDesc_.size_ += streams[i].Size();
			vtxStream.Write(streams[i].Data(), streams[i].Size());
		}

		vertexBuffer_ = GPU::Manager::CreateBuffer(vertexDesc_, vtxStream.Data(), "compressed_model_vb");

		indexDesc_.bindFlags_ = GPU::BindFlags::INDEX_BUFFER | GPU::BindFlags::SHADER_RESOURCE;
		indexDesc_.size_ = numIndices * 4;
		indexBuffer_ = GPU::Manager::CreateBuffer(indexDesc_, idxStream.Data(), "compressed_model_ib");

		GPU::DrawBindingSetDesc dbsDesc;
		i32 offset = 0;
		for(i32 streamIdx = 0; streamIdx < currStream; ++streamIdx)
		{
			const i32 stride = GPU::GetStride(elements.data(), numElements, streamIdx);
			dbsDesc.vbs_[streamIdx].resource_ = vertexBuffer_;
			dbsDesc.vbs_[streamIdx].offset_ = offset;
			dbsDesc.vbs_[streamIdx].size_ = stride * numVertices;
			dbsDesc.vbs_[streamIdx].stride_ = stride;

			offset += stride * numVertices;
		}

		dbsDesc.ib_.resource_ = indexBuffer_;
		dbsDesc.ib_.offset_ = 0;
		dbsDesc.ib_.size_ = (i32)indexDesc_.size_;
		dbsDesc.ib_.stride_ = 4;
		dbs_ = GPU::Manager::CreateDrawBindingSet(dbsDesc, "compressed_model_dbs");

		techs_.resize(materials_.size());
		compressedTechs_.resize(materials_.size());
		for(i32 i = 0; i < materials_.size(); ++i)
		{
			materials_[i].WaitUntilReady();
			compressedMaterials_[i].WaitUntilReady();

			techs_[i].material_ = materials_[i];
			compressedTechs_[i].material_ = compressedMaterials_[i];
		}

		techDesc_.SetVertexElements(elements_);
		techDesc_.SetTopology(GPU::TopologyType::TRIANGLE);

		compressedTechDesc_.SetTopology(GPU::TopologyType::TRIANGLE);

		objectBindings_ = Graphics::Shader::CreateSharedBindingSet("ObjectBindings");
		geometryBindings_ = Graphics::Shader::CreateSharedBindingSet("GeometryBindings");
	}
}

CompressedModel::~CompressedModel()
{
	GPU::Manager::DestroyResource(vertexBuffer_);
	GPU::Manager::DestroyResource(indexBuffer_);
	GPU::Manager::DestroyResource(dbs_);
}

void CompressedModel::DrawClusters(DrawContext& drawCtx, ObjectConstants object)
{
	if(auto event = drawCtx.cmdList_.Eventf(0x0, "CompressedModel"))
	{
		i32 numObjects = 1;
		const i32 objectDataSize = sizeof(ObjectConstants);


		// Allocate command list memory.
		auto* objects = drawCtx.cmdList_.Alloc<ObjectConstants>(numObjects);

		// Update all render packet uniforms.
		for(i32 idx = 0; idx < numObjects; ++idx)
			objects[idx] = object;
		drawCtx.cmdList_.UpdateBuffer(drawCtx.objectSBHandle_, 0, sizeof(ObjectConstants) * numObjects, objects);

		for(i32 idx = 0; idx < numObjects; ++idx)
		{
			for(i32 meshIdx = 0; meshIdx < meshes_.size(); ++meshIdx)
			{
				auto* techs = &techs_[meshIdx];
				if(useCompressed_)
					techs = &compressedTechs_[meshIdx];

				auto it = techs->passIndices_.find(drawCtx.passName_);
				if(it != nullptr)
				{
					const auto& mesh = meshes_[meshIdx];
					auto& tech = techs->passTechniques_[*it];
					if(drawCtx.customBindFn_)
						drawCtx.customBindFn_(drawCtx, tech);

					if(geometryBindings_)
					{
						geometryBindings_.Set(
						    "geomParams", GPU::Binding::CBuffer(paramsBuffer_[meshIdx], 0, sizeof(GeometryParams)));
						geometryBindings_.Set(
						    "geomPosition", GPU::Binding::Texture2D(positionTex_[meshIdx], positionFmt_, 0, 1));
						geometryBindings_.Set(
						    "geomNormal", GPU::Binding::Texture2D(normalTex_[meshIdx], normalFmt_, 0, 1));
					}

					objectBindings_.Set("inObject",
					    GPU::Binding::Buffer(drawCtx.objectSBHandle_, GPU::Format::INVALID, 0, 1, objectDataSize));
					auto geometryBind = drawCtx.shaderCtx_.BeginBindingScope(geometryBindings_);
					auto objectBind = drawCtx.shaderCtx_.BeginBindingScope(objectBindings_);
					GPU::Handle ps;
					Core::ArrayView<GPU::PipelineBinding> pb;
					if(drawCtx.shaderCtx_.CommitBindings(tech, ps, pb))
					{
						drawCtx.cmdList_.Draw(ps, pb, dbs_, drawCtx.fbs_, drawCtx.drawState_,
						    GPU::PrimitiveTopology::TRIANGLE_LIST, 0, 0, mesh.numIndices_, 0, 1);
					}
				}
			}
		}
	}
}
