#include "Renderer/HiZOcclusion.h"

#include "Component/PrimitiveComponent.h"
#include "Core/Paths.h"
#include "Renderer/SceneProxy.h"
#include "Renderer/ShaderResource.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>

namespace
{
	constexpr uint32 GThreadGroupSize = 8;
	constexpr float GNearDistanceProtection = 100.0f;
	constexpr float GBoundsInflateScale = 1.05f;
	constexpr float GDepthBias = 0.02f;
	constexpr float GCameraTranslationThreshold = 125.0f;
	constexpr float GCameraRotationDotThreshold = 0.990f;
	constexpr uint8 GVisibleGraceFrameCount = 1;
	constexpr uint32 GTargetReadbackResolution = 256;
	constexpr float GScreenTexelExpand = 0.75f;

	template <typename T>
	inline void SafeRelease(T*& InObject)
	{
		if (InObject)
		{
			InObject->Release();
			InObject = nullptr;
		}
	}

	FVector ExtractForwardFromViewMatrix(const FMatrix& ViewMatrix)
	{
		return FVector(ViewMatrix[0][0], ViewMatrix[1][0], ViewMatrix[2][0]).GetSafeNormal();
	}

	FVector4 TransformPoint(const FVector& Point, const FMatrix& Matrix)
	{
		return FVector4(
			Point.X * Matrix[0][0] + Point.Y * Matrix[1][0] + Point.Z * Matrix[2][0] + Matrix[3][0],
			Point.X * Matrix[0][1] + Point.Y * Matrix[1][1] + Point.Z * Matrix[2][1] + Matrix[3][1],
			Point.X * Matrix[0][2] + Point.Y * Matrix[1][2] + Point.Z * Matrix[2][2] + Matrix[3][2],
			Point.X * Matrix[0][3] + Point.Y * Matrix[1][3] + Point.Z * Matrix[2][3] + Matrix[3][3]);
	}

	uint32 ComputeMipCount(uint32 Width, uint32 Height)
	{
		uint32 MaxDimension = (std::max)(Width, Height);
		uint32 MipCount = 1;
		while (MaxDimension > 1)
		{
			MaxDimension >>= 1;
			++MipCount;
		}
		return MipCount;
	}

	uint32 SelectReadbackMip(uint32 Width, uint32 Height)
	{
		uint32 MipIndex = 0;
		while (((std::max)(Width >> MipIndex, Height >> MipIndex)) > GTargetReadbackResolution)
		{
			++MipIndex;
		}
		return MipIndex;
	}
}

FHiZOcclusion::~FHiZOcclusion()
{
	Release();
}

bool FHiZOcclusion::Initialize(ID3D11Device* InDevice, ID3D11DeviceContext* InDeviceContext)
{
	Release();

	Device = InDevice;
	DeviceContext = InDeviceContext;
	if (!Device || !DeviceContext)
	{
		return false;
	}

	return CreateShaders() && CreateConstantBuffer();
}

void FHiZOcclusion::Release()
{
	ResolvedDepths.clear();
	VisibilityHistory.clear();
	bHasResolvedReadback = false;
	LastResolvedFrameNumber = 0;
	LastInputCommandCount = 0;
	LastEligibleCommandCount = 0;
	LastUnsupportedCommandCount = 0;
	LastUnsupportedNoSceneProxyCount = 0;
	LastUnsupportedNonOpaquePassCount = 0;
	LastUnsupportedProxyTypeCount = 0;
	LastUnsupportedNoSceneProxyCount = 0;
	LastUnsupportedNonOpaquePassCount = 0;
	LastUnsupportedProxyTypeCount = 0;
	LastVisibleCommandCount = 0;
	LastCulledCommandCount = 0;
	LastKeepNoCompatibleReadbackCount = 0;
	LastKeepHistoryCount = 0;
	LastKeepNearCount = 0;
	LastKeepProjectionFailCount = 0;
	LastKeepScreenEdgeCount = 0;
	LastKeepSmallRectCount = 0;
	LastKeepSampleVisibleCount = 0;
	FirstUnsupportedDebugName[0] = '\0';
	DepthWidth = 0;
	DepthHeight = 0;
	HiZMipCount = 0;
	ReadbackMipIndex = 0;
	ReadbackWidth = 0;
	ReadbackHeight = 0;
	CurrentFrameNumber = 0;
	NextReadbackSlot = 0;

	ReleaseReadbackSlots();

	for (ID3D11UnorderedAccessView*& UAV : HiZMipUAVs)
	{
		SafeRelease(UAV);
	}
	HiZMipUAVs.clear();

	for (ID3D11ShaderResourceView*& SRV : HiZMipSRVs)
	{
		SafeRelease(SRV);
	}
	HiZMipSRVs.clear();

	SafeRelease(HiZTexture);
	SafeRelease(DebugPreviewSRV);
	SafeRelease(DebugPreviewTexture);
	SafeRelease(ReduceConstantBuffer);
	SafeRelease(CopyDepthCS);
	SafeRelease(ReduceMinCS);

	DeviceContext = nullptr;
	Device = nullptr;
}

