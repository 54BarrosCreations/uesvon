// Fill out your copyright notice in the Description page of Project Settings.

#include "SVONVolume.h"
#include "Engine/CollisionProfile.h"
#include "Components/BrushComponent.h"
#include "DrawDebugHelpers.h"
#include <chrono>

using namespace std::chrono;

ASVONVolume::ASVONVolume(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	GetBrushComponent()->Mobility = EComponentMobility::Static;

	BrushColor = FColor(255, 255, 255, 255);

	bColored = true;

	FBox bounds = GetComponentsBoundingBox(true);
	bounds.GetCenterAndExtents(myOrigin, myExtent);
}

#if WITH_EDITOR

void ASVONVolume::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{ 
	Super::PostEditChangeProperty(PropertyChangedEvent);
}

void ASVONVolume::PostEditUndo()
{
	Super::PostEditUndo();
}

void ASVONVolume::OnPostShapeChanged()
{

}

#endif // WITH_EDITOR

/************************************************************************/
/* Regenerates the Sparse Voxel Octree Navmesh                          */
/************************************************************************/
bool ASVONVolume::Generate()
{
	FlushPersistentDebugLines(GetWorld());

	// Get bounds and extent
	FBox bounds = GetComponentsBoundingBox(true);
	bounds.GetCenterAndExtents(myOrigin, myExtent);

	// Setup timing
	milliseconds startMs = duration_cast<milliseconds>(
		system_clock::now().time_since_epoch()
		);

	// Clear data (for now)
	myBlockedIndices.Empty();
	myData.myLayers.Empty();

	myNumLayers = myVoxelPower + 1;

	// Rasterize at Layer 1
	FirstPassRasterize();

	// Allocate the leaf node data
	myData.myLeafNodes.Empty();
	myData.myLeafNodes.AddDefaulted(myBlockedIndices[0].Num() * 8 * 0.25f);

	// Add layers
	for (int i = 0; i < myNumLayers; i++)
	{
		myData.myLayers.Emplace();
	}

	// Rasterize layer, bottom up, adding parent/child links
	for (int i = 0; i < myNumLayers; i++)
	{
		RasterizeLayer(i);
	}

	// Now traverse down, adding neighbour links
	for (int i = myNumLayers - 2; i >= 0; i--)
	{
		BuildNeighbourLinks(i);
	}

	int32 buildTime = (duration_cast<milliseconds>(
		system_clock::now().time_since_epoch()
		) - startMs).count();

	int32 totalNodes = 0;

	for (int i = 0; i < myNumLayers; i++)
	{
		totalNodes += myData.myLayers[i].Num();
	}

	int32 totalBytes = sizeof(SVONNode) * totalNodes;
	totalBytes += sizeof(SVONLeafNode) * myData.myLeafNodes.Num();

	UE_LOG(UESVON, Display, TEXT("Generation Time : %d"), buildTime);
	UE_LOG(UESVON, Display, TEXT("Total Layers-Nodes : %d-%d"), myNumLayers, totalNodes);
	UE_LOG(UESVON, Display, TEXT("Total Leaf Nodes : %d"), myData.myLeafNodes.Num());
	UE_LOG(UESVON, Display, TEXT("Total Size (bytes): %d"), totalBytes);


	return true;
}

bool ASVONVolume::FirstPassRasterize()
{
	// Add the first layer of blocking
	myBlockedIndices.Emplace();

	int32 numNodes = GetNodesInLayer(1);
	for (int32 i = 0; i < numNodes; i++)
	{
		FVector position;
		GetNodePosition(1, i, position);
		FCollisionQueryParams params;
		params.bFindInitialOverlaps = true;
		params.bTraceComplex = false;
		params.TraceTag = "SVONFirstPassRasterize";
		if (GetWorld()->OverlapBlockingTestByChannel(position, FQuat::Identity, myCollisionChannel, FCollisionShape::MakeBox(FVector(GetVoxelSize(1) * 0.5f)), params))
		{
			myBlockedIndices[0].Add(i);
		}
	}

	int layerIndex = 0;

	while (myBlockedIndices[layerIndex].Num() > 1)
	{
		// Add a new layer to structure
		myBlockedIndices.Emplace();
		// Add any parent morton codes to the new layer
		for (mortoncode_t& code : myBlockedIndices[layerIndex])
		{
			myBlockedIndices[layerIndex + 1].Add(code >> 3);
		}
		layerIndex++;
	}

	return true;
}

