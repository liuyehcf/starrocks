// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#include "exec/vectorized/chunks_sorter_rank_topn.h"

#include "exec/vectorized/sorting/sort_permute.h"
#include "exec/vectorized/sorting/sorting.h"

namespace starrocks::vectorized {

ChunksSorterRankTopn::ChunksSorterRankTopn(RuntimeState* state, const std::vector<ExprContext*>* sort_exprs,
                                           const std::vector<bool>* is_asc_order,
                                           const std::vector<bool>* is_null_first, const std::string& sort_keys,
                                           size_t offset, size_t limit, size_t max_buffered_chunks)
        : ChunksSorterTopn(state, sort_exprs, is_asc_order, is_null_first, sort_keys, offset, limit,
                           max_buffered_chunks) {}

Status ChunksSorterRankTopn::_filter_and_sort_data(RuntimeState* state,
                                                   std::pair<Permutation, Permutation>& permutations,
                                                   DataSegments& segments, size_t chunk_size) {
    ScopedTimer<MonotonicStopWatch> timer(_sort_timer);

    DCHECK(_get_number_of_order_by_columns() > 0) << "order by columns can't be empty";

    // Limit by rank allows rows to exceed the rank value itself
    const auto minimum_number_of_rows_to_sort = _limit;

    if (_init_merged_segment) {
        std::vector<std::vector<uint8_t>> filter_array;
        uint32_t least_num, middle_num;

        const auto merged_segment_size = _merged_segment.chunk->num_rows();
        RETURN_IF_ERROR(_merged_segment.get_filter_array(segments, merged_segment_size, filter_array, _sort_order_flag,
                                                         _null_first_flag, least_num, middle_num));

        timer.stop();
        {
            ScopedTimer<MonotonicStopWatch> timer(_build_timer);
            permutations.first.resize(least_num);
            // BEFORE's size is enough, so we ignore IN.
            if (least_num >= minimum_number_of_rows_to_sort) {
                // use filter_array to set permutations.first.
                _set_permutation_before(permutations.first, segments.size(), filter_array);
            } else if (minimum_number_of_rows_to_sort > 1) {
                // if minimum_number_of_rows_to_sort == 1, first row and last row is the same identity. so we do nothing.
                // BEFORE's size < minimum_number_of_rows_to_sort, we need set permutations.first and permutations.second.
                permutations.second.resize(middle_num);

                // use filter_array to set permutations.first and permutations.second.
                _set_permutation_complete(permutations, segments.size(), filter_array);
            }
        }
        timer.start();
    }

    return _partial_sort_col_wise(state, permutations, segments, chunk_size, minimum_number_of_rows_to_sort);
}

Status ChunksSorterRankTopn::_merge_sort_data_as_merged_segment(RuntimeState* state,
                                                                std::pair<Permutation, Permutation>& new_permutation,
                                                                DataSegments& segments) {
    ScopedTimer<MonotonicStopWatch> timer(_merge_timer);

    size_t sort_row_number = _get_number_of_rows_to_sort();
    DCHECK(sort_row_number > 0) << "output rows can't be empty";

    if (_init_merged_segment) {
        RETURN_IF_ERROR(_hybrid_sort_common(state, new_permutation, segments, sort_row_number));
    } else {
        // the first batch chunks, just new_permutation.second.
        RETURN_IF_ERROR(_hybrid_sort_first_time(state, new_permutation.second, segments, sort_row_number));
        _init_merged_segment = true;
    }

    // include release memory'time in _merge_timer.
    Permutation().swap(new_permutation.first);
    Permutation().swap(new_permutation.second);

    DataSegments().swap(segments);
    return Status::OK();
}

Status ChunksSorterRankTopn::_partial_sort_col_wise(RuntimeState* state,
                                                    std::pair<Permutation, Permutation>& permutations,
                                                    DataSegments& segments, const size_t chunk_size,
                                                    size_t number_of_rows_to_sort) {
    std::vector<Columns> vertical_chunks;
    for (auto& segment : segments) {
        vertical_chunks.push_back(segment.order_by_columns);
    }
    auto do_sort = [&](Permutation& perm, size_t limit) {
        return sort_vertical_chunks(state->cancelled_ref(), vertical_chunks, _sort_order_flag, _null_first_flag, perm,
                                    limit);
    };

    size_t first_size = std::min(permutations.first.size(), number_of_rows_to_sort);

    // Sort the first, then the second
    if (first_size > 0) {
        RETURN_IF_CANCELLED(state);
        RETURN_IF_ERROR(do_sort(permutations.first, first_size));
    }

    if (number_of_rows_to_sort > first_size) {
        RETURN_IF_CANCELLED(state);
        RETURN_IF_ERROR(do_sort(permutations.second, number_of_rows_to_sort - first_size));
    }

    return Status::OK();
}
}; // namespace starrocks::vectorized
