#include "PrimitiveComponent.h"
#include "Object/Class.h"
#include "Serializer/Archive.h"
#include "Debug/EngineLog.h"
#include "Actor/Actor.h"
#include "Scene/Scene.h"
IMPLEMENT_RTTI(UPrimitiveComponent, USceneComponent)

FBoxSphereBounds UPrimitiveComponent::GetLocalBounds() const
{
	return { FVector(0, 0, 0), 0.f, FVector(0, 0, 0) };
}

void UPrimitiveComponent::UpdateBounds()
{
	bBoundsDirty = true;

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
