// Copyright (c) OpenMMLab. All rights reserved.

#include "src/turbomind/models/llama/LlamaBatch.h"
#include "src/turbomind/kernels/attention/data_type.h"
#include "src/turbomind/kernels/decoding_kernels.h"
#include "src/turbomind/kernels/gpt_kernels.h"
#include "src/turbomind/kernels/sampling_topk_kernels.h"
#include "src/turbomind/macro.h"
#include "src/turbomind/models/llama/BlockManager.h"
#include "src/turbomind/models/llama/LlamaNcclGuard.h"
#include "src/turbomind/models/llama/LlamaV2.h"
#include "src/turbomind/models/llama/Request.h"
#include "src/turbomind/models/llama/SequenceManager.h"
#include "src/turbomind/models/llama/copy.h"
#include "src/turbomind/models/llama/llama_kernels.h"
#include "src/turbomind/models/llama/llama_utils.h"
#include "src/turbomind/utils/Tensor.h"
#include "src/turbomind/utils/constant.h"
#include "src/turbomind/utils/cuda_utils.h"
#include "src/turbomind/utils/debug_utils.h"
#include "src/turbomind/utils/gemm_test/gemm_func.h"
#include "src/turbomind/utils/logger.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace turbomind {

void PrintDecodeTokens(
    const int* token_ids, int max_seq_len, int batch_sizse, cudaStream_t stream, const std::string& msg)
{
    // tokens in [S, B] layout
    std::vector<int> tokens(max_seq_len * batch_sizse);
    check_cuda_error(cudaMemcpyAsync(tokens.data(), token_ids, sizeof(int) * tokens.size(), cudaMemcpyDefault, stream));
    check_cuda_error(cudaStreamSynchronize(stream));

    printf("[%s] ", msg.c_str());
    for (int j = 0; j < max_seq_len; ++j) {
        printf("%5d ", j);
    }
    printf("\n");
    for (int i = 0; i < batch_sizse; ++i) {
        printf("[%s] ", msg.c_str());
        for (int j = 0; j < max_seq_len; ++j) {
            // std::cout << sb_tokens[j * batch_size + i] << " ";
            printf("%5d ", tokens[j * batch_sizse + i]);
        }
        printf("\n");
    }
}
void ClearState(BatchState& s)
{
    std::fill_n(s.requests.begin(), s.size, nullptr);
    std::fill_n(s.sequences.begin(), s.size, nullptr);
    s.size = s.active_size = 0;
}

void DropEmbeddings(const Sequence& seq)
{
    int    seq_len = seq.tokens.size();
    int    num_emb = seq.input_embeddings.size();
    size_t sz      = num_emb;
    for (; sz >= 1; sz--) {
        if (seq.input_embedding_ranges[sz - 1].second <= seq_len) {
            break;
        }
    }
    // should we keep part of embedding?
    seq.input_embeddings.resize(sz);
    seq.input_embedding_ranges.resize(sz);
}

template<typename T>
void LlamaBatch<T>::RejectInvalidRequests(Requests& stop_reqs, Requests& infer_reqs)
{
    std::unordered_map<uint64_t, int> occurrence;

    auto count_occurrence = [&occurrence](const Requests& rs) {
        for (const auto& r : rs) {
            ++occurrence[r->id];
        }
    };

    auto reject = [](const char* type, std::shared_ptr<Request>& req, int ec) {
        TM_LOG_WARNING(
            "[RejectInvalidRequests] Skipping invalid %s request for id %ld, code = %d", type, (long)req->id, ec);
        req->signal.set_value(ec);
        req.reset();
    };

    auto handle_conflict_or_invalid = [this, &occurrence, &reject](Requests& rs, const char* type) {
        for (auto& r : rs) {
            if (r) {
                int ec = 0;

                const int  input_length = r->inputs[rank_].getVal<int>("input_lengths", 0);
                const auto get_offset   = [&](int token_count) {
                    return std::max(0, std::min(token_count, r->inputs[rank_].getVal<int>("step", token_count)));
                };

                if (occurrence[r->id] != 1) {
                    ec = Request::kConflict;
                }
                else if (r->start_flag && r->stop_flag) {
                    ec = Request::kInvalid;
                }
                else if (input_length > session_len_) {
                    ec = Request::kTooLong;
                }
                else if (!r->start_flag) {
                    if (auto seq = sequence_manager_->Get(r->id); seq == nullptr) {
                        ec = Request::kInvalid;
                    }
                    else if (get_offset(seq->tokens.size()) + input_length > session_len_) {
                        ec = Request::kTooLong;
                    }
                }

                if (ec) {
                    reject(type, r, ec);
                }
            }
        }
    };

    auto drop_invalid = [](Requests& rs) {
        int count = 0;
        for (int i = 0; i < rs.size(); ++i) {
            if (rs[i]) {
                rs[count++] = std::move(rs[i]);
            }
        }
        rs.resize(count);
    };

    count_occurrence(stop_reqs);
    count_occurrence(infer_reqs);

    if (!stop_reqs.empty()) {
        handle_conflict_or_invalid(stop_reqs, "stop");

        // invalidate stop-only requests for inactive sequences
        for (auto& r : stop_reqs) {
            if (r && r->end_flag == false) {
                int ec = Request::kInactive;
                for (int i = 0; i < state_->size; ++i) {
                    if (state_->requests[i] && state_->requests[i]->id == r->id) {
                        ec = 0;
                        break;
                    }
                }
                if (ec) {
                    reject("stop", r, ec);
                }
            }
        }

        drop_invalid(stop_reqs);
    }

    if (!infer_reqs.empty()) {
        handle_conflict_or_invalid(infer_reqs, "infer");

        // invalidate requests for busy sequences
        for (auto& r : infer_reqs) {
            if (r) {
                for (int i = 0; i < state_->size; ++i) {
                    if (state_->requests[i] && state_->requests[i]->id == r->id) {
                        reject("infer", r, Request::kBusy);
                        break;
                    }
                }
            }
        }

        drop_invalid(infer_reqs);
    }
}

template<typename T>
auto LlamaBatch<T>::ProcessStopRequests(const Requests& requests) -> std::vector<Signal>
{
    NvtxScope           scope("stop_request");
    std::vector<Signal> signals;
    int                 count = 0;
    for (const auto& r : requests) {
        int ec = Request::kFail;
        // find matching active sequence
        for (int i = 0; i < state_->size; ++i) {
            // stop & optionally erase active sequence
            if (state_->requests[i] && state_->requests[i]->id == r->id) {
                ec = 0;
                signals.push_back(Interrupt(i, true, r->end_flag));
                ++count;
                break;
            }
        }
        // mismatch, try erase inactive sequence, in this case there is no active request to interrupt
        if (ec && r->end_flag) {
            if (sequence_manager_->Erase(r->id)) {
                ec = 0;
            }
        }
        signals.push_back([=] {
            if (rank_ == 0) {
                r->signal.set_value(ec);
            }
        });
    }
    if (count) {
        check_cuda_error(cudaStreamSynchronize(stream_));
    }
    return signals;
}

template<typename T>
void LlamaBatch<T>::ProcessInferRequests(const Requests& requests)
{
    NvtxScope scope("infer_request");
    auto&     state = *incoming_;

    FT_CHECK(state.size == 0);
    FT_CHECK(state.active_size == 0);

    std::vector<int> existing_idx;

    int idx = 0;
    for (const auto& r : requests) {
        FT_CHECK(!state.requests[idx]);

        if (rank_ == 0) {
            TM_LOG_WARNING("[ProcessInferRequests] Request for %ld received.", (long)r->id);
        }

        state.requests[idx] = r;

        // get sequence for the request
        state.sequences[idx] = r->start_flag ? sequence_manager_->Create(r->id) : sequence_manager_->Get(r->id);
        FT_CHECK(state.sequences[idx]);

        auto& seq = *state.sequences[idx];

        seq.iter = 0;

        if (int step = r->inputs[rank_].getVal<int>("step", -1); step >= 0) {
            if (step <= seq.tokens.size()) {
                seq.tokens.resize(step);
                seq.cache_len = std::min(seq.cache_len, step);
                DropEmbeddings(seq);
            }
            else if (rank_ == 0) {
                TM_LOG_WARNING(
                    "[ProcessInferRequests] Skipping invalid step (%d) setting for ID %ld", step, (long)seq.id);
            }
        }

        const int  input_length = r->inputs[rank_].getVal<int>("input_lengths");
        const int* input_ids    = r->inputs[rank_].getPtr<int>("input_ids");

        // `output_ids` contains all token ids of the sequences
        const auto output_ids_base = state.output_ids + session_len_ * idx;
        auto       output_ids      = output_ids_base;

        auto incoming_input_ids = state.input_ids + session_len_ * idx;

        // copy history tokens
        if (!seq.tokens.empty()) {
            output_ids = Copy(seq.tokens.data(), seq.tokens.size(), output_ids);
        }

        // copy input tokens
        if (input_length) {
            output_ids = Copy(input_ids, input_length, output_ids);

            Copy(input_ids, input_length, incoming_input_ids);
        }

        // copy input embeddings
        if (r->inputs[rank_].isExist("input_embedding_ranges")) {
            const auto range_tensor = r->inputs[rank_].at("input_embedding_ranges");
            const auto emb_tensor   = r->inputs[rank_].at("input_embeddings");
            const int* ranges       = range_tensor.getPtr<int>();

            auto check_embeddings = [&](int& num_valid_embeddings) {
                if (range_tensor.shape.size() != 3 || range_tensor.shape[2] % 2 != 0) {
                    return false;
                }
                int embedding_count  = range_tensor.shape[1];
                int embedding_length = 0;
                int pre_end          = -1;

                for (size_t i = 0; i < embedding_count; i++) {
                    int begin = ranges[i * 2];
                    int end   = ranges[i * 2 + 1];
                    embedding_length += (end - begin);
                    if (begin < 0 || end < 0) {
                        break;
                    }
                    if (begin >= end || end > input_length || begin < pre_end
                        || embedding_length * model_->hidden_units_ * sizeof(T) > emb_tensor.shape[1]) {
                        return false;
                    }
                    pre_end              = end;
                    num_valid_embeddings = i + 1;
                }
                return true;
            };

            int num_valid_embeddings = 0;
            if (!check_embeddings(num_valid_embeddings)) {
                TM_LOG_WARNING("[ImageFeature] Skip invalid input embeddings, id = %ld, input_length = %d, "
                               "input embeddings = %s, range_tensor = %s",
                               (long)seq.id,
                               input_length,
                               emb_tensor.toString().c_str(),
                               range_tensor.toString().c_str());
            }
            else {
                char* emb_tensor_ptr = emb_tensor.getPtr<char>();
                for (size_t i = 0; i < num_valid_embeddings; i++) {
                    int    begin = ranges[i * 2];
                    int    end   = ranges[i * 2 + 1];
                    size_t count = (end - begin) * model_->hidden_units_ * sizeof(T);
                    seq.input_embeddings.emplace_back((std::byte*)emb_tensor_ptr, (std::byte*)(emb_tensor_ptr + count));
                    seq.input_embedding_ranges.emplace_back(begin + seq.tokens.size(), end + seq.tokens.size());
                    emb_tensor_ptr += count;
                }
            }
        }

        // total context length (history + input)
        state.h_prompt_length[idx]  = output_ids - output_ids_base;
        state.h_context_length[idx] = output_ids - output_ids_base;
        state.h_finished[idx]       = false;

        const int request_output_len = state.requests[idx]->inputs[rank_].getVal<int>("request_output_len");
        state.seq_len_limit[idx]     = state.h_context_length[idx] + request_output_len;
        // `length_criterion` sets finish flag when step >= seq_limit_len, however when step == seq_limit_len
        // the actual sequence length is seq_limit_len + 1, hence seq_limit_len must truncated to session_len - 1
        if (state.seq_len_limit[idx] >= session_len_) {
            state.seq_len_limit[idx] = session_len_ - 1;
            if (rank_ == 0) {
                const int trunc_output_len = state.seq_len_limit[idx] - state.h_context_length[idx];
                TM_LOG_WARNING(
                    "[ProcessInferRequests] [%ld] total sequence length (%d + %d) exceeds `session_len` (%d), `request_output_len` is truncated to %d",
                    (long)seq.id,
                    state.h_context_length[idx],
                    request_output_len,
                    (int)session_len_,
                    trunc_output_len);
            }
        }

        // compute rope scaling factor
        if (r->start_flag) {
            seq.rope_theta = model_->attn_params_.rotary_embedding_base;
            if (model_->attn_params_.use_dynamic_ntk) {
                auto scaling_factor = model_->attn_params_.rope_scaling_factor;
                if (scaling_factor >= 1.f) {  // infer by `seq_len_limit`
                    auto max_seq_len = state.seq_len_limit[idx];
                    auto max_pos_emb = model_->attn_params_.max_position_embeddings;
                    if (max_seq_len > max_pos_emb) {
                        scaling_factor = scaling_factor * max_seq_len / max_pos_emb - (scaling_factor - 1);
                        float rope_dim = model_->attn_params_.rotary_embedding_dim;
                        seq.rope_theta *= powf(scaling_factor, rope_dim / (rope_dim - 2.f));
                        TM_LOG_INFO("[ProcessInferRequests] %ld rope_scaling_factor: %f, rope_theta = %f",
                                    (long)seq.id,
                                    scaling_factor,
                                    seq.rope_theta);
                    }
                }
            }
        }
        state.h_rope_theta[idx] = seq.rope_theta;

        if (r->start_flag) {
            // prepare to initialize random state for new sequence
            h_random_seed_[idx] = r->inputs[rank_].getVal<unsigned long long>("random_seed", 0);
        }
        else {
            // Recover device states if not a new sequence
            h_curand_state_[existing_idx.size()] = *(curandState_t*)seq.random_state.data();
            existing_idx.push_back(idx);
        }

        // ! SHARED STATE IS MODIFIED, BARRIER SYNCHRONIZATION REQUIRED
        // assign priority based on arrival time
        if (rank_ == 0) {
            r->unique_id = request_count_++;
        }

        // increment pointer
        idx++;
    }

    state.size = idx;

    // when there are new sequences
    if (state.size != existing_idx.size()) {
        // copy random seeds to device
        Copy(h_random_seed_, state.size, d_random_seed_);
        // initialize random states
        invokeCurandBatchInitialize(state.curand_state, state.size, d_random_seed_, stream_);
        sync_check_cuda_error();
    }

    if (!existing_idx.empty()) {
        // copy existing curand states to device
        Copy(h_curand_state_, existing_idx.size(), d_curand_state_);
        // insert the states to their correct positions in the batch
        IndexedCopy({}, existing_idx, std::tuple{d_curand_state_, state.curand_state, 1});
    }
}

