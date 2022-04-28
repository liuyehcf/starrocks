// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#pragma once

#include "column/chunk.h"
#include "column/column_hash.h"
#include "column/column_helper.h"
#include "column/hash_set.h"
#include "column/type_traits.h"
#include "column/vectorized_fwd.h"
#include "util/phmap/phmap.h"

namespace starrocks::vectorized {

using Chunks = std::vector<ChunkPtr>;

struct PartitionChunks {
    Chunks chunks;

    // Used to save the indexes of this partition
    std::vector<uint32_t> select_indexes;
    // Used to save the remain size of last chunk in chunks
    // Avoid virtual function call `chunks->back()->num_rows()`
    int32_t remain_size = 0;
};

// =====================
// one level partition hash map
template <PhmapSeed seed>
using Int8PartitionHashMap = phmap::flat_hash_map<int8_t, PartitionChunks, StdHashWithSeed<int8_t, seed>>;
template <PhmapSeed seed>
using Int16PartitionHashMap = phmap::flat_hash_map<int16_t, PartitionChunks, StdHashWithSeed<int16_t, seed>>;
template <PhmapSeed seed>
using Int32PartitionHashMap = phmap::flat_hash_map<int32_t, PartitionChunks, StdHashWithSeed<int32_t, seed>>;
template <PhmapSeed seed>
using Int64PartitionHashMap = phmap::flat_hash_map<int64_t, PartitionChunks, StdHashWithSeed<int64_t, seed>>;
template <PhmapSeed seed>
using Int128PartitionHashMap = phmap::flat_hash_map<int128_t, PartitionChunks, Hash128WithSeed<seed>>;
template <PhmapSeed seed>
using DatePartitionHashMap = phmap::flat_hash_map<DateValue, PartitionChunks, StdHashWithSeed<DateValue, seed>>;
template <PhmapSeed seed>
using TimeStampPartitionHashMap =
        phmap::flat_hash_map<TimestampValue, PartitionChunks, StdHashWithSeed<TimestampValue, seed>>;
template <PhmapSeed seed>
using SlicePartitionHashMap = phmap::flat_hash_map<Slice, PartitionChunks, SliceHashWithSeed<seed>, SliceEqual>;

// ==================
// one level fixed size slice hash map
template <PhmapSeed seed>
using FixedSize4SlicePartitionHashMap =
        phmap::flat_hash_map<SliceKey4, PartitionChunks, FixedSizeSliceKeyHash<SliceKey4, seed>>;
template <PhmapSeed seed>
using FixedSize8SlicePartitionHashMap =
        phmap::flat_hash_map<SliceKey8, PartitionChunks, FixedSizeSliceKeyHash<SliceKey8, seed>>;
template <PhmapSeed seed>
using FixedSize16SlicePartitionHashMap =
        phmap::flat_hash_map<SliceKey16, PartitionChunks, FixedSizeSliceKeyHash<SliceKey16, seed>>;

struct PartitionHashMapBase {
    const int32_t chunk_size;

    PartitionHashMapBase(int32_t chunk_size) : chunk_size(chunk_size) {}

protected:
    void alloc_new_buffer(PartitionChunks& value, const ChunkPtr& chunk) {
        value.chunks.push_back(chunk->clone_empty());
        value.select_indexes.clear();
        value.select_indexes.reserve(chunk_size);
        value.remain_size = chunk_size;
    }

    void flush(PartitionChunks& value, const ChunkPtr& chunk) {
        if (!value.chunks.empty() && !value.select_indexes.empty()) {
            value.chunks.back()->append_selective(*chunk, value.select_indexes.data(), 0, value.select_indexes.size());
            value.select_indexes.clear();
            value.select_indexes.reserve(chunk_size);
            value.remain_size = chunk_size - value.chunks.back()->num_rows();
        }
    }

    template <typename HashMap, typename KeyLoader>
    void append_chunk_for_one_key(HashMap& hash_map, ChunkPtr chunk, KeyLoader&& key_loader) {
        const auto size = chunk->num_rows();
        for (uint32_t i = 0; i < size; i++) {
            const auto& key = key_loader(i);
            auto& value = hash_map[key];
            if (value.chunks.empty() || value.remain_size <= 0) {
                if (!value.chunks.empty()) {
                    value.chunks.back()->append_selective(*chunk, value.select_indexes.data(), 0,
                                                          value.select_indexes.size());
                }

                alloc_new_buffer(value, chunk);
            }
            value.select_indexes.push_back(i);
            value.remain_size--;
        }

        for (auto& [key, value] : hash_map) {
            flush(value, chunk);
        }
    }

