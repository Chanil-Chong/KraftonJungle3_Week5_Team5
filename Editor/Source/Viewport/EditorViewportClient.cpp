#include "EditorViewportClient.h"

#include "EditorEngine.h"
#include "EditorViewportRegistry.h"
#include "UI/EditorUI.h"
#include "Core/Paths.h"
#include "Renderer/Material.h"
#include "Renderer/MaterialManager.h"
#include "Renderer/Renderer.h"
#include "Renderer/RenderStateManager.h"
#include "Renderer/ShaderMap.h"
#include "Math/MathUtility.h"
#include "imgui.h"
#include "Viewport.h"

#include <cmath>

FEditorViewportClient::FEditorViewportClient(
	FEditorEngine& InEditorEngine,
	FEditorUI& InEditorUI,
	FEditorViewportRegistry& InViewportRegistry,
	FWindowsWindow* InMainWindow)
	: EditorUI(InEditorUI)
	, MainWindow(InMainWindow)
	, EditorEngine(InEditorEngine)
	, ViewportRegistry(InViewportRegistry)
{
}

void FEditorViewportClient::Attach(FEngine* Engine, FRenderer* Renderer)
{
	FEditorEngine* EditorEngine = static_cast<FEditorEngine*>(Engine);
	if (!EditorEngine || !Renderer)
	{
		return;
	}

	EditorUI.Initialize(EditorEngine);
	EditorUI.AttachToRenderer(Renderer);

	BlitRenderer.Initialize(Renderer->GetDevice());

	// Cache wireframe material for wireframe view mode.
	WireFrameMaterial = FMaterialManager::Get().FindByName(WireframeMaterialName);
	CreateGridResource(Renderer);
}

void FEditorViewportClient::CreateGridResource(FRenderer* Renderer)
{
	ID3D11Device* Device = Renderer->GetDevice();
	if (Device)
	{
		constexpr int32 GridVertexCount = 42;

		GridMesh = std::make_unique<FDynamicMesh>();
		GridMesh->Topology = EMeshTopology::EMT_TriangleList;
		for (int32 i = 0; i < GridVertexCount; ++i)
		{
			FVertex Vertex;
			GridMesh->Vertices.push_back(Vertex);
			GridMesh->Indices.push_back(i);
		}
		GridMesh->CreateVertexAndIndexBuffer(Device);

		std::wstring ShaderDirW = FPaths::ShaderDir();
		std::wstring VSPath = ShaderDirW + L"AxisVertexShader.hlsl";
		std::wstring PSPath = ShaderDirW + L"AxisPixelShader.hlsl";
		auto VS = FShaderMap::Get().GetOrCreateVertexShader(Device, VSPath.c_str());
		auto PS = FShaderMap::Get().GetOrCreatePixelShader(Device, PSPath.c_str());

		GridMaterial = std::make_shared<FMaterial>();
		GridMaterial->SetOriginName("M_EditorGrid");
		GridMaterial->SetVertexShader(VS);
		GridMaterial->SetPixelShader(PS);

		int32 SlotIndex = GridMaterial->CreateConstantBuffer(Device, 64);
		if (SlotIndex >= 0)
		{
			GridMaterial->RegisterParameter("GridSize", SlotIndex, 0, 4);
			GridMaterial->RegisterParameter("LineThickness", SlotIndex, 4, 4);
			GridMaterial->RegisterParameter("GridAxisU", SlotIndex, 16, 12);
			GridMaterial->RegisterParameter("GridAxisV", SlotIndex, 32, 12);
			GridMaterial->RegisterParameter("ViewForward", SlotIndex, 48, 12);

			float DefaultGridSize = 10.0f;
			float DefaultLineThickness = 1.0f;
			const FVector DefaultGridAxisU = FVector::ForwardVector;
			const FVector DefaultGridAxisV = FVector::RightVector;
			const FVector DefaultViewForward = FVector::ForwardVector;
			GridMaterial->SetParameterData("GridSize", &DefaultGridSize, 4);
			GridMaterial->SetParameterData("LineThickness", &DefaultLineThickness, 4);
			GridMaterial->SetParameterData("GridAxisU", &DefaultGridAxisU, sizeof(FVector));
			GridMaterial->SetParameterData("GridAxisV", &DefaultGridAxisV, sizeof(FVector));
			GridMaterial->SetParameterData("ViewForward", &DefaultViewForward, sizeof(FVector));
		}
	}
}