void FHiZOcclusion::OnResize()
{
	bHasResolvedReadback = false;
	ResolvedDepths.clear();
	ResolvedViewMatrix = FMatrix::Identity;
	ResolvedProjectionMatrix = FMatrix::Identity;
	ResolvedViewProjectionMatrix = FMatrix::Identity;
	ResolvedCameraPosition = FVector::ZeroVector;
	ResolvedViewForward = FVector::ForwardVector;
	ResolvedViewportWidth = 0;
	ResolvedViewportHeight = 0;
	LastResolvedFrameNumber = 0;
	DepthWidth = 0;
	DepthHeight = 0;
	HiZMipCount = 0;
	ReadbackMipIndex = 0;
	ReadbackWidth = 0;
	ReadbackHeight = 0;

	ReleaseReadbackSlots();

	for (ID3D11UnorderedAccessView*& UAV : HiZMipUAVs)
	{
		SafeRelease(UAV);
	}
	HiZMipUAVs.clear();

	for (ID3D11ShaderResourceView*& SRV : HiZMipSRVs)
	{
		SafeRelease(SRV);
	}
	HiZMipSRVs.clear();

	SafeRelease(HiZTexture);
	SafeRelease(DebugPreviewSRV);
	SafeRelease(DebugPreviewTexture);
}

void FHiZOcclusion::BeginFrame(uint64 InFrameNumber)
{
	CurrentFrameNumber = InFrameNumber;
	DecayVisibilityHistory();
	ResolveOldReadback();
}

bool FHiZOcclusion::ApplyCPUCulling(
	FRenderCommandQueue& InOutQueue,
	const FMatrix& CurrentViewMatrix,
	const FMatrix& CurrentProjectionMatrix,
	const FVector& CurrentCameraPosition,
	uint32 CurrentViewportWidth,
	uint32 CurrentViewportHeight)
{
	LastInputCommandCount = static_cast<uint32>(InOutQueue.Commands.size());
	LastEligibleCommandCount = 0;
	LastUnsupportedCommandCount = 0;
	LastUnsupportedNoSceneProxyCount = 0;
	LastUnsupportedNonOpaquePassCount = 0;
	LastUnsupportedProxyTypeCount = 0;
	LastVisibleCommandCount = LastInputCommandCount;
	LastCulledCommandCount = 0;
	LastKeepNoCompatibleReadbackCount = 0;
	LastKeepHistoryCount = 0;
	LastKeepNearCount = 0;
	LastKeepProjectionFailCount = 0;
	LastKeepScreenEdgeCount = 0;
	LastKeepSmallRectCount = 0;
	LastKeepSampleVisibleCount = 0;
	FirstUnsupportedDebugName[0] = '\0';

	if (!bHasResolvedReadback || InOutQueue.Commands.empty())
	{
		return false;
	}

	if (!HasCompatibleReadback(CurrentCameraPosition, CurrentViewMatrix, CurrentProjectionMatrix, CurrentViewportWidth, CurrentViewportHeight))
	{
		LastKeepNoCompatibleReadbackCount = LastInputCommandCount;
		return false;
	}

	TArray<FRenderCommand> VisibleCommands;
	VisibleCommands.reserve(InOutQueue.Commands.size());

	for (const FRenderCommand& Command : InOutQueue.Commands)
	{
		const bool bOpaquePass = (!Command.bOverrideRenderPass || Command.RenderPass == ERenderPass::Opaque);
		if (!Command.SceneProxy)
		{
			++LastUnsupportedCommandCount;
			++LastUnsupportedNoSceneProxyCount;
			if (FirstUnsupportedDebugName[0] == '\0')
			{
				std::snprintf(FirstUnsupportedDebugName, sizeof(FirstUnsupportedDebugName), "NoSceneProxy");
			}
			VisibleCommands.push_back(Command);
			continue;
		}
		if (!bOpaquePass)
		{
			++LastUnsupportedCommandCount;
			++LastUnsupportedNonOpaquePassCount;
			if (FirstUnsupportedDebugName[0] == '\0')
			{
				std::snprintf(FirstUnsupportedDebugName, sizeof(FirstUnsupportedDebugName), "NonOpaquePass");
			}
			VisibleCommands.push_back(Command);
			continue;
		}
		if (!Command.SceneProxy->CanApplyCoarseOcclusion())
		{
			++LastUnsupportedCommandCount;
			++LastUnsupportedProxyTypeCount;
			if (FirstUnsupportedDebugName[0] == '\0')
			{
				std::snprintf(FirstUnsupportedDebugName, sizeof(FirstUnsupportedDebugName), "%s", Command.SceneProxy->GetCoarseOcclusionDebugName());
			}
			VisibleCommands.push_back(Command);
			continue;
		}

		++LastEligibleCommandCount;
		const EVisibilityResult VisibilityResult = EvaluateVisibilityConservative(
			Command.SceneProxy,
			Command.SceneProxy->GetBounds(),
			CurrentCameraPosition,
			CurrentViewMatrix,
			CurrentProjectionMatrix,
			CurrentViewportWidth,
			CurrentViewportHeight);

		switch (VisibilityResult)
		{
		case EVisibilityResult::Visible_NoCompatibleReadback:
			++LastKeepNoCompatibleReadbackCount;
			VisibleCommands.push_back(Command);
			break;
		case EVisibilityResult::Visible_History:
			++LastKeepHistoryCount;
			VisibleCommands.push_back(Command);
			break;
		case EVisibilityResult::Visible_Near:
			++LastKeepNearCount;
			VisibleCommands.push_back(Command);
			break;
		case EVisibilityResult::Visible_ProjectionFail:
			++LastKeepProjectionFailCount;
			VisibleCommands.push_back(Command);
			break;
		case EVisibilityResult::Visible_ScreenEdge:
			++LastKeepScreenEdgeCount;
			VisibleCommands.push_back(Command);
			break;
		case EVisibilityResult::Visible_SmallRect:
			++LastKeepSmallRectCount;
			VisibleCommands.push_back(Command);
			break;
		case EVisibilityResult::Visible_SampleMismatch:
			++LastKeepSampleVisibleCount;
			VisibleCommands.push_back(Command);
			MarkVisible(Command.SceneProxy);
			break;
		case EVisibilityResult::Culled_Occluded:
			break;
		default:
			VisibleCommands.push_back(Command);
			break;
		}
	}

	InOutQueue.Commands = std::move(VisibleCommands);
	LastVisibleCommandCount = static_cast<uint32>(InOutQueue.Commands.size());
	LastCulledCommandCount = (LastInputCommandCount > LastVisibleCommandCount) ? (LastInputCommandCount - LastVisibleCommandCount) : 0;
	return true;
}

