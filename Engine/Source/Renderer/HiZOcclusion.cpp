#include "Renderer/HiZOcclusion.h"

#include "Core/Paths.h"
#include "Renderer/Material.h"
#include "Renderer/ObjectUniformStream.h"
#include "Renderer/RenderMesh.h"
#include "Renderer/RenderStateManager.h"
#include "Renderer/Renderer.h"
#include "Renderer/SceneRenderer.h"
#include "Renderer/Shader.h"
#include "Renderer/ShaderMap.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace
{
	void ClearUnusedMaterialConstantBuffers(ID3D11DeviceContext* DeviceContext, uint32 PreviousCount, uint32 CurrentCount)
	{
		if (!DeviceContext || CurrentCount >= PreviousCount)
		{
			return;
		}

		ID3D11Buffer* NullBuffer = nullptr;
		for (uint32 Index = CurrentCount; Index < PreviousCount; ++Index)
		{
			const UINT Slot = FMaterial::MaterialCBStartSlot + static_cast<UINT>(Index);
			DeviceContext->VSSetConstantBuffers(Slot, 1, &NullBuffer);
			DeviceContext->PSSetConstantBuffers(Slot, 1, &NullBuffer);
		}
	}

	uint32 DivideRoundUp(uint32 Value, uint32 Divisor)
	{
		return (Value + Divisor - 1) / Divisor;
	}

	uint32 ComputeViewportWidth(const D3D11_VIEWPORT& Viewport)
	{
		return static_cast<uint32>((std::max)(0.0f, Viewport.Width));
	}

	uint32 ComputeViewportHeight(const D3D11_VIEWPORT& Viewport)
	{
		return static_cast<uint32>((std::max)(0.0f, Viewport.Height));
	}

	uint32 ComputeViewportOffset(float Value)
	{
		return static_cast<uint32>((std::max)(0.0f, std::floor(Value + 0.5f)));
	}
}

FHiZOcclusion::~FHiZOcclusion()
{
	Release();
}

void FHiZOcclusion::ReleaseCom(IUnknown*& Resource)
{
	if (Resource)
	{
		Resource->Release();
		Resource = nullptr;
	}
}

uint32 FHiZOcclusion::ComputeMipCount(uint32 Width, uint32 Height)
{
	uint32 Dimension = (std::max)(Width, Height);
	uint32 MipCount = 1;
	while (Dimension > 1)
	{
		Dimension = (std::max)(1u, Dimension >> 1);
		++MipCount;
	}
	return MipCount;
}

bool FHiZOcclusion::Initialize()
{
	return CreateComputeShaders();
}

bool FHiZOcclusion::CreateComputeShaders()
{
	if (!Renderer || !Renderer->Device)
	{
		return false;
	}

	const std::wstring ShaderDir = FPaths::ShaderDir();
	CopyDepthCS = FShaderMap::Get().GetOrCreateComputeShader(Renderer->Device, (ShaderDir + L"HiZCopyDepthCS.hlsl").c_str());
	ReduceCS = FShaderMap::Get().GetOrCreateComputeShader(Renderer->Device, (ShaderDir + L"HiZReduceCS.hlsl").c_str());
	CullCS = FShaderMap::Get().GetOrCreateComputeShader(Renderer->Device, (ShaderDir + L"HiZOcclusionCullCS.hlsl").c_str());
	return CopyDepthCS != nullptr && ReduceCS != nullptr && CullCS != nullptr;
}

void FHiZOcclusion::ReleaseViewResources(FViewResources& View)
{
	for (ID3D11UnorderedAccessView*& UAV : View.HiZMipUAVs)
	{
		IUnknown* Resource = reinterpret_cast<IUnknown*>(UAV);
		ReleaseCom(Resource);
		UAV = nullptr;
	}
	View.HiZMipUAVs.clear();

	for (ID3D11ShaderResourceView*& SRV : View.HiZMipSRVs)
	{
		IUnknown* Resource = reinterpret_cast<IUnknown*>(SRV);
		ReleaseCom(Resource);
		SRV = nullptr;
	}
	View.HiZMipSRVs.clear();

	IUnknown* Resource = reinterpret_cast<IUnknown*>(View.HiZFullSRV);
	ReleaseCom(Resource);
	View.HiZFullSRV = nullptr;

	Resource = reinterpret_cast<IUnknown*>(View.HiZTexture);
	ReleaseCom(Resource);
	View.HiZTexture = nullptr;

	View.Width = 0;
	View.Height = 0;
	View.MipCount = 0;
	View.HistoryViewProjection = FMatrix::Identity;
	View.bHasValidHistory = false;
}

void FHiZOcclusion::ReleaseAllViewResources()
{
	for (auto& Pair : Views)
	{
		if (Pair.second)
		{
			ReleaseViewResources(*Pair.second);
		}
	}
	Views.clear();
	LastTouchedViewKey = 0;
}

