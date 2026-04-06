#pragma once

#include "CoreMinimal.h"
#include "Renderer/RenderCommand.h"
#include <d3d11.h>
#include <unordered_map>

struct FBoxSphereBounds;

class ENGINE_API FHiZOcclusion
{
public:
	struct FDebugInfo
	{
		bool bInitialized = false;
		bool bHasResolvedReadback = false;
		uint32 DepthWidth = 0;
		uint32 DepthHeight = 0;
		uint32 HiZMipCount = 0;
		uint32 ReadbackMipIndex = 0;
		uint32 ReadbackWidth = 0;
		uint32 ReadbackHeight = 0;
		uint64 CurrentFrameNumber = 0;
		uint64 LastResolvedFrameNumber = 0;
		uint32 PendingReadbackCount = 0;
		uint32 LastInputCommandCount = 0;
		uint32 LastEligibleCommandCount = 0;
		uint32 LastUnsupportedCommandCount = 0;
		uint32 LastUnsupportedNoSceneProxyCount = 0;
		uint32 LastUnsupportedNonOpaquePassCount = 0;
		uint32 LastUnsupportedProxyTypeCount = 0;
		uint32 LastVisibleCommandCount = 0;
		uint32 LastCulledCommandCount = 0;
		uint32 LastKeepNoCompatibleReadbackCount = 0;
		uint32 LastKeepHistoryCount = 0;
		uint32 LastKeepNearCount = 0;
		uint32 LastKeepProjectionFailCount = 0;
		uint32 LastKeepScreenEdgeCount = 0;
		uint32 LastKeepSmallRectCount = 0;
		uint32 LastKeepSampleVisibleCount = 0;
		char FirstUnsupportedDebugName[32] = "";
	};
	FHiZOcclusion() = default;
	~FHiZOcclusion();

	bool Initialize(ID3D11Device* InDevice, ID3D11DeviceContext* InDeviceContext);
	void Release();
	void BeginFrame(uint64 InFrameNumber);
	void OnResize();

	bool ApplyCPUCulling(
		FRenderCommandQueue& InOutQueue,
		const FMatrix& CurrentViewMatrix,
		const FMatrix& CurrentProjectionMatrix,
		const FVector& CurrentCameraPosition,
		uint32 CurrentViewportWidth,
		uint32 CurrentViewportHeight);

	bool BuildHZBAndScheduleReadback(
		ID3D11ShaderResourceView* InDepthSRV,
		uint32 InDepthWidth,
		uint32 InDepthHeight,
		uint32 InDepthTopLeftX,
		uint32 InDepthTopLeftY,
		const FMatrix& CaptureViewMatrix,
		const FMatrix& CaptureProjectionMatrix,
		const FVector& CaptureCameraPosition);

	uint32 GetMipCount() const { return static_cast<uint32>(HiZMipSRVs.size()); }
	ID3D11ShaderResourceView* GetMipSRV(uint32 InMipIndex) const { return InMipIndex < HiZMipSRVs.size() ? HiZMipSRVs[InMipIndex] : nullptr; }
	ID3D11ShaderResourceView* GetDebugPreviewSRV() const { return DebugPreviewSRV; }
	FDebugInfo GetDebugInfo() const;

private:
	struct FHiZReduceConstants
	{
		uint32 SrcWidth = 0;
		uint32 SrcHeight = 0;
		uint32 DstWidth = 0;
		uint32 DstHeight = 0;
		uint32 SrcOffsetX = 0;
		uint32 SrcOffsetY = 0;
	};

	struct FReadbackSlot
	{
		ID3D11Texture2D* Texture = nullptr;
		uint64 SubmittedFrame = 0;
		bool bPending = false;
		FMatrix ViewMatrix = FMatrix::Identity;
		FMatrix ProjectionMatrix = FMatrix::Identity;
		FMatrix ViewProjectionMatrix = FMatrix::Identity;
		FVector CameraPosition = FVector::ZeroVector;
		FVector ViewForward = FVector::ForwardVector;
		uint32 ViewportWidth = 0;
		uint32 ViewportHeight = 0;
	};

	struct FVisibilityHistory
	{
		uint8 GraceFrames = 0;
	};

	enum class EVisibilityResult : uint8
	{
		Visible_NoCompatibleReadback,
		Visible_History,
		Visible_Near,
		Visible_ProjectionFail,
		Visible_ScreenEdge,
		Visible_SmallRect,
		Visible_SampleMismatch,
		Culled_Occluded,
	};

