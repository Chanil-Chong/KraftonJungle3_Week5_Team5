#include "Picker.h"

#include "Scene/Scene.h"
#include "Actor/Actor.h"
#include "Camera/Camera.h"
#include "Component/PrimitiveComponent.h"
#include "Component/SubUVComponent.h"
#include "Component/TextComponent.h"
#include "Component/UUIDBillboardComponent.h"
#include "Component/StaticMeshComponent.h"
#include "Renderer/MeshData.h"
#include "Component/SkyComponent.h"
#include "Viewport/Viewport.h"
#include "PickingUtils.h"


FRay FPicker::ScreenToRay(const FViewportEntry& Entry, int32 ScreenX, int32 ScreenY) const
{
	if (!Entry.Viewport)
	{
		return { FVector::ZeroVector, FVector::ForwardVector };
	}

	const auto& Rect = Entry.Viewport->GetRect();
	if (Rect.Width <= 0 || Rect.Height <= 0)
	{
		return { FVector::ZeroVector, FVector::ForwardVector };
	}

	const float AspectRatio = static_cast<float>(Rect.Width) / static_cast<float>(Rect.Height);

	const FMatrix ViewMatrix = Entry.LocalState.BuildViewMatrix();
	const FMatrix ProjMatrix = Entry.LocalState.BuildProjMatrix(AspectRatio);
	const FMatrix ViewInverse = ViewMatrix.GetInverse();

	// 픽셀 중심 기준 half-pixel offset
	const float NdcX = (2.0f * (ScreenX + 0.5f) / Rect.Width) - 1.0f;
	const float NdcY = 1.0f - (2.0f * (ScreenY + 0.5f) / Rect.Height);

	if (Entry.LocalState.ProjectionType != EViewportType::Perspective)
	{
		const float ViewHeight = Entry.LocalState.OrthoZoom * 2.0f;
		const float ViewWidth = ViewHeight * AspectRatio;

		const float ViewRight = NdcX * (ViewWidth * 0.5f);
		const float ViewUp = NdcY * (ViewHeight * 0.5f);

		FVector RayOrigin;
		RayOrigin.X = ViewRight * ViewInverse.M[1][0] + ViewUp * ViewInverse.M[2][0] + ViewInverse.M[3][0];
		RayOrigin.Y = ViewRight * ViewInverse.M[1][1] + ViewUp * ViewInverse.M[2][1] + ViewInverse.M[3][1];
		RayOrigin.Z = ViewRight * ViewInverse.M[1][2] + ViewUp * ViewInverse.M[2][2] + ViewInverse.M[3][2];

		FVector Forward = FVector::ForwardVector;

		switch (Entry.LocalState.ProjectionType)
		{
		case EViewportType::OrthoTop:
			Forward = FVector::DownVector;
			break;

		case EViewportType::OrthoBottom:
			Forward = FVector::UpVector;
			break;

		case EViewportType::OrthoLeft:
			Forward = FVector::RightVector;
			break;

		case EViewportType::OrthoRight:
			Forward = FVector::LeftVector;
			break;

		case EViewportType::OrthoFront:
			Forward = FVector::BackwardVector;
			break;

		case EViewportType::OrthoBack:
			Forward = FVector::ForwardVector;
			break;

		default:
			break;
		}

		return { RayOrigin, Forward };
	}

	const float ViewForward = 1.0f;
	const float ViewRight = NdcX / ProjMatrix.M[1][0];
	const float ViewUp = NdcY / ProjMatrix.M[2][1];

	FVector RayDirectionWorld;
	RayDirectionWorld.X = ViewForward * ViewInverse.M[0][0] + ViewRight * ViewInverse.M[1][0] + ViewUp * ViewInverse.M[2][0];
	RayDirectionWorld.Y = ViewForward * ViewInverse.M[0][1] + ViewRight * ViewInverse.M[1][1] + ViewUp * ViewInverse.M[2][1];
	RayDirectionWorld.Z = ViewForward * ViewInverse.M[0][2] + ViewRight * ViewInverse.M[1][2] + ViewUp * ViewInverse.M[2][2];
	RayDirectionWorld = RayDirectionWorld.GetSafeNormal();

	FVector RayOrigin;
	RayOrigin.X = ViewInverse.M[3][0];
	RayOrigin.Y = ViewInverse.M[3][1];
	RayOrigin.Z = ViewInverse.M[3][2];

	return { RayOrigin, RayDirectionWorld };
}


