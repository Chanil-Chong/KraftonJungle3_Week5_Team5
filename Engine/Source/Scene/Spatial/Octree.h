#pragma once

#include "CoreMinimal.h"
#include "OctreeNode.h"

/*
* 공간 분할을 위한 Octree 자료구조
*/
class FOctree
{
public:
	/**
	 * 루트 영역과 분할 조건을 받아 Octree를 생성한다.
	 *
	 * \param InRootBounds 트리 전체를 감싸는 루트 AABB
	 * \param InMaxDepth 노드가 분할될 수 있는 최대 깊이
	 * \param InMaxElementsPerNode 노드 하나가 가질 수 있는 최대 요소 수
	 */
	FOctree(const FAABB& InRootBounds, uint32 InMaxDepth, uint32 InMaxElementsPerNode);
	~FOctree();

	/** 루트 노드를 새로 생성한다. */
	void Clear();

	/** 새 요소를 Octree에 삽입 */
	void Insert(const FOctreeElement& Element);

	/** 주어진 AABB와 겹치는 요소를 모두 찾아 결과 OutResults에 담는다 */
	void QueryAABB(const FAABB& QueryBounds, TArray<const FOctreeElement*>& OutResults) const;

	// Ray와 교차할 가능성이 있는 요소 탐색.
	void QueryRay(const FVector& RayOrigin, const FVector& RayDirection, TArray<const FOctreeElement*>& OutResults) const;

private:
	FOctreeNode* Root = nullptr;
	uint32 MaxDepth = 8;
	uint32 MaxElementsPerNode = 8;

private:
	/* 삽입 대상 요소를 적절한 자식 노드까지 내려 보내는 재귀 함수 */
	void InsertRecursive(FOctreeNode* Node, const FOctreeElement& Element);

	/* 
	* 현재 노드를 8개의 자식 노드로 분할 
	* TODO: 만약 Octree를 팀에서 사용하게 된다면 Loose Octree로 개선
	*/
	void SplitNode(FOctreeNode* Node);

	/* 분할 이후 현재 노드의 요소를 자식 노드로 다시 배치 */
	void RedistributeElements(FOctreeNode* Node);

	/* 주어진 Bounds를 완전히 포함하는 자식 인덱스를 찾고, 없으면 -1을 반환 */
	int32 FindContainingChildIndex(const FOctreeNode* Node, const FAABB& Bounds) const;

	/* 부모 경계와 자식 인덱스를 바탕으로 자식 노드의 AABB를 계산 */
	FAABB ComputeChildBounds(const FAABB& ParentBounds, int32 ChildIndex) const;

	/* AABB 질의를 수행하며 필요한 자식 노드만 재귀적으로 탐색 */
	void QueryAABBRecursive(const FOctreeNode* Node, const FAABB& QueryBounds, TArray<const FOctreeElement*>& OutResults) const;

	// Ray 질의를 재귀적으로 수행
	void QueryRayRecursive(const FOctreeNode* Node, const FVector& RayOrigin, const FVector& RayDirection, TArray<const FOctreeElement*>& OutResults) const;
};
