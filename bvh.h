#pragma once

// enable the use of SSE in the AABB intersection function
#define USE_SSE

// bin count for binned BVH building
#define BINS 8

namespace Tmpl8
{

// minimalist triangle struct
struct Tri { float3 vertex0, vertex1, vertex2; float3 centroid; };

// additional triangle data, for texturing and shading
struct TriEx { float2 uv0, uv1, uv2; float3 N0, N1, N2; };

// minimalist AABB struct with grow functionality
struct aabb
{
	float3 bmin = 1e30f, bmax = -1e30f;
	void grow( float3 p ) { bmin = fminf( bmin, p ); bmax = fmaxf( bmax, p ); }
	void grow( aabb& b ) { if (b.bmin.x != 1e30f) { grow( b.bmin ); grow( b.bmax ); } }
	float area()
	{
		float3 e = bmax - bmin; // box extent
		return e.x * e.y + e.y * e.z + e.z * e.x;
	}
};

// intersection record, carefully tuned to be 16 bytes in size
struct Intersection
{
	float t;		// intersection distance along ray
	float u, v;		// barycentric coordinates of the intersection
	uint instPrim;	// instance index (12 bit) and primitive index (20 bit)
};

// ray struct, prepared for SIMD AABB intersection
__declspec(align(64)) struct Ray
{
	Ray() { O4 = D4 = rD4 = _mm_set1_ps( 1 ); }
	union { struct { float3 O; float dummy1; }; __m128 O4; };
	union { struct { float3 D; float dummy2; }; __m128 D4; };
	union { struct { float3 rD; float dummy3; }; __m128 rD4; };
	Intersection hit; // total ray size: 64 bytes
};

// 32-byte BVH node struct
struct BVHNode
{
	union { struct { float3 aabbMin; uint leftFirst; }; __m128 aabbMin4; };
	union { struct { float3 aabbMax; uint triCount; }; __m128 aabbMax4; };
	bool isLeaf() { return triCount > 0; } // empty BVH leaves do not exist
	float CalculateNodeCost()
	{
		float3 e = aabbMax - aabbMin; // extent of the node
		return (e.x * e.y + e.y * e.z + e.z * e.x) * triCount;
	}
};

// bounding volume hierarchy, to be used as BLAS
class BVH
{
public:
	BVH() = default;
	BVH( class Mesh* mesh );
	void Build();
	void Refit();
	void Intersect( Ray& ray, uint instanceIdx );
private:
	void Subdivide( uint nodeIdx );
	void UpdateNodeBounds( uint nodeIdx );
	float FindBestSplitPlane( BVHNode& node, int& axis, float& splitPos );
	class Mesh* mesh = 0;
public:
	uint* triIdx = 0;
	uint nodesUsed;
	BVHNode* bvhNode = 0;
};

// minimalist mesh class
class Mesh
{
public:
	Mesh() = default;
	Mesh( const char* objFile, const char* texFile );
	Tri* tri;				// triangle data for intersection
	TriEx* triEx;			// triangle data for shading
	int triCount = 0;
	BVH* bvh;
	Surface* texture;
	float3* P, * N;
};

// instance of a BVH, with transform and world bounds
class BVHInstance
{
public:
	BVHInstance() = default;
	BVHInstance( BVH* blas, uint index ) : bvh( blas ), idx( index ) { SetTransform( mat4() ); }
	void SetTransform( mat4& transform );
	mat4& GetTransform() { return transform; }
	void Intersect( Ray& ray );
private:
	mat4 transform;
	mat4 invTransform; // inverse transform
public:
	aabb bounds; // in world space
private:
	BVH* bvh = 0;
	uint idx;
	int dummy[7];
};

// top-level BVH node
struct TLASNode
{
	union { struct { float dummy1[3]; uint leftRight; }; struct { float dummy3[3]; unsigned short left, right; }; float3 aabbMin; };
	union { struct { float dummy2[3]; uint BLAS; }; float3 aabbMax; };
	bool isLeaf() { return leftRight == 0; }
};

// custom kD-tree, used for quick TLAS construction
class KDTree
{
public:
	struct KDNode
	{
		union
		{
			struct { uint left, right, parax; float splitPos; };	// for an interior node
			struct { uint first, count, dummy1, dummy2; };			// for a leaf node, 16 bytes
		};
		union
		{
			__m128 bmin4; struct { float3 bmin; float w0; };
		};															// 16 bytes
		union
		{
			__m128 bmax4; struct { float3 bmax; float w1; };
		};															// 16 bytes
		union
		{
			__m128 minSize4; struct { float3 minSize; float w2; };
		};															// 16 bytes, total: 64 bytes
		bool isLeaf() { return (parax & 7) > 3; }
	};
	struct Bounds
	{
		union { __m128 bmin4; struct { float3 bmin; float w0; }; };
		union { __m128 bmax4; struct { float3 bmax; float w1; }; };
	};
	void swap( const uint a, const uint b )
	{
		uint t = tlasIdx[a]; tlasIdx[a] = tlasIdx[b]; tlasIdx[b] = t;
	}
	KDTree() = default;
	KDTree( TLASNode* tlasNodes, const uint N )
	{
		// allocate space for nodes and indices
		tlas = tlasNodes;			// copy of the original array of tlas nodes
		bounds = (Bounds*)_aligned_malloc( (N + 1) * 2 * sizeof( Bounds ), 64 );
		blasCount = N;				// blasCount remains constant
		tlasCount = N;				// tlasCount will grow during aggl. clustering
		leaf = new uint[N * 2];		// for delete op, we need to know which leaf contains a particular tlas
		node = (KDNode*)_aligned_malloc( sizeof( KDNode ) * N * 2, 64 ); // pre-allocate kdtree nodes, aligned
		memset( node, 0, sizeof( KDNode ) * N * 2 );
		tlasIdx = new uint[(N + 1) * 2]; // tlas array indirection so we can store ranges of nodes in leaves
	}
	void rebuild()
	{
		// we'll assume we get the same number of TLAS nodes each time
		tlasCount = blasCount;
		Timer t;
		for (uint i = 1; i <= blasCount; i++)
		{
			tlasIdx[i - 1] = i;
			// we only use the bounds of the tlas nodes, so we make a SIMD friendly copy of that data
			bounds[i].bmin = tlas[i].aabbMin, bounds[i].w0 = 0;
			bounds[i].bmax = tlas[i].aabbMax, bounds[i].w1 = 0;
		}
		printf( "bounds: %.2fms ", t.elapsed() );
		// subdivide root node
		node[0].first = 0, node[0].count = blasCount, node[0].parax = 7;
		nodePtr = 1;				// root = 0, so node 1 is the first node we can create
		subdivide( node[0] );		// recursively subdivide the root node
		// refit cluster and leaf minima
		minRefit();
	}
	void minRefit()
	{
		// "each node keeps it's cluster's minimum box sizes in each axis"
		for (int i = nodePtr - 1; i >= 0; i--) if (node[i].isLeaf())
		{
			node[i].minSize = float3( 1e30f );
			node[i].bmin = float3( 1e30f );
			node[i].bmax = float3( -1e30f );
			for (uint j = 0; j < node[i].count; j++)
			{
				uint idx = tlasIdx[node[i].first + j];
				leaf[idx] = i;		// remember that we can find tlas[idx] in node[i], which is a leaf
				float3 tlSize = 0.5f* (bounds[idx].bmax - bounds[idx].bmin);
				float3 C = (bounds[idx].bmax + bounds[idx].bmin) * 0.5f;
				node[i].minSize = fminf( node[i].minSize, tlSize );
				node[i].bmin = fminf( node[i].bmin, C );
				node[i].bmax = fmaxf( node[i].bmax, C );
			}
		}
		else
		{
			node[i].minSize = fminf( node[node[i].left].minSize, node[node[i].right].minSize );
			node[i].bmin = fminf( node[node[i].left].bmin, node[node[i].right].bmin );
			node[i].bmax = fmaxf( node[node[i].left].bmax, node[node[i].right].bmax );
		}
	}
	void recurseRefit( uint idx )
	{
		while (1)
		{
			if (idx == 0) break;
			idx = node[idx].parax >> 3;
			node[idx].minSize = fminf( node[node[idx].left].minSize, node[node[idx].right].minSize );
			node[idx].bmin = fminf( node[node[idx].left].bmin, node[node[idx].right].bmin );
			node[idx].bmax = fmaxf( node[node[idx].left].bmax, node[node[idx].right].bmax );
		}
	}
	void subdivide( KDNode& node, uint depth = 0 )
	{
		// update node bounds
		node.bmin = float3( 1e30f ), node.bmax = float3( -1e30f );
		node.minSize = float3( 1e30f );
		for (uint i = 0; i < node.count; i++)
		{
			Bounds& tln = bounds[tlasIdx[node.first + i]];
			float3 C = (tln.bmin + tln.bmax) * 0.5f;
			node.minSize = fminf( node.minSize, 0.5f * (tln.bmax - tln.bmax) );
			node.bmin = fminf( node.bmin, C ), node.bmax = fmaxf( node.bmax, C );
		}
		// terminate if we are down to 1 tlas
		if (node.count < 2) return;
		// claim left and right child nodes
		uint axis = dominantAxis( node.bmax - node.bmin );
		float center = (node.bmin[axis] + node.bmax[axis]) * 0.5f;
	#if 1
		// try to balance (works quite well but doesn't seem to pay off)
		if (node.count > 150)
		{
			// count how many would go to the left
			int leftCount = 0;
			for( uint i = 0; i < node.count; i++ )
			{
				Bounds& tl = bounds[tlasIdx[node.first + i]];
				float3 P = (tl.bmin + tl.bmax) * 0.5f;
				if (P[axis] <= center) leftCount++;
			}
			float ratio = max( 0.15f, min( 0.85f, (float)leftCount / (float)node.count ) );
			center = ratio * node.bmin[axis] + (1 - ratio) * node.bmax[axis];
		}
	#endif
		partition( node, center, axis );
		if (this->node[nodePtr].count == 0 || this->node[nodePtr + 1].count == 0) return; // split failed
		uint leftIdx = nodePtr;
		node.left = leftIdx, node.right = leftIdx + 1, nodePtr += 2;
		node.parax = (node.parax & 0xfffffff8) + axis, node.splitPos = center;
		subdivide( this->node[leftIdx], depth + 1 );
		subdivide( this->node[leftIdx + 1], depth + 1 );
	}
	void partition( KDNode& node, float splitPos, uint axis )
	{
		int N = node.count, first = node.first, last = first + N;
		if (N < 3) last = first + 1; else while (1)
		{
			Bounds& tl = bounds[tlasIdx[first]];
			float3 P = (tl.bmin + tl.bmax) * 0.5f;
			if (P[axis] > splitPos) swap( first, --last ); else first++;
			if (first >= last) break;
		}
		KDNode& left = this->node[nodePtr];
		KDNode& right = this->node[nodePtr + 1];
		left.first = node.first, right.first = last;
		left.count = right.first - left.first;
		left.parax = right.parax = (((uint)(&node - this->node)) << 3) + 7;
		right.count = N - left.count;
	}
	void add( uint idx )
	{
		// capture bounds of new node
		bounds[idx].bmin = tlas[idx].aabbMin, bounds[idx].w0 = 0;
		bounds[idx].bmax = tlas[idx].aabbMax, bounds[idx].w1 = 0;
		// create an index for the new node
		Bounds& newTLAS = bounds[idx];
		float3 C = (newTLAS.bmin + newTLAS.bmax) * 0.5f;
		tlasIdx[tlasCount++] = idx;
		// claim a new KDNode for the tlas and make it a leaf
		uint leafIdx, intIdx, nidx;
		KDNode& leafNode = node[leafIdx = freed[0]];
		leaf[idx] = leafIdx;
		leafNode.first = tlasCount - 1, leafNode.count = 1;
		leafNode.bmin = leafNode.bmax = C;
		leafNode.minSize = 0.5f* (newTLAS.bmax - newTLAS.bmin);
		// we'll also need a new interior node
		intIdx = freed[1];
		// see where we should insert it
		float3 P = (newTLAS.bmin + newTLAS.bmax) * 0.5f;
		KDNode* n = &node[nidx = 0];
		while (1) if (n->isLeaf())
		{
			float3 Pn;
			if (nidx == 0) // special case: root is leaf ==> tree consists of only one node
			{
				node[intIdx] = node[0];		// interior node slot is overwritten with old root (now sibling)
				node[intIdx].parax &= 7;	// sibling's parent is the root node
				node[leafIdx].parax = 7;	// new node's parent is the root node
				// 'split' the new KDNode over the greatest axis of separation
				Pn = (node[intIdx].bmin + node[intIdx].bmax) * 0.5f;
				// and finally, redirect leaf entries for old root
				for (uint j = 0; j < node[intIdx].count; j++)
					leaf[tlasIdx[node[intIdx].first + j]] = intIdx;
				// put the new leaf and n in the correct fields
				nidx = intIdx, intIdx = 0, node[intIdx].parax = 0;
			}
			else
			{
				// replace one child of the parent by the new interior node
				KDNode& parent = node[n->parax >> 3];
				if (parent.left == nidx) parent.left = intIdx; else parent.right = intIdx;
				// rewire parent pointers
				node[intIdx].parax = n->parax & 0xfffffff8;
				n->parax = leafNode.parax = (intIdx << 3) + 7;
				// 'split' the new KDNode over the greatest axis of separation
				Pn = (n->bmin + n->bmax) * 0.5f;
			}
			// put the new leaf and n in the correct fields
			uint axis = dominantAxis( P - Pn );
			node[intIdx].parax += axis;
			node[intIdx].splitPos = ((Pn + P) * 0.5f)[axis];
			if (P[axis] < node[intIdx].splitPos)
				node[intIdx].left = leafIdx, node[intIdx].right = nidx;
			else
				node[intIdx].right = leafIdx, node[intIdx].left = nidx;
			break;
		}
		else // traverse
			n = &node[nidx = ((P[n->parax & 7] < n->splitPos) ? n->left : n->right)];
		// refit
		recurseRefit( leaf[idx] );
	}
	void removeLeaf( uint idx )
	{
		// determine which node to delete for tlas[idx]: must be a leaf
		uint toDelete = leaf[idx]; // note: no need to clear the leaf[idx] entry
		if (node[toDelete].count > 1) // special case: multiple TLASes in one node, rare
		{
			KDNode& n = node[toDelete];
			for (uint j = 0; j < n.count; j++) if (tlasIdx[n.first + j] == idx)
				tlasIdx[n.first + j] = tlasIdx[n.first + n.count-- - 1];
			freed[0] = nodePtr++, freed[1] = nodePtr++;
			return;
		}
		uint parentIdx = node[toDelete].parax >> 3;
		KDNode& parent = node[parentIdx];
		uint sibling = parent.left == toDelete ? parent.right : parent.left;
		node[sibling].parax = (parent.parax & 0xfffffff8) + (node[sibling].parax & 7);
		parent = node[sibling]; // by value, but rather elegant
		if (parent.isLeaf()) // redirect leaf entries if the sibling is a leaf
			for (uint j = 0; j < parent.count; j++)
				leaf[tlasIdx[parent.first + j]] = parentIdx;
		else // make sure child nodes point to the new index
			node[parent.left].parax = (parentIdx << 3) + (node[parent.left].parax & 7),
			node[parent.right].parax = (parentIdx << 3) + (node[parent.right].parax & 7);
		freed[0] = sibling, freed[1] = toDelete;
	}
	int FindNearest( const uint A, uint& startB, float& startSA )
	{
		// keep all hot data together
		__declspec(align(64)) struct TravState
		{
			__m128 Pa4, tlasAbmin4, tlasAbmax4;
			uint n, stackPtr, bestB;
			float smallestSA; // exactly one cacheline
		} state;
		uint stack[60];
		uint& n = state.n, & stackPtr = state.stackPtr, & bestB = state.bestB;
		float& smallestSA = state.smallestSA;
		n = 0, stackPtr = 0, smallestSA = startSA, bestB = startB;
		// gather data for node A
		__m128& tlasAbmin4 = state.tlasAbmin4;
		__m128& tlasAbmax4 = state.tlasAbmax4;
		tlasAbmin4 = bounds[A].bmin4, tlasAbmax4 = bounds[A].bmax4;
		__m128& Pa4 = state.Pa4;
		Pa4 = _mm_mul_ps( _mm_set_ps1( 0.5f ), _mm_add_ps( tlasAbmin4, tlasAbmax4 ) );
		const __m128 half4 = _mm_set_ps1( 0.5f );
		const __m128 extentA4 = _mm_sub_ps( tlasAbmax4, tlasAbmin4 );
		const __m128 halfExtentA4 = _mm_mul_ps( half4, _mm_sub_ps( tlasAbmax4, tlasAbmin4 ) );
		// walk the tree
		while (1)
		{
			while (1)
			{
				if (node[n].isLeaf())
				{
					// loop over the BLASes stored in this leaf
					for (uint i = 0; i < node[n].count; i++)
					{
						uint B = tlasIdx[node[n].first + i];
						if (B == A) continue;
						const __m128 size4 = _mm_sub_ps( _mm_max_ps( tlasAbmax4, bounds[B].bmax4 ), _mm_min_ps( tlasAbmax4, bounds[B].bmin4 ) );
						const float SA = size4.m128_f32[0] * size4.m128_f32[1] + size4.m128_f32[1] * size4.m128_f32[2] +
							size4.m128_f32[2] * size4.m128_f32[0];
						if (SA < smallestSA) smallestSA = SA, bestB = B;
					}
					break;
				}
				// consider recursing into branches, sorted by distance
				uint t, nearNode = node[n].left, farNode = node[n].right;
				if (Pa4.m128_f32[node[n].parax & 7] > node[n].splitPos) t = nearNode, nearNode = farNode, farNode = t;
				const __m128 v0a = _mm_max_ps( _mm_sub_ps( node[nearNode].bmin4, Pa4 ), _mm_sub_ps( Pa4, node[nearNode].bmax4 ) );
				const __m128 v0b = _mm_max_ps( _mm_sub_ps( node[farNode].bmin4, Pa4 ), _mm_sub_ps( Pa4, node[farNode].bmax4 ) );
				const __m128 d4a = _mm_max_ps( extentA4, _mm_sub_ps( v0a, _mm_add_ps( node[nearNode].minSize4, halfExtentA4 ) ) );
				const __m128 d4b = _mm_max_ps( extentA4, _mm_sub_ps( v0b, _mm_add_ps( node[farNode].minSize4, halfExtentA4 ) ) );
				const float sa1 = d4a.m128_f32[0] * d4a.m128_f32[1] + d4a.m128_f32[1] * d4a.m128_f32[2] + d4a.m128_f32[2] * d4a.m128_f32[0];
				const float sa2 = d4b.m128_f32[0] * d4b.m128_f32[1] + d4b.m128_f32[1] * d4b.m128_f32[2] + d4b.m128_f32[2] * d4b.m128_f32[0];
				const float diff1 = sa1 - smallestSA, diff2 = sa2 - smallestSA;
				const uint visit = (*(uint*)&diff1 >> 31) * 2 + (*(uint*)&diff2 >> 31);
				if (!visit) break;
				if (visit == 3) stack[stackPtr++] = farNode, n = nearNode;
				else if (visit == 2) n = nearNode; else n = farNode;
			}
			if (stackPtr == 0) break;
			n = stack[--stackPtr];
		}
		// all done; return best match
		startB = bestB;
		startSA = smallestSA;
		return bestB;
	}
	// data
	KDNode* node = 0;
	TLASNode* tlas = 0;
	Bounds* bounds = 0;
	uint* leaf = 0, * tlasIdx = 0, nodePtr = 1, tlasCount = 0, blasCount = 0, freed[2] = { 0, 0 };
};

// top-level BVH class
class TLAS
{
public:
	TLAS() = default;
	TLAS( BVHInstance* bvhList, int N );
	void Build();
	void BuildQuick();
	void Intersect( Ray& ray );
private:
	int FindBestMatch( int N, int A );
public:
	TLASNode* tlasNode = 0;
	BVHInstance* blas = 0;
	uint nodesUsed, blasCount;
	uint* nodeIdx = 0;
	KDTree* kdtree = 0;
};

} // namespace Tmpl8

// EOF