	struct FProjectedReadbackRect
	{
		int32 MinX = 0;
		int32 MinY = 0;
		int32 MaxX = 0;
		int32 MaxY = 0;
		float ClosestDepth = 0.0f;
		bool bTouchesScreenEdge = false;
		bool bTooSmallForReliableCull = false;
	};

	bool EnsureResources(uint32 InDepthWidth, uint32 InDepthHeight);
	bool CreateShaders();
	bool CreateConstantBuffer();
	void ReleaseReadbackSlots();
	void ResolveOldReadback();
	void DecayVisibilityHistory();
	void MarkVisible(const void* VisibilityKey);
	EVisibilityResult EvaluateVisibilityConservative(
		const void* VisibilityKey,
		const FBoxSphereBounds& Bounds,
		const FVector& CurrentCameraPosition,
		const FMatrix& CurrentViewMatrix,
		const FMatrix& CurrentProjectionMatrix,
		uint32 CurrentViewportWidth,
		uint32 CurrentViewportHeight) const;
	bool HasCompatibleReadback(
		const FVector& CurrentCameraPosition,
		const FMatrix& CurrentViewMatrix,
		const FMatrix& CurrentProjectionMatrix,
		uint32 CurrentViewportWidth,
		uint32 CurrentViewportHeight) const;
	bool ProjectBoundsToReadbackRect(const FBoxSphereBounds& Bounds, FProjectedReadbackRect& OutRect) const;
	bool IsRectOccludedConservative(const FProjectedReadbackRect& Rect) const;
	void UpdateReduceConstants(uint32 SrcWidth, uint32 SrcHeight, uint32 DstWidth, uint32 DstHeight, uint32 SrcOffsetX = 0, uint32 SrcOffsetY = 0);

private:
	ID3D11Device* Device = nullptr;
	ID3D11DeviceContext* DeviceContext = nullptr;
	ID3D11ComputeShader* CopyDepthCS = nullptr;
	ID3D11ComputeShader* ReduceMinCS = nullptr;
	ID3D11Buffer* ReduceConstantBuffer = nullptr;

	ID3D11Texture2D* HiZTexture = nullptr;
	TArray<ID3D11ShaderResourceView*> HiZMipSRVs;
	TArray<ID3D11UnorderedAccessView*> HiZMipUAVs;
	TStaticArray<FReadbackSlot, 3> ReadbackSlots;

	uint32 DepthWidth = 0;
	uint32 DepthHeight = 0;
	uint32 HiZMipCount = 0;
	uint32 ReadbackMipIndex = 0;
	uint32 ReadbackWidth = 0;
	uint32 ReadbackHeight = 0;
	uint64 CurrentFrameNumber = 0;
	uint32 NextReadbackSlot = 0;

	TArray<float> ResolvedDepths;
	bool bHasResolvedReadback = false;
	FMatrix ResolvedViewMatrix = FMatrix::Identity;
	FMatrix ResolvedProjectionMatrix = FMatrix::Identity;
	FMatrix ResolvedViewProjectionMatrix = FMatrix::Identity;
	FVector ResolvedCameraPosition = FVector::ZeroVector;
	FVector ResolvedViewForward = FVector::ForwardVector;
	uint32 ResolvedViewportWidth = 0;
	uint32 ResolvedViewportHeight = 0;
	uint64 LastResolvedFrameNumber = 0;

	ID3D11Texture2D* DebugPreviewTexture = nullptr;
	ID3D11ShaderResourceView* DebugPreviewSRV = nullptr;

	uint32 LastInputCommandCount = 0;
	uint32 LastEligibleCommandCount = 0;
	uint32 LastUnsupportedCommandCount = 0;
	uint32 LastUnsupportedNoSceneProxyCount = 0;
	uint32 LastUnsupportedNonOpaquePassCount = 0;
	uint32 LastUnsupportedProxyTypeCount = 0;
	uint32 LastVisibleCommandCount = 0;
	uint32 LastCulledCommandCount = 0;
	uint32 LastKeepNoCompatibleReadbackCount = 0;
	uint32 LastKeepHistoryCount = 0;
	uint32 LastKeepNearCount = 0;
	uint32 LastKeepProjectionFailCount = 0;
	uint32 LastKeepScreenEdgeCount = 0;
	uint32 LastKeepSmallRectCount = 0;
	uint32 LastKeepSampleVisibleCount = 0;
	char FirstUnsupportedDebugName[32] = {};

	mutable std::unordered_map<const void*, FVisibilityHistory> VisibilityHistory;
};
