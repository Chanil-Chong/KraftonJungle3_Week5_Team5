#include "Frustum.h"
#include "Component/PrimitiveComponent.h"

void FFrustum::ExtractFromVP(const FMatrix& VP)
{
	// Row-major VP: column extraction via M[row][col]
	// Left:   col3 + col0
	Planes[Left]   = { VP[0][3] + VP[0][0], VP[1][3] + VP[1][0], VP[2][3] + VP[2][0], VP[3][3] + VP[3][0] };
	// Right:  col3 - col0
	Planes[Right]  = { VP[0][3] - VP[0][0], VP[1][3] - VP[1][0], VP[2][3] - VP[2][0], VP[3][3] - VP[3][0] };		
	// Bottom: col3 + col1
	Planes[Bottom] = { VP[0][3] + VP[0][1], VP[1][3] + VP[1][1], VP[2][3] + VP[2][1], VP[3][3] + VP[3][1] };
	// Top:    col3 - col1
	Planes[Top]    = { VP[0][3] - VP[0][1], VP[1][3] - VP[1][1], VP[2][3] - VP[2][1], VP[3][3] - VP[3][1] };
	// Near:   col3 + col2
	Planes[Near]   = { VP[0][3] + VP[0][2], VP[1][3] + VP[1][2], VP[2][3] + VP[2][2], VP[3][3] + VP[3][2] };
	// Far:    col3 - col2
	Planes[Far]    = { VP[0][3] - VP[0][2], VP[1][3] - VP[1][2], VP[2][3] - VP[2][2], VP[3][3] - VP[3][2] };

	for (int32 i = 0; i < PlaneCount; ++i)
	{
		Planes[i].Normalize();
	}
}

bool FFrustum::IsVisible(const FBoxSphereBounds& Sphere) const
{
	// XMPlaneDotCoord: dot(plane.xyz, point) + plane.w (= signed distance)
	// Sphere is outside if distance < -Radius on any plane
	const DirectX::XMVECTOR Center = DirectX::XMVectorSet(Sphere.Center.X, Sphere.Center.Y, Sphere.Center.Z, 1.0f);
	const float NegRadius = -Sphere.Radius;

	for (int32 i = 0; i < PlaneCount; ++i)
	{
		const DirectX::XMVECTOR Plane = DirectX::XMLoadFloat4(reinterpret_cast<const DirectX::XMFLOAT4*>(&Planes[i]));
		if (DirectX::XMVectorGetX(DirectX::XMPlaneDotCoord(Plane, Center)) < NegRadius)
		{
			return false;
		}
	}
	return true;
}