template<typename T>
void LlamaBatch<T>::AdjustMaxInputCount(GenerationState&                    g,
                                        const std::vector<const Sequence*>& sequences,
                                        const std::vector<int>&             context_length)
{
    int input_count = 0;
    for (int i = 0; i < sequences.size(); ++i) {
        input_count += context_length[i] - sequences[i]->cache_len;
    }
    const int batch_size = sequences.size();
    input_count -= batch_size;

    // min tokens per iter for satisfying max prefill iters constraint
    input_count = (input_count + max_prefill_iters_ - 1) / max_prefill_iters_;

    if (g.min_input_count.empty()) {
        g.min_input_count.resize(max_prefill_iters_);
    }
    g.min_input_count.pop_front();
    g.min_input_count.push_back(input_count);
    /// TODO: sub-optimal when there are inactive sequences due to memory constraint
    for (auto& x : g.min_input_count) {
        x = std::max(x, input_count);
    }

    input_count = std::max(g.min_input_count.front() + batch_size, num_tokens_per_iter_);
    input_count = std::min(input_count, max_context_token_num_);
    // update max input count
    g.max_input_count1 = input_count;
    g.max_input_count2 = std::min(input_count + extra_tokens_per_iter_, max_context_token_num_);
}

template<typename T>
void LlamaBatch<T>::Initialize(GenerationState& g)
{
    NvtxScope                                scope("initialize");
    std::vector<const Sequence*>             sequences;
    std::vector<Sequence::Status>            status;
    std::vector<uint64_t>                    priorities;
    std::vector<int>                         context_lengths;
    std::vector<std::pair<BatchState*, int>> coords;

    // count the holes introduced by finished requests in from previous iteration or stop requests from
    // current iteration
    int holes{};
    int active_holes{};
    for (int i = 0; i < state_->size; ++i) {
        if (!state_->requests[i]) {
            ++holes;
            if (i < state_->active_size) {
                ++active_holes;
            }
        }
    }

    auto process = [&](BatchState* state) {
        for (int i = 0; i < state->size; ++i) {
            if (auto& r = state->requests[i]) {
                sequences.push_back(state->sequences[i]);
                status.push_back(state->sequences[i]->status);
                priorities.push_back(r->unique_id);
                context_lengths.push_back(state->h_context_length[i]);
                coords.emplace_back(state, i);
            }
        }
    };

    process(state_);
    process(incoming_);

    auto adjust = [this, &g](const Sequences&        sequences,
                             const std::vector<int>& context_length) -> std::pair<int, int> {
        AdjustMaxInputCount(g, sequences, context_length);
        return {g.max_input_count1, g.max_input_count2};
    };

    // TM_LOG_INFO("max_input_count %d", max_input_count);
    auto outcome = sequence_manager_->Materialize(sequences, context_lengths, priorities, step_length_, adjust);

    if (outcome.allocation || outcome.swap_in || outcome.swap_out) {
        dbg(outcome);
    }

    bool exchange = outcome.swap_in + outcome.swap_out > 0;

    std::vector<int> idxs(sequences.size());
    std::iota(idxs.begin(), idxs.end(), 0);

    if (exchange || holes || incoming_->size) {
        // put active ones first
        auto active_end = std::stable_partition(idxs.begin(), idxs.end(), [&](int idx) {
            return sequences[idx]->status == Sequence::kActive;  // current status
        });

        // all blocks are not enough to hold a single sequence
        if (!sequences.empty()) {
            FT_CHECK_WITH_INFO(active_end != idxs.begin(), "No enough blocks.");
        }

        // move the partial seq to the back
        auto partial_beg = std::stable_partition(idxs.begin(), active_end, [&](int i) {
            return sequences[i]->cache_len + sequences[i]->input_length == context_lengths[i];
        });
        FT_CHECK(active_end - partial_beg <= 1);

        auto swapin_beg = std::stable_partition(idxs.begin(), partial_beg, [&](int i) {
            return status[i] == Sequence::kActive;  // past status
        });

        // sort swap-ins according to input length
        if (swapin_beg != partial_beg) {
            std::stable_sort(swapin_beg, partial_beg, [&](int i, int j) {
                return sequences[i]->input_length < sequences[j]->input_length;
            });
        }

        // Copy sequence states to back buffer
        FT_CHECK(back_->size == 0 && back_->active_size == 0);
        std::vector<std::tuple<BatchState*, BatchState*, int, int>> cpys;
        for (const auto& i : idxs) {
            auto& s = *sequences[i];
            if (s.status == Sequence::kActive) {
                ++back_->active_size;
            }
            cpys.emplace_back(coords[i].first, back_, coords[i].second, back_->size++);
        }
        CopyState(cpys);
        // Swap the buffers
        std::swap(state_, back_);

        ClearState(*back_);
        ClearState(*incoming_);
    }

    FT_CHECK(state_->size <= max_batch_size_);

    /// Update block ptrs when there were
    //  1. swap-in or swap-out
    //  2. holes in the active buffer
    //  3. new allocations (for existing active sequences)
    if (exchange || active_holes || outcome.allocation) {
        // Prepare intermediate buffers
        h_cu_block_counts_[0] = 0;

        auto block_ptrs = h_block_ptrs_;

        const int batch_size = state_->active_size;

        for (int i = 0; i < batch_size; ++i) {
            const auto& seq = *state_->sequences[i];

            // cumulative num of blocks
            h_cu_block_counts_[i + 1] = h_cu_block_counts_[i] + seq.blocks.size();

            FT_CHECK_WITH_INFO(h_cu_block_counts_[i + 1] <= sequence_manager_->max_block_count(),
                               std::to_string(h_cu_block_counts_[i + 1]));

            block_ptrs = std::transform(seq.blocks.cbegin(), seq.blocks.cend(), block_ptrs, [&](int block_id) {
                return reinterpret_cast<uintptr_t>(sequence_manager_->GetBlockPtr(block_id));
            });
        }

        static_assert(sizeof(uintptr_t) == sizeof(void*));

        Copy(h_cu_block_counts_, batch_size + 1, cu_block_counts_);
        Copy(h_block_ptrs_, h_cu_block_counts_[batch_size], block_ptrs_);
        // Copy(h_k_block_ptrs_, h_cu_block_counts_[batch_size], k_block_ptrs_);
        // Copy(h_v_block_ptrs_, h_cu_block_counts_[batch_size], v_block_ptrs_);
    }

    const int batch_size = state_->active_size;

    // check if the last sequence is partial
    int partial     = 0;
    int partial_len = -1;
    if (state_->active_size) {
        const int i = state_->active_size - 1;
        partial = state_->sequences[i]->cache_len + state_->sequences[i]->input_length != state_->h_context_length[i];
        if (partial) {
            // backup full context length of partial
            partial_len = state_->h_context_length[i];
            // replace with partial context length
            state_->h_context_length[i] = state_->sequences[i]->cache_len + state_->sequences[i]->input_length;
        }
    }

    const int max_context_len = *std::max_element(state_->h_context_length, state_->h_context_length + batch_size);

    std::vector<uint64_t> unique_ids(batch_size);
    for (int i = 0; i < batch_size; ++i) {
        unique_ids[i] = state_->requests[i]->unique_id;
    }

    // Real-time context length that will change during generation
    Copy(state_->h_context_length, batch_size, context_length_buf_);
    Copy(state_->h_finished, batch_size, finished_buf_);
    Copy(state_->h_rope_theta, batch_size, rope_theta_);

    bool skip_init_sampling = std::equal(g.unique_ids.begin(),  //
                                         g.unique_ids.end() - g.partial,
                                         unique_ids.begin(),
                                         unique_ids.end() - partial);

    g.partial                = partial;
    g.partial_context_legnth = partial_len;
    g.unique_ids             = std::move(unique_ids);
    g.finished_count         = 0;

    if (!skip_init_sampling) {
        g.max_init_ctx_len = max_context_len;
        g.step             = max_context_len;
        InitializeSampling(g);
    }
}

template<typename T>
void LlamaBatch<T>::CopyState(const std::vector<std::tuple<BatchState*, BatchState*, int, int>>& desc)
{
    if (desc.empty()) {
        return;
    }

    std::vector<int> idxs(desc.size());
    std::iota(idxs.begin(), idxs.end(), 0);

    std::sort(idxs.begin(), idxs.end(), [&](int i, int j) { return desc[i] < desc[j]; });

    auto get_signature = [&](int i) -> std::pair<BatchState*, BatchState*> {
        return std::make_pair(std::get<0>(desc[idxs[i]]), std::get<1>(desc[idxs[i]]));
    };

    std::vector<int> offsets;
    auto             current = get_signature(0);
    offsets.push_back(0);
    for (int i = 0; i < idxs.size(); ++i) {
        if (auto signature = get_signature(i); signature != current) {
            current = signature;
            offsets.push_back(i);
        }
    }
    offsets.push_back(idxs.size());

    for (int bi = 1; bi < offsets.size(); ++bi) {
        int beg = offsets[bi - 1];
        int end = offsets[bi];

        if (beg == end) {
            continue;
        }

        auto [s, d] = get_signature(beg);

        std::vector<int> s_idx;
        std::vector<int> d_idx;
        for (int i = beg; i < end; ++i) {
            s_idx.push_back(std::get<2>(desc[idxs[i]]));
            d_idx.push_back(std::get<3>(desc[idxs[i]]));
        }

        IndexedCopy(s_idx,
                    d_idx,
                    std::tuple{s->output_ids, d->output_ids, session_len_},
                    std::tuple{s->curand_state, d->curand_state, 1},
                    std::tuple{s->input_ids, d->input_ids, session_len_});
    }

    for (const auto& [s, d, si, di] : desc) {
        d->h_prompt_length[di]  = s->h_prompt_length[si];
        d->h_context_length[di] = s->h_context_length[si];
        d->h_finished[di]       = s->h_finished[si];
        d->h_rope_theta[di]     = s->h_rope_theta[si];
        d->seq_len_limit[di]    = s->seq_len_limit[si];
        d->sequences[di]        = s->sequences[si];
        d->requests[di]         = s->requests[si];
    }
}