void FHiZOcclusion::Release()
{
	bPreparedCulling = false;
	bCommandBuffersValid = false;
	CullCommandCount = 0;
	CachedCommandSignature = 0;
	OpaqueCommandToIndirectSlot.clear();
	CPUCullCommands.clear();
	CPUIndirectArgs.clear();
	ReleaseAllViewResources();

	if (CullCS)
	{
		CullCS->Release();
		CullCS.reset();
	}
	if (ReduceCS)
	{
		ReduceCS->Release();
		ReduceCS.reset();
	}
	if (CopyDepthCS)
	{
		CopyDepthCS->Release();
		CopyDepthCS.reset();
	}

	IUnknown* Resource = reinterpret_cast<IUnknown*>(DebugReadbackTexture);
	ReleaseCom(Resource);
	DebugReadbackTexture = nullptr;

	Resource = reinterpret_cast<IUnknown*>(DebugPreviewSRV);
	ReleaseCom(Resource);
	DebugPreviewSRV = nullptr;

	Resource = reinterpret_cast<IUnknown*>(DebugPreviewTexture);
	ReleaseCom(Resource);
	DebugPreviewTexture = nullptr;

	Resource = reinterpret_cast<IUnknown*>(IndirectArgsUAV);
	ReleaseCom(Resource);
	IndirectArgsUAV = nullptr;

	Resource = reinterpret_cast<IUnknown*>(IndirectArgsTemplateBuffer);
	ReleaseCom(Resource);
	IndirectArgsTemplateBuffer = nullptr;

	Resource = reinterpret_cast<IUnknown*>(IndirectArgsBuffer);
	ReleaseCom(Resource);
	IndirectArgsBuffer = nullptr;

	Resource = reinterpret_cast<IUnknown*>(CommandBufferSRV);
	ReleaseCom(Resource);
	CommandBufferSRV = nullptr;

	Resource = reinterpret_cast<IUnknown*>(CommandBuffer);
	ReleaseCom(Resource);
	CommandBuffer = nullptr;

	Resource = reinterpret_cast<IUnknown*>(CullConstantBuffer);
	ReleaseCom(Resource);
	CullConstantBuffer = nullptr;

	Resource = reinterpret_cast<IUnknown*>(ReduceConstantBuffer);
	ReleaseCom(Resource);
	ReduceConstantBuffer = nullptr;

	Resource = reinterpret_cast<IUnknown*>(CopyConstantBuffer);
	ReleaseCom(Resource);
	CopyConstantBuffer = nullptr;

	MaxCommandCount = 0;
	DebugPreviewWidth = 0;
	DebugPreviewHeight = 0;
	DebugPreviewMipIndex = UINT32_MAX;
	DebugPreviewViewKey = 0;
	DebugPreviewPixels.clear();
}

FHiZOcclusion::FViewResources* FHiZOcclusion::FindViewResources(uint64 ViewKey)
{
	auto It = Views.find(ViewKey);
	return (It != Views.end() && It->second) ? It->second.get() : nullptr;
}

const FHiZOcclusion::FViewResources* FHiZOcclusion::FindViewResources(uint64 ViewKey) const
{
	auto It = Views.find(ViewKey);
	return (It != Views.end() && It->second) ? It->second.get() : nullptr;
}

FHiZOcclusion::FViewResources* FHiZOcclusion::FindOrCreateViewResources(uint64 ViewKey, uint32 Width, uint32 Height)
{
	if (!Renderer || !Renderer->Device || Width == 0 || Height == 0)
	{
		return nullptr;
	}

	if (ViewKey == 0)
	{
		ViewKey = 1;
	}

	FViewResources* View = FindViewResources(ViewKey);
	if (!View)
	{
		auto NewView = std::make_unique<FViewResources>();
		NewView->ViewKey = ViewKey;
		View = NewView.get();
		Views.emplace(ViewKey, std::move(NewView));
	}

	const uint32 RequiredMipCount = ComputeMipCount(Width, Height);
	const bool bNeedsRebuild =
		View->HiZTexture == nullptr ||
		View->HiZFullSRV == nullptr ||
		View->Width != Width ||
		View->Height != Height ||
		View->MipCount != RequiredMipCount;

	if (!bNeedsRebuild)
	{
		return View;
	}

	ReleaseViewResources(*View);

	D3D11_TEXTURE2D_DESC HiZDesc = {};
	HiZDesc.Width = Width;
	HiZDesc.Height = Height;
	HiZDesc.MipLevels = RequiredMipCount;
	HiZDesc.ArraySize = 1;
	HiZDesc.Format = DXGI_FORMAT_R32_FLOAT;
	HiZDesc.SampleDesc.Count = 1;
	HiZDesc.Usage = D3D11_USAGE_DEFAULT;
	HiZDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	if (FAILED(Renderer->Device->CreateTexture2D(&HiZDesc, nullptr, &View->HiZTexture)))
	{
		ReleaseViewResources(*View);
		return nullptr;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC HiZFullSRVDesc = {};
	HiZFullSRVDesc.Format = DXGI_FORMAT_R32_FLOAT;
	HiZFullSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	HiZFullSRVDesc.Texture2D.MostDetailedMip = 0;
	HiZFullSRVDesc.Texture2D.MipLevels = RequiredMipCount;
	if (FAILED(Renderer->Device->CreateShaderResourceView(View->HiZTexture, &HiZFullSRVDesc, &View->HiZFullSRV)))
	{
		ReleaseViewResources(*View);
		return nullptr;
	}

	View->HiZMipSRVs.resize(RequiredMipCount);
	View->HiZMipUAVs.resize(RequiredMipCount);
	for (uint32 MipIndex = 0; MipIndex < RequiredMipCount; ++MipIndex)
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC MipSRVDesc = {};
		MipSRVDesc.Format = DXGI_FORMAT_R32_FLOAT;
		MipSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		MipSRVDesc.Texture2D.MostDetailedMip = MipIndex;
		MipSRVDesc.Texture2D.MipLevels = 1;
		if (FAILED(Renderer->Device->CreateShaderResourceView(View->HiZTexture, &MipSRVDesc, &View->HiZMipSRVs[MipIndex])))
		{
			ReleaseViewResources(*View);
			return nullptr;
		}

		D3D11_UNORDERED_ACCESS_VIEW_DESC MipUAVDesc = {};
		MipUAVDesc.Format = DXGI_FORMAT_R32_FLOAT;
		MipUAVDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		MipUAVDesc.Texture2D.MipSlice = MipIndex;
		if (FAILED(Renderer->Device->CreateUnorderedAccessView(View->HiZTexture, &MipUAVDesc, &View->HiZMipUAVs[MipIndex])))
		{
			ReleaseViewResources(*View);
			return nullptr;
		}
	}

	View->Width = Width;
	View->Height = Height;
	View->MipCount = RequiredMipCount;
	View->bHasValidHistory = false;
	View->HistoryViewProjection = FMatrix::Identity;
	return View;
}