bool ASVONVolume::GetNodePosition(layerindex_t aLayer, mortoncode_t aCode, FVector& oPosition) const
{
	float voxelSize = GetVoxelSize(aLayer);
	uint_fast32_t x, y, z;
	morton3D_64_decode(aCode, x, y, z);
	oPosition = myOrigin - myExtent + FVector(x * voxelSize, y * voxelSize, z * voxelSize) + FVector(voxelSize * 0.5f);
	return true;
}

// Gets the position of a given link. Returns true if the link is open, false if blocked
bool ASVONVolume::GetLinkPosition(const SVONLink& aLink, FVector& oPosition) const
{
	const SVONNode& node = GetLayer(aLink.GetLayerIndex())[aLink.GetNodeIndex()];

	GetNodePosition(aLink.GetLayerIndex(), node.myCode, oPosition);
	// If this is layer 0, and there are valid children
	if (aLink.GetLayerIndex() == 0 && node.myFirstChild.IsValid())
	{
		float voxelSize = GetVoxelSize(0);
		uint_fast32_t x,y,z;
		morton3D_64_decode(aLink.GetSubnodeIndex(), x,y,z);
		oPosition += FVector(x * voxelSize * 0.25f, y * voxelSize * 0.25f, z * voxelSize * 0.25f) - FVector(voxelSize * 0.375);
		const SVONLeafNode& leafNode = GetLeafNode(node.myFirstChild.myNodeIndex);
		bool isBlocked = leafNode.GetNode(aLink.GetSubnodeIndex());
		return !isBlocked;
	}
	return true;
}

bool ASVONVolume::GetIndexForCode(layerindex_t aLayer, mortoncode_t aCode, nodeindex_t& oIndex) const
{
	const TArray<SVONNode>& layer = GetLayer(aLayer);

	for (int i = 0; i < layer.Num(); i++)
	{
		if (layer[i].myCode == aCode)
		{
			oIndex = i;
			return true;
		}
	}

	return false;
}

const SVONNode& ASVONVolume::GetNode(const SVONLink& aLink) const
{
	if (aLink.GetLayerIndex() < 14)
	{
		return GetLayer(aLink.GetLayerIndex())[aLink.GetNodeIndex()];
	}
	else
	{
		return GetLayer(myNumLayers - 1)[0];
	}
}

const SVONLeafNode& ASVONVolume::GetLeafNode(nodeindex_t aIndex) const
{
	return myData.myLeafNodes[aIndex];
}

void ASVONVolume::GetLeafNeighbours(const SVONLink& aLink, TArray<SVONLink>& oNeighbours) const
{
	mortoncode_t leafIndex = aLink.GetSubnodeIndex();
	const SVONNode& node = GetNode(aLink);
	const SVONLeafNode& leaf = GetLeafNode(node.myFirstChild.GetNodeIndex());

	// Get our starting co-ordinates
	uint_fast32_t x = 0, y = 0, z = 0;
	morton3D_64_decode(leafIndex, x, y, z);

	for (int i = 0; i < 6; i++)
	{
		// Need to switch to signed ints
		int32 sX = x + SVONStatics::dirs[i].X;
		int32 sY = y + SVONStatics::dirs[i].Y;
		int32 sZ = z + SVONStatics::dirs[i].Z;

		// If the neighbour is in bounds of this leaf node
		if (sX >= 0 && sX < 4 && sY >= 0 && sY < 4 && sZ >= 0 && sZ < 4)
		{
			mortoncode_t thisIndex = morton3D_64_encode(sX, sY, sZ);
			// If this node is blocked, then no link in this direction, continue
			if (leaf.GetNode(thisIndex))
			{
				continue;
			}
			else // Otherwise, this is a valid link, add it
			{
				oNeighbours.Emplace(0, aLink.GetNodeIndex(), thisIndex);
				continue;
			}
			
		}
		else // the neighbours is out of bounds, we need to find our neighbour
		{
			const SVONLink& neighbourLink = node.myNeighbours[i];
			const SVONNode& neighbourNode = GetNode(neighbourLink);

			// If the neighbour layer 0 has no leaf nodes, just return it
			if (!neighbourNode.myFirstChild.IsValid())
			{
				oNeighbours.Add(neighbourLink);
				continue;
			}

			const SVONLeafNode& leafNode = GetLeafNode(neighbourNode.myFirstChild.GetNodeIndex());

			if (leafNode.IsCompletelyBlocked())
			{
				// The leaf node is completely blocked, we don't return it
				continue;
			}
			else // Otherwise, we need to find the correct subnode
			{
				if (sX < 0)
					sX = 3;
				else if (sX > 3)
					sX = 0;
				else if (sY < 0)
					sY = 3;
				else if (sY > 3)
					sY = 0;
				else if (sZ < 0)
					sZ = 3;
				else if (sZ > 3)
					sZ = 0;
				//
				mortoncode_t subNodeCode = morton3D_64_encode(sX, sY, sZ);

				// Only return the neighbour if it isn't blocked!
				if (!leafNode.GetNode(subNodeCode))
				{
					oNeighbours.Emplace(0, neighbourNode.myFirstChild.GetNodeIndex(), subNodeCode);
				}
			}
		}
			
	}

}