bool FHiZOcclusion::BuildHZBAndScheduleReadback(
	ID3D11ShaderResourceView* InDepthSRV,
	uint32 InDepthWidth,
	uint32 InDepthHeight,
	uint32 InDepthTopLeftX,
	uint32 InDepthTopLeftY,
	const FMatrix& CaptureViewMatrix,
	const FMatrix& CaptureProjectionMatrix,
	const FVector& CaptureCameraPosition)
{
	if (!InDepthSRV || !EnsureResources(InDepthWidth, InDepthHeight) || !CopyDepthCS || !ReduceMinCS || !DeviceContext)
	{
		return false;
	}

	UpdateReduceConstants(InDepthWidth, InDepthHeight, InDepthWidth, InDepthHeight, InDepthTopLeftX, InDepthTopLeftY);

	ID3D11ShaderResourceView* CopySRV = InDepthSRV;
	ID3D11UnorderedAccessView* CopyUAV = HiZMipUAVs.empty() ? nullptr : HiZMipUAVs[0];
	if (!CopyUAV)
	{
		return false;
	}

	DeviceContext->CSSetShader(CopyDepthCS, nullptr, 0);
	DeviceContext->CSSetConstantBuffers(0, 1, &ReduceConstantBuffer);
	DeviceContext->CSSetShaderResources(0, 1, &CopySRV);
	DeviceContext->CSSetUnorderedAccessViews(0, 1, &CopyUAV, nullptr);
	DeviceContext->Dispatch(
		(InDepthWidth + GThreadGroupSize - 1u) / GThreadGroupSize,
		(InDepthHeight + GThreadGroupSize - 1u) / GThreadGroupSize,
		1);

	ID3D11ShaderResourceView* NullSRV = nullptr;
	ID3D11UnorderedAccessView* NullUAV = nullptr;
	DeviceContext->CSSetShaderResources(0, 1, &NullSRV);
	DeviceContext->CSSetUnorderedAccessViews(0, 1, &NullUAV, nullptr);

	DeviceContext->CSSetShader(ReduceMinCS, nullptr, 0);
	for (uint32 MipIndex = 1; MipIndex < HiZMipCount; ++MipIndex)
	{
		const uint32 SrcWidth = (std::max)(DepthWidth >> (MipIndex - 1u), 1u);
		const uint32 SrcHeight = (std::max)(DepthHeight >> (MipIndex - 1u), 1u);
		const uint32 DstWidth = (std::max)(DepthWidth >> MipIndex, 1u);
		const uint32 DstHeight = (std::max)(DepthHeight >> MipIndex, 1u);
		UpdateReduceConstants(SrcWidth, SrcHeight, DstWidth, DstHeight);

		ID3D11ShaderResourceView* SrcMipSRV = HiZMipSRVs[MipIndex - 1u];
		ID3D11UnorderedAccessView* DstMipUAV = HiZMipUAVs[MipIndex];
		DeviceContext->CSSetConstantBuffers(0, 1, &ReduceConstantBuffer);
		DeviceContext->CSSetShaderResources(0, 1, &SrcMipSRV);
		DeviceContext->CSSetUnorderedAccessViews(0, 1, &DstMipUAV, nullptr);
		DeviceContext->Dispatch(
			(DstWidth + GThreadGroupSize - 1u) / GThreadGroupSize,
			(DstHeight + GThreadGroupSize - 1u) / GThreadGroupSize,
			1);
		DeviceContext->CSSetShaderResources(0, 1, &NullSRV);
		DeviceContext->CSSetUnorderedAccessViews(0, 1, &NullUAV, nullptr);
	}

	DeviceContext->CSSetShader(nullptr, nullptr, 0);

	int32 SelectedSlotIndex = -1;
	for (uint32 SlotOffset = 0; SlotOffset < static_cast<uint32>(ReadbackSlots.size()); ++SlotOffset)
	{
		const uint32 CandidateIndex = (NextReadbackSlot + SlotOffset) % static_cast<uint32>(ReadbackSlots.size());
		FReadbackSlot& CandidateSlot = ReadbackSlots[CandidateIndex];
		if (CandidateSlot.Texture && !CandidateSlot.bPending)
		{
			SelectedSlotIndex = static_cast<int32>(CandidateIndex);
			break;
		}
	}

	if (SelectedSlotIndex >= 0)
	{
		FReadbackSlot& Slot = ReadbackSlots[SelectedSlotIndex];
		const UINT SourceSubresource = D3D11CalcSubresource(ReadbackMipIndex, 0, HiZMipCount);
		DeviceContext->CopySubresourceRegion(Slot.Texture, 0, 0, 0, 0, HiZTexture, SourceSubresource, nullptr);
		Slot.SubmittedFrame = CurrentFrameNumber;
		Slot.bPending = true;
		Slot.ViewMatrix = CaptureViewMatrix;
		Slot.ProjectionMatrix = CaptureProjectionMatrix;
		Slot.ViewProjectionMatrix = CaptureViewMatrix * CaptureProjectionMatrix;
		Slot.CameraPosition = CaptureCameraPosition;
		Slot.ViewForward = ExtractForwardFromViewMatrix(CaptureViewMatrix);
		Slot.ViewportWidth = InDepthWidth;
		Slot.ViewportHeight = InDepthHeight;
		NextReadbackSlot = (static_cast<uint32>(SelectedSlotIndex) + 1u) % static_cast<uint32>(ReadbackSlots.size());
	}

	return true;
}

