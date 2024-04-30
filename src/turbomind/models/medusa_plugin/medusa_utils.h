#pragma once

#include <iostream>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <string>
#include <algorithm>
#include <queue>
#include <map>
#include <memory>
#include <cassert>

namespace turbomind {

struct MedusaPathTreeNode{
  
  int top_k_idx_; // 这个路径节点取的是top_idx的值
  int depth_; //节点层级深度
  int input_token_index_; //在实际输入token时的对应下标
  bool is_leaf_; // 是否为叶子节点（路径末尾）


//   std::unordered_map<int, MedusaPathTreeNode*> childs_; // 子节点
  std::map<int, MedusaPathTreeNode*> childs_; // 子节点
  MedusaPathTreeNode(int top_k_idx, int depth, int input_token_index, bool is_leaf):top_k_idx_(top_k_idx), depth_(depth), input_token_index_(input_token_index), is_leaf_(is_leaf){}

};

class MedusaPathTree{
  public:
    MedusaPathTree() : root_(new MedusaPathTreeNode(0, 0, 0, false)) {}
    ~MedusaPathTree() {
        deleteTree(root_);
        delete[] medusaMask_;
        medusaMask_ = nullptr;
        delete[] medusaTi_;
        medusaTi_ = nullptr;
    }
  public:
    std::vector<std::vector<int>> input_token_idx_of_paths;
    std::vector<int> topk_value_of_paths; // 根据层次遍历得到
  public:
    void insert(std::vector<int> path_tuple); // 插入单条路径
    void insert(std::vector<std::vector<int>> path_tuples);
    void dbg();
    void bfs();
    void dfs();
  	void getOrCreateMedusaTi(int* medusa_ti, int &len);    // rope使用的 medusa_ti, bfs遍历得到
  	void getOrCreateMedusaMask(int* medusa_mask, int &len);  // attention kernel 使用的 Causal Mask, dfs遍历得到, 也会得到路径上每个取top_k的记录 topk_value_of_paths
    void getOutputIds(const int* output_preds, int* output_ids, int* each_path_len, const int medusa_head_num);
    void getBatchedOutputIds(const int* output_preds_batched, int* output_ids_batched, int* each_path_len, const int medusa_head_num, const int batch_num);
    int  getMedusaPathNum();
    int  getMedusaInputLen();
    void getPseudoIdsFromTree(const int* medusa_preds, const int medusa_head_num, const int top_k, const int max_match_count, const int max_match_idx, int* pseudo_inputs);
    void getBatchedPseudoIdsFromTree(const int* medusa_preds_batched, const int medusa_head_num, const int top_k, const int* max_match_count, const int* max_match_idx, int* pseudo_inputs_batched, const int batch_size);
    void getLastMatchIdx(const int& max_match_idx, const int& max_match_count, int& last_input_idx);
    void getBatchedLastMatchIdx(const int* max_match_idx, const int* max_match_count, int* last_input_idx, const int batch_size);
    void getMatchedPartIdx(const int& max_match_idx, const int& max_match_count, int* matched_part_input_idx);
    void getBatchedMatchedPartIdx(const int* max_match_idx, const int* max_match_count, int* matched_part_input_idx, const int batch_size, const int medusa_head_num);

  private:
  	MedusaPathTreeNode* root_ = nullptr;
  	int* medusaMask_ = nullptr;
  	int* medusaTi_ = nullptr;

    int len_; // input token len
    int path_num_;
  private:
    void dbg(MedusaPathTreeNode* node);
    void bfs(MedusaPathTreeNode* root);
    void dfs(MedusaPathTreeNode* node, std::vector<int>& ancestor_ids);
  	void deleteTree(MedusaPathTreeNode* root);
  	
};

class MedusaUtils {
public:
    MedusaUtils(std::string& medusa_path_filename, std::string &aim_model_name)
    :medusa_path_(medusa_path_filename),
    aim_model_name_(aim_model_name)
    {
        medusa_path_tuples_ =  getMedusaPathsFromLocalFile(medusa_path_, aim_model_name_);
        path_num_ = medusa_path_tuples_.size();
        path_tree_.insert(medusa_path_tuples_);
        int* medusa_ti = nullptr;
        int* medusa_mask = nullptr;
        path_tree_.getOrCreateMedusaTi(medusa_ti, input_len_);
        path_tree_.getOrCreateMedusaMask(medusa_mask, input_len_);

        // displayPathTuples(medusa_path_tuples_);
    }

    ~MedusaUtils(){}

public:
    // input:[medusa_head_num, batch_size, topk], output:[path_num, batch_size, medusa_head_num]
    void getTokenIdsAccordingToPath(int* medusa_path_tokens_out, const size_t& path_num,const int* medusa_pred_tokens, std::vector<std::vector<int>>& path_tuples, const int batch_size, const int medusa_head_num, const int K);
    void displayPathTuples(std::vector<std::vector<int>>& path_tuples);
    std::vector<std::vector<int>>& getPathTuples();
    std::pair<size_t, size_t> resultOffsetAndLength(int batch_idx, int path_idx, int batch_size, int medusa_head_num);
    void getMedusaTi(int* medusa_ti);
    void getMedusaMask(int* medusa_mask);
    void getInputLen(int& len);
    void getPathNum(int& path_num);
public:
    std::string medusa_path_;
    std::string aim_model_name_;
    std::vector<std::vector<int>> medusa_path_tuples_;
    size_t path_num_;
    int    input_len_;
    MedusaPathTree path_tree_;
private:
    void paddingTuple(std::vector<int>& tuple, int aim_size, int padding_value);
    std::string removeAllWhiteSpaces(std::string str);
    std::vector<int> parseTupleStr2TupleInt(const std::string& tuple_str);
    std::vector<std::vector<int>> getMedusaPathsFromLocalFile(const std::string& local_path, const std::string& aim_model_name);
};

}  // namespace turbomind