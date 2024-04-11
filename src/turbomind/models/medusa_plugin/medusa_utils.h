#pragma once

#include <iostream>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <string>
#include <algorithm>
namespace turbomind {

struct MedusaPathTreeNode{
  
  int top_k_idx_; // 这个路径节点取的是top_idx的值
  int depth_; //节点层级深度
  int input_token_index_; //在实际输入token时的对应下标
  bool is_leaf_; // 是否为叶子节点（路径末尾）


  std::unordered_map<int, MedusaPathTreeNode*> childs_; // 子节点
  MedusaPathTreeNode(int top_k_idx, int depth, int input_token_index, bool is_leaf):top_k_idx_(top_k_idx), depth_(depth), input_token_index_(input_token_index), is_leaf_(is_leaf){}

};

class MedusaPathTree{
  public:
    MedusaPathTree() : root_(new MedusaPathTreeNode(0, 0, 0, false)) {}
    ~MedusaPathTree() {
        deleteTree(root_);
        delete medusaMask_;
        medusaMask_ = nullptr;
        delete medusaTi_;
        medusaTi_ = nullptr;
    }
  	void insert(std::vector<int> path_tuple); // 插入单条路径
    void insert(std::vector<std::vector<int>> path_tuples);
    void dbg();
  	void getOrCreateMedusaTi();    // rope使用的 medusa_ti, bfs遍历得到
  	void getOrCreateMedusaMask();  // attention kernel 使用的 Causal Mask, dfs遍历得到
  private:
  	MedusaPathTreeNode* root_ = nullptr;
  	int* medusaMask_ = nullptr;
  	int* medusaTi_ = nullptr;
  
    void dbg(MedusaPathTreeNode* node);
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
        // displayPathTuples(medusa_path_tuples_);
    }

    ~MedusaUtils(){}

public:
    // input:[medusa_head_num, batch_size, topk], output:[path_num, batch_size, medusa_head_num]
    void getTokenIdsAccordingToPath(int* medusa_path_tokens_out, const size_t& path_num,const int* medusa_pred_tokens, std::vector<std::vector<int>>& path_tuples, const int batch_size, const int medusa_head_num, const int K);
    void displayPathTuples(std::vector<std::vector<int>>& path_tuples);
    std::vector<std::vector<int>>& getPathTuples();
    size_t getPathNum();
    std::pair<size_t, size_t> resultOffsetAndLength(int batch_idx, int path_idx, int batch_size, int medusa_head_num);
public:
    std::string medusa_path_;
    std::string aim_model_name_;
    std::vector<std::vector<int>> medusa_path_tuples_;
    size_t path_num_;
private:
    void paddingTuple(std::vector<int>& tuple, int aim_size, int padding_value);
    std::string removeAllWhiteSpaces(std::string str);
    std::vector<int> parseTupleStr2TupleInt(const std::string& tuple_str);
    std::vector<std::vector<int>> getMedusaPathsFromLocalFile(const std::string& local_path, const std::string& aim_model_name);
};

}  // namespace turbomind