bool FHiZOcclusion::CreateShaders()
{
	const std::wstring ShaderPath = FPaths::ShaderDir().wstring() + L"HiZOcclusionCS.hlsl";

	auto CopyResource = FShaderResource::GetOrCompile(ShaderPath.c_str(), "CopyDepthMain", "cs_5_0");
	if (!CopyResource || FAILED(Device->CreateComputeShader(CopyResource->GetBufferPointer(), CopyResource->GetBufferSize(), nullptr, &CopyDepthCS)))
	{
		return false;
	}

	auto ReduceResource = FShaderResource::GetOrCompile(ShaderPath.c_str(), "ReduceMinMain", "cs_5_0");
	return ReduceResource && SUCCEEDED(Device->CreateComputeShader(ReduceResource->GetBufferPointer(), ReduceResource->GetBufferSize(), nullptr, &ReduceMinCS));
}

bool FHiZOcclusion::CreateConstantBuffer()
{
	D3D11_BUFFER_DESC Desc = {};
	Desc.Usage = D3D11_USAGE_DYNAMIC;
	Desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	Desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	Desc.ByteWidth = sizeof(FHiZReduceConstants);
	Desc.ByteWidth = (Desc.ByteWidth + 15u) & ~15u;
	return SUCCEEDED(Device->CreateBuffer(&Desc, nullptr, &ReduceConstantBuffer));
}

