#pragma once
#include "Vector.h"

struct FRay
{
	FVector Origin;
	FVector Direction;

	FRay() = default;
	FRay(const FVector& InOrigin, const FVector& InDirection)
		: Origin(InOrigin), Direction(InDirection) { }
};