template<typename T>
void LlamaBatch<T>::AllocateBuffer(size_t batch_size, size_t session_len)
{
    TM_LOG_DEBUG(__PRETTY_FUNCTION__);
    const size_t batchxbeam = batch_size;

    const size_t hidden_units      = model_->hidden_units_;
    const size_t vocab_size        = model_->vocab_size_padded_;
    const size_t head_dim          = model_->size_per_head_;
    const size_t local_kv_head_num = model_->local_kv_head_num_;
    // +1 padding, BlockIterator does not use predicate
    const size_t max_block_count = sequence_manager_->max_block_count() + 1;

    if (model_->lora_params_.policy == LoraPolicy::kPlora) {
        lora_mask_buf_ = (int*)allocator_->reMalloc(lora_mask_buf_, sizeof(int) * max_context_token_num_, false);
        size_t sz      = sizeof(T) * max_context_token_num_ * (hidden_units + model_->lora_params_.max_wo_r);
        context_decoder_output_buf_ = (T*)allocator_->reMalloc(context_decoder_output_buf_, sz, false);
    }
    else {
        context_decoder_output_buf_ = (T*)allocator_->reMalloc(
            context_decoder_output_buf_, sizeof(T) * max_context_token_num_ * hidden_units, false);
    }

    context_decoder_input_buf_ =
        (T*)allocator_->reMalloc(context_decoder_input_buf_, sizeof(T) * max_context_token_num_ * hidden_units, false);
    context_decoder_ids_buf_ =
        (int*)allocator_->reMalloc(context_decoder_ids_buf_, sizeof(int) * max_context_token_num_, false);

    decoder_input_buf_  = (T*)allocator_->reMalloc(decoder_input_buf_, sizeof(T) * batchxbeam * hidden_units, false);
    decoder_output_buf_ = (T*)allocator_->reMalloc(decoder_output_buf_, sizeof(T) * batchxbeam * hidden_units, false);

    input_ids_buf_       = (int*)allocator_->reMalloc(input_ids_buf_, sizeof(int) * batchxbeam * session_len, true);
    input_length_buf_    = (int*)allocator_->reMalloc(input_length_buf_, sizeof(int) * batchxbeam);
    context_length_buf_  = (int*)allocator_->reMalloc(context_length_buf_, sizeof(int) * batchxbeam);
    init_context_length_ = (int*)allocator_->reMalloc(init_context_length_, sizeof(int) * batchxbeam);

    sequence_lengths_ = (int*)allocator_->reMalloc(sequence_lengths_, sizeof(int) * batchxbeam, false);

    cu_block_counts_ = (int*)allocator_->reMalloc(cu_block_counts_, sizeof(int) * (batch_size + 1));
    block_ptrs_      = (uintptr_t*)allocator_->reMalloc(block_ptrs_, sizeof(uintptr_t) * max_block_count);

    logits_buf_       = (float*)allocator_->reMalloc(logits_buf_, sizeof(float) * batchxbeam * vocab_size, false);
    local_logits_buf_ = (float*)allocator_->reMalloc(local_logits_buf_, sizeof(float) * batchxbeam * vocab_size, false);

    sampled_logprobs_ =
        (float*)allocator_->reMalloc(sampled_logprobs_, sizeof(float) * batchxbeam * kMaxLogProb, false);
    sampled_indexes_ =
        (uint32_t*)allocator_->reMalloc(sampled_indexes_, sizeof(uint32_t) * batchxbeam * kMaxLogProb, false);
    sampled_nums_ = (uint32_t*)allocator_->reMalloc(sampled_nums_, sizeof(uint32_t) * batchxbeam, false);

    token_ids_buf_ = (int*)allocator_->reMalloc(token_ids_buf_, sizeof(int) * batchxbeam * session_len * 2, true);

    finished_buf_  = (bool*)allocator_->reMalloc(finished_buf_, sizeof(bool) * batchxbeam, false);
    seq_limit_len_ = (uint32_t*)allocator_->reMalloc(seq_limit_len_, sizeof(uint32_t) * batch_size, false);

    rope_theta_ = (float*)allocator_->reMalloc(rope_theta_, sizeof(float) * batch_size, false);

    medusa_inited_hidden_states_buf_ = (T*)allocator_->reMalloc(
        medusa_inited_hidden_states_buf_, sizeof(T) * medusa_input_length_ * batch_size * hidden_units, true);
    medusa_inited_input_ids_buf_ = (int*)allocator_->reMalloc(
        medusa_inited_input_ids_buf_, sizeof(int) * medusa_input_length_ * batch_size, true);


    medusa_all_hidden_states_buf_ = (T*)allocator_->reMalloc(
        medusa_all_hidden_states_buf_, sizeof(T) * medusa_input_length_ * batch_size * hidden_units, true);
    medusa_verified_hidden_states_buf_ =
        (T*)allocator_->reMalloc(medusa_verified_hidden_states_buf_, sizeof(T) * batch_size * hidden_units, true);


    medusa_token_ids_buf_ =
        (int*)allocator_->reMalloc(medusa_token_ids_buf_, sizeof(int) * batchxbeam * session_len * 2, true);
    medusa_finished_buf_     = (bool*)allocator_->reMalloc(medusa_finished_buf_, sizeof(bool) * batchxbeam, true);
    medusa_sequence_lengths_ = (int*)allocator_->reMalloc(medusa_sequence_lengths_, sizeof(int) * batchxbeam, true);

    medusa_ref_output_ids_buf_ = (int*)allocator_->reMalloc(medusa_ref_output_ids_buf_, sizeof(int) * batchxbeam * medusa_input_length_, true);
    medusa_max_match_length_buf_ =
        (int*)allocator_->reMalloc(medusa_max_match_length_buf_, sizeof(int) * batchxbeam, true);
    h_medusa_max_match_length_buf_ =
        (int*)allocator_->reMalloc(h_medusa_max_match_length_buf_, sizeof(int) * batchxbeam, true, true);
    medusa_max_match_idx_buf_ =
        (int*)allocator_->reMalloc(medusa_max_match_idx_buf_, sizeof(int) * batchxbeam, true);
    h_medusa_max_match_idx_buf_ =
        (int*)allocator_->reMalloc(h_medusa_max_match_idx_buf_, sizeof(int) * batchxbeam, true, true);
    h_medusa_last_match_idx_buf_ =
        (int*)allocator_->reMalloc(h_medusa_last_match_idx_buf_, sizeof(int) * batchxbeam, true, true);
    h_pseudo_inputs_buf_ =
        (int*)allocator_->reMalloc(h_pseudo_inputs_buf_, sizeof(int) * batchxbeam * medusa_input_length_, true, true);
    h_medusa_preds_batched_buf_ =
        (int*)allocator_->reMalloc(h_medusa_preds_batched_buf_, sizeof(int) * batchxbeam * medusa_num_heads_ * medusa_top_k_, true, true);
    medusa_topk_output_ids_buf_ =
        (int*)allocator_->reMalloc(medusa_topk_output_ids_buf_, sizeof(int) * medusa_num_heads_ * batchxbeam * medusa_top_k_, true);

    medusa_verified_length_ = (int*)allocator_->reMalloc(medusa_verified_length_, sizeof(int) * batchxbeam, true);
    medusa_verified_packed_path_ = (int*)allocator_->reMalloc(medusa_verified_packed_path_, sizeof(int) * batchxbeam * (1 + medusa_num_heads_), true);
    h_medusa_verified_packed_path_ = (int*)allocator_->reMalloc(h_medusa_verified_packed_path_, sizeof(int) * batchxbeam * (1 + medusa_num_heads_), true, true);

    last_input_ids_buf_ =
        (int*)allocator_->reMalloc(last_input_ids_buf_, sizeof(int) * batchxbeam * session_len_, true);

    medusa_input_tokens_buf_ = 
        (int*)allocator_->reMalloc(medusa_input_tokens_buf_, sizeof(int) * batchxbeam * medusa_path_num_ * (1 + medusa_num_heads_), true);
    medusa_output_tokens_buf_ = 
        (int*)allocator_->reMalloc(medusa_output_tokens_buf_, sizeof(int) * batchxbeam * medusa_path_num_ * (1 + medusa_num_heads_), true);
    medusa_each_path_len_buf_ = 
        (int*)allocator_->reMalloc(medusa_each_path_len_buf_, sizeof(int) * medusa_path_num_, true);

    d_medusa_ti_ =
        (int*)allocator_->reMalloc(d_medusa_ti_, sizeof(int) * medusa_input_length_, true);
    d_medusa_mask_ =
        (int*)allocator_->reMalloc(d_medusa_mask_, sizeof(int) * medusa_input_length_ * medusa_input_length_, true);
    d_enable_medusa_ =
        (int*)allocator_->reMalloc(d_enable_medusa_, sizeof(int) * batch_size, true);

    is_allocate_buffer_ = true;
}

template<typename T>
void LlamaBatch<T>::AllocatePersistantBuffer(size_t max_batch_size)
{
    d_stop_words_ =
        (int*)allocator_->reMalloc(d_stop_words_, sizeof(int) * max_batch_size * 2 * kMaxStopBadWordsLen, true);
    d_bad_words_ =
        (int*)allocator_->reMalloc(d_bad_words_, sizeof(int) * max_batch_size * 2 * kMaxStopBadWordsLen, true);
    h_stop_words_ =
        (int*)allocator_->reMalloc(h_stop_words_, sizeof(int) * max_batch_size * 2 * kMaxStopBadWordsLen, true, true);
    h_bad_words_ =
        (int*)allocator_->reMalloc(h_bad_words_, sizeof(int) * max_batch_size * 2 * kMaxStopBadWordsLen, true, true);

    h_min_length_    = (int*)allocator_->reMalloc(h_min_length_, sizeof(int) * max_batch_size, true, true);
    h_runtime_top_k_ = (int*)allocator_->reMalloc(h_runtime_top_k_, sizeof(int) * max_batch_size, true, true);
    h_runtime_top_p_ = (float*)allocator_->reMalloc(h_runtime_top_p_, sizeof(float) * max_batch_size, true, true);
    h_temperature_   = (float*)allocator_->reMalloc(h_temperature_, sizeof(float) * max_batch_size, true, true);
    h_repetition_penalty_ =
        (float*)allocator_->reMalloc(h_repetition_penalty_, sizeof(float) * max_batch_size, true, true);

    h_random_seed_ = (unsigned long long*)allocator_->reMalloc(
        h_random_seed_, sizeof(unsigned long long) * max_batch_size, true, true);
    d_random_seed_ = (unsigned long long*)allocator_->reMalloc(
        d_random_seed_, sizeof(unsigned long long) * max_batch_size, true, false);

    h_curand_state_ =
        (curandState_t*)allocator_->reMalloc(h_curand_state_, sizeof(curandState_t) * max_batch_size, true, true);
    d_curand_state_ =
        (curandState_t*)allocator_->reMalloc(d_curand_state_, sizeof(curandState_t) * max_batch_size, true, false);

    d_end_ids_buf_ = (int*)allocator_->reMalloc(d_end_ids_buf_, sizeof(int) * max_batch_size, false);
    h_end_ids_buf_ = (int*)allocator_->reMalloc(h_end_ids_buf_, sizeof(int) * max_batch_size, false, true);

    sampling_params_ = {
        {"stop_words_list", (std::byte*)h_stop_words_, (std::byte*)d_stop_words_},
        {"bad_words_list", (std::byte*)h_bad_words_, (std::byte*)d_bad_words_},
        {"min_length", (std::byte*)h_min_length_, nullptr},
        {"runtime_top_k", (std::byte*)h_runtime_top_k_, nullptr},
        {"runtime_top_p", (std::byte*)h_runtime_top_p_, nullptr},
        {"temperature", (std::byte*)h_temperature_, nullptr},
        {"repetition_penalty", (std::byte*)h_repetition_penalty_, nullptr},
    };

    for (auto& s : states_) {
        s.output_ids = (int*)allocator_->reMalloc(s.output_ids, sizeof(int) * max_batch_size * session_len_, true);
        s.input_ids  = (int*)allocator_->reMalloc(s.input_ids, sizeof(int) * max_batch_size * session_len_, true);
        s.curand_state =
            (curandState_t*)allocator_->reMalloc(s.curand_state, sizeof(curandState_t) * max_batch_size, true);
    }

    const size_t max_block_count = sequence_manager_->max_block_count();

    {
        NcclGuard barrier(model_->tensor_para_, stream_, true);
        h_input_ids_buf_ =
            (int*)allocator_->reMalloc(h_input_ids_buf_, sizeof(int) * max_batch_size * session_len_, false, true);
        h_input_length_buf_ =
            (int*)allocator_->reMalloc(h_input_length_buf_, sizeof(int) * max_batch_size, false, true);

        h_cu_block_counts_ =
            (int*)allocator_->reMalloc(h_cu_block_counts_, sizeof(int) * (max_batch_size + 1), false, true);
        h_block_ptrs_ =
            (uintptr_t*)allocator_->reMalloc(h_block_ptrs_, sizeof(uintptr_t) * max_block_count, false, true);

        for (auto& s : states_) {
            s.h_prompt_length =
                (int*)allocator_->reMalloc(s.h_prompt_length, sizeof(int) * max_batch_size, false, true);
            s.h_context_length =
                (int*)allocator_->reMalloc(s.h_context_length, sizeof(int) * max_batch_size, false, true);
            s.h_finished   = (bool*)allocator_->reMalloc(s.h_finished, sizeof(bool) * max_batch_size * 2, false, true);
            s.h_rope_theta = (float*)allocator_->reMalloc(s.h_rope_theta, sizeof(float) * max_batch_size, false, true);
        }

        h_seq_limit_len_ =
            (uint32_t*)allocator_->reMalloc(h_seq_limit_len_, sizeof(uint32_t) * max_batch_size, false, true);

        h_output_ids_ =
            (int*)allocator_->reMalloc(h_output_ids_, sizeof(int) * max_batch_size * session_len_, false, true);
    }

    h_sampled_logprobs_ =
        (float*)allocator_->reMalloc(h_sampled_logprobs_, sizeof(float) * max_batch_size * kMaxLogProb, false, true);
    h_sampled_indexes_ = (uint32_t*)allocator_->reMalloc(
        h_sampled_indexes_, sizeof(uint32_t) * max_batch_size * kMaxLogProb, false, true);
    h_sampled_nums_ = (uint32_t*)allocator_->reMalloc(h_sampled_nums_, sizeof(uint32_t) * max_batch_size, false, true);

    is_allocate_persistant_buffer_ = true;
}

