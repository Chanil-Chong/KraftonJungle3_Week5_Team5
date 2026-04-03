#include "Octree.h"
#include <algorithm>
#include <limits>
#include <utility>

namespace
{
	bool RayIntersectsAABB(const FVector& RayOrigin, const FVector& RayDirection, const FAABB& Bounds, float& OutTNear, float& OutTFar)
	{
		constexpr float Epsilon = 1.0e-8f;

		float TNear = 0.0f;
		float TFar = (std::numeric_limits<float>::max)();

		auto TestAxis = [&](float Origin, float Dir, float MinV, float MaxV) -> bool
			{
				if (std::abs(Dir) < Epsilon)
				{
					return Origin >= MinV && Origin <= MaxV;
				}

				float T1 = (MinV - Origin) / Dir;
				float T2 = (MaxV - Origin) / Dir;

				if (T1 > T2)
				{
					std::swap(T1, T2);
				}

				TNear = (std::max)(TNear, T1);
				TFar = (std::min)(TFar, T2);

				return TNear <= TFar;
			};

		if (!TestAxis(RayOrigin.X, RayDirection.X, Bounds.Min.X, Bounds.Max.X)) return false;
		if (!TestAxis(RayOrigin.Y, RayDirection.Y, Bounds.Min.Y, Bounds.Max.Y)) return false;
		if (!TestAxis(RayOrigin.Z, RayDirection.Z, Bounds.Min.Z, Bounds.Max.Z)) return false;

		if (TFar < 0.0f)
		{
			return false;
		}

		OutTNear = TNear;
		OutTFar = TFar;
		return true;
	}
}

FOctree::FOctree(const FAABB& InRootBounds, uint32 InMaxDepth, uint32 InMaxElementsPerNode)
{
	MaxDepth = InMaxDepth;
	MaxElementsPerNode = (InMaxElementsPerNode > 0) ? InMaxElementsPerNode : 1;
	Root = new FOctreeNode(InRootBounds, 0);
}

FOctree::~FOctree()
{
	delete Root;
	Root = nullptr;
}

void FOctree::Clear()
{
	if (!Root)
	{
		return;
	}


	const FAABB RootBounds = Root->Bounds;
	delete Root;
	Root = new FOctreeNode(RootBounds, 0);
}

void FOctree::Insert(const FOctreeElement& Element)
{
	if (!Root)
	{
		return;
	}
	// 루트의 Bound에 포함되지 않는 원소라면 제외한다.
	// TODO: 확장 로직 추가
	if (!Root->Bounds.Intersects(Element.Bounds))
	{
		return;
	}
	// 재귀적으로 child를 참고해서 leaf node에 element를 삽입
	InsertRecursive(Root, Element);
}

void FOctree::QueryAABB(const FAABB& QueryBounds, TArray<const FOctreeElement*>& OutResults) const
{
	if (!Root)
	{
		return;
	}
	// 재귀적으로 AABB 탐색
	QueryAABBRecursive(Root, QueryBounds, OutResults);
}

void FOctree::QueryRay(const FVector& RayOrigin, const FVector& RayDirection, TArray<const FOctreeElement*>& OutResults) const
{
	if (!Root)
	{
		return;
	}

	QueryRayRecursive(Root, RayOrigin, RayDirection, OutResults);
}

void FOctree::InsertRecursive(FOctreeNode* Node, const FOctreeElement& Element)
{
	if (!Node)
	{
		return;
	}

	if (Node->IsLeaf())
	{
		// MaxDepth이고, Node의 Element 여유공간이 남아있다면 원소 삽입
		const bool bReachedMaxDepth = Node->Depth >= MaxDepth;
		const bool bHasCapacity = Node->Elements.size() < MaxElementsPerNode;
		if (bReachedMaxDepth || bHasCapacity)
		{
			Node->Elements.push_back(Element);
			return;
		}
		// 아니라면 Node 확장 + 원소 이전
		SplitNode(Node);
		RedistributeElements(Node);
	}

	const int32 ChildIndex = FindContainingChildIndex(Node, Element.Bounds);
	// 노드의 AABB를 완전히 포함하는 자식의 index가 존재하고, 그 index가 null이 아니라면
	if (ChildIndex >= 0 && Node->Children[ChildIndex])
	{
		// 해당 자식에 원소 삽입
		InsertRecursive(Node->Children[ChildIndex], Element);
		return;
	}

	// 아니라면 유일하게 남은 root에 원소 삽입
	Node->Elements.push_back(Element);
}

