#include "OctreeNode.h"

FOctreeNode::~FOctreeNode()
{
	for (int32 ChildIndex = 0; ChildIndex < 8; ++ChildIndex)
	{
		delete Children[ChildIndex];
		Children[ChildIndex] = nullptr;
	}
}