AActor* FPicker::PickActor(UScene* Scene, const FViewportEntry* Entry, int32 ScreenX, int32 ScreenY, FEditorEngine* Engine) const
{
	if (!Scene || !Entry)
	{
		return nullptr;
	}

	const FRay WorldRay = ScreenToRay(*Entry, ScreenX, ScreenY);

	TStatId MyStatId;

	FScopeCycleCounter pickCounter(MyStatId);
	++Engine->TotalPickCount;

	AActor* ClosestActor = nullptr;
	float ClosestDistance = (std::numeric_limits<float>::max)();

	for (AActor* Actor : Scene->GetActors())
	{
		if (!Actor || Actor->IsPendingDestroy() || !Actor->IsVisible())
		{
			continue;
		}

		for (UActorComponent* Component : Actor->GetComponents())
		{
			if (!Component || !Component->IsA(UPrimitiveComponent::StaticClass())) continue;

			// 피킹 제외 대상
			if (Component->IsA(UUUIDBillboardComponent::StaticClass())) continue;
			if (Component->IsA(USkyComponent::StaticClass())) continue;

			UPrimitiveComponent* PrimComp = static_cast<UPrimitiveComponent*>(Component);
			const FBoxSphereBounds Bounds = PrimComp->GetWorldBounds();

			// 1) world sphere broad phase
			float SphereT = 0.0f;
			if (!PickingUtils::RayIntersectsSphere(WorldRay, Bounds.Center, Bounds.Radius, SphereT))
			{
				continue;
			}

			if (SphereT > ClosestDistance)
			{
				continue;
			}

			// Text / SubUV는 sphere만으로 처리
			if (PrimComp->IsA(USubUVComponent::StaticClass()) || PrimComp->IsA(UTextComponent::StaticClass()))
			{
				if (SphereT < ClosestDistance)
				{
					ClosestDistance = SphereT;
					ClosestActor = Actor;
				}
				continue;
			}

			// 2) world AABB broad phase
			const FVector BoxMin = Bounds.Center - Bounds.BoxExtent;
			const FVector BoxMax = Bounds.Center + Bounds.BoxExtent;

			float BoxNear = 0.0f;
			float BoxFar = 0.0f;
			if (!PickingUtils::RayIntersectsAABB(WorldRay, BoxMin, BoxMax, BoxNear, BoxFar))
			{
				continue;
			}

			const float BoundsHitT = (BoxNear >= 0.0f) ? BoxNear : BoxFar;
			if (BoundsHitT > ClosestDistance)
			{
				continue;
			}

			// 3) StaticMesh는 local ray로 정밀 검사
			if (PrimComp->IsA(UStaticMeshComponent::StaticClass()))
			{
				UStaticMeshComponent* SMC = static_cast<UStaticMeshComponent*>(PrimComp);
				FRenderMesh* Mesh = SMC->GetRenderMesh();

				// 메쉬가 없거나 정점이 비어있으면 패스
				if (!Mesh || Mesh->Vertices.empty() || Mesh->Indices.empty()) continue;

				const FMatrix World = SMC->GetWorldTransform();
				const FMatrix InvWorld = World.GetInverse();

				FRay LocalRay;
				LocalRay.Origin = PickingUtils::TransformPointRowVector(WorldRay.Origin, InvWorld);
				LocalRay.Direction = PickingUtils::TransformVectorRowVector(WorldRay.Direction, InvWorld).GetSafeNormal();

				if (LocalRay.Direction.IsZero())
				{
					continue;
				}

				for (uint32 Index = 0; Index + 2 < Mesh->Indices.size(); Index += 3)
				{
					// ⭐ 이제 무조건 신형 FVertex 구조체를 쓰므로 코드가 하나로 통합됩니다.
					const FVector& P0 = Mesh->Vertices[Mesh->Indices[Index]].Position;
					const FVector& P1 = Mesh->Vertices[Mesh->Indices[Index + 1]].Position;
					const FVector& P2 = Mesh->Vertices[Mesh->Indices[Index + 2]].Position;

					float LocalDistance = 0.0f;
					if (!PickingUtils::RayTriangleIntersect(LocalRay, P0, P1, P2, LocalDistance))
					{
						continue;
					}

					// local hit point -> world hit point
					const FVector LocalHitPoint = LocalRay.Origin + LocalRay.Direction * LocalDistance;
					const FVector WorldHitPoint = PickingUtils::TransformPointRowVector(LocalHitPoint, World);

					// 비교는 반드시 월드 거리로
					const float WorldDistance = (WorldHitPoint - WorldRay.Origin).Size();

					if (WorldDistance < ClosestDistance)
					{
						ClosestDistance = WorldDistance;
						ClosestActor = Actor;
					}
				}

				continue;
			}

			// StaticMesh가 아닌 일반 Primitive는 bounds hit만으로 선택
			if (BoundsHitT < ClosestDistance)
			{
				ClosestDistance = BoundsHitT;
				ClosestActor = Actor;
			}
		}
	}

	Engine->LastPickTime = pickCounter.FinishMilliseconds();
	Engine->TotalPickTime += Engine->LastPickTime;
	return ClosestActor;
}