#pragma once

#include "core/array.h"
#include "core/types.h"
#include "core/library.h"
#include "core/os.h"
#include "gpu/resources.h"
#include "gpu/types.h"
#include "gpu/utils.h"

#include <dxgi1_4.h>
#include <dxgidebug.h>

#include <dxgiformat.h>
#include <d3d12.h>
#include <wrl.h>

#include <amd_ags.h>

// Windows header crap.
#undef DrawState

typedef HRESULT(WINAPI* PFN_CREATE_DXGI_FACTORY)(UINT flags, REFIID _riid, void** _factory);
typedef HRESULT(WINAPI* PFN_GET_DXGI_DEBUG_INTERFACE)(UINT flags, REFIID _riid, void** _debug);

namespace GPU
{
	using Microsoft::WRL::ComPtr;

	/**
	 * Library handling.
	 */
	extern Core::LibHandle DXGIDebugHandle;
	extern Core::LibHandle DXGIHandle;
	extern Core::LibHandle D3D12Handle;
	extern PFN_GET_DXGI_DEBUG_INTERFACE DXGIGetDebugInterface1Fn;
	extern PFN_CREATE_DXGI_FACTORY DXGICreateDXGIFactory2Fn;
	extern PFN_D3D12_CREATE_DEVICE D3D12CreateDeviceFn;
	extern PFN_D3D12_GET_DEBUG_INTERFACE D3D12GetDebugInterfaceFn;
	extern PFN_D3D12_SERIALIZE_ROOT_SIGNATURE D3D12SerializeRootSignatureFn;

	ErrorCode LoadLibraries();

	/**
	 * Enums.
	 */
	enum class RootSignatureType
	{
		INVALID = -1,
		GRAPHICS,
		COMPUTE,
		MAX
	};

	enum class DescriptorHeapSubType : i32
	{
		INVALID = -1,
		CBV = 0,
		SRV,
		UAV,
		SAMPLER,
		RTV,
		DSV,
	};


	/**
	 * Conversion.
	 */
	D3D12_RESOURCE_FLAGS GetResourceFlags(BindFlags bindFlags);
	D3D12_RESOURCE_STATES GetResourceStates(BindFlags bindFlags);
	D3D12_RESOURCE_STATES GetDefaultResourceState(BindFlags bindFlags);
	D3D12_RESOURCE_DIMENSION GetResourceDimension(TextureType type);
	D3D12_SRV_DIMENSION GetSRVDimension(ViewDimension dim);
	D3D12_UAV_DIMENSION GetUAVDimension(ViewDimension dim);
	D3D12_RTV_DIMENSION GetRTVDimension(ViewDimension dim);
	D3D12_DSV_DIMENSION GetDSVDimension(ViewDimension dim);
	DXGI_FORMAT GetFormat(Format format);
	D3D12_PRIMITIVE_TOPOLOGY GetPrimitiveTopology(PrimitiveTopology topology);
	D3D12_RESOURCE_DESC GetResourceDesc(const BufferDesc& desc);
	D3D12_RESOURCE_DESC GetResourceDesc(const TextureDesc& desc);
	D3D12_BOX GetBox(i32 w, i32 h, i32 d);
	D3D12_BOX GetBox(const Box& box);
	Box GetBox(const D3D12_BOX& box);
	D3D12_SUBRESOURCE_FOOTPRINT GetFootprint(const Footprint& footprint);

	/**
	 * Sampler.
	 */
	D3D12_TEXTURE_ADDRESS_MODE GetAddressingMode(AddressingMode addressMode);
	D3D12_FILTER GetFilteringMode(FilteringMode min, FilteringMode mag, u32 anisotropy);
	D3D12_SAMPLER_DESC GetSampler(const GPU::SamplerState& state);
	D3D12_STATIC_SAMPLER_DESC GetStaticSampler(const GPU::SamplerState& state);

	/**
	 * Barriers.
	 */
	D3D12_RESOURCE_BARRIER TransitionBarrier(
	    ID3D12Resource* res, UINT subRsc, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

	/**
	 * Utility.
	 */
	void SetObjectName(ID3D12Object* object, const char* name);
	void GetObjectName(ID3D12Object* object, char* outName, i32& outSize);
	void WaitOnFence(ID3D12Fence* fence, HANDLE event, u64 value);

	static const i32 COMMAND_LIST_BATCH_SIZE = 32;
	static const i64 UPLOAD_AUTO_FLUSH_COMMANDS = 30;
	static const i64 UPLOAD_AUTO_FLUSH_BYTES = 8 * 1024 * 1024;

/**
	 * Enable descriptor debug data.
	 */
#define ENABLE_DESCRIPTOR_DEBUG_DATA (0)


	/**
	 * Descriptor debug data.
	 */
	struct D3D12DescriptorDebugData
	{
		DescriptorHeapSubType subType_ = DescriptorHeapSubType::INVALID;
		const struct D3D12Resource* resource_ = nullptr;
		Core::Array<char, 32> name_ = {};
	};

	/**
	 * Clear descriptor range.
	 */
	void ClearDescriptorRange(ID3D12DescriptorHeap* d3dDescriptorHeap, DescriptorHeapSubType subType,
	    D3D12_CPU_DESCRIPTOR_HANDLE handle, i32 numDescriptors,
	    Core::ArrayView<D3D12DescriptorDebugData> debugDataBase = Core::ArrayView<D3D12DescriptorDebugData>());

} // namespace GPU

#if !defined(_RELEASE)
#define CHECK_ERRORCODE(a) DBG_ASSERT((a) == ErrorCode::OK)
#define CHECK_D3D(a) DBG_ASSERT((a) == S_OK)
#define CHECK_D3D_RESULT(hr, a)                                                                                        \
	hr = a;                                                                                                            \
	DBG_ASSERT((hr) == S_OK)
#else
#define CHECK_ERRORCODE(a) a
#define CHECK_D3D(a) a
#define CHECK_D3D_RESULT(hr, a) hr = a
#endif