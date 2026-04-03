#pragma once
#include "CoreMinimal.h"

class AActor;
struct FRay;

struct FKDBox
{
	FVector Min;
	FVector Max;

	FVector GetCenter() const { return (Min + Max) * 0.5f; }
	FVector GetExtent() const { return (Max - Min) * 0.5f; }
	int32   GetLongestAxis() const;
};


struct FKDEntry
{
	AActor* Actor = nullptr;
	FKDBox   WorldBox;
	FMatrix  WorldMatrix;
	FMatrix  WorldMatrixInverse;
};


struct FKDNode
{
	FKDBox  Bounds;

	int32   SplitAxis = -1;
	float   SplitPos = 0.f;

	int32   LeftChild = -1;
	int32   RightChild = -1;

	int32   EntryStart = -1;
	int32   EntryCount = 0;

	bool IsLeaf() const { return SplitAxis == -1; }
};


class FKDTree
{
public:
	void    Build(const TArray<AActor*>& Actors);
	AActor* QueryRay(const FRay& Ray) const;
	void    Clear();

private:
	int32 BuildRecursive(
		TArray<int32>& Indices,
		int32 Depth
	);

	void TraverseRecursive(
		int32       NodeIndex,
		const FRay& Ray,
		float& OutBestT,
		AActor*& OutBestActor) const;

	void CheckLeaf(
		const FKDNode& Node,
		const FRay& Ray,
		float& OutBestT,
		AActor*& OutBestActor) const;

private:
	TArray<FKDNode>  Nodes;
	TArray<FKDEntry> Entries;
	TArray<int32> LeafEntryIndices;

	static constexpr int32 MaxLeafEntries = 8;
	static constexpr int32 MaxDepth = 20;
};