void FOctree::SplitNode(FOctreeNode* Node)
{
	// 루트노드가 있거나, 루트 노드의 자식이 있다면 스킵
	if (!Node || !Node->IsLeaf())
	{
		return;
	}
	// Node의 Children 배열에 OctreeNode 생성(자식 노드 8개를 생성한다)
	for (int32 ChildIndex = 0; ChildIndex < 8; ++ChildIndex)
	{
		Node->Children[ChildIndex] = new FOctreeNode(ComputeChildBounds(Node->Bounds, ChildIndex), Node->Depth + 1);
	}
}

void FOctree::RedistributeElements(FOctreeNode* Node)
{
	if (!Node || Node->IsLeaf() || Node->Elements.empty())
	{
		return;
	}

	TArray<FOctreeElement> RemainingElements;
	RemainingElements.reserve(Node->Elements.size());

	for (const FOctreeElement& Element : Node->Elements)
	{
		const int32 ChildIndex = FindContainingChildIndex(Node, Element.Bounds);
		if (ChildIndex >= 0 && Node->Children[ChildIndex])
		{
			InsertRecursive(Node->Children[ChildIndex], Element);
		}
		else
		{
			RemainingElements.push_back(Element);
		}
	}
	// 노드를 모두 삽입한다
	Node->Elements = std::move(RemainingElements);
}

int32 FOctree::FindContainingChildIndex(const FOctreeNode* Node, const FAABB& Bounds) const
{
	if (!Node)
	{
		return -1;
	}

	for (int32 ChildIndex = 0; ChildIndex < 8; ++ChildIndex)
	{
		const FAABB ChildBounds = ComputeChildBounds(Node->Bounds, ChildIndex);
		if (ChildBounds.Contains(Bounds))
		{
			return ChildIndex;
		}
	}

	return -1;
}

FAABB FOctree::ComputeChildBounds(const FAABB& ParentBounds, int32 ChildIndex) const
{
	const FVector Center = ParentBounds.GetCenter();

	FAABB ChildBounds;
	// x 범위 결정
	ChildBounds.Min.X = (ChildIndex & 1) ? Center.X : ParentBounds.Min.X;
	ChildBounds.Max.X = (ChildIndex & 1) ? ParentBounds.Max.X : Center.X;
	// y 범위 결정
	ChildBounds.Min.Y = (ChildIndex & 2) ? Center.Y : ParentBounds.Min.Y;
	ChildBounds.Max.Y = (ChildIndex & 2) ? ParentBounds.Max.Y : Center.Y;
	// z 범위 결정
	ChildBounds.Min.Z = (ChildIndex & 4) ? Center.Z : ParentBounds.Min.Z;
	ChildBounds.Max.Z = (ChildIndex & 4) ? ParentBounds.Max.Z : Center.Z;
	// 자식의 bound 범위를 반환
	return ChildBounds;
}

void FOctree::QueryAABBRecursive(const FOctreeNode* Node, const FAABB& QueryBounds, TArray<const FOctreeElement*>& OutResults) const
{
	// Node안에 포함되는지 Bound 검사
	if (!Node || !Node->Bounds.Intersects(QueryBounds))
	{
		return;
	}

	for (const FOctreeElement& Element : Node->Elements)
	{
		// Element가 해당 범위 안에 교차라도 한다면 원소로 추가
		if (Element.Bounds.Intersects(QueryBounds))
		{
			OutResults.push_back(&Element);
		}
	}
	// 노드의 자식에도 재귀적으로 순회
	for (const FOctreeNode* Child : Node->Children)
	{
		if (Child)
		{
			QueryAABBRecursive(Child, QueryBounds, OutResults);
		}
	}
}

void FOctree::QueryRayRecursive(const FOctreeNode* Node, const FVector& RayOrigin, const FVector& RayDirection, TArray<const FOctreeElement*>& OutResults) const
{
	if (!Node)
	{
		return;
	}

	float NodeTNear = 0.0f;
	float NodeTFar = 0.0f;
	if (!RayIntersectsAABB(RayOrigin, RayDirection, Node->Bounds, NodeTNear, NodeTFar))
	{
		return;
	}

	for (const FOctreeElement& Element : Node->Elements)
	{
		float ElementTNear = 0.0f;
		float ElementTFar = 0.0f;
		if (RayIntersectsAABB(RayOrigin, RayDirection, Element.Bounds, ElementTNear, ElementTFar))
		{
			OutResults.push_back(&Element);
		}
	}

	for (int32 ChildIndex = 0; ChildIndex < 8; ++ChildIndex)
	{
		if (Node->Children[ChildIndex])
		{
			QueryRayRecursive(Node->Children[ChildIndex], RayOrigin, RayDirection, OutResults);
		}
	}
}