bool FHiZOcclusion::EnsureResources(uint32 InDepthWidth, uint32 InDepthHeight)
{
	if (!Device)
	{
		return false;
	}

	const uint32 DesiredMipCount = ComputeMipCount(InDepthWidth, InDepthHeight);
	const uint32 DesiredReadbackMip = SelectReadbackMip(InDepthWidth, InDepthHeight);
	const uint32 DesiredReadbackWidth = (std::max)(InDepthWidth >> DesiredReadbackMip, 1u);
	const uint32 DesiredReadbackHeight = (std::max)(InDepthHeight >> DesiredReadbackMip, 1u);

	if (HiZTexture && DepthWidth == InDepthWidth && DepthHeight == InDepthHeight &&
		HiZMipCount == DesiredMipCount && ReadbackMipIndex == DesiredReadbackMip &&
		ReadbackWidth == DesiredReadbackWidth && ReadbackHeight == DesiredReadbackHeight)
	{
		return true;
	}

	OnResize();

	DepthWidth = InDepthWidth;
	DepthHeight = InDepthHeight;
	HiZMipCount = DesiredMipCount;
	ReadbackMipIndex = DesiredReadbackMip;
	ReadbackWidth = DesiredReadbackWidth;
	ReadbackHeight = DesiredReadbackHeight;

	D3D11_TEXTURE2D_DESC HiZDesc = {};
	HiZDesc.Width = DepthWidth;
	HiZDesc.Height = DepthHeight;
	HiZDesc.MipLevels = HiZMipCount;
	HiZDesc.ArraySize = 1;
	HiZDesc.Format = DXGI_FORMAT_R32_FLOAT;
	HiZDesc.SampleDesc.Count = 1;
	HiZDesc.Usage = D3D11_USAGE_DEFAULT;
	HiZDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	if (FAILED(Device->CreateTexture2D(&HiZDesc, nullptr, &HiZTexture)))
	{
		OnResize();
		return false;
	}

	HiZMipSRVs.reserve(HiZMipCount);
	HiZMipUAVs.reserve(HiZMipCount);
	for (uint32 MipIndex = 0; MipIndex < HiZMipCount; ++MipIndex)
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
		SRVDesc.Format = DXGI_FORMAT_R32_FLOAT;
		SRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		SRVDesc.Texture2D.MostDetailedMip = MipIndex;
		SRVDesc.Texture2D.MipLevels = 1;

		ID3D11ShaderResourceView* SRV = nullptr;
		if (FAILED(Device->CreateShaderResourceView(HiZTexture, &SRVDesc, &SRV)))
		{
			OnResize();
			return false;
		}
		HiZMipSRVs.push_back(SRV);

		D3D11_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};
		UAVDesc.Format = DXGI_FORMAT_R32_FLOAT;
		UAVDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		UAVDesc.Texture2D.MipSlice = MipIndex;

		ID3D11UnorderedAccessView* UAV = nullptr;
		if (FAILED(Device->CreateUnorderedAccessView(HiZTexture, &UAVDesc, &UAV)))
		{
			OnResize();
			return false;
		}
		HiZMipUAVs.push_back(UAV);
	}

	for (FReadbackSlot& Slot : ReadbackSlots)
	{
		D3D11_TEXTURE2D_DESC ReadbackDesc = {};
		ReadbackDesc.Width = ReadbackWidth;
		ReadbackDesc.Height = ReadbackHeight;
		ReadbackDesc.MipLevels = 1;
		ReadbackDesc.ArraySize = 1;
		ReadbackDesc.Format = DXGI_FORMAT_R32_FLOAT;
		ReadbackDesc.SampleDesc.Count = 1;
		ReadbackDesc.Usage = D3D11_USAGE_STAGING;
		ReadbackDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

		if (FAILED(Device->CreateTexture2D(&ReadbackDesc, nullptr, &Slot.Texture)))
		{
			OnResize();
			return false;
		}
	}


	D3D11_TEXTURE2D_DESC DebugDesc = {};
	DebugDesc.Width = ReadbackWidth;
	DebugDesc.Height = ReadbackHeight;
	DebugDesc.MipLevels = 1;
	DebugDesc.ArraySize = 1;
	DebugDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	DebugDesc.SampleDesc.Count = 1;
	DebugDesc.Usage = D3D11_USAGE_DYNAMIC;
	DebugDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	DebugDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	if (FAILED(Device->CreateTexture2D(&DebugDesc, nullptr, &DebugPreviewTexture)))
	{
		OnResize();
		return false;
	}

	if (FAILED(Device->CreateShaderResourceView(DebugPreviewTexture, nullptr, &DebugPreviewSRV)))
	{
		OnResize();
		return false;
	}

	ResolvedDepths.resize(static_cast<size_t>(ReadbackWidth) * static_cast<size_t>(ReadbackHeight));
	return true;
}

void FHiZOcclusion::ReleaseReadbackSlots()
{
	for (FReadbackSlot& Slot : ReadbackSlots)
	{
		SafeRelease(Slot.Texture);
		Slot.ViewMatrix = FMatrix::Identity;
		Slot.ProjectionMatrix = FMatrix::Identity;
		Slot.ViewProjectionMatrix = FMatrix::Identity;
		Slot.CameraPosition = FVector::ZeroVector;
		Slot.ViewForward = FVector::ForwardVector;
		Slot.ViewportWidth = 0;
		Slot.ViewportHeight = 0;
		Slot.SubmittedFrame = 0;
		Slot.bPending = false;
	}
}

void FHiZOcclusion::ResolveOldReadback()
{
	if (!DeviceContext || !ReadbackWidth || !ReadbackHeight)
	{
		return;
	}

	for (FReadbackSlot& Slot : ReadbackSlots)
	{
		if (!Slot.Texture || !Slot.bPending || CurrentFrameNumber < (Slot.SubmittedFrame + 2u))
		{
			continue;
		}

		if (bHasResolvedReadback && Slot.SubmittedFrame < LastResolvedFrameNumber)
		{
			Slot.bPending = false;
			continue;
		}

		D3D11_MAPPED_SUBRESOURCE Mapped = {};
		const HRESULT Hr = DeviceContext->Map(Slot.Texture, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &Mapped);
		if (Hr == DXGI_ERROR_WAS_STILL_DRAWING)
		{
			continue;
		}
		if (FAILED(Hr))
		{
			Slot.bPending = false;
			continue;
		}

		for (uint32 Y = 0; Y < ReadbackHeight; ++Y)
		{
			const float* SourceRow = reinterpret_cast<const float*>(static_cast<const uint8*>(Mapped.pData) + static_cast<size_t>(Mapped.RowPitch) * Y);
			float* DestRow = ResolvedDepths.data() + static_cast<size_t>(Y) * ReadbackWidth;
			memcpy(DestRow, SourceRow, sizeof(float) * ReadbackWidth);
		}

		DeviceContext->Unmap(Slot.Texture, 0);
		Slot.bPending = false;

		bHasResolvedReadback = true;
		LastResolvedFrameNumber = Slot.SubmittedFrame;

		if (DebugPreviewTexture)
		{
			D3D11_MAPPED_SUBRESOURCE DebugMapped = {};
			if (SUCCEEDED(DeviceContext->Map(DebugPreviewTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &DebugMapped)))
			{
				for (uint32 Y = 0; Y < ReadbackHeight; ++Y)
				{
					const float* SourceRow = ResolvedDepths.data() + static_cast<size_t>(Y) * ReadbackWidth;
					uint8* DestRow = static_cast<uint8*>(DebugMapped.pData) + static_cast<size_t>(DebugMapped.RowPitch) * Y;
					for (uint32 X = 0; X < ReadbackWidth; ++X)
					{
						const float Depth = (std::max)(0.0f, (std::min)(1.0f, SourceRow[X]));
						const float Visual = std::pow(Depth, 0.15f);
						const uint8 Red = static_cast<uint8>(Visual * 255.0f + 0.5f);
						uint8* Pixel = DestRow + static_cast<size_t>(X) * 4u;
						Pixel[0] = 0;
						Pixel[1] = 0;
						Pixel[2] = Red;
						Pixel[3] = 255;
					}
				}
				DeviceContext->Unmap(DebugPreviewTexture, 0);
			}
		}

		ResolvedViewMatrix = Slot.ViewMatrix;
		ResolvedProjectionMatrix = Slot.ProjectionMatrix;
		ResolvedViewProjectionMatrix = Slot.ViewProjectionMatrix;
		ResolvedCameraPosition = Slot.CameraPosition;
		ResolvedViewForward = Slot.ViewForward;
		ResolvedViewportWidth = Slot.ViewportWidth;
		ResolvedViewportHeight = Slot.ViewportHeight;
	}
}

