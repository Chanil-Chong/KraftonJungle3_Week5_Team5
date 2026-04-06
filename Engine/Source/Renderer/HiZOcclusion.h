#pragma once

#include "CoreMinimal.h"
#include "Renderer/SceneRenderTypes.h"
#include <d3d11.h>
#include <memory>
#include <unordered_map>

class FRenderer;
class FComputeShader;
struct FSceneRenderFrame;

class ENGINE_API FHiZOcclusion
{
public:
	explicit FHiZOcclusion(FRenderer* InRenderer = nullptr)
		: Renderer(InRenderer)
	{
	}

	~FHiZOcclusion();

	FHiZOcclusion(const FHiZOcclusion&) = delete;
	FHiZOcclusion& operator=(const FHiZOcclusion&) = delete;

	FHiZOcclusion(FHiZOcclusion&&) noexcept = default;
	FHiZOcclusion& operator=(FHiZOcclusion&&) noexcept = default;

	void SetRenderer(FRenderer* InRenderer) { Renderer = InRenderer; }
	bool Initialize();
	void Release();

	bool Prepare(const FSceneRenderFrame& Frame, const FMatrix& ViewMatrix, const FMatrix& ProjectionMatrix);
	void FinalizeFromActiveDepth();
	void ExecuteOpaqueQueue(const TArray<FMeshDrawCommand>& InCommands) const;
	bool HasPreparedCulling() const { return bPreparedCulling && CullCommandCount > 0; }

	ID3D11ShaderResourceView* GetHiZFullSRV() const;
	ID3D11ShaderResourceView* GetHiZMipSRV(uint32 MipIndex) const;
	uint32 GetResourceMipCount() const;
	ID3D11ShaderResourceView* GetHiZDebugSRV(uint32 MipIndex);
	bool HasValidHistoryForCurrentView() const;
	uint64 GetCurrentDebugViewKey() const;

private:
	struct FHiZCullCommandGPU
	{
		FVector Center = FVector::ZeroVector;
		float Padding0 = 0.0f;
		FVector Extent = FVector::ZeroVector;
		uint32 Flags = 0;
	};

	struct FHiZCopyConstants
	{
		uint32 SrcOffsetX = 0;
		uint32 SrcOffsetY = 0;
		uint32 DstWidth = 0;
		uint32 DstHeight = 0;
	};

	struct FHiZReduceConstants
	{
		uint32 SrcWidth = 0;
		uint32 SrcHeight = 0;
		uint32 DstWidth = 0;
		uint32 DstHeight = 0;
	};

	struct FHiZCullConstants
	{
		FMatrix ViewProjection = FMatrix::Identity;
		uint32 ViewWidth = 0;
		uint32 ViewHeight = 0;
		uint32 MipCount = 0;
		uint32 CommandCount = 0;
	};

	struct FViewResources
	{
		uint64 ViewKey = 0;
		ID3D11Texture2D* HiZTexture = nullptr;
		ID3D11ShaderResourceView* HiZFullSRV = nullptr;
		TArray<ID3D11ShaderResourceView*> HiZMipSRVs;
		TArray<ID3D11UnorderedAccessView*> HiZMipUAVs;
		uint32 Width = 0;
		uint32 Height = 0;
		uint32 MipCount = 0;
		FMatrix HistoryViewProjection = FMatrix::Identity;
		bool bHasValidHistory = false;
	};

	static void ReleaseCom(IUnknown*& Resource);
	static uint32 ComputeMipCount(uint32 Width, uint32 Height);

	bool CreateComputeShaders();
	bool EnsureSharedBuffers(size_t RequiredCommandCount);
	bool BuildCommandBuffers(const TArray<FMeshDrawCommand>& OpaqueCommands);
	void UpdateCullConstants(const FViewResources& View);
	void GenerateHiZ(FViewResources& View) const;
	void DispatchCull(const FViewResources& View) const;
	uint64 ComputeCommandSignature(const TArray<FMeshDrawCommand>& OpaqueCommands) const;

	void ReleaseViewResources(FViewResources& View);
	void ReleaseAllViewResources();
	FViewResources* FindViewResources(uint64 ViewKey);
	const FViewResources* FindViewResources(uint64 ViewKey) const;
	FViewResources* FindOrCreateViewResources(uint64 ViewKey, uint32 Width, uint32 Height);
	FViewResources* GetDebugViewResources();
	const FViewResources* GetDebugViewResources() const;
	bool EnsureDebugPreviewResources(uint32 Width, uint32 Height);

private:
	FRenderer* Renderer = nullptr;

	std::unordered_map<uint64, std::unique_ptr<FViewResources>> Views;
	uint64 LastTouchedViewKey = 0;

	ID3D11Buffer* CopyConstantBuffer = nullptr;
	ID3D11Buffer* ReduceConstantBuffer = nullptr;
	ID3D11Buffer* CullConstantBuffer = nullptr;
	ID3D11Buffer* CommandBuffer = nullptr;
	ID3D11ShaderResourceView* CommandBufferSRV = nullptr;
	ID3D11Buffer* IndirectArgsBuffer = nullptr;
	ID3D11Buffer* IndirectArgsTemplateBuffer = nullptr;
	ID3D11UnorderedAccessView* IndirectArgsUAV = nullptr;

	std::shared_ptr<FComputeShader> CopyDepthCS;
	std::shared_ptr<FComputeShader> ReduceCS;
	std::shared_ptr<FComputeShader> CullCS;

	TArray<FHiZCullCommandGPU> CPUCullCommands;
	TArray<D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS> CPUIndirectArgs;
	TArray<uint32> OpaqueCommandToIndirectSlot;

	uint32 MaxCommandCount = 0;
	uint32 CullCommandCount = 0;
	uint64 CachedCommandSignature = 0;
	bool bPreparedCulling = false;
	bool bCommandBuffersValid = false;

	ID3D11Texture2D* DebugPreviewTexture = nullptr;
	ID3D11ShaderResourceView* DebugPreviewSRV = nullptr;
	ID3D11Texture2D* DebugReadbackTexture = nullptr;
	uint32 DebugPreviewWidth = 0;
	uint32 DebugPreviewHeight = 0;
	uint32 DebugPreviewMipIndex = UINT32_MAX;
	uint64 DebugPreviewViewKey = 0;
	TArray<uint8> DebugPreviewPixels;
};