bool FHiZOcclusion::EnsureSharedBuffers(size_t RequiredCommandCount)
{
	if (!Renderer || !Renderer->Device)
	{
		return false;
	}

	const bool bNeedBufferRebuild =
		!CopyConstantBuffer ||
		!ReduceConstantBuffer ||
		!CullConstantBuffer ||
		!CommandBuffer ||
		!CommandBufferSRV ||
		!IndirectArgsBuffer ||
		!IndirectArgsUAV ||
		MaxCommandCount < RequiredCommandCount;

	if (!bNeedBufferRebuild)
	{
		return true;
	}

	IUnknown* Resource = reinterpret_cast<IUnknown*>(IndirectArgsUAV);
	ReleaseCom(Resource);
	IndirectArgsUAV = nullptr;

	Resource = reinterpret_cast<IUnknown*>(IndirectArgsTemplateBuffer);
	ReleaseCom(Resource);
	IndirectArgsTemplateBuffer = nullptr;

	Resource = reinterpret_cast<IUnknown*>(IndirectArgsBuffer);
	ReleaseCom(Resource);
	IndirectArgsBuffer = nullptr;

	Resource = reinterpret_cast<IUnknown*>(CommandBufferSRV);
	ReleaseCom(Resource);
	CommandBufferSRV = nullptr;

	Resource = reinterpret_cast<IUnknown*>(CommandBuffer);
	ReleaseCom(Resource);
	CommandBuffer = nullptr;

	Resource = reinterpret_cast<IUnknown*>(CullConstantBuffer);
	ReleaseCom(Resource);
	CullConstantBuffer = nullptr;

	Resource = reinterpret_cast<IUnknown*>(ReduceConstantBuffer);
	ReleaseCom(Resource);
	ReduceConstantBuffer = nullptr;

	Resource = reinterpret_cast<IUnknown*>(CopyConstantBuffer);
	ReleaseCom(Resource);
	CopyConstantBuffer = nullptr;

	D3D11_BUFFER_DESC CopyCBDesc = {};
	CopyCBDesc.ByteWidth = (sizeof(FHiZCopyConstants) + 15) & ~15u;
	CopyCBDesc.Usage = D3D11_USAGE_DYNAMIC;
	CopyCBDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	CopyCBDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(Renderer->Device->CreateBuffer(&CopyCBDesc, nullptr, &CopyConstantBuffer)))
	{
		Release();
		return false;
	}

	D3D11_BUFFER_DESC ReduceCBDesc = {};
	ReduceCBDesc.ByteWidth = (sizeof(FHiZReduceConstants) + 15) & ~15u;
	ReduceCBDesc.Usage = D3D11_USAGE_DYNAMIC;
	ReduceCBDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	ReduceCBDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(Renderer->Device->CreateBuffer(&ReduceCBDesc, nullptr, &ReduceConstantBuffer)))
	{
		Release();
		return false;
	}

	D3D11_BUFFER_DESC CullCBDesc = {};
	CullCBDesc.ByteWidth = (sizeof(FHiZCullConstants) + 15) & ~15u;
	CullCBDesc.Usage = D3D11_USAGE_DYNAMIC;
	CullCBDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	CullCBDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(Renderer->Device->CreateBuffer(&CullCBDesc, nullptr, &CullConstantBuffer)))
	{
		Release();
		return false;
	}

	const uint32 SafeCommandCount = static_cast<uint32>((std::max)(RequiredCommandCount, static_cast<size_t>(1)));
	D3D11_BUFFER_DESC CommandDesc = {};
	CommandDesc.ByteWidth = sizeof(FHiZCullCommandGPU) * SafeCommandCount;
	CommandDesc.Usage = D3D11_USAGE_DEFAULT;
	CommandDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	CommandDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	CommandDesc.StructureByteStride = sizeof(FHiZCullCommandGPU);
	if (FAILED(Renderer->Device->CreateBuffer(&CommandDesc, nullptr, &CommandBuffer)))
	{
		Release();
		return false;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC CommandSRVDesc = {};
	CommandSRVDesc.Format = DXGI_FORMAT_UNKNOWN;
	CommandSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	CommandSRVDesc.Buffer.FirstElement = 0;
	CommandSRVDesc.Buffer.NumElements = SafeCommandCount;
	if (FAILED(Renderer->Device->CreateShaderResourceView(CommandBuffer, &CommandSRVDesc, &CommandBufferSRV)))
	{
		Release();
		return false;
	}

	D3D11_BUFFER_DESC ArgsDesc = {};
	ArgsDesc.ByteWidth = sizeof(D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS) * SafeCommandCount;
	ArgsDesc.Usage = D3D11_USAGE_DEFAULT;
	ArgsDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	ArgsDesc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
	if (FAILED(Renderer->Device->CreateBuffer(&ArgsDesc, nullptr, &IndirectArgsBuffer)))
	{
		Release();
		return false;
	}

	ArgsDesc.BindFlags = 0;
	ArgsDesc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
	if (FAILED(Renderer->Device->CreateBuffer(&ArgsDesc, nullptr, &IndirectArgsTemplateBuffer)))
	{
		Release();
		return false;
	}

	D3D11_UNORDERED_ACCESS_VIEW_DESC ArgsUAVDesc = {};
	ArgsUAVDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	ArgsUAVDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	ArgsUAVDesc.Buffer.FirstElement = 0;
	ArgsUAVDesc.Buffer.NumElements = ArgsDesc.ByteWidth / sizeof(uint32);
	ArgsUAVDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
	if (FAILED(Renderer->Device->CreateUnorderedAccessView(IndirectArgsBuffer, &ArgsUAVDesc, &IndirectArgsUAV)))
	{
		Release();
		return false;
	}

	MaxCommandCount = SafeCommandCount;
	bCommandBuffersValid = false;
	CachedCommandSignature = 0;
	return true;
}