void FEditorViewportClient::UpdateGridMesh(const FVector& CameraPos)
{
	const float Range = 1000.0f;
	float GridSize = 10.0f;

	FVector AxisU = FVector::ForwardVector;
	FVector AxisV = FVector::RightVector;

	GridMaterial->GetParameterData("GridAxisU", &AxisU, sizeof(FVector));
	GridMaterial->GetParameterData("GridAxisV", &AxisV, sizeof(FVector));
	GridMaterial->GetParameterData("GridSize", &GridSize, sizeof(float));

	const float CamU = FVector::DotProduct(CameraPos, AxisU);
	const float CamV = FVector::DotProduct(CameraPos, AxisV);
	const float snapU = std::floor(CamU / GridSize) * GridSize;
	const float snapV = std::floor(CamV / GridSize) * GridSize;

	const int32 MaxLineCount = 100;
	const int32 LineCount = FMath::Min((int32)(Range / GridSize), MaxLineCount);

	GridMesh->Vertices.clear();
	GridMesh->Indices.clear();

	for (int32 i = -LineCount; i <= LineCount; ++i)
	{
		const float v = snapV + i * GridSize;
		const FVector lineOrigin = AxisV * v;
		const FVector lineDir = AxisU;
		const int32 axisNo = (std::abs(v) < GridSize * 0.01f) ? 0 : -1;
		AppendLineQuad(lineOrigin, lineDir, Range, axisNo, CameraPos);
	}

	for (int32 i = -LineCount; i <= LineCount; ++i)
	{
		const float u = snapU + i * GridSize;
		const FVector lineOrigin = AxisU * u;
		const FVector lineDir = AxisV;
		const int32 axisNo = (std::abs(u) < GridSize * 0.01f) ? 1 : -1;
		AppendLineQuad(lineOrigin, lineDir, Range, axisNo, CameraPos);
	}

	GridMesh->bIsDirty = true;
}

void FEditorViewportClient::AppendLineQuad(const FVector& Origin, const FVector& Dir, float HalfLength, int32 AxisNo, FVector CamPos)
{
	const FVector ToCamera = (CamPos - Origin).GetSafeNormal();
	const FVector SideDir = FVector::CrossProduct(Dir, ToCamera).GetSafeNormal();

	const FVector Diff = CamPos - Origin;
	const float Dist = std::sqrt(Diff.X * Diff.X + Diff.Y * Diff.Y + Diff.Z * Diff.Z);
	const float Thickness = FMath::Max(0.05f, Dist * 0.002f);

	const FVector2 corners[6] =
	{
		{-1, -1}, { 1, -1}, {-1,  1},
		{ 1, -1}, {-1,  1}, { 1,  1}
	};

	const uint32 baseIndex = (uint32)GridMesh->Vertices.size();

	for (int32 i = 0; i < 6; ++i)
	{
		FVertex v;
		v.Position = Origin
			+ Dir * (HalfLength * corners[i].X)
			+ SideDir * (Thickness * corners[i].Y);
		v.Normal = Dir;
		v.UV = corners[i];
		v.Color = FVector4((float)AxisNo, corners[i].Y, 1.0f, 0.0f);
		GridMesh->Vertices.push_back(v);
		GridMesh->Indices.push_back(baseIndex + i);
	}
}

void FEditorViewportClient::Detach(FEngine* Engine, FRenderer* Renderer)
{
	Gizmo.EndDrag();
	EditorUI.DetachFromRenderer(Renderer);

	BlitRenderer.Release();

	GridMesh.reset();
	GridMaterial.reset();
}

void FEditorViewportClient::Tick(FEngine* Engine, float DeltaTime)
{
	IViewportClient::Tick(Engine, DeltaTime);
	FEditorEngine* EditorEngine = static_cast<FEditorEngine*>(Engine);
	InputService.TickCameraNavigation(Engine, EditorEngine, ViewportRegistry, Gizmo);
}