void ASVONVolume::GetNeighbours(const SVONLink& aLink, TArray<SVONLink>& oNeighbours) const
{
	const SVONNode& node = GetNode(aLink);

	for (int i = 0; i < 6; i++)
	{
		const SVONLink& neighbourLink = node.myNeighbours[i];

		if (!neighbourLink.IsValid())
			continue;

		
		const SVONNode& neighbour = GetNode(neighbourLink);

		// If the neighbour has no children, we just use it
		if (!neighbour.myFirstChild.IsValid())
		{
			oNeighbours.Add(neighbourLink);
			continue;
		}

		// TODO: This recursive section should be the most accurate, ensuring that when pathfinding down multiple levels (say, 2 to leaf),
		// That all valid edge nodes (with no children) in that direction are considered
		// Is does mean that the search *explodes* in this scenario.

		//TArray<SVONLink> workingSet;

		//workingSet.Push(neighbourLink);

		//// Otherwise, we gotta recurse down 

		//while (workingSet.Num() > 0)
		//{
		//	// Pop off the neighbour
		//	SVONLink thisLink = workingSet.Pop();

		//	// If it's above layer 0, we need to add 4 children to explore
		//	if (thisLink.GetLayerIndex() > 0)
		//	{
		//		for (const nodeindex_t& index : SVONStatics::dirChildOffsets[i])
		//		{
		//			// Each of the childnodes
		//			SVONLink link = neighbour.myFirstChild;
		//			link.myNodeIndex += index;
		//			const SVONNode& linkNode = GetNode(link);

		//			if (linkNode.HasChildren()) // If it has children, add them to the list to keep going down
		//			{
		//				workingSet.Emplace(link.GetLayerIndex(), link.GetNodeIndex(), link.GetSubnodeIndex());
		//			}
		//			else // Or just add to the outgoing links
		//			{
		//				oNeighbours.Add(link);
		//			}
		//		}
		//	}
		//	else
		//	{
		//		for (const nodeindex_t& leafIndex : SVONStatics::dirLeafChildOffsets[i])
		//		{
		//			// Each of the childnodes
		//			SVONLink link = neighbour.myFirstChild;
		//			//link.mySubnodeIndex = leafIndex;
		//			const SVONLeafNode& leafNode = GetLeafNode(link.myNodeIndex);

		//			if (!leafNode.GetNode(leafIndex))
		//			{
		//				oNeighbours.Add(link);
		//			}
		//		}
		//	}
		//}



		// If the neighbour has children and is a leaf node, we need to add 16 leaf voxels 
		else if (neighbour.myFirstChild.GetLayerIndex() == 0)
		{
			for (const nodeindex_t& index : SVONStatics::dirLeafChildOffsets[i])
			{
				// This is the link to our first child, we just need to add our offsets
				SVONLink link = neighbour.myFirstChild;
				if(!GetLeafNode(link.GetNodeIndex()).GetNode(index))
					oNeighbours.Emplace(link.GetLayerIndex(), link.GetNodeIndex(), index );
			}
		}
		else // If the neighbour has children and isn't a leaf, we just add 4
			//TODO: the problem with this is that you no longer have the direction information to know which subnodes to select,
			//   in the case that *this* child has children
		{
			for (const nodeindex_t& index : SVONStatics::dirChildOffsets[i])
			{
				// This is the link to our first child, we just need to add our offsets
				SVONLink link = neighbour.myFirstChild;
				oNeighbours.Emplace(link.GetLayerIndex(), link.GetNodeIndex() + index, link.GetSubnodeIndex());
			}
		}
	}
}

