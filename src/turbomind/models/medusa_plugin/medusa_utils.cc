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
        for(int i = 0; i < node->depth_; i++){
            std::cout << "\t";
        }
        std::cout << node->top_k_idx_;
        if(node->is_leaf_){
            std::cout << "(l)"; 
        }
        std::cout << std::endl;
        for(std::pair<int, MedusaPathTreeNode*> each_pair: node->childs_){
            MedusaPathTreeNode* child_node = each_pair.second;
            dbg(child_node);
        }
    }
}

void MedusaPathTree::getOrCreateMedusaTi(int* medusa_ti, int &len){
    if(!medusaTi_){
        bfs();
    }
    medusa_ti = medusaTi_;
    len = len_;
}
void MedusaPathTree::bfs(){
    bfs(root_);
} 
void MedusaPathTree::bfs(MedusaPathTreeNode* root){
    std::queue<MedusaPathTreeNode*> q;
    q.push(root);

    std::map<int, int> depth_count;
    len_ = 0;

    while(!q.empty()){
        MedusaPathTreeNode* node = q.front();
        q.pop();
        
        if(!node)break;


        node->input_token_index_ = len_++;
#if 1
        std::cout << node->input_token_index_ << ":" << node->input_token_index_ << std::endl;
#endif        
        ++depth_count[node->depth_];

        for(std::pair<int, MedusaPathTreeNode*> each_pair: node->childs_){
            MedusaPathTreeNode* child_node = each_pair.second;
            q.push(child_node);
        }
    }
    medusaTi_ = new int[len_];
    // medusaMask_ = new int[len_ * len_];

    int l = 0, r = 0;
    for(auto it:depth_count){
        int pos = it.first;
        int cnt = it.second;
        r = l + cnt;
        while(l < r){
            medusaTi_[l++] = pos;
        }
        l = r;
    }
#if 1
    std::cout << "[debug] medusaTi_ = ";
    for(int i = 0; i < len_; i++){
        std::cout << medusaTi_[i] << " ";
    }
    std::cout << std::endl;
#endif

} 
void MedusaPathTree::dfs(){
    medusaMask_ = new int[len_ * len_];
    std::fill_n(medusaMask_, len_ * len_, 0);

    std::vector<int> ancestor_ids;
    dfs(root_, ancestor_ids);
} 
void MedusaPathTree::dfs(MedusaPathTreeNode* node, std::vector<int>& ancestor_ids){
    if(node){
        ancestor_ids.push_back(node->input_token_index_);
        int &row = node->input_token_index_;
        #if 1
            std::cout << "===" << std::endl;
            std::cout << std::endl;
            std::cout << "depth = " << node->depth_ << std::endl;
            std::cout << "topk = " << node->top_k_idx_ << std::endl;
            std::cout << "ancestor_ids = ";
        #endif
        for(const int &col : ancestor_ids){
            medusaMask_[row * len_ + col] = 1;
            #if 1
                std::cout << col << " ";
            #endif
        }
        #if 1
            std::cout << std::endl;
        #endif
        for(std::pair<int, MedusaPathTreeNode*> each_pair: node->childs_){
            MedusaPathTreeNode* child_node = each_pair.second;
            dfs(child_node, ancestor_ids);
        }
        ancestor_ids.pop_back();
    }
} 
void MedusaPathTree::getOrCreateMedusaMask(int* medusa_mask, int &len){
    if(!medusaMask_){
        dfs();
    }
    medusa_mask = medusaMask_;
    len = len_;
#if 1
    std::cout << "[debug] medusaMask_ = " << std::endl;
    for(int i = 0; i < len_; i++){
        for(int j = 0; j < len_; j++){
            std::cout << medusaMask_[i * len_ + j] << " ";
        }
        std::cout << std::endl;
    }
#endif
}

