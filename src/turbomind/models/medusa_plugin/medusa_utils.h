#include <iostream>
#include <vector>
#include <unordered_map>

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
  	MedusaPathTreeNode* root_;
  	int* medusaMask_;
  	int* medusaTi_;
  
    void dbg(MedusaPathTreeNode* node);
  	void deleteTree(MedusaPathTreeNode* root);
  	
};

class MedusaUtils {
public:
    MedusaUtils();
    ~MedusaUtils();
    MedusaUtils(const MedusaUtils&) = delete;
    MedusaUtils& operator=(const MedusaUtils&) = delete;

private:
    //[42][5]
    std::vector<std::vector<int>> path_tuples;
    
};

}  // namespace turbomind