float ASVONVolume::GetVoxelSize(layerindex_t aLayer) const
{
	return (myExtent.X / FMath::Pow(2, myVoxelPower)) * (FMath::Pow(2.0f, aLayer + 1));
}


bool ASVONVolume::IsReadyForNavigation()
{
	return myIsReadyForNavigation;
}


int32 ASVONVolume::GetNodesInLayer(layerindex_t aLayer)
{
	return FMath::Pow(FMath::Pow(2, (myVoxelPower - (aLayer))), 3);
}

int32 ASVONVolume::GetNodesPerSide(layerindex_t aLayer)
{
	return FMath::Pow(2, (myVoxelPower - (aLayer)));
}

void ASVONVolume::BeginPlay()
{
	if (!myIsReadyForNavigation)
	{
		Generate();
		myIsReadyForNavigation = true;
	}
}

void ASVONVolume::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
}

void ASVONVolume::PostUnregisterAllComponents()
{
	Super::PostUnregisterAllComponents();
}

void ASVONVolume::BuildNeighbourLinks(layerindex_t aLayer)
{

	TArray<SVONNode>& layer = GetLayer(aLayer);
	layerindex_t searchLayer = aLayer;

	// For each node
	for (nodeindex_t i = 0; i < layer.Num(); i++)
	{
		SVONNode& node = layer[i];
		// Get our world co-ordinate
		uint_fast32_t x, y, z;
		morton3D_64_decode(node.myCode, x, y, z);
		nodeindex_t backtrackIndex = -1;
		nodeindex_t index = i;
		FVector nodePos;
		GetNodePosition(aLayer, node.myCode, nodePos);

		// For each direction
		for (int d = 0; d < 6; d++)
		{
			SVONLink& linkToUpdate = node.myNeighbours[d];

			backtrackIndex = index;

			while (!FindLinkInDirection(searchLayer, index, d, linkToUpdate, nodePos)
				&& aLayer < myData.myLayers.Num() - 2)
			{
				SVONLink& parent = GetLayer(searchLayer)[index].myParent;
				if (parent.IsValid())
				{
					index = parent.myNodeIndex;
					searchLayer = parent.myLayerIndex;
				}
				else
				{
					searchLayer++;
					GetIndexForCode(searchLayer, node.myCode >> 3, index);
				}

			}
			index = backtrackIndex;
			searchLayer = aLayer;
		}
	}
}

bool ASVONVolume::FindLinkInDirection(layerindex_t aLayer, const nodeindex_t aNodeIndex, uint8 aDir, SVONLink& oLinkToUpdate, FVector& aStartPosForDebug)
{
	int32 maxCoord = GetNodesPerSide(aLayer);
	SVONNode& node = GetLayer(aLayer)[aNodeIndex];
	TArray<SVONNode>& layer = GetLayer(aLayer);

	// Get our world co-ordinate
	uint_fast32_t x = 0, y = 0, z = 0;
	morton3D_64_decode(node.myCode, x, y, z);
	int32 sX = x, sY = y, sZ = z;
	// Add the direction
	sX += SVONStatics::dirs[aDir].X;
	sY += SVONStatics::dirs[aDir].Y;
	sZ += SVONStatics::dirs[aDir].Z;

	// If the coords are out of bounds, the link is invalid.
	if (sX < 0 || sX >= maxCoord || sY < 0 || sY >= maxCoord || sZ < 0 || sZ >= maxCoord)
	{
		oLinkToUpdate.SetInvalid();
		if (myShowNeighbourLinks)
		{
			FVector startPos, endPos;
			GetNodePosition(aLayer, node.myCode, startPos);
			endPos = startPos + (FVector(SVONStatics::dirs[aDir]) * 100.f);
			DrawDebugLine(GetWorld(), aStartPosForDebug, endPos, FColor::Red, true, -1.f, 0, .0f);
		}
		return true;
	}
	x = sX; y = sY; z = sZ;
	// Get the morton code for the direction
	mortoncode_t thisCode = morton3D_64_encode(x, y, z);
	bool isHigher = thisCode > node.myCode;
	int32 nodeDelta = (isHigher ?  1 : - 1);

	while ((aNodeIndex + nodeDelta) < layer.Num() && aNodeIndex + nodeDelta >= 0)
	{
		// This is the node we're looking for
		if (layer[aNodeIndex + nodeDelta].myCode == thisCode)
		{
			const SVONNode& thisNode = layer[aNodeIndex + nodeDelta];
			// This is a leaf node
			if (aLayer == 0 && thisNode.HasChildren())
			{
				// Set invalid link if the leaf node is completely blocked, no point linking to it
				if (GetLeafNode(thisNode.myFirstChild.GetNodeIndex()).IsCompletelyBlocked())
				{
					oLinkToUpdate.SetInvalid();
					return true;
				}
			}
			// Otherwise, use this link
			oLinkToUpdate.myLayerIndex = aLayer;
			check(aNodeIndex + nodeDelta < layer.Num());
			oLinkToUpdate.myNodeIndex = aNodeIndex + nodeDelta;
			if (myShowNeighbourLinks)
			{
				FVector endPos;
				GetNodePosition(aLayer, thisCode, endPos);
				DrawDebugLine(GetWorld(), aStartPosForDebug, endPos, SVONStatics::myLinkColors[aLayer], true, -1.f, 0, .0f);
			}
			return true;
		}
		// If we've passed the code we're looking for, it's not on this layer
		else if ((isHigher && layer[aNodeIndex + nodeDelta].myCode > thisCode) || (!isHigher && layer[aNodeIndex + nodeDelta].myCode < thisCode))
		{
			return false;
		}

		nodeDelta += (isHigher ? 1 : -1);
	}



	// I'm not entirely sure if it's valid to reach the end? Hmmm...
	return false;

}

