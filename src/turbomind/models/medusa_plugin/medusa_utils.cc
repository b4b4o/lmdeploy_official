#include "src/turbomind/models/medusa_plugin/medusa_utils.h"
namespace turbomind{

void MedusaPathTree::insert(std::vector<int> path_tuple){
    MedusaPathTreeNode* node = root_;
    int depth = 0;
    for(int path:path_tuple){
        ++depth;
        if(!node->childs_[path]){
            node->childs_[path] = new MedusaPathTreeNode(path, depth, 0, false);
        }
        node = node->childs_[path];
    }
    node->is_leaf_ = true;
}
void MedusaPathTree::insert(std::vector<std::vector<int>> path_tuples){
    for(std::vector<int> path_tuple: path_tuples){
        insert(path_tuple);
    }
}
void MedusaPathTree::deleteTree(MedusaPathTreeNode* node){
    if(node){
        for(std::pair<int, MedusaPathTreeNode*> each_pair: node->childs_){
            MedusaPathTreeNode* child_node = each_pair.second;
            deleteTree(child_node);
        }
        delete node;
    }
}
void MedusaPathTree::dbg(){
    dbg(root_);
}
void MedusaPathTree::dbg(MedusaPathTreeNode* node){
    if(node){
        for(std::pair<int, MedusaPathTreeNode*> each_pair: node->childs_){
            MedusaPathTreeNode* child_node = each_pair.second;
            for(int i = 0; i < child_node->depth_; i++){
                std::cout << "\t";
            }
            std::cout << child_node->top_k_idx_ << std::endl;
            dbg(child_node);
        }
    }
}

}