uint64 FHiZOcclusion::ComputeCommandSignature(const TArray<FMeshDrawCommand>& OpaqueCommands) const
{
	uint64 Hash = 1469598103934665603ull;
	auto HashCombine = [&Hash](uint64 Value)
	{
		Hash ^= Value;
		Hash *= 1099511628211ull;
	};

	HashCombine(static_cast<uint64>(OpaqueCommands.size()));
	for (const FMeshDrawCommand& Command : OpaqueCommands)
	{
		HashCombine(reinterpret_cast<uint64>(Command.RenderMesh));
		HashCombine(static_cast<uint64>(Command.IndexStart) | (static_cast<uint64>(Command.IndexCount) << 32));
		HashCombine(Command.bCanUseHiZOcclusion ? 1ull : 0ull);

		uint32 Bits = 0;
		std::memcpy(&Bits, &Command.Bounds.Center.X, sizeof(uint32));
		HashCombine(Bits);
		std::memcpy(&Bits, &Command.Bounds.Center.Y, sizeof(uint32));
		HashCombine(Bits);
		std::memcpy(&Bits, &Command.Bounds.Center.Z, sizeof(uint32));
		HashCombine(Bits);
		std::memcpy(&Bits, &Command.Bounds.BoxExtent.X, sizeof(uint32));
		HashCombine(Bits);
		std::memcpy(&Bits, &Command.Bounds.BoxExtent.Y, sizeof(uint32));
		HashCombine(Bits);
		std::memcpy(&Bits, &Command.Bounds.BoxExtent.Z, sizeof(uint32));
		HashCombine(Bits);
	}

	return Hash;
}

bool FHiZOcclusion::BuildCommandBuffers(const TArray<FMeshDrawCommand>& OpaqueCommands)
{
	if (!Renderer || !Renderer->DeviceContext || !CommandBuffer || !IndirectArgsBuffer || !IndirectArgsTemplateBuffer)
	{
		return false;
	}

	const uint64 NewCommandSignature = ComputeCommandSignature(OpaqueCommands);
	if (bCommandBuffersValid && NewCommandSignature == CachedCommandSignature)
	{
		CullCommandCount = static_cast<uint32>(CPUIndirectArgs.size());
		return CullCommandCount > 0;
	}

	OpaqueCommandToIndirectSlot.assign(OpaqueCommands.size(), UINT32_MAX);
	CPUCullCommands.clear();
	CPUIndirectArgs.clear();
	CPUCullCommands.reserve(OpaqueCommands.size());
	CPUIndirectArgs.reserve(OpaqueCommands.size());

	for (size_t CommandIndex = 0; CommandIndex < OpaqueCommands.size(); ++CommandIndex)
	{
		const FMeshDrawCommand& Command = OpaqueCommands[CommandIndex];
		if (!Command.bCanUseHiZOcclusion || !Command.RenderMesh || Command.RenderMesh->Indices.empty())
		{
			continue;
		}

		const uint32 TotalIndexCount = static_cast<uint32>(Command.RenderMesh->Indices.size());
		if (Command.IndexStart >= TotalIndexCount)
		{
			continue;
		}

		const uint32 RemainingIndexCount = TotalIndexCount - Command.IndexStart;
		const uint32 DrawCount = Command.IndexCount > 0
			? (std::min)(Command.IndexCount, RemainingIndexCount)
			: RemainingIndexCount;
		if (DrawCount == 0)
		{
			continue;
		}

		const uint32 Slot = static_cast<uint32>(CPUIndirectArgs.size());
		OpaqueCommandToIndirectSlot[CommandIndex] = Slot;

		FHiZCullCommandGPU GPUCommand = {};
		GPUCommand.Center = Command.Bounds.Center;
		GPUCommand.Extent = Command.Bounds.BoxExtent;
		GPUCommand.Flags = 1u;
		CPUCullCommands.push_back(GPUCommand);

		D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS Args = {};
		Args.IndexCountPerInstance = DrawCount;
		Args.InstanceCount = 1u;
		Args.StartIndexLocation = Command.IndexStart;
		Args.BaseVertexLocation = 0;
		Args.StartInstanceLocation = 0;
		CPUIndirectArgs.push_back(Args);
	}

	CullCommandCount = static_cast<uint32>(CPUIndirectArgs.size());
	if (CullCommandCount == 0)
	{
		bPreparedCulling = false;
		return false;
	}

	Renderer->DeviceContext->UpdateSubresource(CommandBuffer, 0, nullptr, CPUCullCommands.data(), 0, 0);
	Renderer->DeviceContext->UpdateSubresource(IndirectArgsTemplateBuffer, 0, nullptr, CPUIndirectArgs.data(), 0, 0);
	Renderer->DeviceContext->CopyResource(IndirectArgsBuffer, IndirectArgsTemplateBuffer);

	CachedCommandSignature = NewCommandSignature;
	bCommandBuffersValid = true;
	return true;
}

void FHiZOcclusion::UpdateCullConstants(const FViewResources& View)
{
	if (!Renderer || !Renderer->DeviceContext || !CullConstantBuffer)
	{
		return;
	}

	FHiZCullConstants Constants = {};
	Constants.ViewProjection = View.HistoryViewProjection.GetTransposed();
	Constants.ViewWidth = View.Width;
	Constants.ViewHeight = View.Height;
	Constants.MipCount = View.MipCount;
	Constants.CommandCount = CullCommandCount;

	D3D11_MAPPED_SUBRESOURCE Mapped = {};
	if (SUCCEEDED(Renderer->DeviceContext->Map(CullConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped)))
	{
		std::memcpy(Mapped.pData, &Constants, sizeof(Constants));
		Renderer->DeviceContext->Unmap(CullConstantBuffer, 0);
	}
}