    template <typename HashMap, typename KeyLoader>
    void append_chunk_for_one_nullable_key(HashMap& hash_map, PartitionChunks& null_key_value, ChunkPtr chunk,
                                           const NullableColumn* nullable_key_column, KeyLoader&& key_loader) {
        if (nullable_key_column->only_null()) {
            const auto size = chunk->num_rows();
            if (null_key_value.chunks.empty() || null_key_value.remain_size <= 0) {
                if (!null_key_value.chunks.empty()) {
                    null_key_value.chunks.back()->append_selective(*chunk, null_key_value.select_indexes.data(), 0,
                                                                   null_key_value.select_indexes.size());
                }

                alloc_new_buffer(null_key_value, chunk);
            }
            int32_t offset = 0;
            auto cur_remain_size = size;
            while (null_key_value.remain_size < cur_remain_size) {
                null_key_value.chunks.back()->append(*chunk, offset, null_key_value.remain_size);
                offset += null_key_value.remain_size;
                cur_remain_size -= null_key_value.remain_size;

                alloc_new_buffer(null_key_value, chunk);
            }
            null_key_value.chunks.back()->append(*chunk, offset, cur_remain_size);
        } else {
            const auto& null_flag_data = nullable_key_column->null_column()->get_data();
            const auto size = chunk->num_rows();

            for (uint32_t i = 0; i < size; i++) {
                PartitionChunks* value_ptr = nullptr;
                if (null_flag_data[i] == 1) {
                    value_ptr = &null_key_value;
                } else {
                    const auto& key = key_loader(i);
                    value_ptr = &hash_map[key];
                }

                auto& value = *value_ptr;
                if (value.chunks.empty() || value.remain_size <= 0) {
                    if (!value.chunks.empty()) {
                        value.chunks.back()->append_selective(*chunk, value.select_indexes.data(), 0,
                                                              value.select_indexes.size());
                    }

                    alloc_new_buffer(value, chunk);
                }
                value.select_indexes.push_back(i);
                value.remain_size--;
            }

            for (auto& [_, value] : hash_map) {
                flush(value, chunk);
            }
            flush(null_key_value, chunk);
        }
    }
};

template <PrimitiveType primitive_type, typename HashMap>
struct PartitionHashMapWithOneNumberKey : public PartitionHashMapBase {
    using Iterator = typename HashMap::iterator;
    using ColumnType = RunTimeColumnType<primitive_type>;
    using FieldType = RunTimeCppType<primitive_type>;
    HashMap hash_map;

    PartitionHashMapWithOneNumberKey(int32_t chunk_size) : PartitionHashMapBase(chunk_size) {}

    void append_chunk(ChunkPtr chunk, const Columns& key_columns) {
        DCHECK(!key_columns[0]->is_nullable());
        const auto* key_column = down_cast<ColumnType*>(key_columns[0].get());
        const auto& key_column_data = key_column->get_data();
        append_chunk_for_one_key(hash_map, chunk, [&](uint32_t offset) { return key_column_data[offset]; });
    }
};

template <PrimitiveType primitive_type, typename HashMap>
struct PartitionHashMapWithOneNullableNumberKey : public PartitionHashMapBase {
    using Iterator = typename HashMap::iterator;
    using ColumnType = RunTimeColumnType<primitive_type>;
    using FieldType = RunTimeCppType<primitive_type>;
    HashMap hash_map;
    PartitionChunks null_key_value;

    PartitionHashMapWithOneNullableNumberKey(int32_t chunk_size) : PartitionHashMapBase(chunk_size) {}

    void append_chunk(ChunkPtr chunk, const Columns& key_columns) {
        DCHECK(key_columns[0]->is_nullable());
        const auto* nullable_key_column = ColumnHelper::as_raw_column<NullableColumn>(key_columns[0].get());
        const auto& key_column_data = down_cast<ColumnType*>(nullable_key_column->data_column().get())->get_data();
        append_chunk_for_one_nullable_key(hash_map, null_key_value, chunk, nullable_key_column,
                                          [&](uint32_t offset) { return key_column_data[offset]; });
    }
};

template <typename HashMap>
struct PartitionHashMapWithOneStringKey : public PartitionHashMapBase {
    using Iterator = typename HashMap::iterator;
    HashMap hash_map;

    PartitionHashMapWithOneStringKey(int32_t chunk_size) : PartitionHashMapBase(chunk_size) {}

    void append_chunk(ChunkPtr chunk, const Columns& key_columns) {
        DCHECK(!key_columns[0]->is_nullable());
        const auto* key_column = down_cast<BinaryColumn*>(key_columns[0].get());
        append_chunk_for_one_key(hash_map, chunk, [&](uint32_t offset) { return key_column->get_slice(offset); });
    }
};

template <typename HashMap>
struct PartitionHashMapWithOneNullableStringKey : public PartitionHashMapBase {
    using Iterator = typename HashMap::iterator;
    HashMap hash_map;
    PartitionChunks null_key_value;

    PartitionHashMapWithOneNullableStringKey(int32_t chunk_size) : PartitionHashMapBase(chunk_size) {}

    void append_chunk(ChunkPtr chunk, const Columns& key_columns) {
        DCHECK(key_columns[0]->is_nullable());
        const auto* nullable_key_column = ColumnHelper::as_raw_column<NullableColumn>(key_columns[0].get());
        const auto* key_column = down_cast<BinaryColumn*>(nullable_key_column->data_column().get());
        append_chunk_for_one_nullable_key(hash_map, null_key_value, chunk, nullable_key_column,
                                          [&](uint32_t offset) { return key_column->get_slice(offset); });
    }
};

// TODO(hcf) to be implemented
template <typename HashMap>
struct PartitionHashMapWithSerializedKey {
    using Iterator = typename HashMap::iterator;
    HashMap hash_map;

    PartitionHashMapWithSerializedKey(int32_t chunk_size) {}
    void append_chunk(ChunkPtr chunk, const Columns& key_columns) {}
};

// TODO(hcf) to be implemented
template <typename HashMap>
struct PartitionHashMapWithSerializedKeyFixedSize {
    using Iterator = typename HashMap::iterator;
    HashMap hash_map;
    bool has_null_column = false;
    int fixed_byte_size = -1; // unset state

    PartitionHashMapWithSerializedKeyFixedSize(int32_t chunk_size) {}
    void append_chunk(ChunkPtr chunk, const Columns& key_columns) {}
};

} // namespace starrocks::vectorized
