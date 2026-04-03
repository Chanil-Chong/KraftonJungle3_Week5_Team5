#pragma once

#include "Viewport/ViewportTypes.h"

class AActor;
class UScene;
struct FRay;

class FPicker
{
public:
	FRay ScreenToRay(const FViewportEntry& Entry, int32 ScreenX, int32 ScreenY) const;

    // 씬의 모든 Actor를 대상으로 피킹 (가장 가까운 Actor 반환)
    AActor* PickActor(UScene* Scene, const FViewportEntry* Entry, int32 ScreenX, int32 ScreenY) const;
};