bool FHiZOcclusion::Prepare(const FSceneRenderFrame& Frame, const FMatrix& ViewMatrix, const FMatrix& ProjectionMatrix)
{
	(void)ViewMatrix;
	(void)ProjectionMatrix;
	bPreparedCulling = false;
	CullCommandCount = 0;

	if (!Renderer || !Renderer->Device || !Renderer->DeviceContext || !CullCS)
	{
		return false;
	}

	const TArray<FMeshDrawCommand>& OpaqueCommands = Frame.GetPassQueue(ERenderPass::Opaque);
	if (OpaqueCommands.empty())
	{
		return false;
	}

	UINT ViewportCount = 1;
	D3D11_VIEWPORT ActiveViewport = {};
	Renderer->DeviceContext->RSGetViewports(&ViewportCount, &ActiveViewport);
	if (ViewportCount == 0)
	{
		return false;
	}

	const uint32 Width = ComputeViewportWidth(ActiveViewport);
	const uint32 Height = ComputeViewportHeight(ActiveViewport);
	if (Width == 0 || Height == 0)
	{
		return false;
	}

	const uint64 ViewKey = Renderer->GetActiveHiZViewKey();
	const FViewResources* View = FindViewResources(ViewKey == 0 ? 1 : ViewKey);
	if (!View || !View->bHasValidHistory || !View->HiZFullSRV || View->Width != Width || View->Height != Height)
	{
		return false;
	}

	if (!EnsureSharedBuffers(OpaqueCommands.size()))
	{
		return false;
	}

	if (!BuildCommandBuffers(OpaqueCommands))
	{
		return false;
	}

	Renderer->DeviceContext->CopyResource(IndirectArgsBuffer, IndirectArgsTemplateBuffer);
	UpdateCullConstants(*View);
	DispatchCull(*View);
	LastTouchedViewKey = View->ViewKey;
	bPreparedCulling = true;
	return true;
}

void FHiZOcclusion::FinalizeFromActiveDepth()
{
	if (!Renderer || !Renderer->Device || !Renderer->DeviceContext || !CopyDepthCS || !ReduceCS)
	{
		return;
	}

	if (!Renderer->ActiveDepthShaderResourceView)
	{
		return;
	}

	UINT ViewportCount = 1;
	D3D11_VIEWPORT ActiveViewport = {};
	Renderer->DeviceContext->RSGetViewports(&ViewportCount, &ActiveViewport);
	if (ViewportCount == 0)
	{
		return;
	}

	const uint32 Width = ComputeViewportWidth(ActiveViewport);
	const uint32 Height = ComputeViewportHeight(ActiveViewport);
	if (Width == 0 || Height == 0)
	{
		return;
	}

	if (!EnsureSharedBuffers(MaxCommandCount))
	{
		return;
	}

	const uint64 ViewKey = Renderer->GetActiveHiZViewKey() == 0 ? 1 : Renderer->GetActiveHiZViewKey();
	FViewResources* View = FindOrCreateViewResources(ViewKey, Width, Height);
	if (!View)
	{
		return;
	}

	ID3D11DeviceContext* Context = Renderer->DeviceContext;
	ID3D11RenderTargetView* SavedRTV = nullptr;
	ID3D11DepthStencilView* SavedDSV = nullptr;
	Context->OMGetRenderTargets(1, &SavedRTV, &SavedDSV);

	UINT SavedViewportCount = 1;
	D3D11_VIEWPORT SavedViewport = {};
	Context->RSGetViewports(&SavedViewportCount, &SavedViewport);

	Context->OMSetRenderTargets(0, nullptr, nullptr);
	GenerateHiZ(*View);
	View->HistoryViewProjection = Renderer->ViewMatrix * Renderer->ProjectionMatrix;
	View->bHasValidHistory = true;
	LastTouchedViewKey = View->ViewKey;
	DebugPreviewMipIndex = UINT32_MAX;
	DebugPreviewViewKey = 0;

	Context->OMSetRenderTargets(SavedRTV ? 1 : 0, &SavedRTV, SavedDSV);
	if (SavedViewportCount > 0)
	{
		Context->RSSetViewports(SavedViewportCount, &SavedViewport);
	}

	if (SavedRTV)
	{
		SavedRTV->Release();
	}
	if (SavedDSV)
	{
		SavedDSV->Release();
	}
}