void ASVONVolume::RasterizeLeafNode(FVector& aOrigin, nodeindex_t aLeafIndex)
{
	for (int i = 0; i < 64; i++)
	{

		uint_fast32_t x, y, z;
		morton3D_64_decode(i, x, y, z);
		float leafVoxelSize = GetVoxelSize(0) * 0.25f;
		FVector position = aOrigin + FVector(x * leafVoxelSize, y * leafVoxelSize, z * leafVoxelSize) + FVector(leafVoxelSize * 0.5f);

		if (aLeafIndex >= myData.myLeafNodes.Num() - 1)
			myData.myLeafNodes.AddDefaulted(1);

		if(IsBlocked(position, leafVoxelSize * 0.5f))
		{
			myData.myLeafNodes[aLeafIndex].SetNode(i);

			if (myShowLeafVoxels) {
				DrawDebugBox(GetWorld(), position, FVector(leafVoxelSize * 0.5f), FQuat::Identity, FColor::Red, true, -1.f, 0, .0f);
			}
		}
	}
}

TArray<SVONNode>& ASVONVolume::GetLayer(layerindex_t aLayer)
{
	return myData.myLayers[aLayer];
}

const TArray<SVONNode>& ASVONVolume::GetLayer(layerindex_t aLayer) const
{
	return myData.myLayers[aLayer];
}

// Check for blocking...using this cached set for each layer for now for fast lookups
bool ASVONVolume::IsAnyMemberBlocked(layerindex_t aLayer, mortoncode_t aCode)
{
	mortoncode_t parentCode = aCode >> 3;

	if (aLayer == myBlockedIndices.Num())
	{
		return true;
	}
	// The parent of this code is blocked
	if (myBlockedIndices[aLayer].Contains(parentCode))
	{
		return true;
	}

	return false;
}

bool ASVONVolume::IsBlocked(const FVector& aPosition, const float aSize) const
{
	FCollisionQueryParams params;
	params.bFindInitialOverlaps = true;
	params.bTraceComplex = false;
	params.TraceTag = "SVONLeafRasterize";

	return GetWorld()->OverlapBlockingTestByChannel(aPosition, FQuat::Identity, myCollisionChannel, FCollisionShape::MakeBox(FVector(aSize)), params);
}

bool ASVONVolume::SetNeighbour(const layerindex_t aLayer, const nodeindex_t aArrayIndex, const dir aDirection)
{
	return false;
}


