///////////////////////////////////////////////////////////////////////////////
// Production code of Cuckoo Breeding Ground (CBG) hash table.
// See "research_cuckoo_cbg.md" for scientific paper explaining the options.
///////////////////////////////////////////////////////////////////////////////
//
// Written by Alain Espinosa <alainesp at gmail.com> in 2018 and placed
// under the MIT license (see LICENSE file for a full definition).
//
///////////////////////////////////////////////////////////////////////////////
//
// CBG version 0.1 (consider alpha version).
// Common operations are implemented but many, particularly special ones, are
// not. Many optimizations opportunities remains unexploited. This is intended as
// an early prototype for early adopters.
//
// Tested on Windows 64 bits with VC++ 2017.
// Easy to support 32 bits, Linux and other compilers, but currently not done.
// Comments and PR are welcomed.
//
///////////////////////////////////////////////////////////////////////////////
// Code Architecture
//-----------------------------------------------------------------------------
//
// Class 'cbg::cbg_internal::CBG_IMPL' contains the main algorithm with common
// operations. It doesn't depends in any particular data layout. Class
// 'cbg::cbg_internal::CBG_MAP_IMPL' only add mapping operations.
//
// Data management is provided by the DATA and METADATA template parameters.
// Currently we provide 3 different layouts:
//
// - "Struct of Arrays" (SoA): Metadata overhead of 2 bytes. Each metadata, 
//    keys and values are on a different array. The fastest for negative 
//    queries.
//
// - "Array of Structs" (AoS): Metadata overhead of one byte. Each metadata, 
//    keys and values are in the same array each one after the other. Heavy use
//    of unaligned memory access. The fastest for positive queries.
//
// - "Array of Blocks" (AoB): Metadata overhead of one byte. Each metadata, 
//    keys and values are in the same array on blocks. Don't use unaligned
//    memory access but performance suffers. Intended for positive queries.
///////////////////////////////////////////////////////////////////////////////
// NUM_ELEMS_BUCKET parameter selection:
//
// - Fastest possible : NUM_ELEMS_BUCKET=2 with load_factor < 50%
// - No memory waste  : NUM_ELEMS_BUCKET=4 with load_factor > 95%
// - Balanced approach: NUM_ELEMS_BUCKET=3 with 50% < load_factor < 95%
///////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <tuple>

#if defined(_MSC_VER) && defined (_WIN64)
#include <intrin.h>// should be part of all recent Visual Studio
#pragma intrinsic(_umul128)
#endif // defined(_MSC_VER) && defined (_WIN64)

namespace cbg
{
// Internal implementations
namespace cbg_internal
{
///////////////////////////////////////////////////////////////////////////////
// Data layout is "Struct of Arrays"
///////////////////////////////////////////////////////////////////////////////
// Metadata layout
struct MetadataLayout_SoA
{
	uint16_t* metadata;

	MetadataLayout_SoA() noexcept : metadata(nullptr)
	{}
	MetadataLayout_SoA(size_t num_bins) noexcept
	{
		metadata = (uint16_t*)malloc(num_bins * sizeof(uint16_t));
		memset(metadata, 0, num_bins * sizeof(uint16_t));
	}
	~MetadataLayout_SoA() noexcept
	{
		free(metadata);
		metadata = nullptr;
	}
	__forceinline void Clear(size_t initial_pos, size_t size_in_bins) noexcept
	{
		memset(metadata + initial_pos, 0, size_in_bins * sizeof(uint16_t));
	}
	__forceinline void ReallocMetadata(size_t new_num_bins) noexcept
	{
		metadata = (uint16_t*)realloc(metadata, new_num_bins * sizeof(uint16_t));
	}