void FHiZOcclusion::GenerateHiZ(FViewResources& View) const
{
	if (!Renderer || !Renderer->DeviceContext || !CopyDepthCS || !ReduceCS || !Renderer->ActiveDepthShaderResourceView || View.HiZMipUAVs.empty())
	{
		return;
	}

	UINT ViewportCount = 1;
	D3D11_VIEWPORT ActiveViewport = {};
	Renderer->DeviceContext->RSGetViewports(&ViewportCount, &ActiveViewport);
	if (ViewportCount == 0)
	{
		return;
	}

	ID3D11DeviceContext* Context = Renderer->DeviceContext;

	FHiZCopyConstants CopyConstants = {};
	CopyConstants.SrcOffsetX = ComputeViewportOffset(ActiveViewport.TopLeftX);
	CopyConstants.SrcOffsetY = ComputeViewportOffset(ActiveViewport.TopLeftY);
	CopyConstants.DstWidth = View.Width;
	CopyConstants.DstHeight = View.Height;

	D3D11_MAPPED_SUBRESOURCE CopyMapped = {};
	if (SUCCEEDED(Context->Map(CopyConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &CopyMapped)))
	{
		std::memcpy(CopyMapped.pData, &CopyConstants, sizeof(CopyConstants));
		Context->Unmap(CopyConstantBuffer, 0);
	}

	ID3D11Buffer* CopyCB = CopyConstantBuffer;
	ID3D11ShaderResourceView* CopySRV = Renderer->ActiveDepthShaderResourceView;
	ID3D11UnorderedAccessView* CopyUAV = View.HiZMipUAVs[0];
	CopyDepthCS->Bind(Context);
	Context->CSSetConstantBuffers(0, 1, &CopyCB);
	Context->CSSetShaderResources(0, 1, &CopySRV);
	Context->CSSetUnorderedAccessViews(0, 1, &CopyUAV, nullptr);
	Context->Dispatch(DivideRoundUp(View.Width, 8), DivideRoundUp(View.Height, 8), 1);

	ID3D11ShaderResourceView* NullSRV = nullptr;
	ID3D11UnorderedAccessView* NullUAV = nullptr;
	ID3D11Buffer* NullCB = nullptr;
	Context->CSSetConstantBuffers(0, 1, &NullCB);
	Context->CSSetShaderResources(0, 1, &NullSRV);
	Context->CSSetUnorderedAccessViews(0, 1, &NullUAV, nullptr);

	for (uint32 MipIndex = 1; MipIndex < View.MipCount; ++MipIndex)
	{
		const uint32 SrcWidth = (std::max)(1u, View.Width >> (MipIndex - 1));
		const uint32 SrcHeight = (std::max)(1u, View.Height >> (MipIndex - 1));
		const uint32 DstWidth = (std::max)(1u, View.Width >> MipIndex);
		const uint32 DstHeight = (std::max)(1u, View.Height >> MipIndex);

		FHiZReduceConstants Constants = {};
		Constants.SrcWidth = SrcWidth;
		Constants.SrcHeight = SrcHeight;
		Constants.DstWidth = DstWidth;
		Constants.DstHeight = DstHeight;

		D3D11_MAPPED_SUBRESOURCE Mapped = {};
		if (SUCCEEDED(Context->Map(ReduceConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped)))
		{
			std::memcpy(Mapped.pData, &Constants, sizeof(Constants));
			Context->Unmap(ReduceConstantBuffer, 0);
		}

		ID3D11Buffer* ConstantBuffer = ReduceConstantBuffer;
		ReduceCS->Bind(Context);
		Context->CSSetConstantBuffers(0, 1, &ConstantBuffer);

		ID3D11ShaderResourceView* SrcSRV = View.HiZMipSRVs[MipIndex - 1];
		ID3D11UnorderedAccessView* DstUAV = View.HiZMipUAVs[MipIndex];
		Context->CSSetShaderResources(0, 1, &SrcSRV);
		Context->CSSetUnorderedAccessViews(0, 1, &DstUAV, nullptr);
		Context->Dispatch(DivideRoundUp(DstWidth, 8), DivideRoundUp(DstHeight, 8), 1);
		Context->CSSetConstantBuffers(0, 1, &NullCB);
		Context->CSSetShaderResources(0, 1, &NullSRV);
		Context->CSSetUnorderedAccessViews(0, 1, &NullUAV, nullptr);
	}

	Context->CSSetShader(nullptr, nullptr, 0);
}

void FHiZOcclusion::DispatchCull(const FViewResources& View) const
{
	if (!Renderer || !Renderer->DeviceContext || !CullCS || !CommandBufferSRV || !View.HiZFullSRV || !IndirectArgsUAV || !CullConstantBuffer || CullCommandCount == 0)
	{
		return;
	}

	ID3D11DeviceContext* Context = Renderer->DeviceContext;
	CullCS->Bind(Context);

	ID3D11Buffer* ConstantBuffer = CullConstantBuffer;
	Context->CSSetConstantBuffers(0, 1, &ConstantBuffer);

	ID3D11ShaderResourceView* SRVs[2] = { CommandBufferSRV, View.HiZFullSRV };
	Context->CSSetShaderResources(0, 2, SRVs);
	Context->CSSetUnorderedAccessViews(0, 1, &IndirectArgsUAV, nullptr);
	Context->Dispatch(DivideRoundUp(CullCommandCount, 64), 1, 1);

	ID3D11ShaderResourceView* NullSRVs[2] = { nullptr, nullptr };
	ID3D11UnorderedAccessView* NullUAV = nullptr;
	ID3D11Buffer* NullCB = nullptr;
	Context->CSSetConstantBuffers(0, 1, &NullCB);
	Context->CSSetShaderResources(0, 2, NullSRVs);
	Context->CSSetUnorderedAccessViews(0, 1, &NullUAV, nullptr);
	Context->CSSetShader(nullptr, nullptr, 0);
}

const FHiZOcclusion::FViewResources* FHiZOcclusion::GetDebugViewResources() const
{
	uint64 ViewKey = Renderer ? Renderer->GetActiveHiZViewKey() : 0;
	if (ViewKey == 0)
	{
		ViewKey = LastTouchedViewKey;
	}
	if (ViewKey == 0)
	{
		return nullptr;
	}
	const FViewResources* View = FindViewResources(ViewKey);
	if (View && View->bHasValidHistory)
	{
		return View;
	}
	if (ViewKey != LastTouchedViewKey)
	{
		View = FindViewResources(LastTouchedViewKey);
		if (View && View->bHasValidHistory)
		{
			return View;
		}
	}
	return nullptr;
}

FHiZOcclusion::FViewResources* FHiZOcclusion::GetDebugViewResources()
{
	return const_cast<FViewResources*>(static_cast<const FHiZOcclusion*>(this)->GetDebugViewResources());
}

ID3D11ShaderResourceView* FHiZOcclusion::GetHiZFullSRV() const
{
	const FViewResources* View = GetDebugViewResources();
	return View ? View->HiZFullSRV : nullptr;
}