void MedusaUtils::getTokenIdsAccordingToPath(int* medusa_path_tokens_out, const size_t& path_num, const int* medusa_pred_tokens, std::vector<std::vector<int>>& path_tuples, const int batch_size, const int medusa_head_num, const int K){
    // input:[medusa_head_num, batch_size, topk], output:[path_num, batch_size, medusa_head_num]

    auto to_src_idx = [batch_size, K](int h_idx, int b_idx, int topk_idx){
        return h_idx * batch_size * K + b_idx * K + topk_idx;
    };
    auto to_dst_idx = [batch_size, medusa_head_num](int p_idx, int b_idx, int h_idx){
        return p_idx * batch_size * medusa_head_num + b_idx * medusa_head_num + h_idx;
    };

    for(int b_id = 0; b_id < batch_size; ++b_id){
        for(int path_id = 0; path_id < path_num; ++path_id){
            std::vector<int> &path_tuple = path_tuples[path_id];
            paddingTuple(path_tuple, medusa_head_num, 0); // if path[medusa_head i] not defined, we use topk = 0 as default for i.
            for(int head_id = 0; head_id < medusa_head_num; ++head_id){
                int topk_id = path_tuple[head_id];
                int pred_token_value = medusa_pred_tokens[to_src_idx(head_id, b_id, topk_id)];
                medusa_path_tokens_out[to_dst_idx(path_id, b_id, head_id)] = pred_token_value;
            }
        }
    }
}
std::vector<int> MedusaUtils::parseTupleStr2TupleInt(const std::string& tuple_str) {
    // tuple_str format is without ')' 
    // like:
    // "[(0, 1", "[(0, 1, "
    std::vector<int> tuple;
    std::stringstream ss(tuple_str); 
    std::string item;
    // remove the util '('
    std::getline(ss, item, '(');
    while (std::getline(ss, item, ',')) {
        tuple.push_back(std::stoi(item));
    }
    return tuple;
}
std::vector<std::vector<int>> MedusaUtils::getMedusaPathsFromLocalFile(const std::string& local_path, const std::string& aim_model_name){
    // the medusa_paths format should like :  
    //    modelname = [(0,), (0, 1), (2, 9, ), ...]
    std::ifstream file(local_path);
    std::vector<std::vector<int>> tuples;
    std::string line;

    while (std::getline(file, line)) {
        std::stringstream ss(line);
        std::string readin_model_name;
        std::string tuple_str;
        std::getline(ss, readin_model_name, '=');
        // TODO:removeBlank
        std::cout << "before model:'" << readin_model_name << "'" << std::endl;
        readin_model_name = removeAllWhiteSpaces(readin_model_name);
        std::cout << "after model:'" << readin_model_name << "'" << std::endl;

        if(readin_model_name != aim_model_name){
            continue;
        }else{
            while (std::getline(ss, tuple_str, ')')) {
                // std::cout << tuple_str << std::endl;
                if (tuple_str.size() > 1) {
                    std::vector<int> tuple = parseTupleStr2TupleInt(tuple_str);
                    tuples.push_back(tuple);
                }
            }
            return tuples;
        }
    }
    return tuples;
}

void MedusaUtils::displayPathTuples(std::vector<std::vector<int>>& path_tuples){
    // path_tuples is a [path_num, path_length] 2D vector.
    const int n = path_tuples.size();

    std::cout << "[";
    for(int i = 0; i < n; i++){
        // paddingTuple(path_tuples[i], 5, -1);
        const int m = path_tuples[i].size();
        std::cout << "[";
        for(int j = 0; j < m; j++){
            std::cout << "(" << j << "):" << path_tuples[i][j];
            if(j != m - 1)std::cout << ", ";
        }
        std::cout << "]\n";
    }
    std::cout << "]\n";
}
std::string MedusaUtils::removeAllWhiteSpaces(std::string str){
    str.erase(std::remove_if(str.begin(), str.end(), [](unsigned char ch) {
        return std::isspace(ch);
    }), str.end());
    return str;
}
void MedusaUtils::paddingTuple(std::vector<int>& tuple, int aim_size, int padding_value){
    if (tuple.size() < aim_size) {
        tuple.resize(aim_size, padding_value);
    }
}
std::vector<std::vector<int>>& MedusaUtils::getPathTuples(){
    return medusa_path_tuples_;
}

std::pair<size_t, size_t> MedusaUtils::resultOffsetAndLength(int batch_idx, int path_idx, int batch_size, int medusa_head_num){
    return std::pair<size_t, size_t> {path_idx * batch_size * medusa_head_num + batch_idx * medusa_head_num, medusa_head_num};
}
size_t MedusaUtils::getPathNum(){
    return path_num_;
}

}