void FHiZOcclusion::DecayVisibilityHistory()
{
	for (auto It = VisibilityHistory.begin(); It != VisibilityHistory.end(); )
	{
		if (It->second.GraceFrames > 0)
		{
			--It->second.GraceFrames;
		}

		if (It->second.GraceFrames == 0)
		{
			It = VisibilityHistory.erase(It);
		}
		else
		{
			++It;
		}
	}
}

void FHiZOcclusion::MarkVisible(const void* VisibilityKey)
{
	if (!VisibilityKey)
	{
		return;
	}

	VisibilityHistory[VisibilityKey].GraceFrames = GVisibleGraceFrameCount;
}

bool FHiZOcclusion::HasCompatibleReadback(
	const FVector& CurrentCameraPosition,
	const FMatrix& CurrentViewMatrix,
	const FMatrix& CurrentProjectionMatrix,
	uint32 CurrentViewportWidth,
	uint32 CurrentViewportHeight) const
{
	if (!bHasResolvedReadback || ResolvedDepths.empty() || !ReadbackWidth || !ReadbackHeight)
	{
		return false;
	}

	if (CurrentViewportWidth != ResolvedViewportWidth || CurrentViewportHeight != ResolvedViewportHeight)
	{
		return false;
	}

	const FVector CameraDelta = CurrentCameraPosition - ResolvedCameraPosition;
	if (CameraDelta.SizeSquared() > (GCameraTranslationThreshold * GCameraTranslationThreshold))
	{
		return false;
	}

	const FVector CurrentForward = ExtractForwardFromViewMatrix(CurrentViewMatrix);
	if (FVector::DotProduct(CurrentForward, ResolvedViewForward) < GCameraRotationDotThreshold)
	{
		return false;
	}

	for (int32 Row = 0; Row < 4; ++Row)
	{
		for (int32 Col = 0; Col < 4; ++Col)
		{
			if (std::fabs(CurrentProjectionMatrix[Row][Col] - ResolvedProjectionMatrix[Row][Col]) > 0.001f)
			{
				return false;
			}
		}
	}

	return true;
}

FHiZOcclusion::EVisibilityResult FHiZOcclusion::EvaluateVisibilityConservative(
	const void* VisibilityKey,
	const FBoxSphereBounds& Bounds,
	const FVector& CurrentCameraPosition,
	const FMatrix& CurrentViewMatrix,
	const FMatrix& CurrentProjectionMatrix,
	uint32 CurrentViewportWidth,
	uint32 CurrentViewportHeight) const
{
	if (!HasCompatibleReadback(CurrentCameraPosition, CurrentViewMatrix, CurrentProjectionMatrix, CurrentViewportWidth, CurrentViewportHeight))
	{
		return EVisibilityResult::Visible_NoCompatibleReadback;
	}

	const auto It = VisibilityHistory.find(VisibilityKey);
	if (It != VisibilityHistory.end() && It->second.GraceFrames > 0)
	{
		return EVisibilityResult::Visible_History;
	}

	const FVector ToObject = Bounds.Center - CurrentCameraPosition;
	const float DistanceSquared = ToObject.SizeSquared();
	if (DistanceSquared < (GNearDistanceProtection * GNearDistanceProtection))
	{
		return EVisibilityResult::Visible_Near;
	}

	FProjectedReadbackRect Rect;
	if (!ProjectBoundsToReadbackRect(Bounds, Rect))
	{
		return EVisibilityResult::Visible_ProjectionFail;
	}

	if (Rect.bTouchesScreenEdge)
	{
		return EVisibilityResult::Visible_ScreenEdge;
	}

	if (Rect.bTooSmallForReliableCull)
	{
		return EVisibilityResult::Visible_SmallRect;
	}

	if (!IsRectOccludedConservative(Rect))
	{
		return EVisibilityResult::Visible_SampleMismatch;
	}

	return EVisibilityResult::Culled_Occluded;
}