ID3D11ShaderResourceView* FHiZOcclusion::GetHiZMipSRV(uint32 MipIndex) const
{
	const FViewResources* View = GetDebugViewResources();
	return (View && MipIndex < static_cast<uint32>(View->HiZMipSRVs.size())) ? View->HiZMipSRVs[MipIndex] : nullptr;
}

uint32 FHiZOcclusion::GetResourceMipCount() const
{
	const FViewResources* View = GetDebugViewResources();
	return View ? View->MipCount : 0;
}

bool FHiZOcclusion::HasValidHistoryForCurrentView() const
{
	return GetDebugViewResources() != nullptr;
}

uint64 FHiZOcclusion::GetCurrentDebugViewKey() const
{
	const FViewResources* View = GetDebugViewResources();
	return View ? View->ViewKey : 0;
}

bool FHiZOcclusion::EnsureDebugPreviewResources(uint32 Width, uint32 Height)
{
	if (!Renderer || !Renderer->Device || Width == 0 || Height == 0)
	{
		return false;
	}

	const bool bNeedsRebuild =
		DebugPreviewTexture == nullptr ||
		DebugPreviewSRV == nullptr ||
		DebugReadbackTexture == nullptr ||
		DebugPreviewWidth != Width ||
		DebugPreviewHeight != Height;

	if (!bNeedsRebuild)
	{
		return true;
	}

	IUnknown* Resource = reinterpret_cast<IUnknown*>(DebugReadbackTexture);
	ReleaseCom(Resource);
	DebugReadbackTexture = nullptr;

	Resource = reinterpret_cast<IUnknown*>(DebugPreviewSRV);
	ReleaseCom(Resource);
	DebugPreviewSRV = nullptr;

	Resource = reinterpret_cast<IUnknown*>(DebugPreviewTexture);
	ReleaseCom(Resource);
	DebugPreviewTexture = nullptr;

	D3D11_TEXTURE2D_DESC PreviewDesc = {};
	PreviewDesc.Width = Width;
	PreviewDesc.Height = Height;
	PreviewDesc.MipLevels = 1;
	PreviewDesc.ArraySize = 1;
	PreviewDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	PreviewDesc.SampleDesc.Count = 1;
	PreviewDesc.Usage = D3D11_USAGE_DEFAULT;
	PreviewDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(Renderer->Device->CreateTexture2D(&PreviewDesc, nullptr, &DebugPreviewTexture)))
	{
		return false;
	}

	if (FAILED(Renderer->Device->CreateShaderResourceView(DebugPreviewTexture, nullptr, &DebugPreviewSRV)))
	{
		Resource = reinterpret_cast<IUnknown*>(DebugPreviewTexture);
		ReleaseCom(Resource);
		DebugPreviewTexture = nullptr;
		return false;
	}

	D3D11_TEXTURE2D_DESC ReadbackDesc = PreviewDesc;
	ReadbackDesc.Format = DXGI_FORMAT_R32_FLOAT;
	ReadbackDesc.Usage = D3D11_USAGE_STAGING;
	ReadbackDesc.BindFlags = 0;
	ReadbackDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if (FAILED(Renderer->Device->CreateTexture2D(&ReadbackDesc, nullptr, &DebugReadbackTexture)))
	{
		Resource = reinterpret_cast<IUnknown*>(DebugPreviewSRV);
		ReleaseCom(Resource);
		DebugPreviewSRV = nullptr;
		Resource = reinterpret_cast<IUnknown*>(DebugPreviewTexture);
		ReleaseCom(Resource);
		DebugPreviewTexture = nullptr;
		return false;
	}

	DebugPreviewWidth = Width;
	DebugPreviewHeight = Height;
	DebugPreviewPixels.assign(static_cast<size_t>(Width) * static_cast<size_t>(Height) * 4u, 0u);
	return true;
}

ID3D11ShaderResourceView* FHiZOcclusion::GetHiZDebugSRV(uint32 MipIndex)
{
	FViewResources* View = GetDebugViewResources();
	if (!View || !Renderer || !Renderer->DeviceContext || !View->bHasValidHistory || MipIndex >= View->MipCount)
	{
		return nullptr;
	}

	const uint32 Width = (std::max)(1u, View->Width >> MipIndex);
	const uint32 Height = (std::max)(1u, View->Height >> MipIndex);
	if (!EnsureDebugPreviewResources(Width, Height))
	{
		return View->HiZMipSRVs[MipIndex];
	}

	if (DebugPreviewViewKey == View->ViewKey && DebugPreviewMipIndex == MipIndex && DebugPreviewSRV)
	{
		return DebugPreviewSRV;
	}

	const UINT SourceSubresource = D3D11CalcSubresource(MipIndex, 0, View->MipCount);
	Renderer->DeviceContext->CopySubresourceRegion(DebugReadbackTexture, 0, 0, 0, 0, View->HiZTexture, SourceSubresource, nullptr);

	D3D11_MAPPED_SUBRESOURCE Mapped = {};
	if (!SUCCEEDED(Renderer->DeviceContext->Map(DebugReadbackTexture, 0, D3D11_MAP_READ, 0, &Mapped)))
	{
		return View->HiZMipSRVs[MipIndex];
	}

	float MinValue = std::numeric_limits<float>::max();
	float MaxValue = std::numeric_limits<float>::lowest();
	for (uint32 Y = 0; Y < Height; ++Y)
	{
		const float* Row = reinterpret_cast<const float*>(static_cast<const uint8*>(Mapped.pData) + static_cast<size_t>(Y) * Mapped.RowPitch);
		for (uint32 X = 0; X < Width; ++X)
		{
			const float Value = Row[X];
			if (!std::isfinite(Value))
			{
				continue;
			}
			MinValue = (std::min)(MinValue, Value);
			MaxValue = (std::max)(MaxValue, Value);
		}
	}

	if (!std::isfinite(MinValue) || !std::isfinite(MaxValue))
	{
		MinValue = 0.0f;
		MaxValue = 1.0f;
	}
	float Range = MaxValue - MinValue;
	if (Range < 1.0e-6f)
	{
		Range = 1.0f;
	}

	for (uint32 Y = 0; Y < Height; ++Y)
	{
		const float* Row = reinterpret_cast<const float*>(static_cast<const uint8*>(Mapped.pData) + static_cast<size_t>(Y) * Mapped.RowPitch);
		uint8* OutRow = DebugPreviewPixels.data() + (static_cast<size_t>(Y) * Width * 4u);
		for (uint32 X = 0; X < Width; ++X)
		{
			float Normalized = (Row[X] - MinValue) / Range;
			Normalized = (std::max)(0.0f, (std::min)(1.0f, Normalized));
			const uint8 Gray = static_cast<uint8>(Normalized * 255.0f + 0.5f);
			OutRow[X * 4 + 0] = Gray;
			OutRow[X * 4 + 1] = Gray;
			OutRow[X * 4 + 2] = Gray;
			OutRow[X * 4 + 3] = 255u;
		}
	}

	Renderer->DeviceContext->Unmap(DebugReadbackTexture, 0);
	Renderer->DeviceContext->UpdateSubresource(DebugPreviewTexture, 0, nullptr, DebugPreviewPixels.data(), Width * 4u, 0);
	DebugPreviewViewKey = View->ViewKey;
	DebugPreviewMipIndex = MipIndex;
	return DebugPreviewSRV;
}

