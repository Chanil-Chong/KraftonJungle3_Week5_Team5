#pragma once
#include "CoreMinimal.h"
#include <DirectXMath.h>

struct FBoxSphereBounds;

struct FPlane4
{
	float A, B, C, D;

	void Normalize()
	{
		// XMPlaneNormalize: (A,B,C,D) / length(A,B,C) — replaces scalar sqrt
		DirectX::XMVECTOR V = DirectX::XMLoadFloat4(reinterpret_cast<const DirectX::XMFLOAT4*>(this));
		V = DirectX::XMPlaneNormalize(V);
		DirectX::XMStoreFloat4(reinterpret_cast<DirectX::XMFLOAT4*>(this), V);
	}

	float DistanceTo(const FVector& Point) const
	{
		return A * Point.X + B * Point.Y + C * Point.Z + D;
	}
};

struct FBoundingSphere
{
	FVector Center;
	float Radius;
};

class ENGINE_API FFrustum
{
public:
	enum { Left = 0, Right, Bottom, Top, Near, Far, PlaneCount };

	void ExtractFromVP(const FMatrix& VP);
	bool IsVisible(const FBoxSphereBounds& Sphere) const;

private:
	FPlane4 Planes[PlaneCount];
};