void ASVONVolume::RasterizeLayer(layerindex_t aLayer)
{
	nodeindex_t leafIndex = 0;
	// Layer 0 Leaf nodes are special
	if (aLayer == 0)
	{
		// Run through all our coordinates
		int32 numNodes = GetNodesInLayer(aLayer);
		for (int32 i = 0; i < numNodes; i++)
		{
			int index = (i);

			// If we know this node needs to be added, from the low res first pass
			if (myBlockedIndices[0].Contains(i >> 3))
			{
				// Add a node
				index = GetLayer(aLayer).Emplace();
				SVONNode& node = GetLayer(aLayer)[index];

				// Set my code and position
				node.myCode = (i);

				FVector nodePos;
				GetNodePosition(aLayer, node.myCode, nodePos);

				// Debug stuff
				if (myShowMortonCodes) {
					DrawDebugString(GetWorld(), nodePos, FString::FromInt(node.myCode), nullptr, SVONStatics::myLayerColors[aLayer], -1, false);
				}
				if (myShowVoxels) {
					DrawDebugBox(GetWorld(), nodePos, FVector(GetVoxelSize(aLayer) * 0.5f), FQuat::Identity, SVONStatics::myLayerColors[aLayer], true, -1.f, 0, .0f);
				}
				
				// Now check if we have any blocking, and search leaf nodes
				FVector position;
				GetNodePosition(0, i, position);

				FCollisionQueryParams params;
				params.bFindInitialOverlaps = true;
				params.bTraceComplex = false;
				params.TraceTag = "SVONRasterize";

				if(IsBlocked(position, GetVoxelSize(0) * 0.5f))
				{
					// Rasterize my leaf nodes
					FVector leafOrigin = nodePos - (FVector(GetVoxelSize(aLayer) * 0.5f));
					RasterizeLeafNode(leafOrigin, leafIndex);
					node.myFirstChild.SetLayerIndex(0);
					node.myFirstChild.SetNodeIndex(leafIndex);
					node.myFirstChild.SetSubnodeIndex(0);
					leafIndex++;
				}
				else
				{
					myData.myLeafNodes.AddDefaulted(1);
					leafIndex++;
					node.myFirstChild.SetInvalid();
				}


			}
		}
	}
	// Deal with the other layers
	else if (GetLayer(aLayer - 1).Num() > 1)
	{
		int nodeCounter = 0;
		int32 numNodes = GetNodesInLayer(aLayer);
		for (int32 i = 0; i < numNodes; i++)
		{
			// Do we have any blocking children, or siblings?
			// Remember we must have 8 children per parent
			if (IsAnyMemberBlocked(aLayer, i))
			{
				// Add a node
				int32 index = GetLayer(aLayer).Emplace();
				nodeCounter++;
				SVONNode& node = GetLayer(aLayer)[index];
				// Set details
				node.myCode = i;
				nodeindex_t childIndex = 0;
				if (GetIndexForCode(aLayer - 1, node.myCode << 3, childIndex))
				{
					// Set parent->child links
					node.myFirstChild.SetLayerIndex(aLayer - 1);
					node.myFirstChild.SetNodeIndex(childIndex);
					// Set child->parent links, this can probably be done smarter, as we're duplicating work here
					for (int iter = 0; iter < 8; iter++)
					{
						GetLayer(node.myFirstChild.GetLayerIndex())[node.myFirstChild.GetNodeIndex() + iter].myParent.SetLayerIndex(aLayer);
						GetLayer(node.myFirstChild.GetLayerIndex())[node.myFirstChild.GetNodeIndex() + iter].myParent.SetNodeIndex(index);
					}
					
					if (myShowParentChildLinks) // Debug all the things
					{
						FVector startPos, endPos;
						GetNodePosition(aLayer, node.myCode, startPos);
						GetNodePosition(aLayer - 1, node.myCode << 3, endPos);
						DrawDebugDirectionalArrow(GetWorld(), startPos, endPos, 0.f, SVONStatics::myLinkColors[aLayer], true);
					}
				}
				else
				{
					node.myFirstChild.SetInvalid();
				}

				if (myShowMortonCodes || myShowVoxels)
				{
					FVector nodePos;
					GetNodePosition(aLayer, i, nodePos);

					// Debug stuff
					if (myShowVoxels) {
						DrawDebugBox(GetWorld(), nodePos, FVector(GetVoxelSize(aLayer) * 0.5f), FQuat::Identity, SVONStatics::myLayerColors[aLayer], true, -1.f, 0, .0f);
					}
					if (myShowMortonCodes) {
						DrawDebugString(GetWorld(), nodePos, FString::FromInt(node.myCode), nullptr, SVONStatics::myLayerColors[aLayer], -1, false);
					}
				}

			}
		}
	}

}