void FEditorViewportClient::HandleMessage(FEngine* Engine, HWND Hwnd, UINT Msg, WPARAM WParam, LPARAM LParam)
{
	FEditorEngine* EditorEngine = static_cast<FEditorEngine*>(Engine);
	InputService.HandleMessage(
		Engine,
		EditorEngine,
		Hwnd,
		Msg,
		WParam,
		LParam,
		ViewportRegistry,
		Picker,
		Gizmo,
		[this]()
		{
			EditorUI.SyncSelectedActorProperty();
		});
}

void FEditorViewportClient::HandleFileDoubleClick(const FString& FilePath)
{
	AssetInteractionService.HandleFileDoubleClick(EditorUI, ViewportRegistry, FilePath);
}

void FEditorViewportClient::HandleFileDropOnViewport(const FString& FilePath)
{
	AssetInteractionService.HandleFileDropOnViewport(
		EditorUI,
		Picker,
		ViewportRegistry,
		InputService.GetScreenMouseX(),
		InputService.GetScreenMouseY(),
		FilePath);
}

void FEditorViewportClient::BuildRenderCommands(FEngine* Engine, UScene* Scene, const FFrustum& Frustum, const FShowFlags& Flags, const FVector& CameraPosition, FRenderCommandQueue& OutQueue)
{
	if (!Engine)
	{
		return;
	}
	IViewportClient::BuildRenderCommands(Engine, Scene, Frustum, Flags, CameraPosition, OutQueue);
}

void FEditorViewportClient::Render(FEngine* Engine, FRenderer* Renderer)
{
	if (!Renderer)
	{
		return;
	}

	SyncViewportRectsFromDock();
	FEditorEngine* EditorEngine = static_cast<FEditorEngine*>(Engine);
	RenderService.RenderAll(
		Engine,
		Renderer,
		EditorEngine,
		ViewportRegistry,
		EditorUI,
		Gizmo,
		BlitRenderer,
		WireFrameMaterial,
		GridMesh.get(),
		GridMaterial.get(),
		[this](const FVector& CameraPos)
		{
			UpdateGridMesh(CameraPos);
		},
		[this](FEngine* InEngine, UScene* Scene, const FFrustum& Frustum, const FShowFlags& Flags, const FVector& CameraPosition, FRenderCommandQueue& OutQueue)
		{
			BuildRenderCommands(InEngine, Scene, Frustum, Flags, CameraPosition, OutQueue);
		});
}

void FEditorViewportClient::SyncViewportRectsFromDock()
{
	FRect Central;
	if (!EditorUI.GetCentralDockRect(Central) || !Central.IsValid())
	{
		// First-frame fallback when dock rect is not ready.
		if (!ImGui::GetCurrentContext())
		{
			return;
		}
		ImGuiViewport* VP = ImGui::GetMainViewport();
		if (!VP || VP->WorkSize.x <= 0 || VP->WorkSize.y <= 0)
		{
			return;
		}
		// Convert viewport absolute coordinates to client coordinates.
		Central.X      = static_cast<int32>(VP->WorkPos.x - VP->Pos.x);
		Central.Y      = static_cast<int32>(VP->WorkPos.y - VP->Pos.y);
		Central.Width  = static_cast<int32>(VP->WorkSize.x);
		Central.Height = static_cast<int32>(VP->WorkSize.y);
	}
	
	FSlateApplication* Slate = EditorEngine.GetSlateApplication();
	if (Slate)
	{
		constexpr int32 HeaderHeight = 34;
		FRect ViewportArea = Central;
		if (ViewportArea.Height > HeaderHeight)
		{
			ViewportArea.Y += HeaderHeight;
			ViewportArea.Height -= HeaderHeight;
		}
		Slate->SetViewportAreaRect(ViewportArea);

		for (FViewportEntry& Entry : ViewportRegistry.GetEntries())
		{
			Entry.bActive = Slate->IsViewportActive(Entry.Id);
		}
	}
}

