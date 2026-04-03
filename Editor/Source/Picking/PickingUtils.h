#pragma once
#include "Math/Ray.h"
#include "Math/Vector.h"
#include "Math/Matrix.h"

namespace PickingUtils
{
	FVector TransformPointRowVector(const FVector& P, const FMatrix& M);
	FVector TransformVectorRowVector(const FVector& V, const FMatrix& M);

	bool RayIntersectsSphere(const FRay& Ray, const FVector& Center, float Radius, float& OutT);

	bool RayIntersectsAABB(
		const FRay& Ray, 
		const FVector& BoxMin, 
		const FVector& BoxMax, 
		float& OutTNear, 
		float& OutTFar);

	bool RayTriangleIntersect(
		const FRay& Ray, 
		const FVector& V0, 
		const FVector& V1, 
		const FVector& V2, 
		float& OutDistance);
}