template<typename T>
void LlamaBatch<T>::FreeBuffer()
{
    TM_LOG_DEBUG(__PRETTY_FUNCTION__);
    if (is_allocate_buffer_) {
        allocator_->free((void**)&context_decoder_input_buf_);
        allocator_->free((void**)&context_decoder_output_buf_);
        allocator_->free((void**)&context_decoder_ids_buf_);
        allocator_->free((void**)&lora_mask_buf_);

        allocator_->free((void**)&decoder_input_buf_);
        allocator_->free((void**)&decoder_output_buf_);

        allocator_->free((void**)&input_ids_buf_);
        allocator_->free((void**)&input_length_buf_);
        allocator_->free((void**)&context_length_buf_);
        allocator_->free((void**)&init_context_length_);

        allocator_->free((void**)&sequence_lengths_);

        allocator_->free((void**)&cu_block_counts_);
        allocator_->free((void**)&block_ptrs_);

        allocator_->free((void**)&logits_buf_);
        allocator_->free((void**)&local_logits_buf_);

        if (local_context_logits_buf_) {
            allocator_->free((void**)&local_context_logits_buf_);
        }
        if (context_logits_buf_) {
            allocator_->free((void**)&context_logits_buf_);
        }

        allocator_->free((void**)&token_ids_buf_);

        allocator_->free((void**)&d_end_ids_buf_);
        allocator_->free((void**)&h_end_ids_buf_, true);

        allocator_->free((void**)&finished_buf_);
        allocator_->free((void**)&seq_limit_len_);

        allocator_->free((void**)&rope_theta_);

        allocator_->free((void**)&sampled_logprobs_);
        allocator_->free((void**)&sampled_indexes_);
        allocator_->free((void**)&sampled_nums_);

        allocator_->free((void**)&medusa_all_hidden_states_buf_);
        allocator_->free((void**)&medusa_verified_hidden_states_buf_);

        allocator_->free((void**)&medusa_inited_hidden_states_buf_);
        allocator_->free((void**)&medusa_inited_input_ids_buf_);

        if (medusa_logits_buf_) {
            allocator_->free((void**)&medusa_logits_buf_);
        }
        if (medusa_local_logits_buf_) {
            allocator_->free((void**)&medusa_local_logits_buf_);
        }

        allocator_->free((void**)&medusa_token_ids_buf_);
        allocator_->free((void**)&medusa_finished_buf_);
        allocator_->free((void**)&medusa_sequence_lengths_);

        allocator_->free((void**)&medusa_ref_output_ids_buf_);
        allocator_->free((void**)&medusa_max_match_length_buf_);
        allocator_->free((void**)&medusa_max_match_idx_buf_);
        allocator_->free((void**)&h_medusa_max_match_length_buf_, true);
        allocator_->free((void**)&h_medusa_max_match_idx_buf_, true);
        allocator_->free((void**)&h_medusa_last_match_idx_buf_, true);
        allocator_->free((void**)&h_pseudo_inputs_buf_, true);
        allocator_->free((void**)&h_medusa_preds_batched_buf_, true);

        allocator_->free((void**)&medusa_topk_output_ids_buf_);

        allocator_->free((void**)&medusa_verified_length_);
        allocator_->free((void**)&medusa_verified_packed_path_);
        allocator_->free((void**)&h_medusa_verified_packed_path_, true);

        allocator_->free((void**)&last_input_ids_buf_);

        allocator_->free((void**)&medusa_input_tokens_buf_);
        allocator_->free((void**)&medusa_output_tokens_buf_);
        allocator_->free((void**)&medusa_each_path_len_buf_);

        allocator_->free((void**)&d_medusa_ti_);
        allocator_->free((void**)&d_medusa_mask_);
        allocator_->free((void**)&d_enable_medusa_);

        is_allocate_buffer_ = false;
    }

    if (is_allocate_persistant_buffer_) {

        allocator_->free((void**)&d_stop_words_);
        allocator_->free((void**)&h_stop_words_, true);
        allocator_->free((void**)&d_bad_words_);
        allocator_->free((void**)&h_bad_words_, true);
        allocator_->free((void**)&d_random_seed_);
        allocator_->free((void**)&h_random_seed_, true);
        allocator_->free((void**)&d_curand_state_);
        allocator_->free((void**)&h_curand_state_, true);

        for (auto& s : states_) {
            allocator_->free((void**)&s.h_context_length, true);
            allocator_->free((void**)&s.h_finished, true);
            allocator_->free((void**)&s.h_rope_theta, true);
            allocator_->free((void**)&s.output_ids);
            allocator_->free((void**)&s.input_ids);
            allocator_->free((void**)&s.curand_state);
        }
        allocator_->free((void**)&h_cu_block_counts_, true);
        allocator_->free((void**)&h_block_ptrs_, true);
        allocator_->free((void**)&h_input_ids_buf_, true);
        allocator_->free((void**)&h_input_length_buf_, true);
        allocator_->free((void**)&h_seq_limit_len_, true);

        allocator_->free((void**)&h_output_ids_, true);

        allocator_->free((void**)&h_sampled_logprobs_);
        allocator_->free((void**)&h_sampled_indexes_);
        allocator_->free((void**)&h_sampled_nums_);

        allocator_->free((void**)&medusa_topk_output_ids_buf_);

        is_allocate_persistant_buffer_ = false;
    }
}

template<typename T>
LlamaBatch<T>::LlamaBatch(
    const EngineParams& params, int cache_block_seq_len, int quant_policy, LlamaV2<T>* model, int medusa_num_heads):
    max_batch_size_(params.max_batch_size),
    max_context_token_num_(params.max_context_token_num),
    session_len_(params.session_len),
    rank_(model->tensor_para_.rank_),
    debug_(model->debug_),
    step_length_(params.step_length),
    model_(model),
    data_type_(getTensorType<T>()),
    num_tokens_per_iter_(params.num_tokens_per_iter),
    extra_tokens_per_iter_(params.extra_tokens_per_iter),
    max_prefill_iters_(params.max_prefill_iters),
    medusa_num_heads_(medusa_num_heads),
    medusa_enable_(medusa_num_heads != 0)
{
    // medusa_enable_ = false;
    stream_         = model_->stream_;
    allocator_      = model_->allocator_;
    cublas_wrapper_ = model_->cublas_wrapper_;

    const int elem_bits = quant_policy ? quant_policy : bitsof<T>;

    auto get_free_size = [&] {
        return GetSyncFreeMemSize(*model_->shared_state_->barrier, model_->shared_state_->free_size);
    };

    SequenceManager::BlockConfig block_config{
        (int)model_->size_per_head_,
        (int)model_->local_kv_head_num_,
        cache_block_seq_len,
        elem_bits == bitsof<T> ? 0 : bitsof<T>,
        elem_bits,
    };

    sequence_manager_.reset(new SequenceManager{model_->num_layer_,
                                                block_config,
                                                params.cache_max_block_count,
                                                params.cache_chunk_size,
                                                model->tensor_para_.rank_,
                                                allocator_,
                                                get_free_size});

    const size_t max_session_len = sequence_manager_->max_block_count() * cache_block_seq_len;
    if (max_session_len < session_len_) {
        if (rank_ == 0) {
            TM_LOG_WARNING("No enough blocks for `session_len` (%d), `session_len` truncated to %d.",
                           session_len_,
                           max_session_len);
        }
        session_len_ = max_session_len;
    }

    FT_CHECK(max_context_token_num_ >= session_len_);

    for (auto& s : states_) {
        s.requests.resize(max_batch_size_);
        s.sequences.resize(max_batch_size_);
        s.seq_len_limit.resize(max_batch_size_);
    }

    state_    = &states_[0];
    back_     = &states_[1];
    incoming_ = &states_[2];

    // todo: fix this to config.
    std::string medusa_path_filename = "/workdir/lmdeploy_official/src/turbomind/models/medusa_plugin/medusa_choices.info";
    std::string aim_model_name = "mc_sim_7b_63";
    medusa_utils_ = std::make_unique<MedusaUtils>(medusa_path_filename, aim_model_name);
    medusa_utils_->getInputLen(medusa_input_length_); // 64
    medusa_utils_->getPathNum(medusa_path_num_);

    AllocateBuffer(max_batch_size_, session_len_);
    AllocatePersistantBuffer(max_batch_size_);
}

