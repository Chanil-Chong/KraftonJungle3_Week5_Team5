#pragma once
#include "imgui.h"

struct FRect;
class FEditorEngine;

class FFpsOverlayWindow
{
public:
    void Render(FEditorEngine* Engine, const FRect& AreaRect);

private:
    static constexpr int kHistorySize = 128;

    enum EGraphMetric : int
    {
        GM_FPS = 0,
        GM_FrameTime,
        GM_Build,
        GM_Exec,
        GM_Present,
        GM_GUI,
        GM_Post,
        GM_DrawCalls,
        GM_MapCalls,
        GM_Picking,
        GM_Count
    };

    float History[GM_Count][kHistorySize] = {};
    int   HistoryOffset  = 0;
    int   SelectedMetric = GM_FPS;
};