bool FHiZOcclusion::ProjectBoundsToReadbackRect(const FBoxSphereBounds& Bounds, FProjectedReadbackRect& OutRect) const
{
	if (!ReadbackWidth || !ReadbackHeight)
	{
		return false;
	}

	const FVector InflatedExtent = Bounds.BoxExtent * GBoundsInflateScale;
	const FVector Corners[8] =
	{
		Bounds.Center + FVector(-InflatedExtent.X, -InflatedExtent.Y, -InflatedExtent.Z),
		Bounds.Center + FVector(-InflatedExtent.X, -InflatedExtent.Y,  InflatedExtent.Z),
		Bounds.Center + FVector(-InflatedExtent.X,  InflatedExtent.Y, -InflatedExtent.Z),
		Bounds.Center + FVector(-InflatedExtent.X,  InflatedExtent.Y,  InflatedExtent.Z),
		Bounds.Center + FVector( InflatedExtent.X, -InflatedExtent.Y, -InflatedExtent.Z),
		Bounds.Center + FVector( InflatedExtent.X, -InflatedExtent.Y,  InflatedExtent.Z),
		Bounds.Center + FVector( InflatedExtent.X,  InflatedExtent.Y, -InflatedExtent.Z),
		Bounds.Center + FVector( InflatedExtent.X,  InflatedExtent.Y,  InflatedExtent.Z)
	};

	float MinNdcX = 1.0f;
	float MinNdcY = 1.0f;
	float MaxNdcX = -1.0f;
	float MaxNdcY = -1.0f;
	OutRect.ClosestDepth = 0.0f;

	for (const FVector& Corner : Corners)
	{
		const FVector4 Clip = TransformPoint(Corner, ResolvedViewProjectionMatrix);
		if (Clip.W <= 1.0e-4f)
		{
			return false;
		}

		const float InvW = 1.0f / Clip.W;
		const float NdcX = Clip.X * InvW;
		const float NdcY = Clip.Y * InvW;
		const float NdcZ = Clip.Z * InvW;

		if (NdcZ < 0.0f || NdcZ > 1.0f)
		{
			return false;
		}

		MinNdcX = (std::min)(MinNdcX, NdcX);
		MinNdcY = (std::min)(MinNdcY, NdcY);
		MaxNdcX = (std::max)(MaxNdcX, NdcX);
		MaxNdcY = (std::max)(MaxNdcY, NdcY);
		OutRect.ClosestDepth = (std::max)(OutRect.ClosestDepth, NdcZ);
	}

	if (MaxNdcX < -1.0f || MinNdcX > 1.0f || MaxNdcY < -1.0f || MinNdcY > 1.0f)
	{
		return false;
	}

	const float ClampedMinNdcX = (std::max)(-1.0f, MinNdcX);
	const float ClampedMaxNdcX = (std::min)( 1.0f, MaxNdcX);
	const float ClampedMinNdcY = (std::max)(-1.0f, MinNdcY);
	const float ClampedMaxNdcY = (std::min)( 1.0f, MaxNdcY);

	const float MinU = ClampedMinNdcX * 0.5f + 0.5f;
	const float MaxU = ClampedMaxNdcX * 0.5f + 0.5f;
	const float MinV = 1.0f - (ClampedMaxNdcY * 0.5f + 0.5f);
	const float MaxV = 1.0f - (ClampedMinNdcY * 0.5f + 0.5f);

	const float PixelMinX = MinU * static_cast<float>(ReadbackWidth) - GScreenTexelExpand;
	const float PixelMaxX = MaxU * static_cast<float>(ReadbackWidth) + GScreenTexelExpand;
	const float PixelMinY = MinV * static_cast<float>(ReadbackHeight) - GScreenTexelExpand;
	const float PixelMaxY = MaxV * static_cast<float>(ReadbackHeight) + GScreenTexelExpand;

	OutRect.MinX = static_cast<int32>(std::floor(PixelMinX));
	OutRect.MaxX = static_cast<int32>(std::ceil(PixelMaxX));
	OutRect.MinY = static_cast<int32>(std::floor(PixelMinY));
	OutRect.MaxY = static_cast<int32>(std::ceil(PixelMaxY));

	OutRect.bTouchesScreenEdge =
		(MinNdcX <= -0.98f) || (MaxNdcX >= 0.98f) ||
		(MinNdcY <= -0.98f) || (MaxNdcY >= 0.98f);

	OutRect.MinX = (std::max)(OutRect.MinX, 0);
	OutRect.MinY = (std::max)(OutRect.MinY, 0);
	OutRect.MaxX = (std::min)(OutRect.MaxX, static_cast<int32>(ReadbackWidth) - 1);
	OutRect.MaxY = (std::min)(OutRect.MaxY, static_cast<int32>(ReadbackHeight) - 1);

	if (OutRect.MinX > OutRect.MaxX || OutRect.MinY > OutRect.MaxY)
	{
		return false;
	}

	const int32 RectWidth = OutRect.MaxX - OutRect.MinX + 1;
	const int32 RectHeight = OutRect.MaxY - OutRect.MinY + 1;
	OutRect.bTooSmallForReliableCull = (RectWidth < 3 || RectHeight < 3);

	return true;
}