template<typename T>
void LlamaBatch<T>::InitializeSampling(const GenerationState& g)
{
    NvtxScope _("InitSampling");
    const int batch_size = state_->active_size - g.partial;
    if (batch_size == 0) {
        return;
    }

    // Context length at initialization, will stay constant until re-initialziation
    Copy(context_length_buf_, batch_size, init_context_length_);

    Copy(context_length_buf_, batch_size, sequence_lengths_);
    // `sequence_lengths_` will be increased by dynamic decode
    // note that in decoder and in output "sequence length" has different semantic
    // - in decoder it means length of sequence that has kv cache already computed
    // - in output it means length of all tokens (the last generated token does not have k/v cache computed yet)
    invokePlusScalar(sequence_lengths_, -1, batch_size, stream_);
    sync_check_cuda_error();

    Clear(token_ids_buf_, batch_size * session_len_);
    invokeTransposeAxis01(token_ids_buf_, state_->output_ids, batch_size, session_len_, 1, stream_);
    sync_check_cuda_error();

    // token_ids_buf_[s, b]
    // ABCDe            ABCDe     e
    // ABCDEFGHIJk      ABCDEFGHIJk
    // ABCDEFGHi    ->  ABCDEFGHi i
    // ABCDEFGh         ABCDEFGh  h
    // ABCd             ABCd      d
    invokePadLastTokenIds(token_ids_buf_, init_context_length_, g.max_init_ctx_len, batch_size, stream_);
    sync_check_cuda_error();

    // seq_limit_len_, will be compared to `step` instead of `sequence_length`, so padding len should be accounted for
    for (int i = 0; i < batch_size; ++i) {
        h_seq_limit_len_[i] = state_->seq_len_limit[i] + (g.max_init_ctx_len - state_->h_context_length[i]);
    }
    Copy(h_seq_limit_len_, batch_size, seq_limit_len_);

    TensorMap inputs;
    for (const auto& [name, h_ptr, d_ptr] : sampling_params_) {
        // find an exemplar that matches the param name
        const Tensor* ptr{};
        for (int i = 0; i < batch_size; ++i) {
            if (state_->requests[i]->inputs[rank_].isExist(name)) {
                ptr = &state_->requests[i]->inputs[rank_].at(name);
                break;
            }
        }
        // fill the batch of the param
        if (ptr) {
            const auto& ref   = *ptr;
            auto        shape = ref.shape;
            FT_CHECK(shape[0] == 1);
            shape[0]                = batch_size;
            const int size_in_bytes = ref.sizeBytes();

            int max_list_length = 0;
            if (name == "bad_words_list" || name == "stop_words_list") {
                for (int i = 0; i < batch_size; ++i) {
                    if (state_->requests[i]->inputs[rank_].isExist(name)) {
                        Tensor& src = state_->requests[i]->inputs[rank_].at(name);
                        FT_CHECK(src.shape.size() == 3 && src.shape[1] == 2 && src.shape[2] <= kMaxStopBadWordsLen);
                        max_list_length = std::max(max_list_length, (int)src.shape[2]);
                    }
                }
                std::fill_n((int*)h_ptr, batch_size * 2 * max_list_length, -1);
                shape[2] = max_list_length;
            }
            else {
                memset(h_ptr, 0, size_in_bytes * batch_size);
            }
            for (int i = 0; i < batch_size; ++i) {
                FT_CHECK(state_->requests[i] != nullptr);
                if (state_->requests[i]->inputs[rank_].isExist(name)) {
                    Tensor& src = state_->requests[i]->inputs[rank_].at(name);
                    if (name == "bad_words_list" || name == "stop_words_list") {
                        int list_length = src.shape[2];
                        std::copy_n(src.getPtr<std::byte>(),
                                    sizeof(int) * list_length,
                                    h_ptr + i * sizeof(int) * 2 * max_list_length);
                        std::copy_n(src.getPtr<std::byte>() + sizeof(int) * list_length,
                                    sizeof(int) * list_length,
                                    h_ptr + i * sizeof(int) * 2 * max_list_length + sizeof(int) * max_list_length);
                    }
                    else {
                        FT_CHECK(ref.shape == src.shape);
                        std::copy_n(src.getPtr<std::byte>(), size_in_bytes, h_ptr + size_in_bytes * i);
                    }
                }
            }
            if (d_ptr) {
                if (name == "bad_words_list" || name == "stop_words_list") {
                    Copy(h_ptr, batch_size * sizeof(int) * 2 * max_list_length, d_ptr);
                }
                else {
                    Copy(h_ptr, batch_size * size_in_bytes, d_ptr);
                }
            }
            inputs.insert({name, {d_ptr ? MEMORY_GPU : MEMORY_CPU, ref.type, shape, d_ptr ? d_ptr : h_ptr}});
            if (debug_ && rank_ == 0) {
                TM_LOG_INFO("[initializeSampling] %s", format({name, inputs.at(name)}).c_str());
            }
        }
    }

    // MinLengthPenalty
    if (inputs.isExist("min_length")) {
        inputs.insert({"prompt_length", {MEMORY_CPU, TYPE_INT32, {(size_t)batch_size}, state_->h_prompt_length}});
        inputs.insert({"context_length", {MEMORY_CPU, TYPE_INT32, {(size_t)batch_size}, state_->h_context_length}});
    }

    // init for eos
    std::fill_n(h_end_ids_buf_, batch_size, model_->end_id_);
    Copy(h_end_ids_buf_, batch_size, d_end_ids_buf_);
    inputs.insert({"end_id", {MEMORY_GPU, TYPE_INT32, {(size_t)batch_size}, d_end_ids_buf_}});

    inputs_ = std::move(inputs);

    model_->dynamic_decode_layer_->setup(batch_size, 1, &inputs_);

    TensorMap outputs;
    for (int i = 0; i < batch_size; i++) {
        if (state_->requests[i]->inputs[rank_].isExist("logprobs")) {
            outputs.insert(
                {"sampled_logprobs", {MEMORY_GPU, TYPE_FP32, {(size_t)batch_size, 1, kMaxLogProb}, sampled_logprobs_}});
            outputs.insert(
                {"sampled_indexes", {MEMORY_GPU, TYPE_UINT32, {(size_t)batch_size, 1, kMaxLogProb}, sampled_indexes_}});
            outputs.insert({"sampled_nums", {MEMORY_GPU, TYPE_UINT32, {(size_t)batch_size, 1}, sampled_nums_}});

            break;
        }
    }
    outputs_ = std::move(outputs);
}

template<typename T>
void LlamaBatch<T>::OutputContextLogits(T*                                  context_decoder_output,
                                        const std::vector<int>&             indices,
                                        const std::vector<int>&             lengths,
                                        const std::vector<const Sequence*>& sequences)
{
    std::vector<float*> output_logits;
    int                 num_token = 0;
    {
        bool is_return_logits = false;
        for (int k = 0; k < indices.size(); ++k) {
            auto& request = state_->requests[indices[k]];
            auto  logits  = request->outputs[rank_].getPtr<float>("logits", nullptr);
            if (logits && sequences[k]->cache_len + lengths[k] <= sequences[k]->tokens.size()) {
                logits = nullptr;
            }
            output_logits.push_back(logits);
            num_token += lengths[k];
            if (output_logits.back()) {
                is_return_logits = true;
            }
        }
        if (!is_return_logits) {
            return;
        }
    }

    if (context_logits_buf_ == nullptr) {
        NcclGuard guard(model_->tensor_para_, stream_, true);
        context_logits_buf_ =
            (float*)allocator_->malloc(sizeof(float) * model_->vocab_size_padded_ * max_context_token_num_);
        const auto tp = model_->tensor_para_.world_size_;
        if (tp > 1) {
            FT_CHECK(model_->vocab_size_padded_ % tp == 0);
            const auto local_vocab_size = model_->vocab_size_padded_ / tp;
            local_context_logits_buf_ =
                (float*)allocator_->malloc(sizeof(float) * model_->vocab_size_padded_ * max_context_token_num_);
        }
    }

    model_->postDecodeEmbedding(context_logits_buf_, local_context_logits_buf_, context_decoder_output, num_token);

    auto logits = context_logits_buf_;

    for (int k = 0; k < indices.size(); ++k) {
        if (output_logits[k]) {
            auto src_ptr       = logits;
            auto dst_ptr       = output_logits[k];
            int  num_new_token = 0;
            if (sequences[k]->cache_len < sequences[k]->tokens.size()) {
                num_new_token = sequences[k]->cache_len + lengths[k] - sequences[k]->tokens.size();
                src_ptr += (lengths[k] - num_new_token) * model_->vocab_size_padded_;
            }
            else {
                num_new_token = lengths[k];
                dst_ptr += (sequences[k]->cache_len - sequences[k]->tokens.size()) * model_->vocab_size_;
            }
            if (model_->vocab_size_padded_ == model_->vocab_size_) {
                Copy(src_ptr, model_->vocab_size_ * num_new_token, dst_ptr);
            }
            else {
                for (int tok = 0; tok < num_new_token; tok++) {
                    Copy(src_ptr, model_->vocab_size_, dst_ptr);
                    src_ptr += model_->vocab_size_padded_;
                    dst_ptr += model_->vocab_size_;
                }
            }
        }
        logits += model_->vocab_size_padded_ * lengths[k];
    }
}

template<typename T>
auto LlamaBatch<T>::Finish(GenerationState& g) -> std::vector<Signal>
{
    NvtxScope scope("Finish");
    const int batch_size = state_->active_size;

    if (batch_size - g.partial) {
        FT_CHECK(g.step >= 0);

        // [s,b] -> [b,s] and skip padding in [context_len, max_context_len)
        // todo:fix this
        invokeGatherOutput(state_->output_ids, // dst , output_ids;
                           state_->input_ids,  // next_input_ids;
                           token_ids_buf_, // src, now generate
                           last_input_ids_buf_, //  last generate
                           medusa_verified_length_,
                           init_context_length_,
                           medusa_verified_packed_path_,
                           g.max_init_ctx_len,
                           g.step + medusa_input_length_,
                           session_len_,
                           batch_size - g.partial,
                           medusa_input_length_,
                           medusa_num_heads_,
                           stream_);

        Clear(token_ids_buf_, batch_size * session_len_);
        invokeTransposeAxis01(token_ids_buf_, state_->output_ids, batch_size, session_len_, 1, stream_);

        invokePadLastTokenIds(token_ids_buf_, init_context_length_, g.max_init_ctx_len, batch_size, stream_);

        g.step += max_len_;

        PrintDecodeTokens(token_ids_buf_, g.step, batch_size - g.partial, stream_, "Finish");

        sync_check_cuda_error();
    }

    Copy(state_->output_ids, batch_size * session_len_, h_output_ids_);

    Copy(medusa_finished_buf_, batch_size, state_->h_finished);
    Clear(medusa_finished_buf_, batch_size);

    Copy(sequence_lengths_, batch_size, state_->h_context_length);

    Copy(sampled_logprobs_, batch_size * kMaxLogProb, h_sampled_logprobs_);
    Copy(sampled_indexes_, batch_size * kMaxLogProb, h_sampled_indexes_);
    Copy(sampled_nums_, batch_size, h_sampled_nums_);

    check_cuda_error(cudaStreamSynchronize(stream_));

    // invariant: context_length = sequence_length + 1, so that h_context_length include all (including the one just
    // generated) tokens
    for (int i = 0; i < batch_size; ++i) {
        if (medusa_enable_) {
            state_->h_context_length[i] += medusa_input_length_;
        }
        else {
            ++state_->h_context_length[i];
        }
    }

    {  // set output tokens ids and sequence length
        int* output_ptr = h_output_ids_;
        for (int i = 0; i < batch_size - g.partial; ++i) {
            if (state_->requests[i] && (state_->requests[i]->stream_cb || state_->h_finished[i])) {
                auto      output_ids = state_->requests[i]->outputs[rank_].getPtr<int>("output_ids");
                auto      output_len = state_->requests[i]->outputs[rank_].getPtr<int>("sequence_length");
                const int count      = state_->h_context_length[i] - medusa_input_length_; // todo: figure out this.
                // TODO: sync history output tokens at when receiving the request and copy the last token here
                std::copy(output_ptr, output_ptr + count, output_ids);
                *output_len = count;
            }
            output_ptr += session_len_;
        }
    }

    {  // output logprobs
        float*    sampled_logprobs_ptr = h_sampled_logprobs_;
        uint32_t* sampled_indexes_ptr  = h_sampled_indexes_;
        uint32_t* sampled_nums_ptr     = h_sampled_nums_;
        for (int i = 0; i < batch_size - g.partial; ++i) {
            if (state_->requests[i] && state_->requests[i]->inputs[rank_].isExist("logprobs")) {
                auto logprob_vals    = state_->requests[i]->outputs[rank_].getPtr<float>("logprob_vals");
                auto logprob_indexes = state_->requests[i]->outputs[rank_].getPtr<uint32_t>("logprob_indexes");
                auto logprob_nums    = state_->requests[i]->outputs[rank_].getPtr<uint32_t>("logprob_nums");

                int offset = state_->h_context_length[i] - state_->h_prompt_length[i] - 1;
                std::copy(sampled_logprobs_ptr,
                          sampled_logprobs_ptr + *sampled_nums_ptr,
                          logprob_vals + offset * kMaxLogProb);
                std::copy(sampled_indexes_ptr,
                          sampled_indexes_ptr + *sampled_nums_ptr,
                          logprob_indexes + offset * kMaxLogProb);
                *(logprob_nums + offset) = *sampled_nums_ptr;
            }
            sampled_logprobs_ptr += kMaxLogProb;
            sampled_indexes_ptr += kMaxLogProb;
            sampled_nums_ptr++;
        }
    }

    if (debug_ && rank_ == 0) {
        for (int i = 0; i < batch_size; ++i) {
            // ss << (i ? ", " : "") << "(" << state_->h_context_length[i] << "," << state_->h_finished[i] << ")";
            std::vector<int> tokens(state_->h_context_length[i]);
            Copy(state_->output_ids + i * session_len_, tokens.size(), tokens.data());
            cudaStreamSynchronize(stream_);
            std::stringstream ss;
            for (const auto& t : tokens) {
                ss << " " << t;
            }
            TM_LOG_INFO("[Finish] slot %d, tokens [%s]", i, ss.str().c_str());
        }
    }

    if(rank_ == 0){

        for (int i = 0; i < batch_size; ++i) {
                // ss << (i ? ", " : "") << "(" << state_->h_context_length[i] << "," << state_->h_finished[i] << ")";
                std::vector<int> tokens(state_->h_context_length[i]);
                Copy(state_->output_ids + i * session_len_, tokens.size(), tokens.data());
                cudaStreamSynchronize(stream_);
                std::stringstream ss;
                for (const auto& t : tokens) {
                    ss << " " << t;
                }
                std::cout << "[Finish] slot " << i <<" tokens ["<< ss.str().c_str() << "]" << std::endl ;
        }
        std::cout << "=======================" << std::endl;
    }

    std::vector<Signal> signals;
    {
        NvtxScope _("stream_and_completion_signal");
        for (int i = 0; i < batch_size - g.partial; ++i) {
            if (state_->requests[i]) {
                if (state_->h_finished[i]) {
                    // Interrupt finished sequences and move the request handle into the signal closure
                    signals.push_back(Interrupt(i));
                    ++g.finished_count;
                }
                else if (state_->requests[i]->stream_cb) {
                    // Create signals by copying the request handles for non-finished streaming requests
                    signals.push_back([this, r = state_->requests[i]] {
                        if (rank_ == 0) {
                            r->stream_cb(&r->outputs[rank_].get());
                        }
                    });
                }
            }
        }
        if (g.finished_count) {
            // synchronize for interrupted sequences
            check_cuda_error(cudaStreamSynchronize(stream_));
        }
    }

    if (g.partial) {
        const int i = batch_size - 1;
        // recover full context length of partial
        state_->h_context_length[i] = g.partial_context_legnth;
    }

    return signals;
}

