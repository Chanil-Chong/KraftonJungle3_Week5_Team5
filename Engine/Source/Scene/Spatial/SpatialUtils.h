#pragma once
#include "CoreMinimal.h"

/*
* 물체의 Bounding Box 연산에 필요한 AABB
*/
struct FAABB
{
	FVector Min;
	FVector Max;

	FVector GetCenter() const
	{
		return (Min + Max) * 0.5f;
	}

	FVector GetExtent() const
	{
		return (Max - Min) * 0.5f;
	}

	/* 이 AABB 안에 Other AABB가 포함되는지 검사하는 함수 */
	bool Contains(const FAABB& Other) const
	{
		return
			Min.X <= Other.Min.X && Max.X >= Other.Max.X &&
			Min.Y <= Other.Min.Y && Max.Y >= Other.Max.Y &&
			Min.Z <= Other.Min.Z && Max.Z >= Other.Max.Z;
	}
	/* 앞의 AABB와 Other AABB가 겹치는지 검사하는 함수 */
	bool Intersects(const FAABB& Other) const
	{
		return !(
			Max.X < Other.Min.X || Min.X > Other.Max.X ||
			Max.Y < Other.Min.Y || Min.Y > Other.Max.Y ||
			Max.Z < Other.Min.Z || Min.Z > Other.Max.Z
			);
	}
};