#include "src/turbomind/models/medusa_plugin/medusa_utils.h"

int main(){
    turbomind::MedusaPathTree tree;
    
    // std::string path_filename = "/workdir/lmdeploy/src/turbomind/models/medusa_plugin/medusa_choices.info";
    // std::string path_modelname = "mc_sim_7b_63";

    // turbomind::MedusaUtils utils(path_filename, path_modelname);
    // utils.displayPathTuples(utils.medusa_path_tuples_);

    std::vector<std::vector<int>> path_tuples{{1, 1, 1}, {1, 2}, {2, 1, 1}, {2, 1, 2}, {2, 3}};
    // tree.insert(utils.medusa_path_tuples_);
    tree.insert(path_tuples);
    tree.dbg();

    int* medusa_ti;
    int* medusa_mask;
    int len;
    tree.getOrCreateMedusaTi(medusa_ti, len);
    tree.getOrCreateMedusaMask(medusa_mask, len);

    int medusa_head_num = 5;
    int batch_num = 4;
    
    const int input_len_verify = 10;
    int input_len = tree.getMedusaInputLen();
    assert(input_len == input_len_verify);
    
    const int path_num_verify = 5;
    int path_num = tree.getMedusaPathNum();
    assert(path_num_verify == path_num);

    std::unique_ptr<int[]> output_preds(new int[batch_num * input_len]);
    std::unique_ptr<int[]> output_ids(new int[batch_num * path_num * (1 + medusa_head_num)]);
    int base = 1;
    for(int b = 0; b < batch_num; b++){
        base *= 10;
        for(int i = 0; i < input_len; i++){
            output_preds[i + b * input_len] = i + base;
        }
    }
    // tree.getOutputIds(output_preds.get(), output_ids.get(), medusa_head_num);
    tree.getBatchedOutputIds(output_preds.get(), output_ids.get(), medusa_head_num, batch_num);
    
    std::cout << "[debug] outputids." << std::endl;
    for(int b = 0; b < batch_num; b++){
        std::cout << "batch " << b << std::endl;
        for(int i = 0; i < path_num; i++){
            for(int j = 0; j < (1 + medusa_head_num); j++){
                std::cout << output_ids[i * (1 + medusa_head_num) + j + b * (path_num * (1 + medusa_head_num))] << " ";
            }
            std::cout << std::endl;
        }
    }

    return 0;
}