template<typename T>
auto LlamaBatch<T>::Interrupt(int index, bool force_stop, bool force_end) -> Signal
{
    if (rank_ == 0) {
        TM_LOG_INFO("[Interrupt] slot = %d, id = %lu", index, (long)state_->requests[index]->id);
    }

    if (debug_ && rank_ == 0) {
        std::vector<int> tokens(state_->h_context_length[index]);
        Copy(state_->output_ids + index * session_len_, tokens.size(), tokens.data());
        cudaStreamSynchronize(stream_);
        std::stringstream ss;
        for (const auto& t : tokens) {
            ss << " " << t;
        }
        TM_LOG_INFO("[Interrupt] slot %d, tokens [%s]", index, ss.str().c_str());
    }

    if (state_->requests[index]->end_flag || force_end) {
        // Sequence is ending this round or a stop request is issued to end it
        FT_CHECK(sequence_manager_->Erase(state_->requests[index]->id));
    }
    else {
        const int output_len = state_->h_context_length[index];
        auto&     seq        = *state_->sequences[index];

        // Update token IDs
        seq.tokens.resize(output_len);
        const auto output_ids_data = state_->requests[index]->outputs[rank_].at("output_ids").getPtr<int>();
        std::copy_n(output_ids_data, output_len, seq.tokens.data());

        // Save random state in host memory
        seq.random_state.resize(sizeof(curandState_t));
        // This async copy must be synchronized by the caller
        Copy(state_->curand_state + index, 1, (curandState_t*)seq.random_state.data());

        // Set unlock flag for corresponding blocks, will be unlocked in the next `Materialize()`
        sequence_manager_->UpdateAndSetUnlock(seq);
    }

    state_->sequences[index] = nullptr;

    // move the request handle into the signal
    return [this, r = std::move(state_->requests[index])] {
        if (rank_ == 0) {
            r->signal.set_value(0);
        }
    };
}

template<typename T>
void LlamaBatch<T>::InternalThreadEntry(int device_id)
{
    // TM_LOG_INFO("[InternalThreadEntry] %d", (int)rank_);
    check_cuda_error(cudaSetDevice(device_id));

    auto& shared_state = model_->shared_state_;

    auto& request_queue  = shared_state->request_queue;
    auto& infer_requests = shared_state->infer_requests;
    auto& stop_requests  = shared_state->stop_requests;

    GenerationState g{};

    constexpr int request_interval = 1;
    long          request_counter  = 0;

    while (1) {
        if (rank_ == 0) {
            const int  free_slot_count = max_batch_size_ - state_->size + g.finished_count;
            const bool is_empty        = (free_slot_count == max_batch_size_);
            stop_requests.clear();
            infer_requests.clear();
            if (is_empty || request_counter % request_interval == 0) {
                // Block if batch is empty
                request_queue.dequeue(stop_requests, infer_requests, free_slot_count, is_empty, shared_state->abort);
                if (!shared_state->abort) {
                    RejectInvalidRequests(stop_requests, infer_requests);
                }
            }
        }

        NvtxScope scope("mainloop");

        // wait while rank-0 is dequeueing
        shared_state->barrier->wait();

        if (shared_state->abort) {
            TM_LOG_INFO("[InternalThreadEntry] stop requested.");
            return;
        }

        auto signals = ProcessStopRequests(stop_requests);

        // Shared `priority` field will be assigned by rank-0
        ProcessInferRequests(infer_requests);

        // Wait while shared `requests` is being used
        shared_state->barrier->wait();

        SendSignals(std::move(signals));

        Initialize(g);

        FT_CHECK(step_length_ == 1);

        if (state_->active_size) {
            for (int i = 0; i < step_length_; ++i) {
                //
                auto cont = Forward(g, i);
                //
                if (auto signals = Finish(g); !signals.empty()) {
                    if (g.finished_count) {
                        // Finished requests and corresponding output tensors will be released when notified
                        // wait for all ranks to ensure no rank (except for output thread) will access related
                        // resources
                        shared_state->barrier->wait();
                    }
                    SendSignals(std::move(signals));
                }
                if (!cont) {  // early exit
                    break;
                }
            }
        }

        ++request_counter;
    }

    FT_CHECK(0);
}

template<typename T>
void LlamaBatch<T>::SendSignals(std::vector<Signal> signals)
{
    if (rank_ != 0 || signals.empty()) {
        return;
    }
    {
        std::lock_guard lock{output_mutex_};
        output_signals_.insert(output_signals_.end(),  //
                               std::move_iterator{signals.begin()},
                               std::move_iterator{signals.end()});
    }
    output_cv_.notify_one();
}

template<typename T>
void LlamaBatch<T>::Start()
{
    TM_LOG_INFO("LlamaBatch<T>::Start()");
    int device_id = -1;
    check_cuda_error(cudaGetDevice(&device_id));
    internal_thread_ = std::thread(&LlamaBatch::InternalThreadEntry, this, device_id);
    if (rank_ == 0) {
        output_thread_ = std::thread(&LlamaBatch::OutputThreadEntry, this);
    }
}

template<typename T>
void LlamaBatch<T>::OutputThreadEntry()
{
    while (true) {
        std::vector<Signal> signals;
        {
            // Wait for signals to come
            std::unique_lock lock(output_mutex_);
            output_cv_.wait(lock, [&] { return !output_signals_.empty() || output_stop_token_; });
            if (output_stop_token_) {
                TM_LOG_INFO("[OutputThreadEntry] stop requested.");
                return;
            }
            signals = std::move(output_signals_);
        }
        if (rank_ == 0 && model_->ffi_lock_) {
            model_->ffi_lock_(1);
        }
        // invoke stream cbs & signals
        for (const auto& s : signals) {
            s();
        }
        if (rank_ == 0 && model_->ffi_lock_) {
            model_->ffi_lock_(0);
        }
    }
}


