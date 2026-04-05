#include "FpsOverlayWindow.h"

#include "EditorEngine.h"
#include "Core/Timer.h"
#include "Renderer/Renderer.h"
#include "Viewport/ViewportTypes.h"

#include "imgui.h"

#include <cstdio>

void FFpsOverlayWindow::Render(FEditorEngine* Engine, const FRect& AreaRect)
{
    if (!Engine)
    {
        return;
    }

    const FTimer& Timer = Engine->GetTimer();
    const float FPS = Timer.GetDisplayFPS();
    const float FrameTimeMs = Timer.GetFrameTimeMs();

    uint32 DrawCallCount = 0;
    FRenderFrameStats RenderStats = {};
    if (FRenderer* Renderer = Engine->GetRenderer())
    {
        RenderStats = Renderer->GetLastFrameStats();
        DrawCallCount = RenderStats.DrawCallCount;
    }

    const double LastPickTime = Engine->LastPickTime;
    const uint16 TotalPickCount = Engine->TotalPickCount;
    const double TotalPickTime = Engine->TotalPickTime;

    ImGuiViewport* MainVp = ImGui::GetMainViewport();
    const float PosX = MainVp->Pos.x + static_cast<float>(AreaRect.X) + 10.0f;
    const float PosY = MainVp->Pos.y + static_cast<float>(AreaRect.Y) + 10.0f;

    ImGui::SetNextWindowPos(ImVec2(PosX, PosY), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::SetNextWindowSize(ImVec2(0.0f, 0.0f), ImGuiCond_Always);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 4.0f));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.02f, 0.02f, 0.72f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.95f, 0.95f, 1.00f));

    ImGuiWindowFlags Flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_AlwaysAutoResize;

    const bool bOpen = ImGui::Begin("##FpsOverlay", nullptr, Flags);

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);

    if (!bOpen)
    {
        ImGui::End();
        return;
    }

    // FPS 색상: 60 이상 초록, 30~60 노랑, 30 미만 빨강
    ImVec4 FpsColor;
    if (FPS >= 60.0f)
        FpsColor = ImVec4(0.2f, 1.0f, 0.2f, 1.0f);
    else if (FPS >= 30.0f)
        FpsColor = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
    else
        FpsColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);

    ImGui::TextColored(FpsColor, "FPS: %.1f", FPS);
    ImGui::SameLine();
    ImGui::TextDisabled("(%.3f ms)", FrameTimeMs);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(
            "FPS / Frame Time\n"
            "엔진 타이머 기준 전체 프레임 시간입니다.\n"
            "렌더뿐 아니라 게임 로직, 에디터 틱 등 프레임 전체를 반영합니다.\n"
            "값은 직전 완료 프레임 기준입니다.\n\n"
            "색상 기준: 초록 >= 60fps | 노랑 30~59fps | 빨강 < 30fps");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    // 모든 수치 히스토리 업데이트
    History[GM_FPS][HistoryOffset]       = FPS;
    History[GM_FrameTime][HistoryOffset] = FrameTimeMs;
    History[GM_Build][HistoryOffset]     = static_cast<float>(RenderStats.BuildRenderFrameMs);
    History[GM_Exec][HistoryOffset]      = static_cast<float>(RenderStats.ExecuteCommandsMs);
    History[GM_Present][HistoryOffset]   = static_cast<float>(RenderStats.PresentMs);
    History[GM_GUI][HistoryOffset]       = static_cast<float>(RenderStats.GUIRenderMs);
    History[GM_Post][HistoryOffset]      = static_cast<float>(RenderStats.GUIPostPresentMs);
    History[GM_DrawCalls][HistoryOffset] = static_cast<float>(DrawCallCount);
    History[GM_MapCalls][HistoryOffset]  = static_cast<float>(RenderStats.BufferMapCount);
    History[GM_Picking][HistoryOffset]   = static_cast<float>(LastPickTime);
    HistoryOffset = (HistoryOffset + 1) % kHistorySize;

    // 그래프 메트릭 이름 목록
    static const char* MetricNames[GM_Count] = {
        "FPS", "Frame Time (ms)", "Build (ms)", "Exec (ms)", "Present (ms)",
        "GUI (ms)", "Post (ms)", "Draw Calls", "Map Calls", "Picking (ms)"
    };

    // 전환 버튼 + 그래프
    if (ImGui::ArrowButton("##prev_metric", ImGuiDir_Left))
        SelectedMetric = (SelectedMetric - 1 + GM_Count) % GM_Count;
    ImGui::SameLine();
    if (ImGui::ArrowButton("##next_metric", ImGuiDir_Right))
        SelectedMetric = (SelectedMetric + 1) % GM_Count;
    ImGui::SameLine();
    ImGui::TextDisabled("%s", MetricNames[SelectedMetric]);

    // 선택된 수치의 최대값 계산
    const float* CurHistory = History[SelectedMetric];
    float MaxVal = 0.0f;
    for (int i = 0; i < kHistorySize; ++i)
        if (CurHistory[i] > MaxVal) MaxVal = CurHistory[i];

    // 최소 스케일 보장
    if (SelectedMetric == GM_FPS)
    {
        if (MaxVal < 60.0f) MaxVal = 60.0f;
    }
    else
    {
        if (MaxVal < 1.0f) MaxVal = 1.0f;
    }

    char GraphLabel[32];
    if (SelectedMetric == GM_FPS || SelectedMetric == GM_DrawCalls || SelectedMetric == GM_MapCalls)
        snprintf(GraphLabel, sizeof(GraphLabel), "%.0f", CurHistory[(HistoryOffset - 1 + kHistorySize) % kHistorySize]);
    else
        snprintf(GraphLabel, sizeof(GraphLabel), "%.2f ms", CurHistory[(HistoryOffset - 1 + kHistorySize) % kHistorySize]);

    ImGui::PushStyleColor(ImGuiCol_PlotLines, FpsColor);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.05f, 0.05f, 0.05f, 0.6f));
    ImGui::PlotLines("##stat_graph", CurHistory, kHistorySize, HistoryOffset,
        GraphLabel, 0.0f, MaxVal * 1.2f, ImVec2(240.0f, 50.0f));
    ImGui::PopStyleColor(2);

    // 툴팁 헬퍼: 직전 아이템에 hover 시 설명 팝업 표시
    auto InfoTooltip = [](const char* Desc)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
            ImGui::TextUnformatted(Desc);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    };

    ImGui::Separator();

    // --- Draw Calls ---
    ImGui::Text("Draw Calls : %u", DrawCallCount);
    InfoTooltip(
        "Draw Calls\n"
        "엔진 렌더 경로에서 센 실제 Draw/DrawIndexed 호출 수입니다.\n"
        "ImGui 직접 드로우와 에디터 blit 드로우는 이 카운터에 포함되지 않습니다.");

    // --- Render ---
    ImGui::Text("Render     : Build %.3f | Exec %.3f | Present %.3f | GUI %.3f | Post %.3f ms",
        static_cast<float>(RenderStats.BuildRenderFrameMs),
        static_cast<float>(RenderStats.ExecuteCommandsMs),
        static_cast<float>(RenderStats.PresentMs),
        static_cast<float>(RenderStats.GUIRenderMs),
        static_cast<float>(RenderStats.GUIPostPresentMs));
    InfoTooltip(
        "Render 타이밍 (ms)\n"
        "- Build  : SceneProxy/RenderCommand -> 패스별 draw command 변환 CPU 시간\n"
        "- Exec   : ExecuteCommands() 전체 시간. Build를 포함한 상위 값이므로\n"
        "           Build + Upload + Outline 합계와 정확히 일치하지 않을 수 있습니다.\n"
        "- Present: 메인 스왑체인 Present() 시간.\n"
        "           이 값이 튀면 GPU 대기 또는 vsync 동기화 스톨을 의심하세요.\n"
        "- GUI    : ImGui 본체 렌더 시간\n"
        "- Post   : GUIPostPresent 시간. 에디터 보조 뷰포트/플랫폼 윈도우 present 포함");

    // --- Queues ---
    ImGui::Text("Queues     : Calls %u | MaxExec %.3f | Upload %.3f (%u) | Outline %.3f (%u)",
        RenderStats.ExecuteCommandsCalls,
        static_cast<float>(RenderStats.MaxExecuteCommandsMs),
        static_cast<float>(RenderStats.MeshUploadMs),
        RenderStats.MeshUploadCount,
        static_cast<float>(RenderStats.OutlineMs),
        RenderStats.OutlineItemCount);
    InfoTooltip(
        "Queues\n"
        "- Calls      : ExecuteCommands() 호출 횟수.\n"
        "               활성 뷰포트 수만큼 호출되고, Slate UI flush에서 1회 추가됩니다.\n"
        "- MaxExec    : 그 프레임에서 가장 느렸던 단일 ExecuteCommands() 1회 시간(ms).\n"
        "               특정 뷰포트만 느린지 진단할 때 유용합니다.\n"
        "- Upload (n) : dirty mesh를 GPU 버퍼로 올린 시간(ms) / 업로드한 mesh 개수\n"
        "- Outline (n): 아웃라인 패스 시간(ms) / 아웃라인 대상 개수");

    // --- Commands ---
    ImGui::Text("Commands   : In %u | O %u | A %u | ND %u | UI %u",
        RenderStats.SubmittedCommandCount,
        RenderStats.OpaqueCommandCount,
        RenderStats.AlphaCommandCount,
        RenderStats.NoDepthCommandCount,
        RenderStats.UICommandCount);
    InfoTooltip(
        "Commands\n"
        "- In : 렌더러에 제출된 원본 FRenderCommand 수.\n"
        "       scene proxy 1개가 여러 섹션 draw로 쪼개질 수 있어 draw call과 1:1이 아닙니다.\n"
        "- O  : Opaque 패스 draw command 수\n"
        "- A  : Alpha (투명) 패스 draw command 수\n"
        "- ND : NoDepth 패스 draw command 수\n"
        "- UI : UI 패스 draw command 수");

    // --- Driver Hint ---
    ImGui::Text("Driver Hint: Map %u | CreateBuf %u | CreateTex %u",
        RenderStats.BufferMapCount,
        RenderStats.BufferCreateCount,
        RenderStats.TextureCreateCount);
    InfoTooltip(
        "Driver Hint (D3D11 드라이버 호출 계측)\n"
        "- Map      : D3D11 Map 호출 수.\n"
        "             높으면 동적 버퍼/상수버퍼 갱신이 많다는 뜻입니다.\n"
        "- CreateBuf: 그 프레임에 새로 생성된 D3D11 버퍼 수. 평소 0에 가까워야 정상입니다.\n"
        "- CreateTex: 그 프레임에 새로 생성된 D3D11 텍스처 수. 평소 0에 가까워야 좋습니다.\n"
        "Upload / Map / CreateBuf / CreateTex 가 튄다면 리소스 갱신·재생성 패턴을 의심하세요.");

    // --- Binds ---
    ImGui::Text("Binds      : Mat %u | Mesh %u | Topo %u | Obj %u",
        RenderStats.MaterialBindCount,
        RenderStats.MeshBindCount,
        RenderStats.TopologyBindCount,
        RenderStats.ObjectBindCount);
    InfoTooltip(
        "State Binds (상태 변경 횟수)\n"
        "- Mat  : Material bind 횟수. 높으면 셰이더/리소스 전환 churn이 많다는 뜻입니다.\n"
        "- Mesh : Vertex/Index buffer bind 횟수\n"
        "- Topo : Primitive topology 변경 횟수\n"
        "- Obj  : Object constant/uniform bind 횟수. 거의 draw 단위로 움직입니다.\n"
        "Mat/Mesh/Topo가 draw 수 대비 높다면 상태 변경 churn이 심한 편입니다.");

    // --- Picking ---
    ImGui::Text("Picking    : %.3f ms | Count: %u | Total: %.3f ms",
        static_cast<float>(LastPickTime),
        static_cast<uint32>(TotalPickCount),
        static_cast<float>(TotalPickTime));
    InfoTooltip(
        "Picking\n"
        "에디터 오브젝트 선택(피킹) 처리 비용입니다.\n"
        "렌더 파이프라인과 별개로 동작하며, 값이 높으면 선택 처리 로직을 확인하세요.");

    ImGui::End();
}