bool FHiZOcclusion::IsRectOccludedConservative(const FProjectedReadbackRect& Rect) const
{
	if (ResolvedDepths.empty() || !ReadbackWidth || !ReadbackHeight)
	{
		return false;
	}

	int32 SampleMinX = Rect.MinX;
	int32 SampleMaxX = Rect.MaxX;
	int32 SampleMinY = Rect.MinY;
	int32 SampleMaxY = Rect.MaxY;

	if ((SampleMaxX - SampleMinX + 1) > 4)
	{
		++SampleMinX;
		--SampleMaxX;
	}
	if ((SampleMaxY - SampleMinY + 1) > 4)
	{
		++SampleMinY;
		--SampleMaxY;
	}

	if (SampleMinX > SampleMaxX || SampleMinY > SampleMaxY)
	{
		return false;
	}

	const float SampleFractions[3] = { 0.2f, 0.5f, 0.8f };
	for (float SampleYFraction : SampleFractions)
	{
		for (float SampleXFraction : SampleFractions)
		{
			const float SampleXF = static_cast<float>(SampleMinX) + static_cast<float>(SampleMaxX - SampleMinX) * SampleXFraction;
			const float SampleYF = static_cast<float>(SampleMinY) + static_cast<float>(SampleMaxY - SampleMinY) * SampleYFraction;

			int32 SampleX = static_cast<int32>(std::round(SampleXF));
			int32 SampleY = static_cast<int32>(std::round(SampleYF));

			SampleX = (std::max)(Rect.MinX, (std::min)(Rect.MaxX, SampleX));
			SampleY = (std::max)(Rect.MinY, (std::min)(Rect.MaxY, SampleY));

			const float OccluderDepth = ResolvedDepths[static_cast<size_t>(SampleY) * ReadbackWidth + static_cast<size_t>(SampleX)];
			if ((Rect.ClosestDepth + GDepthBias) >= OccluderDepth)
			{
				return false;
			}
		}
	}

	return true;
}


FHiZOcclusion::FDebugInfo FHiZOcclusion::GetDebugInfo() const
{
	FDebugInfo Info;
	Info.bInitialized = (Device != nullptr && DeviceContext != nullptr);
	Info.bHasResolvedReadback = bHasResolvedReadback;
	Info.DepthWidth = DepthWidth;
	Info.DepthHeight = DepthHeight;
	Info.HiZMipCount = HiZMipCount;
	Info.ReadbackMipIndex = ReadbackMipIndex;
	Info.ReadbackWidth = ReadbackWidth;
	Info.ReadbackHeight = ReadbackHeight;
	Info.CurrentFrameNumber = CurrentFrameNumber;
	Info.LastResolvedFrameNumber = LastResolvedFrameNumber;
	Info.LastInputCommandCount = LastInputCommandCount;
	Info.LastEligibleCommandCount = LastEligibleCommandCount;
	Info.LastUnsupportedCommandCount = LastUnsupportedCommandCount;
	Info.LastUnsupportedNoSceneProxyCount = LastUnsupportedNoSceneProxyCount;
	Info.LastUnsupportedNonOpaquePassCount = LastUnsupportedNonOpaquePassCount;
	Info.LastUnsupportedProxyTypeCount = LastUnsupportedProxyTypeCount;
	Info.LastVisibleCommandCount = LastVisibleCommandCount;
	Info.LastCulledCommandCount = LastCulledCommandCount;
	Info.LastKeepNoCompatibleReadbackCount = LastKeepNoCompatibleReadbackCount;
	Info.LastKeepHistoryCount = LastKeepHistoryCount;
	Info.LastKeepNearCount = LastKeepNearCount;
	Info.LastKeepProjectionFailCount = LastKeepProjectionFailCount;
	Info.LastKeepScreenEdgeCount = LastKeepScreenEdgeCount;
	Info.LastKeepSmallRectCount = LastKeepSmallRectCount;
	Info.LastKeepSampleVisibleCount = LastKeepSampleVisibleCount;
	std::snprintf(Info.FirstUnsupportedDebugName, sizeof(Info.FirstUnsupportedDebugName), "%s", FirstUnsupportedDebugName);
	for (const FReadbackSlot& Slot : ReadbackSlots)
	{
		if (Slot.bPending)
		{
			++Info.PendingReadbackCount;
		}
	}
	return Info;
}

void FHiZOcclusion::UpdateReduceConstants(uint32 SrcWidth, uint32 SrcHeight, uint32 DstWidth, uint32 DstHeight, uint32 SrcOffsetX, uint32 SrcOffsetY)
{
	if (!ReduceConstantBuffer || !DeviceContext)
	{
		return;
	}

	D3D11_MAPPED_SUBRESOURCE Mapped = {};
	if (FAILED(DeviceContext->Map(ReduceConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped)))
	{
		return;
	}

	FHiZReduceConstants* Constants = reinterpret_cast<FHiZReduceConstants*>(Mapped.pData);
	Constants->SrcWidth = SrcWidth;
	Constants->SrcHeight = SrcHeight;
	Constants->DstWidth = DstWidth;
	Constants->DstHeight = DstHeight;
	Constants->SrcOffsetX = SrcOffsetX;
	Constants->SrcOffsetY = SrcOffsetY;
	DeviceContext->Unmap(ReduceConstantBuffer, 0);
}