	/////////////////////////////////////////////////////////////////////
	// Metadata coded utilities
	/////////////////////////////////////////////////////////////////////
	//
	// Unlucky  Bucket is   Element        
	// bucket   Reversed    Distance     Labels
	// -------  ----------  --------    -------- 
	//   |          |       |      |    |      |
	//   b7         b6      b5 b4 b3    b2 b1 b0
	// 0b00'000'000
	__forceinline uint16_t at(size_t pos) const noexcept
	{
		return metadata[pos];
	}
	__forceinline uint16_t Get_Label(size_t pos) const noexcept
	{
		return metadata[pos] & 0b00'000'111u;
	}
	__forceinline bool Is_Empty(size_t pos) const noexcept
	{
		return Get_Label(pos) == 0;
	}
	__forceinline void Set_Empty(size_t pos) noexcept
	{
		metadata[pos] &= 0b11'000'000;
	}
	__forceinline uint16_t Get_Hash(size_t pos) const noexcept
	{
		return metadata[pos] & 0xFF00u;
	}
	__forceinline void Update_Bin_At(size_t pos, size_t distance_to_base, bool is_reverse_item, uint_fast16_t label, size_t hash) noexcept
	{
		metadata[pos] = uint16_t((hash & 0xFF00) | (metadata[pos] & 0b11'000'000) | (is_reverse_item ? 0b00'100'000 : 0) | (distance_to_base << 3) | label);
	}
	__forceinline bool Is_Item_In_Reverse_Bucket(size_t pos) const noexcept
	{
		return metadata[pos] & 0b00'100'000;
	}
	__forceinline uint16_t Distance_to_Entry_Bin(size_t pos) const noexcept
	{
		return (metadata[pos] >> 3u) & 0b11u;
	}
	/*__forceinline bool Is_Unlucky_Bucket(size_t pos) const noexcept
	{
	return metadata[pos] & 0b10'000'000;
	}*/
	__forceinline void Set_Unlucky_Bucket(size_t pos) noexcept
	{
		metadata[pos] |= 0b10'000'000;
	}
	__forceinline bool Is_Bucket_Reversed(size_t pos) const noexcept
	{
		return metadata[pos] & 0b01'000'000;
	}
	__forceinline void Set_Bucket_Reversed(size_t pos) noexcept
	{
		metadata[pos] |= 0b01'000'000;
	}
};
// Data layouts
template<class KEY, class HASHER> struct KeyLayout_SoA : public HASHER, public MetadataLayout_SoA
{
	KEY* keys;

	// Constructors
	KeyLayout_SoA() noexcept : keys(nullptr), HASHER(), MetadataLayout_SoA()
	{}
	KeyLayout_SoA(size_t num_buckets) noexcept : HASHER(), MetadataLayout_SoA(num_buckets)
	{
		keys = (KEY*)malloc(num_buckets * sizeof(KEY));
	}
	~KeyLayout_SoA() noexcept
	{
		free(keys);
		keys = nullptr;
	}

	__forceinline void MoveElem(size_t dest, size_t orig) noexcept
	{
		keys[dest] = keys[orig];
	}
	__forceinline void SaveElem(size_t pos, const KEY& elem) noexcept
	{
		keys[pos] = elem;
	}

	__forceinline const KEY& GetKey(size_t pos) const noexcept
	{
		return keys[pos];
	}
	__forceinline KEY GetElem(size_t pos) const noexcept
	{
		return keys[pos];
	}
	__forceinline KEY* GetValue(size_t pos) const noexcept
	{
		return keys + pos;
	}

	__forceinline void ReallocElems(size_t new_num_buckets) noexcept
	{
		keys = (KEY*)realloc(keys, new_num_buckets * sizeof(KEY));
	}
	/////////////////////////////////////////////////////////////////////
	// Utilities
	/////////////////////////////////////////////////////////////////////
	__forceinline std::pair<size_t, size_t> hash_elem(const KEY& elem) const noexcept
	{
		return HASHER::operator()(elem);
	}
};
template<class KEY, class T, class HASHER> struct MapLayout_SoA : public HASHER, public MetadataLayout_SoA
{
	using INSERT_TYPE = std::pair<KEY, T>;

	KEY* keys;
	T* data;

	// Constructors
	MapLayout_SoA() noexcept : keys(nullptr), data(nullptr), HASHER(), MetadataLayout_SoA()
	{}
	MapLayout_SoA(size_t num_buckets) noexcept : HASHER(), MetadataLayout_SoA(num_buckets)
	{
		keys = (KEY*)malloc(num_buckets * sizeof(KEY));
		data = (T*)malloc(num_buckets * sizeof(T));
	}
	~MapLayout_SoA() noexcept
	{
		free(keys);
		free(data);
		keys = nullptr;
		data = nullptr;
	}

	__forceinline void MoveElem(size_t dest, size_t orig) noexcept
	{
		keys[dest] = keys[orig];
		data[dest] = data[orig];
	}
	__forceinline void SaveElem(size_t pos, const INSERT_TYPE& elem) noexcept
	{
		keys[pos] = elem.first;
		data[pos] = elem.second;
	}

	__forceinline const KEY& GetKey(size_t pos) const noexcept
	{
		return keys[pos];
	}
	__forceinline INSERT_TYPE GetElem(size_t pos) const noexcept
	{
		return std::make_pair(keys[pos], data[pos]);
	}
	__forceinline T* GetValue(size_t pos) const noexcept
	{
		return data + pos;
	}

	__forceinline void ReallocElems(size_t new_num_buckets) noexcept
	{
		keys = (KEY*)realloc(keys, new_num_buckets * sizeof(KEY));
		data = (T*)realloc(data, new_num_buckets * sizeof(T));
	}

	/////////////////////////////////////////////////////////////////////
	// Utilities
	/////////////////////////////////////////////////////////////////////
	__forceinline std::pair<size_t, size_t> hash_elem(const KEY& elem) const noexcept
	{
		return HASHER::operator()(elem);
	}
	__forceinline std::pair<size_t, size_t> hash_elem(const INSERT_TYPE& elem) const noexcept
	{
		return HASHER::operator()(elem.first);
	}
};

///////////////////////////////////////////////////////////////////////////////
// Data layout is "Array of Structs"
///////////////////////////////////////////////////////////////////////////////
// Auxiliary class
template<size_t ELEM_SIZE> struct ElemLayout
{
	uint8_t metadata;
	uint8_t elem[ELEM_SIZE];
};
// Metadata layout
template<size_t ELEM_SIZE> struct MetadataLayout_AoS
{
	ElemLayout<ELEM_SIZE>* all_data;

	MetadataLayout_AoS() noexcept : all_data(nullptr)
	{
	}
	MetadataLayout_AoS(size_t num_bins) noexcept
	{
		all_data = (ElemLayout<ELEM_SIZE>*)malloc(num_bins * sizeof(ElemLayout<ELEM_SIZE>));
		for (size_t i = 0; i < num_bins; i++)
			all_data[i].metadata = 0;
	}
	~MetadataLayout_AoS() noexcept
	{
		free(all_data);
		all_data = nullptr;
	}
	__forceinline void Clear(size_t initial_pos, size_t size_in_bins) noexcept
	{
		for (size_t i = initial_pos; i < (initial_pos + size_in_bins); i++)
			all_data[i].metadata = 0;
	}
	__forceinline void ReallocMetadata(size_t new_num_bins) noexcept
	{
		all_data = (ElemLayout<ELEM_SIZE>*)realloc(all_data, new_num_bins * sizeof(ElemLayout<ELEM_SIZE>));
	}

	/////////////////////////////////////////////////////////////////////
	// Metadata coded utilities
	/////////////////////////////////////////////////////////////////////
	//
	// Unlucky  Bucket is   Element        
	// bucket   Reversed    Distance     Labels
	// -------  ----------  --------    -------- 
	//   |          |       |      |    |      |
	//   b7         b6      b5 b4 b3    b2 b1 b0
	// 0b00'000'000
	__forceinline uint8_t at(size_t pos) const noexcept
	{
		return all_data[pos].metadata;
	}
	__forceinline uint8_t Get_Label(size_t pos) const noexcept
	{
		return all_data[pos].metadata & 0b00'000'111;
	}
	__forceinline bool Is_Empty(size_t pos) const noexcept
	{
		return Get_Label(pos) == 0;
	}
	__forceinline void Set_Empty(size_t pos) noexcept
	{
		all_data[pos].metadata &= 0b11'000'000;
	}
	__forceinline uint16_t Get_Hash(size_t pos) const noexcept
	{
		return 0;
	}
	__forceinline void Update_Bin_At(size_t pos, size_t distance_to_base, bool is_reverse_item, uint_fast16_t label, size_t hash) noexcept
	{
		all_data[pos].metadata = (all_data[pos].metadata & 0b11'000'000) | (is_reverse_item ? 0b00'100'000 : 0) | (uint8_t(distance_to_base) << 3) | label;
	}
	__forceinline bool Is_Item_In_Reverse_Bucket(size_t pos) const noexcept
	{
		return all_data[pos].metadata & 0b00'100'000;
	}
	__forceinline uint16_t Distance_to_Entry_Bin(size_t pos) const noexcept
	{
		return (all_data[pos].metadata >> 3) & 0b11;
	}
	/*__forceinline bool Is_Unlucky_Bucket(size_t pos) const noexcept
	{
	return all_data[pos].metadata & 0b10'000'000;
	}*/
	__forceinline void Set_Unlucky_Bucket(size_t pos) noexcept
	{
		all_data[pos].metadata |= 0b10'000'000;
	}
	__forceinline bool Is_Bucket_Reversed(size_t pos) const noexcept
	{
		return all_data[pos].metadata & 0b01'000'000;
	}
	__forceinline void Set_Bucket_Reversed(size_t pos) noexcept
	{
		all_data[pos].metadata |= 0b01'000'000;
	}
};
// Data layouts
template<class KEY, class HASHER> struct KeyLayout_AoS : public HASHER, public MetadataLayout_AoS<sizeof(KEY)>
{
	// Constructors
	KeyLayout_AoS() noexcept : MetadataLayout_AoS<sizeof(KEY)>()
	{}
	KeyLayout_AoS(size_t num_bins) noexcept : MetadataLayout_AoS<sizeof(KEY)>(num_bins)
	{}

	__forceinline void MoveElem(size_t dest, size_t orig) noexcept
	{
		memcpy(all_data[dest].elem, all_data[orig].elem, sizeof(KEY));
	}
	__forceinline void SaveElem(size_t pos, const KEY& elem) noexcept
	{
		memcpy(all_data[pos].elem, &elem, sizeof(KEY));
	}

	__forceinline const KEY& GetKey(size_t pos) const noexcept
	{
		return *((KEY*)(all_data[pos].elem));
	}
	__forceinline KEY GetElem(size_t pos) const noexcept
	{
		return *((KEY*)(all_data[pos].elem));
	}
	__forceinline KEY* GetValue(size_t pos) const noexcept
	{
		return (KEY*)(all_data[pos].elem);
	}

	__forceinline void ReallocElems(size_t new_num_buckets) noexcept
	{
		// Nothing
	}
	/////////////////////////////////////////////////////////////////////
	// Utilities
	/////////////////////////////////////////////////////////////////////
	__forceinline std::pair<size_t, size_t> hash_elem(const KEY& elem) const noexcept
	{
		return HASHER::operator()(elem);
	}
};
template<class KEY, class T, class HASHER> struct MapLayout_AoS : public HASHER, public MetadataLayout_AoS<sizeof(KEY) + sizeof(T)>
{
	using INSERT_TYPE = std::pair<KEY, T>;

	// Constructors
	MapLayout_AoS() noexcept : MetadataLayout_AoS<sizeof(KEY) + sizeof(T)>()
	{}
	MapLayout_AoS(size_t num_buckets) noexcept : MetadataLayout_AoS<sizeof(KEY) + sizeof(T)>(num_buckets)
	{}

	__forceinline void MoveElem(size_t dest, size_t orig) noexcept
	{
		memcpy(all_data[dest].elem, all_data[orig].elem, sizeof(KEY) + sizeof(T));
	}
	__forceinline void SaveElem(size_t pos, const INSERT_TYPE& elem) noexcept
	{
		memcpy(all_data[pos].elem, &elem.first, sizeof(KEY));
		memcpy(all_data[pos].elem + sizeof(KEY), &elem.second, sizeof(T));
	}

	__forceinline const KEY& GetKey(size_t pos) const noexcept
	{
		return *((KEY*)(all_data[pos].elem));
	}
	__forceinline INSERT_TYPE GetElem(size_t pos) const noexcept
	{
		return std::make_pair(*((KEY*)(all_data[pos].elem)), *((T*)(all_data[pos].elem + sizeof(KEY))));
	}
	__forceinline T* GetValue(size_t pos) const noexcept
	{
		return (T*)(all_data[pos].elem + sizeof(KEY));
	}

	__forceinline void ReallocElems(size_t new_num_buckets) noexcept
	{
		// Nothing
	}

	/////////////////////////////////////////////////////////////////////
	// Utilities
	/////////////////////////////////////////////////////////////////////
	__forceinline std::pair<size_t, size_t> hash_elem(const KEY& elem) const noexcept
	{
		return HASHER::operator()(elem);
	}
	__forceinline std::pair<size_t, size_t> hash_elem(const INSERT_TYPE& elem) const noexcept
	{
		return HASHER::operator()(elem.first);
	}
};

///////////////////////////////////////////////////////////////////////////////
// Data layout is "Array of Blocks"
///////////////////////////////////////////////////////////////////////////////
// Auxiliary classes
template<class T> struct BlockKey
{
	static constexpr size_t BLOCK_SIZE = alignof(T);

	uint8_t metadata[BLOCK_SIZE];
	T data[BLOCK_SIZE];
};
template<class KEY, class T> struct MaxAlignOf
{
	static constexpr size_t BLOCK_SIZE = std::max(alignof(KEY), alignof(T));
};
template<class KEY, class T> struct BlockMap
{
	uint8_t metadata[MaxAlignOf<KEY, T>::BLOCK_SIZE];
	KEY keys[MaxAlignOf<KEY, T>::BLOCK_SIZE];
	T data[MaxAlignOf<KEY, T>::BLOCK_SIZE];
};
// Metadata layout
template<size_t BLOCK_SIZE, class BLOCK> struct MetadataLayout_AoB
{
	BLOCK* all_data;

	MetadataLayout_AoB() noexcept : all_data(nullptr)
	{}
	MetadataLayout_AoB(size_t num_bins) noexcept
	{
		num_bins = (num_bins + BLOCK_SIZE - 1) / BLOCK_SIZE;

		all_data = (BLOCK*)malloc(num_bins * sizeof(BLOCK));
		for (size_t i = 0; i < num_bins; i++)
			for (size_t j = 0; j < BLOCK_SIZE; j++)
				all_data[i].metadata[j] = 0;
	}
	~MetadataLayout_AoB() noexcept
	{
		free(all_data);
		all_data = nullptr;
	}
	__forceinline void Clear(size_t initial_pos, size_t size_in_bins) noexcept
	{
		for (size_t i = initial_pos; i < (initial_pos + size_in_bins); i++)
			all_data[i / BLOCK_SIZE].metadata[i % BLOCK_SIZE] = 0;
	}
	__forceinline void ReallocMetadata(size_t new_num_bins) noexcept
	{
		new_num_bins = (new_num_bins + BLOCK_SIZE - 1) / BLOCK_SIZE;
		all_data = (BLOCK*)realloc(all_data, new_num_bins * sizeof(BLOCK));
	}

	/////////////////////////////////////////////////////////////////////
	// Metadata coded utilities
	/////////////////////////////////////////////////////////////////////
	//
	// Unlucky  Bucket is   Element        
	// bucket   Reversed    Distance     Labels
	// -------  ----------  --------    -------- 
	//   |          |       |      |    |      |
	//   b7         b6      b5 b4 b3    b2 b1 b0
	// 0b00'000'000
	__forceinline uint8_t at(size_t pos) const noexcept
	{
		return all_data[pos / BLOCK_SIZE].metadata[pos%BLOCK_SIZE];
	}
	__forceinline uint8_t Get_Label(size_t pos) const noexcept
	{
		return at(pos) & 0b00'000'111u;
	}
	__forceinline bool Is_Empty(size_t pos) const noexcept
	{
		return Get_Label(pos) == 0;
	}
	__forceinline void Set_Empty(size_t pos) noexcept
	{
		all_data[pos / BLOCK_SIZE].metadata[pos%BLOCK_SIZE] &= 0b11'000'000;
	}
	__forceinline uint16_t Get_Hash(size_t /*pos*/) const noexcept
	{
		return 0;
	}
	__forceinline void Update_Bin_At(size_t pos, size_t distance_to_base, bool is_reverse_item, uint_fast16_t label, size_t /*hash*/) noexcept
	{
		all_data[pos / BLOCK_SIZE].metadata[pos%BLOCK_SIZE] = uint8_t((at(pos) & 0b11'000'000) | (is_reverse_item ? 0b00'100'000 : 0) | (distance_to_base << 3) | label);
	}
	__forceinline bool Is_Item_In_Reverse_Bucket(size_t pos) const noexcept
	{
		return at(pos) & 0b00'100'000;
	}
	__forceinline uint16_t Distance_to_Entry_Bin(size_t pos) const noexcept
	{
		return (at(pos) >> 3u) & 0b11u;
	}
	/*__forceinline bool Is_Unlucky_Bucket(size_t pos) const noexcept
	{
	return at(pos) & 0b10'000'000;
	}*/
	__forceinline void Set_Unlucky_Bucket(size_t pos) noexcept
	{
		all_data[pos / BLOCK_SIZE].metadata[pos%BLOCK_SIZE] |= 0b10'000'000;
	}
	__forceinline bool Is_Bucket_Reversed(size_t pos) const noexcept
	{
		return at(pos) & 0b01'000'000;
	}
	__forceinline void Set_Bucket_Reversed(size_t pos) noexcept
	{
		all_data[pos / BLOCK_SIZE].metadata[pos%BLOCK_SIZE] |= 0b01'000'000;
	}
};
// Data layouts
template<class KEY, class HASHER> struct KeyLayout_AoB : public HASHER, public MetadataLayout_AoB<alignof(KEY), BlockKey<KEY>>
{
	static constexpr size_t BLOCK_SIZE = alignof(KEY);

	// Constructors
	KeyLayout_AoB() noexcept : MetadataLayout_AoB()
	{}
	KeyLayout_AoB(size_t num_bins) noexcept : MetadataLayout_AoB(num_bins)
	{}

	__forceinline void MoveElem(size_t dest, size_t orig) noexcept
	{
		all_data[dest / BLOCK_SIZE].data[dest%BLOCK_SIZE] = all_data[orig / BLOCK_SIZE].data[orig%BLOCK_SIZE];
	}
	__forceinline void SaveElem(size_t pos, const KEY& elem) noexcept
	{
		all_data[pos / BLOCK_SIZE].data[pos%BLOCK_SIZE] = elem;
	}

	__forceinline const KEY& GetKey(size_t pos) const noexcept
	{
		return all_data[pos / BLOCK_SIZE].data[pos%BLOCK_SIZE];
	}
	__forceinline KEY GetElem(size_t pos) const noexcept
	{
		return all_data[pos / BLOCK_SIZE].data[pos%BLOCK_SIZE];
	}
	__forceinline KEY* GetValue(size_t pos) const noexcept
	{
		return all_data[pos / BLOCK_SIZE].data + pos%BLOCK_SIZE;
	}

	__forceinline void ReallocElems(size_t new_num_buckets) noexcept
	{
		// Nothing
	}
	/////////////////////////////////////////////////////////////////////
	// Utilities
	/////////////////////////////////////////////////////////////////////
	__forceinline std::pair<size_t, size_t> hash_elem(const KEY& elem) const noexcept
	{
		return HASHER::operator()(elem);
	}
};
template<class KEY, class T, class HASHER> struct MapLayout_AoB : public HASHER, public MetadataLayout_AoB<MaxAlignOf<KEY, T>::BLOCK_SIZE, BlockMap<KEY, T>>
{
	using INSERT_TYPE = std::pair<KEY, T>;
	static constexpr size_t BLOCK_SIZE = std::max(alignof(KEY), alignof(T));

	// Constructors
	MapLayout_AoB() noexcept : MetadataLayout_AoB()
	{}
	MapLayout_AoB(size_t num_buckets) noexcept : MetadataLayout_AoB(num_buckets)
	{}

	__forceinline void MoveElem(size_t dest, size_t orig) noexcept
	{
		all_data[dest / BLOCK_SIZE].keys[dest%BLOCK_SIZE] = all_data[orig / BLOCK_SIZE].keys[orig%BLOCK_SIZE];
		all_data[dest / BLOCK_SIZE].data[dest%BLOCK_SIZE] = all_data[orig / BLOCK_SIZE].data[orig%BLOCK_SIZE];
	}
	__forceinline void SaveElem(size_t pos, const INSERT_TYPE& elem) noexcept
	{
		all_data[pos / BLOCK_SIZE].keys[pos%BLOCK_SIZE] = elem.first;
		all_data[pos / BLOCK_SIZE].data[pos%BLOCK_SIZE] = elem.second;
	}

	__forceinline const KEY& GetKey(size_t pos) const noexcept
	{
		return all_data[pos / BLOCK_SIZE].keys[pos%BLOCK_SIZE];
	}
	__forceinline INSERT_TYPE GetElem(size_t pos) const noexcept
	{
		return std::make_pair(all_data[pos / BLOCK_SIZE].keys[pos%BLOCK_SIZE], all_data[pos / BLOCK_SIZE].data[pos%BLOCK_SIZE]);
	}
	__forceinline T* GetValue(size_t pos) const noexcept
	{
		return all_data[pos / BLOCK_SIZE].data + pos%BLOCK_SIZE;
	}

	__forceinline void ReallocElems(size_t /*new_num_buckets*/) noexcept
	{
		// Nothing
	}

	/////////////////////////////////////////////////////////////////////
	// Utilities
	/////////////////////////////////////////////////////////////////////
	__forceinline std::pair<size_t, size_t> hash_elem(const KEY& elem) const noexcept
	{
		return HASHER::operator()(elem);
	}
	__forceinline std::pair<size_t, size_t> hash_elem(const INSERT_TYPE& elem) const noexcept
	{
		return HASHER::operator()(elem.first);
	}
};

///////////////////////////////////////////////////////////////////////////////
// Basic implementation of CBG.
//
// TODO: Iterator, destructor call when removed, Memory_Allocator
///////////////////////////////////////////////////////////////////////////////
template<size_t NUM_ELEMS_BUCKET, class INSERT_TYPE, class KEY_TYPE, class VALUE_TYPE, class EQ, class DATA, class METADATA, bool IS_SoA> class CBG_IMPL : private EQ, protected DATA
{
protected:
	// Pointers -> found in DATA and METADATA through inheritance
	// Counters
	size_t num_elems;
	size_t num_buckets;
	// Parameters
	float _max_load_factor = 0.9001f;// 90% -> When this load factor is reached the table is grow
	float _grow_factor = 1.1f;// 10% -> How much to grow the table
								// Constants
	static constexpr uint_fast16_t L_MAX = 7;
	static constexpr size_t MIN_BINS_SIZE = 2 * NUM_ELEMS_BUCKET - 2;
	static_assert(NUM_ELEMS_BUCKET >= 2 && NUM_ELEMS_BUCKET <= 4, "To use only 2 bits");

	/////////////////////////////////////////////////////////////////////
	// Utilities
	/////////////////////////////////////////////////////////////////////
	__forceinline bool cmp_elems(size_t pos, const KEY_TYPE& r) const noexcept
	{
		return EQ::operator()(DATA::GetKey(pos), r);
	}

	/////////////////////////////////////////////////////////////////////
	// Given a value "word", produces an integer in [0,p) without division.
	// The function is as fair as possible in the sense that if you iterate
	// through all possible values of "word", then you will generate all
	// possible outputs as uniformly as possible.
	/////////////////////////////////////////////////////////////////////
	static __forceinline uint32_t fastrange32(uint32_t word, uint32_t p)
	{
		return (uint32_t)(((uint64_t)word * (uint64_t)p) >> 32);
	}
	static __forceinline uint64_t fastrange64(uint64_t word, uint64_t p)
	{
#ifdef __SIZEOF_INT128__ // then we know we have a 128-bit int
		return (uint64_t)(((__uint128_t)word * (__uint128_t)p) >> 64);
#elif defined(_MSC_VER) && defined(_WIN64)
		// supported in Visual Studio 2005 and better
		return __umulh(word, p);
#else
		return word % p; // fallback
#endif // __SIZEOF_INT128__
	}
	static __forceinline size_t fastrange(size_t word, size_t p) {
#if (SIZE_MAX == UINT32_MAX)
		return (size_t)fastrange32(word, p);
#else // assume 64-bit
		return (size_t)fastrange64(word, p);
#endif // SIZE_MAX == UINT32_MAX
	}

	/////////////////////////////////////////////////////////////////////
	// Insertion algorithm utilities
	/////////////////////////////////////////////////////////////////////
	std::pair<uint16_t, size_t> Calculate_Minimum(size_t bucket_pos) const noexcept
	{
		uint16_t minimum = METADATA::Get_Label(bucket_pos);
		size_t pos = bucket_pos;

		for (size_t i = 1; minimum && i < NUM_ELEMS_BUCKET; i++)
		{
			uint16_t label_value = METADATA::Get_Label(bucket_pos + i);
			if (minimum > label_value)
			{
				minimum = label_value;
				pos = bucket_pos + i;
			}
		}

		return std::make_pair(minimum, pos);
	}
	__forceinline size_t Belong_to_Bucket(size_t elem_pos) const noexcept
	{
		if (METADATA::Is_Empty(elem_pos))
			return SIZE_MAX;

		return elem_pos + (METADATA::Is_Item_In_Reverse_Bucket(elem_pos) ? NUM_ELEMS_BUCKET - 1 : 0) - METADATA::Distance_to_Entry_Bin(elem_pos);
	}
	size_t Count_Empty(size_t pos) const noexcept
	{
		size_t count = METADATA::Is_Empty(pos) ? 1u : 0u;

		for (size_t i = 1; i < NUM_ELEMS_BUCKET; i++)
			if (METADATA::Is_Empty(pos + i))
				count++;

		return count;
	}
	size_t Count_Elems_In_Bucket_Non_Reversed(size_t bucket_pos) const noexcept
	{
		size_t count = 0;
		if (!METADATA::Is_Item_In_Reverse_Bucket(bucket_pos) && 0 == METADATA::Distance_to_Entry_Bin(bucket_pos))
			count = 1;

		for (size_t i = 1; i < NUM_ELEMS_BUCKET; i++)
		{
			size_t pos = bucket_pos + i;

			if (!METADATA::Is_Item_In_Reverse_Bucket(pos) && i == METADATA::Distance_to_Entry_Bin(pos))
				count++;
		}

		return count;
	}
	void Reverse_Bucket(size_t bucket_pos) noexcept
	{
		METADATA::Set_Bucket_Reversed(bucket_pos);

		size_t j = NUM_ELEMS_BUCKET - 1;
		for (size_t i = 0; i < NUM_ELEMS_BUCKET; i++)
			if (Belong_to_Bucket(bucket_pos + i) == bucket_pos)// Elems belong to our bucket
			{
				for (; !METADATA::Is_Empty(bucket_pos - j); j--)
				{
				}// Find empty space

				METADATA::Update_Bin_At(bucket_pos - j, NUM_ELEMS_BUCKET - 1 - j, true, METADATA::Get_Label(bucket_pos + i), METADATA::Get_Hash(bucket_pos + i));
				METADATA::Set_Empty(bucket_pos + i);
				DATA::MoveElem(bucket_pos - j, bucket_pos + i);
			}
	}
	size_t Find_Empty_Pos_Hopscotch(size_t bucket_pos, size_t bucket_init) noexcept
	{
		size_t empty_pos = SIZE_MAX;

		//////////////////////////////////////////////////////////////////
		// Then try to reverse the bucket
		//////////////////////////////////////////////////////////////////
		if (!METADATA::Is_Bucket_Reversed(bucket_pos) && bucket_pos >= NUM_ELEMS_BUCKET)
		{
			size_t count_empty = Count_Empty(bucket_pos + 1 - NUM_ELEMS_BUCKET);
			if (count_empty)
			{
				size_t count_elems = Count_Elems_In_Bucket_Non_Reversed(bucket_pos);

				if (Belong_to_Bucket(bucket_pos) == bucket_pos)
				{
					if (count_empty > 0)
						count_empty++;
				}

				// TODO: Check this when only one element
				if (count_empty > count_elems)
				{
					Reverse_Bucket(bucket_pos);

					uint16_t min1;
					size_t pos1;
					bucket_init = bucket_pos + (METADATA::Is_Bucket_Reversed(bucket_pos) ? (size_t(1) - NUM_ELEMS_BUCKET) : size_t(0));
					std::tie(min1, pos1) = Calculate_Minimum(bucket_init);
					if (min1 == 0)
						return pos1;
				}
			}
		}

		//////////////////////////////////////////////////////////////////
		// Then try to reverse elems
		//////////////////////////////////////////////////////////////////
		if (bucket_init >= 2 * NUM_ELEMS_BUCKET)
			for (size_t i = 0; i < NUM_ELEMS_BUCKET; i++)
			{
				size_t pos_elem = bucket_init + i;
				if (!METADATA::Is_Item_In_Reverse_Bucket(pos_elem))
				{
					size_t bucket_elem = pos_elem - METADATA::Distance_to_Entry_Bin(pos_elem);

					if (bucket_elem != bucket_pos)
					{
						size_t count_empty = Count_Empty(bucket_elem + 1 - NUM_ELEMS_BUCKET);
						if (count_empty)
						{
							size_t count_elems = Count_Elems_In_Bucket_Non_Reversed(bucket_elem);

							if (Belong_to_Bucket(bucket_elem) == bucket_elem)
							{
								if (count_empty > 0)
									count_empty++;
							}

							// TODO: Check this when only one element
							if (count_empty >= count_elems)
							{
								Reverse_Bucket(bucket_elem);

								uint16_t min1;
								size_t pos1;
								//bucket_init = Calculate_Position_Paged(bucket_pos, (Is_Reversed_Window(bucket_pos) ? (1 - NUM_ELEMS_BUCKET) : 0));
								std::tie(min1, pos1) = Calculate_Minimum(bucket_init);
								if (min1 == 0)
									return pos1;

								break;
							}
						}
					}
				}
			}

		//////////////////////////////////////////////////////////////////
		// Then try to hopscotch for an empty space
		//////////////////////////////////////////////////////////////////
		size_t max_dist_to_move = NUM_ELEMS_BUCKET - 1;
		for (size_t i = 0; i <= max_dist_to_move && (bucket_init + i) < num_buckets; i++)
		{
			if (METADATA::Is_Empty(bucket_init + i))
			{
				// Find element to move
				size_t pos_blank = bucket_init + i;
				while ((pos_blank - bucket_init) >= NUM_ELEMS_BUCKET)
				{
					size_t pos_swap = pos_blank + 1 - NUM_ELEMS_BUCKET;

					for (; (pos_blank - pos_swap) > (NUM_ELEMS_BUCKET - 1 - METADATA::Distance_to_Entry_Bin(pos_swap)); pos_swap++)
					{
					}// TODO: Use a list with the options to not recalculate again

						// Swap elements
					DATA::MoveElem(pos_blank, pos_swap);
					METADATA::Update_Bin_At(pos_blank, METADATA::Distance_to_Entry_Bin(pos_swap) + (pos_blank - pos_swap), METADATA::Is_Item_In_Reverse_Bucket(pos_swap), METADATA::Get_Label(pos_swap), METADATA::Get_Hash(pos_swap));

					pos_blank = pos_swap;
				}

				return pos_blank;
			}
			size_t current_max_move = i + NUM_ELEMS_BUCKET - 1 - METADATA::Distance_to_Entry_Bin(bucket_init + i);
			if (current_max_move > max_dist_to_move)
				max_dist_to_move = current_max_move;
		}

		return empty_pos;
	}

	// Grow table
	void rehash(size_t new_num_buckets) noexcept
	{
		if (new_num_buckets <= num_buckets)
			return;

		std::vector<INSERT_TYPE> secondary_tmp;
		secondary_tmp.reserve(std::max(1ull, num_elems / 8));// reserve 12.5%
		bool need_rehash = true;

		while (need_rehash)
		{
			need_rehash = false;

			size_t old_num_buckets = num_buckets;
			num_buckets = new_num_buckets;
			new_num_buckets += std::max(1ull, new_num_buckets / 128ull);// add 0.8% if fails

																		// Realloc data
			DATA::ReallocElems(num_buckets);
			METADATA::ReallocMetadata(num_buckets);

			// Initialize metadata
			if (old_num_buckets)
				METADATA::Clear(old_num_buckets, num_buckets - old_num_buckets);
			else
				METADATA::Clear(0, num_buckets);
			num_elems = 0;
			for (size_t i = 0; i < (NUM_ELEMS_BUCKET - 1); i++)
				METADATA::Set_Bucket_Reversed(num_buckets - 1 - i);

			// Moves items from old end to new end
			for (size_t i = old_num_buckets - 1; i > 0; i--)
			{
				if (!METADATA::Is_Empty(i))
				{
					size_t hash0, hash1;
					std::tie(hash0, hash1) = DATA::hash_elem(DATA::GetKey(i));
					size_t bucket1_pos_init = fastrange(hash0, num_buckets);
					bucket1_pos_init += METADATA::Is_Bucket_Reversed(bucket1_pos_init) ? (1ull - NUM_ELEMS_BUCKET) : 0;
					bool item_is_moved = false;

					// Try to insert primary
					if (bucket1_pos_init > i)
					{
						uint16_t min1;
						size_t pos1;
						std::tie(min1, pos1) = Calculate_Minimum(bucket1_pos_init);
						if (min1 == 0)
						{
							METADATA::Update_Bin_At(pos1, pos1 - bucket1_pos_init, false, 1, hash1);
							// Put elem
							DATA::MoveElem(pos1, i);
							num_elems++;
							item_is_moved = true;
						}
					}

					// Not moved -> put in temporary list
					if (!item_is_moved)
						secondary_tmp.push_back(DATA::GetElem(i));
				}
				// Clear position
				METADATA::Clear(i, 1);
			}

			// First element
			if (!METADATA::Is_Empty(0))
				secondary_tmp.push_back(DATA::GetElem(0));
			METADATA::Clear(0, 1);

			// Insert other elements
			while (!secondary_tmp.empty() && !need_rehash)
			{
				if (try_insert(secondary_tmp.back()))
					secondary_tmp.pop_back();
				else
					need_rehash = true;
			}
		}
	}
	size_t get_grow_size() const noexcept
	{
		// Last buckets will be reverted, so they need to be outsize the old buckets
		size_t new_num_buckets = std::max(num_buckets + MIN_BINS_SIZE, size_t(num_buckets*_grow_factor));
		if (new_num_buckets < num_buckets)
			new_num_buckets = SIZE_MAX;

		return new_num_buckets;
	}

	// Constructors
	CBG_IMPL() noexcept : num_elems(0), num_buckets(0), EQ(), DATA()
	{}
	CBG_IMPL(size_t expected_num_elems) noexcept : EQ(), DATA(std::max(MIN_BINS_SIZE, expected_num_elems)),
		num_elems(0), num_buckets(std::max(MIN_BINS_SIZE, expected_num_elems))
	{
		for (size_t i = 0; i < (NUM_ELEMS_BUCKET - 1); i++)
			METADATA::Set_Bucket_Reversed(num_buckets - 1 - i);
	}
	~CBG_IMPL() noexcept
	{
		num_elems = 0;
		num_buckets = 0;
	}

	bool try_insert(INSERT_TYPE& elem) noexcept
	{
		while (true)
		{
			size_t hash0, hash1;
			std::tie(hash0, hash1) = DATA::hash_elem(elem);

			// Calculate positions given hash
			size_t bucket1_pos = fastrange(hash0, num_buckets);
			size_t bucket2_pos = fastrange(hash1, num_buckets);

			bool is_reversed_bucket1 = METADATA::Is_Bucket_Reversed(bucket1_pos);
			bool is_reversed_bucket2 = METADATA::Is_Bucket_Reversed(bucket2_pos);
			size_t bucket1_init = bucket1_pos + (is_reversed_bucket1 ? (1ull - NUM_ELEMS_BUCKET) : 0);
			size_t bucket2_init = bucket2_pos + (is_reversed_bucket2 ? (1ull - NUM_ELEMS_BUCKET) : 0);

			// Find minimun label
			uint_fast16_t min1 = METADATA::Get_Label(bucket1_init);
			uint_fast16_t min2 = METADATA::Get_Label(bucket2_init);
			size_t pos1 = bucket1_init;
			size_t pos2 = bucket2_init;
			for (size_t i = 1; i < NUM_ELEMS_BUCKET /*&& (min1 || min2)*/; i++)
			{
				size_t current_pos1 = bucket1_init + i;
				size_t current_pos2 = bucket2_init + i;
				uint_fast16_t label_value1 = METADATA::Get_Label(current_pos1);
				uint_fast16_t label_value2 = METADATA::Get_Label(current_pos2);
				if (min1 > label_value1)
				{
					min1 = label_value1;
					pos1 = current_pos1;
				}
				if (min2 > label_value2)
				{
					min2 = label_value2;
					pos2 = current_pos2;
				}
			}

			//////////////////////////////////////////////////////////////////
			// No secondary added, no unlucky bucket added
			//////////////////////////////////////////////////////////////////
			// First bucket had free space
			if (min1 == 0)
			{
				METADATA::Update_Bin_At(pos1, pos1 - bucket1_init, is_reversed_bucket1, std::min(min2 + 1, L_MAX), hash1);
				// Put elem
				DATA::SaveElem(pos1, elem);
				num_elems++;
				return true;
			}

			size_t empty_pos = Find_Empty_Pos_Hopscotch(bucket1_pos, bucket1_init);
			if (empty_pos != SIZE_MAX)
			{
				is_reversed_bucket1 = METADATA::Is_Bucket_Reversed(bucket1_pos);
				bucket1_init = bucket1_pos + (is_reversed_bucket1 ? (1 - NUM_ELEMS_BUCKET) : 0);
				METADATA::Update_Bin_At(empty_pos, empty_pos - bucket1_init, is_reversed_bucket1, std::min(min2 + 1, L_MAX), hash1);

				// Put elem
				DATA::SaveElem(empty_pos, elem);
				num_elems++;
				return true;
			}

			///////////////////////////////////////////////////////////////////
			// Secondary added, Unlucky bucket added
			//////////////////////////////////////////////////////////////////
			if (min2 == 0)
			{
				METADATA::Set_Unlucky_Bucket(bucket1_pos);
				METADATA::Update_Bin_At(pos2, pos2 - bucket2_init, is_reversed_bucket2, std::min(min1 + 1, L_MAX), hash0);
				// Put elem
				DATA::SaveElem(pos2, elem);
				num_elems++;
				return true;
			}

			if (num_elems * 10 > 9 * num_buckets)// > 90%
			{
				empty_pos = Find_Empty_Pos_Hopscotch(bucket2_pos, bucket2_init);

				if (empty_pos != SIZE_MAX)
				{
					METADATA::Set_Unlucky_Bucket(bucket1_pos);
					is_reversed_bucket2 = METADATA::Is_Bucket_Reversed(bucket2_pos);
					bucket2_init = bucket2_pos + (is_reversed_bucket2 ? (1 - NUM_ELEMS_BUCKET) : 0);
					METADATA::Update_Bin_At(empty_pos, empty_pos - bucket2_init, is_reversed_bucket2, std::min(min1 + 1, L_MAX), hash0);

					// Put elem
					DATA::SaveElem(empty_pos, elem);
					num_elems++;
					return true;
				}
			}

			// Terminating condition
			if (std::min(min1, min2) >= L_MAX)
				return false;

			if (min1 <= min2)// Selected pos in first bucket
			{
				METADATA::Update_Bin_At(pos1, pos1 - bucket1_init, is_reversed_bucket1, std::min(min2 + 1, L_MAX), hash1);
				// Put elem
				INSERT_TYPE victim = DATA::GetElem(pos1);
				DATA::SaveElem(pos1, elem);
				elem = victim;
			}
			else
			{
				METADATA::Set_Unlucky_Bucket(bucket1_pos);
				METADATA::Update_Bin_At(pos2, pos2 - bucket2_init, is_reversed_bucket2, std::min(min1 + 1, L_MAX), hash0);
				// Put elem
				INSERT_TYPE victim = DATA::GetElem(pos2);
				DATA::SaveElem(pos2, elem);
				elem = victim;
			}
		}
	}

	///////////////////////////////////////////////////////////////////////////////
	// Find an element
	///////////////////////////////////////////////////////////////////////////////
	size_t find_position_SoA(const KEY_TYPE& elem) const noexcept
	{
		size_t hash0, hash1;
		std::tie(hash0, hash1) = DATA::hash_elem(elem);

		// Check first bucket
		size_t pos = fastrange(hash0, num_buckets);

		uint16_t c0 = METADATA::at(pos);

		uint16_t h = uint16_t(hash1);
		if (((c0 ^ h) & 0xFF00) == 0 && cmp_elems(pos, elem) && (c0 & 0b111))
			return pos;

		size_t reverse_sum = /*Is_Reversed_Window(pos)*/c0 & 0b01'000'000 ? size_t(-1) : size_t(1);

		pos += reverse_sum;
		uint16_t cc = METADATA::at(pos);
		if (((cc ^ h) & 0xFF00) == 0 && cmp_elems(pos, elem) && (cc & 0b111))
			return pos;
		if (NUM_ELEMS_BUCKET > 2)
		{
			pos += reverse_sum;
			cc = METADATA::at(pos);
			if (((cc ^ h) & 0xFF00) == 0 && cmp_elems(pos, elem) && (cc & 0b111))
				return pos;
		}
		if (NUM_ELEMS_BUCKET > 3)
		{
			pos += reverse_sum;
			cc = METADATA::at(pos);
			if (((cc ^ h) & 0xFF00) == 0 && cmp_elems(pos, elem) && (cc & 0b111))
				return pos;
		}

		// Check second bucket
		if (c0 & 0b10'000'000)//Is_Unlucky_Bucket(pos)
		{
			pos = fastrange(hash1, num_buckets);

			cc = METADATA::at(pos);

			h = uint16_t(hash0);
			if (((cc ^ h) & 0xFF00) == 0 && (cc & 0b111) && cmp_elems(pos, elem))
				return pos;

			reverse_sum = /*Is_Reversed_Window(pos)*/cc & 0b01'000'000 ? size_t(-1) : size_t(1);

			pos += reverse_sum;
			cc = METADATA::at(pos);
			if (((cc ^ h) & 0xFF00) == 0 && (cc & 0b111) && cmp_elems(pos, elem))
				return pos;
			if (NUM_ELEMS_BUCKET > 2)
			{
				pos += reverse_sum;
				cc = METADATA::at(pos);
				if (((cc ^ h) & 0xFF00) == 0 && (cc & 0b111) && cmp_elems(pos, elem))
					return pos;
			}
			if (NUM_ELEMS_BUCKET > 3)
			{
				pos += reverse_sum;
				cc = METADATA::at(pos);
				if (((cc ^ h) & 0xFF00) == 0 && (cc & 0b111) && cmp_elems(pos, elem))
					return pos;
			}
		}

		return SIZE_MAX;
	}
	size_t find_position_AoS(const KEY_TYPE& elem) const noexcept
	{
		size_t hash0, hash1;
		std::tie(hash0, hash1) = DATA::hash_elem(elem);

		// Check first bucket
		size_t pos = fastrange(hash0, num_buckets);

		uint_fast16_t c0 = METADATA::at(pos);

		if (cmp_elems(pos, elem) && (c0 & 0b111))
			return pos;

		size_t reverse_sum = /*Is_Reversed_Window(pos)*/c0 & 0b01'000'000 ? size_t(-1) : size_t(1);

		pos += reverse_sum;
		uint_fast16_t cc = METADATA::at(pos);
		if (cmp_elems(pos, elem) && (cc & 0b111))
			return pos;

		if (NUM_ELEMS_BUCKET > 2)
		{
			pos += reverse_sum;
			cc = METADATA::at(pos);
			if (cmp_elems(pos, elem) && (cc & 0b111))
				return pos;
		}
		if (NUM_ELEMS_BUCKET > 3)
		{
			pos += reverse_sum;
			cc = METADATA::at(pos);
			if (cmp_elems(pos, elem) && (cc & 0b111))
				return pos;
		}

		// Check second bucket
		if (c0 & 0b10'000'000)//Is_Unlucky_Bucket(pos)
		{
			pos = fastrange(hash1, num_buckets);

			cc = METADATA::at(pos);

			if (cmp_elems(pos, elem) && (cc & 0b111))
				return pos;

			reverse_sum = /*Is_Reversed_Window(pos)*/cc & 0b01'000'000 ? size_t(-1) : size_t(1);

			pos += reverse_sum;
			cc = METADATA::at(pos);
			if (cmp_elems(pos, elem) && (cc & 0b111))
				return pos;
			if (NUM_ELEMS_BUCKET > 2)
			{
				pos += reverse_sum;
				cc = METADATA::at(pos);
				if (cmp_elems(pos, elem) && (cc & 0b111))
					return pos;
			}
			if (NUM_ELEMS_BUCKET > 3)
			{
				pos += reverse_sum;
				cc = METADATA::at(pos);
				if (cmp_elems(pos, elem) && (cc & 0b111))
					return pos;
			}
		}

		return SIZE_MAX;
	}
	__forceinline size_t find_position(const KEY_TYPE& elem) const noexcept
	{
		if (IS_SoA)
			return find_position_SoA(elem);
		else
			return find_position_AoS(elem);
	}

public:
	size_t capacity() const noexcept
	{
		return num_buckets;
	}
	size_t size() const noexcept
	{
		return num_elems;
	}
	bool empty() const noexcept
	{
		return num_elems == 0;
	}
	void clear() noexcept
	{
		num_elems = 0;
		METADATA::Clear(0, num_buckets);

		for (size_t i = 0; i < (NUM_ELEMS_BUCKET - 1); i++)
			METADATA::Set_Bucket_Reversed(num_buckets - 1 - i);
	}
	float load_factor() const noexcept
	{
		return size() * 100f / capacity();
	}
	void max_load_factor(float value) noexcept
	{
		_max_load_factor = value;
	}
	float max_load_factor() const noexcept
	{
		return _max_load_factor;
	}
	void grow_factor(float value) noexcept
	{
		_grow_factor = value;
	}
	float grow_factor() const noexcept
	{
		return _grow_factor;
	}

	void reserve(size_t new_capacity) noexcept
	{
		rehash(new_capacity);
	}

	void insert(const INSERT_TYPE& to_insert_elem) noexcept
	{
		if (num_elems >= num_buckets * _max_load_factor)
			rehash(get_grow_size());

		INSERT_TYPE elem = to_insert_elem;
		// TODO: break infinity cycle when -> num_buckets=SIZE_MAX
		while (!try_insert(elem))
			rehash(get_grow_size());
	}

	// Check if an element exist
	uint32_t count(const KEY_TYPE& elem) const noexcept
	{
		return find_position(elem) != SIZE_MAX ? 1u : 0u;
	}

	// TODO: As currently implemented the performance may degrade
	// if many erase operations are done
	uint32_t erase(const KEY_TYPE& elem) noexcept
	{
		size_t elem_pos = find_position(elem);
		if (elem_pos != SIZE_MAX)
		{
			METADATA::Set_Empty(elem_pos);
			return 1;
		}

		return 0;
	}

	void shrink()
	{
		this->~CBG_IMPL();
	}
};

// Map. Only added simple mapping operations.
template<size_t NUM_ELEMS_BUCKET, class KEY, class T, class EQ, class DATA, class METADATA, bool IS_SoA> class CBG_MAP_IMPL : public CBG_IMPL<NUM_ELEMS_BUCKET, std::pair<KEY, T>, KEY, T, EQ, DATA, METADATA, IS_SoA>
{
public:
	CBG_MAP_IMPL() noexcept : CBG_IMPL()
	{}
	CBG_MAP_IMPL(size_t expected_num_elems) noexcept : CBG_IMPL(expected_num_elems)
	{}

	// Map operations
	T& operator[](const KEY& key) noexcept
	{
		size_t key_pos = find_position(key);
		if (key_pos == SIZE_MAX)
		{
			insert(std::make_pair(key, T()));
			key_pos = find_position(key);
		}

		return *DATA::GetValue(key_pos);
	}
	T& operator[](KEY&& key) noexcept
	{
		size_t key_pos = find_position(key);
		if (key_pos == SIZE_MAX)
		{
			insert(std::make_pair(std::move(key), T()));
			key_pos = find_position(key);
		}

		return *DATA::GetValue(key_pos);
	}
	T& at(const KEY& key)
	{
		size_t key_pos = find_position(key);
		if (key_pos == SIZE_MAX)
			throw std::out_of_range("Argument passed to at() was not in the map.");

		return *DATA::GetValue(key_pos);
	}
	const T& at(const KEY& key) const
	{
		size_t key_pos = find_position(key);
		if (key_pos == SIZE_MAX)
			throw std::out_of_range("Argument passed to at() was not in the map.");

		return *DATA::GetValue(key_pos);
	}
};
}// end namespace cbg_internal

///////////////////////////////////////////////////////////////////////////////
// CBG Sets
///////////////////////////////////////////////////////////////////////////////
// (Struct of Arrays)
template<size_t NUM_ELEMS_BUCKET, class T, class HASHER, class EQ = std::equal_to<T>> class Set_SoA :
	public cbg_internal::CBG_IMPL<NUM_ELEMS_BUCKET, T, T, T, EQ, cbg_internal::KeyLayout_SoA<T, HASHER>, cbg_internal::MetadataLayout_SoA, true>
{
public:
	Set_SoA() noexcept : CBG_IMPL()
	{}
	Set_SoA(size_t expected_num_elems) noexcept : CBG_IMPL(expected_num_elems)
	{}
};
// (Array of structs)
template<size_t NUM_ELEMS_BUCKET, class T, class HASHER, class EQ = std::equal_to<T>> class Set_AoS :
	public cbg_internal::CBG_IMPL<NUM_ELEMS_BUCKET, T, T, T, EQ, cbg_internal::KeyLayout_AoS<T, HASHER>, cbg_internal::MetadataLayout_AoS<sizeof(T)>, false>
{
public:
	Set_AoS() noexcept : CBG_IMPL()
	{}
	Set_AoS(size_t expected_num_elems) noexcept : CBG_IMPL(expected_num_elems)
	{}
};
// (Array of blocks)
template<size_t NUM_ELEMS_BUCKET, class T, class HASHER, class EQ = std::equal_to<T>> class Set_AoB :
	public cbg_internal::CBG_IMPL<NUM_ELEMS_BUCKET, T, T, T, EQ, cbg_internal::KeyLayout_AoB<T, HASHER>, cbg_internal::MetadataLayout_AoB<alignof(T), cbg_internal::BlockKey<T>>, false>
{
public:
	Set_AoB() noexcept : CBG_IMPL()
	{}
	Set_AoB(size_t expected_num_elems) noexcept : CBG_IMPL(expected_num_elems)
	{}
};
///////////////////////////////////////////////////////////////////////////////
// CBG Maps
///////////////////////////////////////////////////////////////////////////////
// (Struct of Arrays)
template<size_t NUM_ELEMS_BUCKET, class KEY, class T, class HASHER, class EQ = std::equal_to<KEY>> class Map_SoA :
	public cbg_internal::CBG_MAP_IMPL<NUM_ELEMS_BUCKET, KEY, T, EQ, cbg_internal::MapLayout_SoA<KEY, T, HASHER>, cbg_internal::MetadataLayout_SoA, true>
{
public:
	Map_SoA() noexcept : CBG_MAP_IMPL()
	{}
	Map_SoA(size_t expected_num_elems) noexcept : CBG_MAP_IMPL(expected_num_elems)
	{}
};
// (Array of structs)
template<size_t NUM_ELEMS_BUCKET, class KEY, class T, class HASHER, class EQ = std::equal_to<KEY>> class Map_AoS :
	public cbg_internal::CBG_MAP_IMPL<NUM_ELEMS_BUCKET, KEY, T, EQ, cbg_internal::MapLayout_AoS<KEY, T, HASHER>, cbg_internal::MetadataLayout_AoS<sizeof(KEY) + sizeof(T)>, false>
{
public:
	Map_AoS() noexcept : CBG_MAP_IMPL()
	{}
	Map_AoS(size_t expected_num_elems) noexcept : CBG_MAP_IMPL(expected_num_elems)
	{}
};
// (Array of blocks)
template<size_t NUM_ELEMS_BUCKET, class KEY, class T, class HASHER, class EQ = std::equal_to<KEY>> class Map_AoB :
	public cbg_internal::CBG_MAP_IMPL<NUM_ELEMS_BUCKET, KEY, T, EQ, cbg_internal::MapLayout_AoB<KEY, T, HASHER>, cbg_internal::MetadataLayout_AoB<cbg_internal::MaxAlignOf<KEY, T>::BLOCK_SIZE, cbg_internal::BlockMap<KEY, T>>, false>
{
public:
	Map_AoB() noexcept : CBG_MAP_IMPL()
	{}
	Map_AoB(size_t expected_num_elems) noexcept : CBG_MAP_IMPL(expected_num_elems)
	{}
};
}// end namespace cbg