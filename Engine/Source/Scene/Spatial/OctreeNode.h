#pragma once

#include "CoreMinimal.h"
#include "SpatialUtils.h"

class UPrimitiveComponent;

/*
* Octree 에서 관리하는 요소 정보
*/
struct FOctreeElement
{
	/* 해당 원소의 ID */
	uint64 Id = 0;
	/* 해당 요소의 범위를 나타내는 AABB */
	FAABB Bounds;
	/* 참조용 AABB*/
	UPrimitiveComponent* UserData = nullptr;
};

/*
* Octree Node
*/
class FOctreeNode
{
public:
	/* 현재 Node가 담당하는 공간 범위 */
	FAABB Bounds;
	/* 현재 Node의 깊이 */
	uint32 Depth = 0;
	/* 현재 Node 범위 안에 저장된 요소들 */
	TArray<FOctreeElement> Elements;
	/* 현재 Node의 자식 노드들 */
	FOctreeNode* Children[8] = {};

public:
	FOctreeNode(const FAABB& InBounds, uint32 InDepth)
		: Bounds(InBounds), Depth(InDepth)
	{
	}

	~FOctreeNode();

	/* 현재 노드가 Leaf(자식이 없는 노드)인지 검사한다. */
	bool IsLeaf() const
	{
		for (int32 ChildIndex = 0; ChildIndex < 8; ++ChildIndex)
		{
			if (Children[ChildIndex])
			{
				return false;
			}
		}
		return true;
	}
};
