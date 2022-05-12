// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#pragma once

#include "exec/vectorized/chunks_sorter_topn.h"

namespace starrocks::vectorized {
class ChunksSorterRankTopn : public ChunksSorterTopn {
public:
    ChunksSorterRankTopn(RuntimeState* state, const std::vector<ExprContext*>* sort_exprs,
                         const std::vector<bool>* is_asc, const std::vector<bool>* is_null_first,
                         const std::string& sort_keys, size_t offset = 0, size_t limit = 0,
                         size_t max_buffered_chunks = kDefaultBufferedChunks);

protected:
    virtual Status _filter_and_sort_data(RuntimeState* state, std::pair<Permutation, Permutation>& permutations,
                                         DataSegments& segments, size_t chunk_size) override;

    virtual Status _merge_sort_data_as_merged_segment(RuntimeState* state,
                                                      std::pair<Permutation, Permutation>& new_permutation,
                                                      DataSegments& segments) override;

    virtual Status _partial_sort_col_wise(RuntimeState* state, std::pair<Permutation, Permutation>& permutations,
                                          DataSegments& segments, const size_t chunk_size,
                                          size_t number_of_rows_to_sort) override;
};
} // namespace starrocks::vectorized
