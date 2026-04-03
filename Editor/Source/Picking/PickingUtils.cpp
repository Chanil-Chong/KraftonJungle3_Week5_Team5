#include "PickingUtils.h"

#include <limits>
#include <algorithm>

namespace PickingUtils
{
	FVector TransformPointRowVector(const FVector& P, const FMatrix& M)
	{
		// row-vector 규약:
		// [x y z 1] * M
		return {
			P.X * M.M[0][0] + P.Y * M.M[1][0] + P.Z * M.M[2][0] + M.M[3][0],
			P.X * M.M[0][1] + P.Y * M.M[1][1] + P.Z * M.M[2][1] + M.M[3][1],
			P.X * M.M[0][2] + P.Y * M.M[1][2] + P.Z * M.M[2][2] + M.M[3][2]
		};
	}

	FVector TransformVectorRowVector(const FVector& V, const FMatrix& M)
	{
		// row-vector 규약:
		// [x y z 0] * M
		return {
			V.X * M.M[0][0] + V.Y * M.M[1][0] + V.Z * M.M[2][0],
			V.X * M.M[0][1] + V.Y * M.M[1][1] + V.Z * M.M[2][1],
			V.X * M.M[0][2] + V.Y * M.M[1][2] + V.Z * M.M[2][2]
		};
	}

	bool RayIntersectsSphere(const FRay& Ray, const FVector& Center, float Radius, float& OutT)
	{
		// Ray.Direction은 normalized 상태라고 가정
		const FVector M = Ray.Origin - Center;
		const float B = FVector::DotProduct(M, Ray.Direction);
		const float C = FVector::DotProduct(M, M) - Radius * Radius;

		// 시작점이 sphere 바깥이고, 반대 방향이면 miss
		if (C > 0.0f && B > 0.0f)
		{
			return false;
		}

		const float Discriminant = B * B - C;
		if (Discriminant < 0.0f)
		{
			return false;
		}

		float T = -B - sqrt(Discriminant);

		// origin이 sphere 내부면 0으로 처리
		if (T < 0.0f)
		{
			T = 0.0f;
		}

		OutT = T;
		return true;
	}

	bool RayIntersectsAABB(const FRay& Ray, const FVector& BoxMin, const FVector& BoxMax, float& OutTNear, float& OutTFar)
	{
		constexpr float Epsilon = 1.0e-8f;

		float TNear = 0.0f;
		float TFar = (std::numeric_limits<float>::max)();

		auto TestAxis = [&](float Origin, float Dir, float MinV, float MaxV) -> bool
			{
				// 평행이면 origin이 slab 안에 있어야 함
				if (abs(Dir) < Epsilon)
				{
					return (Origin >= MinV && Origin <= MaxV);
				}

				const float InvDir = 1.0f / Dir;
				float T1 = (MinV - Origin) * InvDir;
				float T2 = (MaxV - Origin) * InvDir;

				if (T1 > T2)
				{
					std::swap(T1, T2);
				}

				TNear = (std::max)(TNear, T1);
				TFar = (std::min)(TFar, T2);

				return TNear <= TFar;
			};

		if (!TestAxis(Ray.Origin.X, Ray.Direction.X, BoxMin.X, BoxMax.X)) return false;
		if (!TestAxis(Ray.Origin.Y, Ray.Direction.Y, BoxMin.Y, BoxMax.Y)) return false;
		if (!TestAxis(Ray.Origin.Z, Ray.Direction.Z, BoxMin.Z, BoxMax.Z)) return false;

		if (TFar < 0.0f)
		{
			return false;
		}

		OutTNear = TNear;
		OutTFar = TFar;
		return true;
	}

	bool RayTriangleIntersect(const FRay& Ray,
		const FVector& V0, const FVector& V1, const FVector& V2,
		float& OutDistance)
	{
		constexpr float Epsilon = 1.e-6f;

		const FVector Edge1 = V1 - V0;
		const FVector Edge2 = V2 - V0;

		const FVector H = FVector::CrossProduct(Ray.Direction, Edge2);
		const float A = FVector::DotProduct(Edge1, H);

		// Render path와 동일하게 back-face는 picking 대상에서 제외한다.
		if (A <= Epsilon)
		{
			return false;
		}

		const float F = 1.0f / A;
		const FVector S = Ray.Origin - V0;
		const float U = F * FVector::DotProduct(S, H);
		if (U < 0.0f || U > 1.0f)
		{
			return false;
		}

		const FVector Q = FVector::CrossProduct(S, Edge1);
		const float V = F * FVector::DotProduct(Ray.Direction, Q);
		if (V < 0.0f || U + V > 1.0f)
		{
			return false;
		}

		const float T = F * FVector::DotProduct(Edge2, Q);
		if (T > Epsilon)
		{
			OutDistance = T;
			return true;
		}

		return false;
	}

}