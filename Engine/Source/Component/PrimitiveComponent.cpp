#include "PrimitiveComponent.h"
#include "Object/Class.h"
#include "Renderer/SceneProxy.h"
#include "Serializer/Archive.h"
#include "Debug/EngineLog.h"
#include "Actor/Actor.h"
#include "Scene/Scene.h"
IMPLEMENT_RTTI(UPrimitiveComponent, USceneComponent)

TArray<UPrimitiveComponent*> UPrimitiveComponent::PendingRenderStateUpdates;

FBoxSphereBounds UPrimitiveComponent::GetLocalBounds() const
{
	return { FVector(0, 0, 0), 0.f, FVector(0, 0, 0) };
}

void UPrimitiveComponent::OnRegister()
{
	USceneComponent::OnRegister();
	MarkRenderStateDirty();
}

void UPrimitiveComponent::OnUnregister()
{
	auto PendingIt = std::remove(PendingRenderStateUpdates.begin(), PendingRenderStateUpdates.end(), this);
	PendingRenderStateUpdates.erase(PendingIt, PendingRenderStateUpdates.end());
	SceneProxy.reset();
	bRenderStateDirty = true;
	bRenderStateUpdateQueued = false;
	USceneComponent::OnUnregister();
}

void UPrimitiveComponent::MarkRenderStateDirty()
{
	bRenderStateDirty = true;
	EnqueueRenderStateUpdate(this);
}

void UPrimitiveComponent::FlushPendingRenderStateUpdates()
{
	if (PendingRenderStateUpdates.empty())
	{
		return;
	}

	TArray<UPrimitiveComponent*> PendingUpdates = std::move(PendingRenderStateUpdates);
	PendingRenderStateUpdates.clear();

	for (UPrimitiveComponent* PrimitiveComponent : PendingUpdates)
	{
		if (!PrimitiveComponent || !PrimitiveComponent->bRenderStateUpdateQueued)
		{
			continue;
		}

		PrimitiveComponent->bRenderStateUpdateQueued = false;
		if (PrimitiveComponent->IsPendingKill() || !PrimitiveComponent->IsRegistered())
		{
			continue;
		}

		if (PrimitiveComponent->bRenderStateDirty)
		{
			PrimitiveComponent->RecreateSceneProxy();
		}
	}
}

std::shared_ptr<FPrimitiveSceneProxy> UPrimitiveComponent::CreateSceneProxy() const
{
	return nullptr;
}

FPrimitiveSceneProxy* UPrimitiveComponent::GetSceneProxy() const
{
	// Render state updates are expected to be processed in FlushPendingRenderStateUpdates
	// on the render command collection boundary.

	return SceneProxy.get();
}

void UPrimitiveComponent::EnqueueRenderStateUpdate(UPrimitiveComponent* InPrimitiveComponent)
{
	if (!InPrimitiveComponent || InPrimitiveComponent->bRenderStateUpdateQueued)
	{
		return;
	}

	InPrimitiveComponent->bRenderStateUpdateQueued = true;
	PendingRenderStateUpdates.push_back(InPrimitiveComponent);
}

void UPrimitiveComponent::RecreateSceneProxy()
{
	SceneProxy = CreateSceneProxy();
	bRenderStateDirty = false;
	bRenderStateUpdateQueued = false;
}

void UPrimitiveComponent::UpdateBounds()
{
	bBoundsDirty = true;
	MarkRenderStateDirty();

	AActor* OwnerActor = GetOwner();
	if (OwnerActor)
	{
		if (UScene* Scene = OwnerActor->GetScene())
		{
			Scene->MarkSpatialDirty();
		}
	}
}

FBoxSphereBounds UPrimitiveComponent::GetWorldBounds() const
{
	if (bBoundsDirty)
	{
		Bounds = CalcBounds(GetWorldTransform());
		bBoundsDirty = false;
	}
	return Bounds;
}

FBoxSphereBounds UPrimitiveComponent::CalcBounds(const FMatrix& LocalToWorld) const
{
	FBoxSphereBounds LocalBound = GetLocalBounds();

	if (LocalBound.Radius <= 0.f && LocalBound.BoxExtent.X == 0.f)
	{
		FVector Translation(LocalToWorld.M[3][0], LocalToWorld.M[3][1], LocalToWorld.M[3][2]);
		return { Translation, 1.0f, FVector(1, 1, 1) };
	}

	FVector Center = LocalToWorld.TransformPosition(LocalBound.Center);

	// Abs upper 3 rows of LocalToWorld via XMVectorAbs, then use
	// XMVector3TransformNormal to replace the 9 scalar multiply-adds.
	Float4 AR0, AR1, AR2;
	DirectX::XMStoreFloat4(&AR0, DirectX::XMVectorAbs(DirectX::XMLoadFloat4(reinterpret_cast<const Float4*>(LocalToWorld.M[0]))));
	DirectX::XMStoreFloat4(&AR1, DirectX::XMVectorAbs(DirectX::XMLoadFloat4(reinterpret_cast<const Float4*>(LocalToWorld.M[1]))));
	DirectX::XMStoreFloat4(&AR2, DirectX::XMVectorAbs(DirectX::XMLoadFloat4(reinterpret_cast<const Float4*>(LocalToWorld.M[2]))));

	const XMMatrix AbsXM(
		AR0.x, AR0.y, AR0.z, AR0.w,
		AR1.x, AR1.y, AR1.z, AR1.w,
		AR2.x, AR2.y, AR2.z, AR2.w,
		0.f,   0.f,   0.f,   1.f
	);
	Float3 ExtentOut;
	DirectX::XMStoreFloat3(&ExtentOut, DirectX::XMVector3TransformNormal(
		DirectX::XMVectorSet(LocalBound.BoxExtent.X, LocalBound.BoxExtent.Y, LocalBound.BoxExtent.Z, 0.f),
		AbsXM
	));
	const FVector WorldBoxExtent(ExtentOut.x, ExtentOut.y, ExtentOut.z);

	return { Center, WorldBoxExtent.Size(), WorldBoxExtent };
}

/*
void UPrimitiveComponent::Serialize(FArchive& Ar)
{
	USceneComponent::Serialize(Ar);
	Ar.Serialize("bDrawDebugBounds", bDrawDebugBounds);
}
*/