void FHiZOcclusion::ExecuteOpaqueQueue(const TArray<FMeshDrawCommand>& InCommands) const
{
	if (!Renderer || !Renderer->DeviceContext || InCommands.empty())
	{
		return;
	}

	FRenderStateManager* RenderStateManager = Renderer->RenderStateManager.get();
	if (RenderStateManager)
	{
		FDepthStencilStateOption DepthState = {};
		DepthState.DepthEnable = true;
		DepthState.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		DepthState.DepthFunc = D3D11_COMPARISON_GREATER;
		RenderStateManager->BindState(RenderStateManager->GetOrCreateDepthStencilState(DepthState));
	}

	FMaterial* CurrentMaterial = nullptr;
	uint32 CurrentMaterialConstantBufferCount = 0;
	FRenderMesh* CurrentMesh = nullptr;
	D3D11_PRIMITIVE_TOPOLOGY CurrentTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;

	for (size_t CommandIndex = 0; CommandIndex < InCommands.size(); ++CommandIndex)
	{
		const FMeshDrawCommand& Command = InCommands[CommandIndex];
		if (!Command.RenderMesh)
		{
			continue;
		}

		FMaterial* Material = Command.Material ? Command.Material : Renderer->GetDefaultMaterial();
		if (!Material)
		{
			continue;
		}

		if (Material != CurrentMaterial)
		{
			const uint32 PreviousMaterialConstantBufferCount = CurrentMaterialConstantBufferCount;
			Material->Bind(Renderer->DeviceContext);
			CurrentMaterialConstantBufferCount = static_cast<uint32>(Material->GetConstantBuffers().size());
			ClearUnusedMaterialConstantBuffers(
				Renderer->DeviceContext,
				PreviousMaterialConstantBufferCount,
				CurrentMaterialConstantBufferCount);
			CurrentMaterial = Material;
		}

		if (CurrentMesh != Command.RenderMesh)
		{
			Command.RenderMesh->Bind(Renderer->DeviceContext);
			CurrentMesh = Command.RenderMesh;
		}

		const D3D11_PRIMITIVE_TOPOLOGY DesiredTopology = static_cast<D3D11_PRIMITIVE_TOPOLOGY>(Command.RenderMesh->Topology);
		if (DesiredTopology != CurrentTopology)
		{
			Renderer->DeviceContext->IASetPrimitiveTopology(DesiredTopology);
			CurrentTopology = DesiredTopology;
		}

		Renderer->ObjectUniformStream->BindAllocation(Command.ObjectUniformAllocation);

		const bool bUseIndirect =
			CommandIndex < OpaqueCommandToIndirectSlot.size() &&
			OpaqueCommandToIndirectSlot[CommandIndex] != UINT32_MAX &&
			IndirectArgsBuffer != nullptr;

		if (bUseIndirect)
		{
			const UINT ByteOffset = OpaqueCommandToIndirectSlot[CommandIndex] * sizeof(D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS);
			++Renderer->FrameDrawCallCount;
			Renderer->DeviceContext->DrawIndexedInstancedIndirect(IndirectArgsBuffer, ByteOffset);
			continue;
		}

		if (!Command.RenderMesh->Indices.empty())
		{
			const uint32 TotalIndexCount = static_cast<uint32>(Command.RenderMesh->Indices.size());
			if (Command.IndexStart >= TotalIndexCount)
			{
				continue;
			}

			const uint32 RemainingIndexCount = TotalIndexCount - Command.IndexStart;
			const uint32 DrawCount = Command.IndexCount > 0
				? (std::min)(Command.IndexCount, RemainingIndexCount)
				: RemainingIndexCount;
			if (DrawCount == 0)
			{
				continue;
			}

			++Renderer->FrameDrawCallCount;
			Renderer->DeviceContext->DrawIndexed(DrawCount, Command.IndexStart, 0);
		}
		else if (!Command.RenderMesh->Vertices.empty())
		{
			++Renderer->FrameDrawCallCount;
			Renderer->DeviceContext->Draw(static_cast<UINT>(Command.RenderMesh->Vertices.size()), 0);
		}
	}
}