template<typename T>
bool LlamaBatch<T>::Forward(GenerationState& g, int iter)
{
    NvtxScope _("Forward");

    FT_CHECK(max_context_token_num_ >= max_batch_size_);

    const int active_size = state_->active_size;

    constexpr int kLogInterval = 10;
    if (rank_ == 0 && (g.step - 1) % kLogInterval == 0) {
        TM_LOG_INFO("------------------------- step = %d -------------------------", g.step - 1);
    }

    int               pf_offset = -1;
    std::vector<int*> input_d_ptrs(active_size);

    medusa_state_vec_.resize(active_size);
    int inited_index = 0;
    int new_index    = 0;

    if (iter == 0) {  // The first iter may have pre-fill tokens
        for (int i = 0; i < active_size; ++i) {
            const auto& seq = *state_->sequences[i];
            // const int   missing    = state_->h_context_length[i] - seq.cache_len;
            FT_CHECK(seq.input_length >= 1);
            h_input_length_buf_[i] = seq.input_length;
            input_d_ptrs[i]        = state_->input_ids + i * session_len_;

            if (seq.input_length > 1 && pf_offset < 0) {
                pf_offset = i;
            }
            if (medusa_enable_) {
                MedusaInit(medusa_state_vec_, inited_index, new_index, i, seq);
            }
        }
        if (pf_offset < 0) {
            pf_offset = active_size;
        }
    }
    else {
        for (int i = 0; i < active_size; ++i) {
            h_input_length_buf_[i] = 1;
            input_d_ptrs[i]        = state_->output_ids + i * session_len_ + state_->h_context_length[i] - 1;
        }
        pf_offset = active_size;
    }

    if (medusa_enable_) {
        pf_offset = 0;
    }

    // These buffers are only accessed when there are prefill workloads
    if (pf_offset != active_size) {
        Copy(state_->h_context_length, active_size, context_length_buf_);
        Copy(h_input_length_buf_, active_size, input_length_buf_);
    }

    // Find mini-batch offsets: input length > 1 ? prefill() : decode()
    // Constraints on mini-batches
    // - `context_decoder_input` and `context_decoder_output` can hold `max_context_token_num_` tokens w/o padding
    std::vector<int> offsets{0};
    // initialize first mini-batch with decode tokens
    int accum_q_count = pf_offset;
    int accum_k_count = 0;  // only for prefill
    for (int i = pf_offset; i < active_size; ++i) {
        FT_CHECK(iter == 0);
        int q_count = accum_q_count + h_input_length_buf_[i];
        int k_count = accum_k_count + state_->h_context_length[i];
        if (q_count <= max_context_token_num_ && k_count <= max_context_token_num_) {
            accum_q_count = q_count;
            accum_k_count = k_count;
        }
        else {
            offsets.push_back(i);
            accum_q_count = h_input_length_buf_[i];
            accum_k_count = state_->h_context_length[i];
        }
    }
    offsets.push_back(active_size);

    // forward on mini-batches
    for (int p = 0; p < (int)offsets.size() - 1; ++p) {
        const int first           = offsets[p];
        const int last            = offsets[p + 1];
        const int mini_batch_size = last - first;
        int*      input_ids       = context_decoder_ids_buf_;
        //
        std::vector<int> decode_indices{};
        std::vector<int> decode_lengths{};

        std::vector<const Sequence*> sequences;

        BatchedCopy batched_copy;
        for (int i = first; i < last; ++i) {
            input_ids = batched_copy.Add(input_d_ptrs[i], h_input_length_buf_[i], input_ids);
            dbg(i, h_input_length_buf_[i]);
            decode_indices.push_back(i);
            decode_lengths.push_back(h_input_length_buf_[i]);
            sequences.push_back(state_->sequences[i]);
        }
        int token_count = input_ids - context_decoder_ids_buf_;

        batched_copy.Submit(stream_);

        const int dc_batch_size = p ? 0 : pf_offset;
        const int pf_batch_size = mini_batch_size - dc_batch_size;

        if (rank_ == 0) {
            if (pf_batch_size) {
                const auto max_q = *std::max_element(h_input_length_buf_ + first, h_input_length_buf_ + last);
                const auto max_k = *std::max_element(state_->h_context_length + first, state_->h_context_length + last);
                TM_LOG_INFO("[Forward] [%d, %d), dc_bsz = %d, pf_bsz = %d, n_tok = %d, max_q = %d, max_k = %d",
                            first,
                            last,
                            dc_batch_size,
                            pf_batch_size,
                            token_count,
                            max_q,
                            max_k);
            }
        }
        // std::unique_ptr<int[]> medusa_ti     = nullptr;
        int *medusa_ti     = nullptr;
        // std::unique_ptr<int[]> medusa_mask     = nullptr;
        int *medusa_mask     = nullptr;
        // this should set by inited/first.
        std::unique_ptr<int[]> enable_medusa     = nullptr;
        int medusa_input_len = *(h_input_length_buf_ + first);
        std::cout << "[before Attention]. " << std::endl;
        std::cout <<  "medusa_input_len = " << medusa_input_len << std::endl;
        // medusa_ti = std::make_unique<int[]>(medusa_input_len);
        // int* medusa_ti_ptr = medusa_ti.get();
        int* medusa_ti_ptr = medusa_ti;
        // medusa_mask = std::make_unique<int[]>(medusa_input_len * medusa_input_len);
        // int* medusa_mask_ptr = medusa_mask.get();
        int* medusa_mask_ptr = medusa_mask;
        enable_medusa = std::make_unique<int[]>(mini_batch_size);
        int* enable_medusa_ptr = enable_medusa.get();
        
        // GET MedusaTi, MedusaMask
        medusa_utils_->getMedusaTi(&medusa_ti_ptr);
        medusa_utils_->getMedusaMask(&medusa_mask_ptr);
        medusa_utils_->getInputLen(medusa_input_len);

        // GET EnableMedusa
        for(int batch_id = 0; batch_id < mini_batch_size; batch_id++){
            if(medusa_state_vec_[batch_id].inited){
                enable_medusa_ptr[batch_id] = 1;
            }
        }


        // // faked medusa_ti
        // for(int i = 0; i < medusa_input_len; i++){
        //     medusa_ti_ptr[i] = i;
        //     for(int j = 0; j < medusa_input_len; j++){
        //         if(i >= j){
        //             medusa_mask_ptr[i * medusa_input_len + j] = 1;
        //         }
        //     }
        // }
        // enable_medusa_ptr[0] = 1;
        // for(int i = 0; i < mini_batch_size;i++){
        //     enable_medusa_ptr[i] = 1;
        // }

        std::cout << "fake medusa_mask:" << std::endl;
        std::cout << "medusa_mask_ptr : " << medusa_mask_ptr << std::endl;
        for(int i = 0; i < medusa_input_len; i++){
            for(int j = 0; j < medusa_input_len; j++){
                std::cout << medusa_mask_ptr[i * medusa_input_len + j]  << " ";
            }
            std::cout << std::endl;
        }

        std::cout << "fake medusa_ti:" << std::endl;
        for(int i = 0; i < medusa_input_len; i++){
            std::cout << medusa_ti_ptr[i] << " ";
        }std::cout << std::endl;

        std::cout << "fake enable_medusa:" << std::endl;
        for(int i = 0; i < mini_batch_size; i++){
            std::cout << enable_medusa_ptr[i] << " ";
        }std::cout << std::endl;

        std::cout << "copy to device." << std::endl;
        // to Device
        Copy(medusa_ti_ptr, medusa_input_len, d_medusa_ti_);
        Copy(medusa_mask_ptr, medusa_input_len * medusa_input_len, d_medusa_mask_);
        Copy(enable_medusa_ptr, mini_batch_size, d_enable_medusa_);

        // medusa_utils_->getMedusaMask(medusa_mask);
        // medusa_utils_->getMedusaTi(medusa_ti);
        // medusa_utils_->getInputLen(medusa_input_len);
        std::cout << "after copy to D" << std::endl;
        //todo bzw
        // auto dbg_func = [&](int* src, int len, std::string cout_str){
        //     std::vector<int>tmps1(len);
        //     Copy(src, len, tmps1.data());

        //     std::cout << cout_str << std::endl;
        //     for(int i = 0; i < len; i++){
        //         std::cout << " " << tmps1[i] ;
        //     }
        //     std::cout << std::endl;
        // };

        std::cout << "[before Attention] token_count = " << token_count << std::endl;
        dbg_func(context_decoder_ids_buf_, token_count, "[before Attn] context_decoder_ids_buf_ = ");
        dbg_func(decoder_output_buf_, 10, "[before Attn] decoder_output_buf_ = ");
        dbg_func(context_decoder_output_buf_, 10, "[before Attn] context_decoder_output_buf_ = ");
        model_->forwardUnified(decoder_output_buf_ + first * model_->hidden_units_, // out
                               context_decoder_output_buf_,  // temp
                               context_decoder_input_buf_,   // temp
                               (void**)block_ptrs_,
                               cu_block_counts_ + first,
                               context_decoder_ids_buf_,  // temp input_ids
                               h_input_length_buf_ + first,
                               state_->h_context_length + first,
                               rope_theta_ + first,
                               finished_buf_ + first,
                               token_count,
                               dc_batch_size,
                               pf_batch_size,
                               lora_mask_buf_,
                               sequences.data(),
                               d_medusa_ti_,
                               d_medusa_mask_,
                               d_enable_medusa_, // according to init/first
                               medusa_input_len);
        std::cout << "[after Attention] after call forward" << std::endl;
        dbg_func(context_decoder_ids_buf_, token_count, "[after Attn] context_decoder_ids_buf_ = ");
        dbg_func(decoder_output_buf_, 10, "[after Attn] decoder_output_buf_ = ");
        dbg_func(context_decoder_output_buf_, 10, "[after Attn] context_decoder_output_buf_ = ");
        if (iter == 0) {
            if (medusa_enable_) {
                std::cout << "[before MedusaCopy]" << std::endl;
                MedusaCopy(mini_batch_size, first);
                std::cout << "[after MedusaCopy]" << std::endl;
            }
            // compute logits of inputs if requested
            OutputContextLogits(context_decoder_output_buf_, decode_indices, decode_lengths, sequences);
        }
    }

    bool should_stop = false;

    if (medusa_enable_) {
        // LMHead, Sampling, Match
        std::cout << "[before MedusaVerify]" << std::endl;
        std::cout << "inited_index = " << inited_index << ", " << "g.max_init_ctx_len = " << g.max_init_ctx_len << std::endl;
        MedusaVerify(inited_index, g.max_init_ctx_len); // 第一次不会走
        std::cout << "[after Match]" << std::endl;
        std::cout << "[after MedusaVerify]" << std::endl;
        std::vector<int> verified_len(active_size, 0);
        int              idx = 0;
        for (int i = 0; i < active_size; i++) {
            if (!medusa_state_vec_[i].inited) {
                std::cout << "batch " << i << ", continued verify ." << std::endl;
                continue;
            }
            verified_len[i] = h_medusa_max_match_length_buf_[idx++];
            std::cout << "batch " << i << ", verified_len = " << verified_len[i] << std::endl;
        }

        Copy(verified_len.data(), active_size, medusa_verified_length_);
        // todo: add invoke here.
        max_len_ = *std::max_element(verified_len.begin(), verified_len.end()) + 1;
        // [b, seq] -> [seq, b]
        invokeTransposeAxis01(last_input_ids_buf_, state_->input_ids, active_size, medusa_input_length_, 1, stream_);

        dbg_func(token_ids_buf_, 100, "[before MedusaGenerate] token_ids_buf_ = ");
        should_stop = MedusaGenerate(inited_index, new_index, g.max_init_ctx_len, g.step); // 生成 1 + num 的结果到 token_ids_buf_
        dbg_func(token_ids_buf_, 100, "[after MedusaGenerate] token_ids_buf_ = ");
    }

    std::fill(h_input_length_buf_, h_input_length_buf_ + active_size, 0);

    std::vector<int> tmp_cache_len(active_size, 0);

    // `SequenceManager` needs real-time value of cache length
    int idx = 0;
    for (int i = 0; i < active_size; ++i) {
        if (state_->requests[i]) {
            FT_CHECK(state_->sequences[i]);
            state_->sequences[i]->iter += 1;
            if (medusa_state_vec_[i].inited) {
                state_->sequences[i]->cache_len += 1 + h_medusa_max_match_length_buf_[idx++]; // fixme: ??? here 
                tmp_cache_len[i] = state_->sequences[i]->cache_len;
            }
            else {
                state_->sequences[i]->cache_len += state_->sequences[i]->input_length;
                tmp_cache_len[i] = state_->sequences[i]->cache_len;
            }
        }
    }

    if (medusa_enable_) {
        Copy(tmp_cache_len.data(), active_size, sequence_lengths_);
    }

    if (!medusa_enable_) {
        bool should_stop{};

        if (active_size > g.partial) {
            model_->postDecodeEmbedding(logits_buf_, local_logits_buf_, decoder_output_buf_, active_size - g.partial);

            FT_CHECK(g.step >= 0);

            // stop-words & bad-words require the matched tokens to be contiguous, so item size > 1 is
            // not supported yet.
            model_->dynamicDecode(token_ids_buf_,
                                  finished_buf_,
                                  sequence_lengths_,
                                  &should_stop,
                                  state_->curand_state,
                                  &inputs_,
                                  &outputs_,
                                  logits_buf_,
                                  seq_limit_len_,
                                  init_context_length_,
                                  d_end_ids_buf_,
                                  g.step,
                                  0,
                                  g.max_init_ctx_len,
                                  session_len_ * 2,
                                  active_size - g.partial,
                                  0);
        }

        if (debug_ && rank_ == 0) {
            std::vector<int> curr(active_size);
            Copy(token_ids_buf_ + g.step * active_size, active_size, curr.data());
            cudaStreamSynchronize(stream_);
            std::stringstream scurr;
            for (int k = 0; k < curr.size(); ++k) {
                scurr << std::setw(6) << curr[k];
            }
            TM_LOG_INFO("[Forward] step = %d, [%s]", g.step - 1, scurr.str().c_str());
        }

        ////////////////////////////////////////////////
        /// ! increase the counters
        g.step += 1;

        return !should_stop;
    }

    return !should_stop;
}

std::ostream& operator<<(std::ostream& os, const MedusaState& medusa_state)
{
    os << "index=" << medusa_state.index << " len=" << medusa_state.len << " verified_len=" << medusa_state.verified_len
       << " inited=" << medusa_state.inited;
    return os;
}

template<typename T>
void LlamaBatch<T>::MedusaInit(
    std::vector<MedusaState>& medusa_state_vec, int& inited_index, int& new_index, const int index, const Sequence& seq)
{
    auto& medusa_state = medusa_state_vec[index];
    medusa_state.len   = seq.input_length;
    if (seq.iter == 0) {
        medusa_state.verified_len = seq.input_length;
        medusa_state.inited       = false;
        medusa_state.index        = new_index++;
    }
    else {
        medusa_state.verified_len = 0;
        medusa_state.inited       = true;
        medusa_state.index        = inited_index++;
    }
}

template<typename T>
void LlamaBatch<T>::MedusaCopy(const int mini_batch_size, const int first)
{
    // output hidden states: token_num * hidden_size
    T* context_decoder_output_src = context_decoder_output_buf_;
    // input ids
    int* context_decoder_ids_src = context_decoder_ids_buf_;
    // hidden_dims
    int hidden_units = model_->hidden_units_;
    // [batch_size, hidden_size]
    std::cout << "first = " << first << std::endl;
    T* last_token_hidden_units = decoder_output_buf_ + first * hidden_units;

    // process each batch
    for (int i = 0; i < mini_batch_size; i++) {
        int global_index = i + first;
        int len          = medusa_state_vec_[global_index].len;
        int inited       = medusa_state_vec_[global_index].inited;
        int index        = medusa_state_vec_[global_index].index;

        T* medusa_all_hidden_states_buf_dst =
            medusa_all_hidden_states_buf_ + global_index * medusa_input_length_ * hidden_units;
        if (inited) {
            // [batch_idx, input_len_, hidden_dims]
            T* medusa_inited_hidden_states_buf_dst =
                medusa_inited_hidden_states_buf_ + index * medusa_input_length_ * hidden_units;
            int* medusa_inited_input_ids_buf_dst = medusa_inited_input_ids_buf_ + index * medusa_input_length_;

            // Initialized request, hidden states
            // H copy [input_len_, hidden_dims] buffer to dst(batch_idx)
            Copy(context_decoder_output_src, len * hidden_units, medusa_inited_hidden_states_buf_dst);
            // Initialized request, input ids
            // Token copy [len] buffer to dst(batch_idx)
            Copy(context_decoder_ids_src, len, medusa_inited_input_ids_buf_dst);
            // H copy [input_len_, hidden_dims] buffer to dst(batch_idx) 
            Copy(context_decoder_output_src, len * hidden_units, medusa_all_hidden_states_buf_dst);
        }
        else {
            //todo
            std::vector<T>tmps1(10);
            Copy(last_token_hidden_units, 10, tmps1.data());
            std::cout << "[MedusaCopy] show:" << std::endl;
            for(int i = 0; i < 10; i++){
                std::cout << " " << tmps1[i] ;
            }
            std::cout << std::endl;
            Copy(last_token_hidden_units, hidden_units, medusa_all_hidden_states_buf_dst);
        }
        // src ptr move
        context_decoder_output_src += len * hidden_units;
        context_decoder_ids_src += len;

        last_token_hidden_units += hidden_units;
    }
}

