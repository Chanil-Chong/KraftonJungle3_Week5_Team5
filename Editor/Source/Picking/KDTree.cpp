#include "KDTree.h"

#include "Actor/Actor.h"
#include "Component/ActorComponent.h"
#include "Component/StaticMeshComponent.h"
#include "PickingUtils.h"


int32 FKDBox::GetLongestAxis() const
{
	float AxisX = Max.X - Min.X;
	float AxisY = Max.Y - Min.Y;
	float AxisZ = Max.Z - Min.Z;

	if (AxisX >= AxisY && AxisX >= AxisZ)
		return 0;

	if (AxisY >= AxisZ)
		return 1;

	return 2; 
}

void FKDTree::Build(const TArray<AActor*>& Actors)
{
	for (AActor* Actor : Actors)
	{
		for (UActorComponent* Component : Actor->GetComponents())
		{
			if (!Component->IsA(UStaticMeshComponent::StaticClass())) continue;

			UStaticMeshComponent* StaticComp = static_cast<UStaticMeshComponent*>(Component);
			const FBoxSphereBounds Bound = StaticComp->GetWorldBounds();

			FKDBox NewBox;
			NewBox.Min = Bound.Center - Bound.BoxExtent;
			NewBox.Max = Bound.Center + Bound.BoxExtent;

			FKDEntry NewEntry;
			NewEntry.Actor = Actor;
			NewEntry.WorldBox = NewBox;
			NewEntry.WorldMatrix = StaticComp->GetWorldTransform();
			NewEntry.WorldMatrixInverse = NewEntry.WorldMatrix.GetInverse();

			Entries.push_back(NewEntry);
		}
	}

	TArray<int32> Indices;
	for (int32 i = 0; i < Entries.size(); i++)
	{
		Indices.push_back(i);
	}


	BuildRecursive(Indices, 0);
}

AActor* FKDTree::QueryRay(const FRay& Ray) const
{

	return nullptr;
}

void FKDTree::Clear()
{
}

int32 FKDTree::BuildRecursive(TArray<int32>& Indices,int32 Depth)
{
	int32 NodeIndex = Nodes.size();
	Nodes.push_back(FKDNode());

	FVector BoxMin(
		(std::numeric_limits<float>::max)(),
		(std::numeric_limits<float>::max)(),
		(std::numeric_limits<float>::max)()
	);

	FVector BoxMax(
		-(std::numeric_limits<float>::max)(),
		-(std::numeric_limits<float>::max)(),
		-(std::numeric_limits<float>::max)()
	);

	for (int32 indx : Indices)
	{
		const FKDBox& Box = Entries[indx].WorldBox;

		if (Box.Min.X < BoxMin.X) BoxMin.X = Box.Min.X;
		if (Box.Min.Y < BoxMin.Y) BoxMin.Y = Box.Min.Y;
		if (Box.Min.Z < BoxMin.Z) BoxMin.Z = Box.Min.Z;

		if (Box.Max.X > BoxMax.X) BoxMax.X = Box.Max.X;
		if (Box.Max.Y > BoxMax.Y) BoxMax.Y = Box.Max.Y;
		if (Box.Max.Z > BoxMax.Z) BoxMax.Z = Box.Max.Z;
	}

	Nodes[NodeIndex].Bounds.Min = BoxMin;
	Nodes[NodeIndex].Bounds.Max = BoxMax;

	if (Depth >= MaxDepth || Indices.size() <= MaxLeafEntries)
	{
		Nodes[NodeIndex].SplitAxis = -1;
		Nodes[NodeIndex].EntryStart = LeafEntryIndices.size();
		Nodes[NodeIndex].EntryCount = Indices.size();

		for (int32 indx : Indices)
		{
			LeafEntryIndices.push_back(indx);
		}

		return NodeIndex;
	}

	int32 Axis = Nodes[NodeIndex].Bounds.GetLongestAxis();
	Nodes[NodeIndex].SplitAxis = Axis;

	int32 Mid = Indices.size() / 2;
	std::nth_element(
		Indices.begin(),
		Indices.begin() + Mid,
		Indices.end(),
		[&](int32 A, int32 B) {
			return Entries[A].WorldBox.GetCenter()[Axis]
				< Entries[B].WorldBox.GetCenter()[Axis];
		});

	Nodes[NodeIndex].SplitPos = Entries[Indices[Mid]].WorldBox.GetCenter()[Axis];

	TArray<int32> Left(Indices.begin(), Indices.begin() + Mid);
	TArray<int32> Right(Indices.begin() + Mid, Indices.end());

	int32 LeftIdx = BuildRecursive(Left, Depth + 1);
	int32 RightIdx = BuildRecursive(Right, Depth + 1);

	Nodes[NodeIndex].LeftChild = LeftIdx;
	Nodes[NodeIndex].RightChild = RightIdx;

	return NodeIndex;
}

void FKDTree::TraverseRecursive(int32 NodeIndex, const FRay& Ray, float& OutBestT, AActor*& OutBestActor) const
{
	if (NodeIndex < 0) return;

	float TNear, TFar;
	if (!PickingUtils::RayIntersectsAABB(Ray, Nodes[NodeIndex].Bounds.Min, Nodes[NodeIndex].Bounds.Max, TNear, TFar))
	{
		return;
	}

	if (TNear > OutBestT)
		return;

	if (Nodes[NodeIndex].IsLeaf())
	{
		CheckLeaf(Nodes[NodeIndex], Ray, OutBestT, OutBestActor);
		return;
	}

	int32 Axis = Nodes[NodeIndex].SplitAxis;
	int32 NearChild = Ray.Origin[Axis] < Nodes[NodeIndex].SplitPos ? Nodes[NodeIndex].LeftChild : Nodes[NodeIndex].RightChild;
	int32 FarChild = (NearChild == Nodes[NodeIndex].LeftChild) ? Nodes[NodeIndex].RightChild : Nodes[NodeIndex].LeftChild;

	TraverseRecursive(NearChild, Ray, OutBestT, OutBestActor);
	TraverseRecursive(FarChild, Ray, OutBestT, OutBestActor);
}

void FKDTree::CheckLeaf(const FKDNode& Node, const FRay& Ray, float& OutBestT, AActor*& OutBestActor) const
{
	for (int i = Node.EntryStart; i < Node.EntryCount + Node.EntryStart; i++)
	{
		float TNear, TFar;

		if (!PickingUtils::RayIntersectsAABB(Ray, Entries[LeafEntryIndices[i]].WorldBox.Min,
			Entries[LeafEntryIndices[i]].WorldBox.Max, TNear, TFar))
			continue;

		if (TNear > OutBestT)
			continue;


	}
}