template<typename T>
void LlamaBatch<T>::MedusaVerify(const int inited_index, const int max_init_ctx_len)
{
    if (inited_index != 0) {
        if (medusa_logits_buf_ == nullptr) {
            NcclGuard guard(model_->tensor_para_, stream_, true);
            medusa_logits_buf_ = (float*)allocator_->malloc(sizeof(float) * model_->vocab_size_padded_ * max_batch_size_);
            const auto tp      = model_->tensor_para_.world_size_;
            if (tp > 1) {
                FT_CHECK(model_->vocab_size_padded_ % tp == 0);
                medusa_local_logits_buf_ = (float*)allocator_->malloc(sizeof(float) * model_->vocab_size_padded_
                                                                      * max_batch_size_);
            }
        }

        size_t hidden_size = model_->hidden_units_;

        // [batch_size, medusa_input_len, hidden_size] -> [medusa_input_len, batch_size, hidden_size]
        invokeTransposeAxis01(medusa_inited_hidden_states_buf_,
                              medusa_inited_hidden_states_buf_,
                              inited_index,
                              medusa_input_length_,
                              hidden_size,
                              stream_);
        int step = 0;
        // for each in medusa_input_length, process a batch
        std::cout << "[before LMHead, Sampling]" <<std::endl;
        for (int i = 0; i < medusa_input_length_; i++) {
            T* medusa_inited_hidden_states_buf_src = medusa_inited_hidden_states_buf_ + i * inited_index * hidden_size;

            bool should_stop{};
            model_->postDecodeEmbedding(
                medusa_logits_buf_, medusa_local_logits_buf_, medusa_inited_hidden_states_buf_src, inited_index);
            model_->dynamicDecode(medusa_token_ids_buf_,
                                  medusa_finished_buf_,
                                  medusa_sequence_lengths_,
                                  &should_stop,
                                  state_->curand_state,
                                  &inputs_,
                                  &outputs_,
                                  medusa_logits_buf_,
                                  seq_limit_len_,
                                  init_context_length_,
                                  d_end_ids_buf_,
                                  step,
                                  0,
                                  max_init_ctx_len,
                                  session_len_ * 2,
                                  inited_index,
                                  0);
            step++;
        }

        std::cout << "[after LMHead, Sampling]" <<std::endl;
        // [medusa_input_length_, batch_size, 1] -> [batch_size, medusa_input_length_, 1]
        invokeTransposeAxis01(
            medusa_ref_output_ids_buf_, medusa_token_ids_buf_, medusa_input_length_, inited_index, 1, stream_);

        std::cout << "[after invokeTransposeAxis01()]" <<std::endl;
        int* h_medusa_max_match_length_dst = h_medusa_max_match_length_buf_;
        std::vector<int> inited_input_ids(medusa_input_length_ * inited_index, -1);
        std::vector<int> ref_output_ids(medusa_input_length_ * inited_index, -2);
        // src:[batch, medusa_input_len]
        Copy(medusa_inited_input_ids_buf_, medusa_input_length_ * inited_index, inited_input_ids.data());
        // src:[batch, ?]
        Copy(medusa_ref_output_ids_buf_, medusa_input_length_ * inited_index, ref_output_ids.data());
        
        // here to call MedusaUtils::GetOutputIds;
        // Unpack the tokens to each path tokens.
        // [b, path_num, 1 + head_num]
        std::vector<int> unpacked_input_ids(inited_index * medusa_path_num_ * (1 + medusa_num_heads_), -1);
        std::vector<int> unpacked_each_path_len(medusa_path_num_, -1);
        std::vector<int> unpacked_output_ids(inited_index * medusa_path_num_ * (1 + medusa_num_heads_), -1);

        std::cout << "[before getBatchedOutputIds()]" <<std::endl;
        // each_path_len is only consider medusa_path, not include root.
        // input tokens [b, path_num, 1+head_num]
        medusa_utils_->path_tree_.getBatchedOutputIds(inited_input_ids.data(), unpacked_input_ids.data(), unpacked_each_path_len.data(), medusa_num_heads_, inited_index);
        // output pred tokens [b, path_num, 1+head_num]
        medusa_utils_->path_tree_.getBatchedOutputIds(ref_output_ids.data(), unpacked_output_ids.data(), nullptr, medusa_num_heads_, inited_index);
        std::cout << "[after getBatchedOutputIds()]" <<std::endl;

        // copy to Device
        Copy(unpacked_input_ids.data(), inited_index * medusa_path_num_ * (1 + medusa_num_heads_), medusa_input_tokens_buf_);
        Copy(unpacked_output_ids.data(), inited_index * medusa_path_num_ * (1 + medusa_num_heads_), medusa_output_tokens_buf_);
        Copy(unpacked_each_path_len.data(), medusa_path_num_, medusa_each_path_len_buf_);

        //call batch match

        std::cout << "[before batched Match]" <<std::endl;
        invokeMedusaBatchMatch(medusa_input_tokens_buf_,
                               medusa_output_tokens_buf_,
                               medusa_each_path_len_buf_,
                               medusa_max_match_length_buf_,
                               medusa_max_match_idx_buf_,
                               inited_index,
                               medusa_path_num_,
                               medusa_num_heads_,
                               stream_);
        // matched_length : [b]
        // matched_idx    : [b]
        Copy(medusa_max_match_length_buf_, inited_index, h_medusa_max_match_length_buf_);
        Copy(medusa_max_match_idx_buf_, inited_index, h_medusa_max_match_idx_buf_);
        // todo: call MedusaUtil::getBatchedLastMatchIdx to get last_match_idx
        std::cout << "[after batched Match]" <<std::endl;
        medusa_utils_->path_tree_.getBatchedLastMatchIdx(h_medusa_max_match_length_buf_, h_medusa_max_match_idx_buf_, h_medusa_last_match_idx_buf_, inited_index);
        
        //medusa_verified_packed_path_:[b, 1 + medusa_head_num], used for gather output
        medusa_utils_->path_tree_.getBatchedMatchedPartIdx(h_medusa_max_match_length_buf_, h_medusa_max_match_idx_buf_, h_medusa_verified_packed_path_, inited_index, medusa_num_heads_);
        Copy(h_medusa_verified_packed_path_, inited_index * medusa_num_heads_, medusa_verified_packed_path_);




        // auto diff = [](const std::vector<int>& lhs,
        //                const std::vector<int>& rhs,
        //                int                     medusa_num_heads,
        //                int                     batch_size) -> std::vector<int> {
        //     int              res_size = (1 + medusa_num_heads) * batch_size;
        //     std::vector<int> res(res_size, 0);
        //     if (lhs.size() != rhs.size() || lhs.size() != res_size || rhs.size() != res_size) {
        //         return res;
        //     }
        //     int stride = 1 + medusa_num_heads;
        //     for (int i = 0; i < batch_size; i++) {
        //         int tmp_match_len = 0;
        //         for (int j = i * stride; j < (i + 1) * stride - 1; ++j) {
        //             if (lhs[j + 1] == rhs[j]) {
        //                 tmp_match_len++;
        //             }
        //             else {
        //                 break;
        //             }
        //         }
        //         res[i] = tmp_match_len;
        //     }
        //     return res;
        // };
        // std::vector<int> res = diff(inited_input_ids, ref_output_ids, medusa_num_heads_, inited_index);

        // Copy(res.data(), inited_index, h_medusa_max_match_length_dst);
    }
}

template<typename T>
bool LlamaBatch<T>::MedusaGenerate(const int inited_index, const int new_index, const int max_init_ctx_len, int& step)
{
    int batch_size = inited_index + new_index;

    if (batch_size != 0) {
        // gather last verified hidden states
        int hidden_size                           = model_->hidden_units_;
        T*  medusa_verified_hidden_states_buf_dst = medusa_verified_hidden_states_buf_; // [batch, hidden_dim], put each batch last H_last
        int inited_idx                            = 0;

        for (int i = 0; i < batch_size; i++) {
            T* medusa_all_hidden_states_buf_src =
                medusa_all_hidden_states_buf_ + i * medusa_input_length_ * hidden_size;
            if (medusa_state_vec_[i].inited) {
                // inited seq last verified
                // find the right last pos.
                std::cout << "[MedusaGenerate] old. choose last match idx = " << h_medusa_last_match_idx_buf_[inited_idx]  << std::endl; 
                medusa_verified_hidden_states_buf_dst =
                    Copy(medusa_all_hidden_states_buf_src + h_medusa_last_match_idx_buf_[inited_idx++] * hidden_size,
                         hidden_size,
                         medusa_verified_hidden_states_buf_dst); 
            }
            else {
                std::cout << "[MedusaGenerate] new. choose last hidden states."  << std::endl; 
                // new seq first
                medusa_verified_hidden_states_buf_dst =
                    Copy(medusa_all_hidden_states_buf_src, hidden_size, medusa_verified_hidden_states_buf_dst);
            }
        }
        
        dbg_func(medusa_all_hidden_states_buf_, 10, "[MedusaGenerate] medusa_all_hidden_states_buf_:");

        // lm head linear
        if (medusa_logits_buf_ == nullptr) {
            NcclGuard guard(model_->tensor_para_, stream_, true);
            medusa_logits_buf_ = (float*)allocator_->malloc(sizeof(float) * model_->vocab_size_padded_ * max_batch_size_);
            const auto tp      = model_->tensor_para_.world_size_;
            if (tp > 1) {
                FT_CHECK(model_->vocab_size_padded_ % tp == 0);
                medusa_local_logits_buf_ = (float*)allocator_->malloc(sizeof(float) * model_->vocab_size_padded_
                                                                      * max_batch_size_);
            }
        }
        bool should_stop{};
        // LM head
        // src medusa_verified_hidden_states_buf_:[batch, hidden_dim] -> dst medusa_logits_buf_:[batch, vocab_size]
        model_->postDecodeEmbedding(
            medusa_logits_buf_, medusa_local_logits_buf_, medusa_verified_hidden_states_buf_, batch_size);
        
        dbg_func(medusa_logits_buf_, batch_size * 10, "[after LMHead] medusa_logits_buf_:");

        // sampling
        // src medusa_logits_buf_:[batch, vocab_size] -> dst medusa_token_ids_buf_:[batch, 1]
        model_->dynamicDecode(medusa_token_ids_buf_,
                              medusa_finished_buf_,
                              medusa_sequence_lengths_,
                              &should_stop,
                              state_->curand_state,
                              &inputs_,
                              &outputs_,
                              medusa_logits_buf_,
                              seq_limit_len_,
                              init_context_length_,
                              d_end_ids_buf_,
                              0,
                              0,
                              max_init_ctx_len,
                              session_len_ * 2,
                              batch_size,
                              step);
        dbg_func(medusa_token_ids_buf_, 10, "[after sampling] medusa_token_ids_buf_ = ");
        // medusa forward
        // H:medusa_verified_hidden_states_buf_[batch_size, hidden_dim] -> [batch_size, medusa_head_num, vocab_size] -> medusa_topk_output_ids_buf_[batch_size, medusa_head_num, top10]

        dbg_func(medusa_verified_hidden_states_buf_, 10, "[after sampling] medusa_verified_hidden_states_buf_ = ");
        model_->medusaForward(medusa_topk_output_ids_buf_, medusa_verified_hidden_states_buf_, batch_size, medusa_top_k_);
        
        dbg_func(medusa_topk_output_ids_buf_, batch_size * medusa_num_heads_ * medusa_top_k_, "[after medusa linear] medusa_topk_output_ids_buf_ = ");

        // D2H, using MedusaUtils
        Copy(medusa_topk_output_ids_buf_, batch_size * medusa_num_heads_ * medusa_top_k_, h_medusa_preds_batched_buf_);
        // src medusa_topk_output_ids_buf_:[batch_size, medusa_head_num, top_k]
        // dst h_pseudo_inputs_buf_: [batch_size, input_len_ - 1]
        
        // todo: check this.
        medusa_utils_->path_tree_.getBatchedPseudoIdsFromTree(h_medusa_preds_batched_buf_, medusa_num_heads_, medusa_top_k_, h_medusa_max_match_length_buf_, h_medusa_max_match_idx_buf_, h_pseudo_inputs_buf_, batch_size);
        
        std::cout << "[in MedusaGenerate] after getBatchedPseudoIdsFromTree" << std::endl;
        // src: [batch_size, input_len_ - 1] -> dst: [input_len_ - 1, batch_size]
        invokeTransposeAxis01(
            h_pseudo_inputs_buf_, h_pseudo_inputs_buf_, batch_size, medusa_input_length_ - 1, 1, stream_);

        // //: fake
        // for(int i = 0; i < batch_size * medusa_input_length_ - 1; i++){
        //     h_pseudo_inputs_buf_[i] = 1000 + i; 
        // }
        // //: end fake
        // [LM] [Med batch pos1][Med batch pos2][Med batch pos3][Med batch pos ...]
        Copy(medusa_token_ids_buf_, batch_size, token_ids_buf_ + step * batch_size);
        // Copy(h_pseudo_inputs_buf_, batch_size * (medusa_input_length_ - 1), token_ids_buf_ + step * batch_size * (medusa_input_length_ - 1)); // Medusa Generate
        Copy(h_pseudo_inputs_buf_, batch_size * (medusa_input_length_ - 1), token_ids_buf_ + (step + 1) * batch_size); // Medusa Generate
        dbg_func(h_pseudo_inputs_buf_, batch_size * (medusa_input_length_ - 1), "[after sampling] h_pseudo_inputs_buf_ = ");


        // std::vector<int>tmps(batch_size * medusa_input_length_, 0);
        // Copy(token_ids_buf_, batch_size * medusa_input_length_, tmps.data());
        // std::cout << "[after GetPseudoIds] show:" << std::endl;
        // for(int i = 0; i < batch_size * medusa_input_length_; i++){
        //     std::cout << " " << tmps[i] ;
        // }
        // std::cout << std::endl;
        return should_stop;
    }
}

template class LlamaBatch<half>;
#ifdef ENABLE_FP32
template class LlamaBatch<float>;
#endif
#ifdef ENABLE_BF16
template class LlamaBatch<__nv_bfloat16>;
#endif

}  // namespace turbomind
