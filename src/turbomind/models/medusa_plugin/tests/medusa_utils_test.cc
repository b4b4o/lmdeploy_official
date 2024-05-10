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
    tree.getOrCreateMedusaTi(&medusa_ti, len);
    tree.getOrCreateMedusaMask(&medusa_mask, len);

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
    std::unique_ptr<int[]> output_each_path_len(new int[path_num]);

    std::unique_ptr<int[]> input_preds(new int[batch_num * input_len]);
    std::unique_ptr<int[]> input_ids(new int[batch_num * path_num * (1 + medusa_head_num)]);
    std::unique_ptr<int[]> input_each_path_len(new int[path_num]);


    int base = 1;
    for(int b = 0; b < batch_num; b++){
        base *= 10;
        for(int i = 0; i < input_len; i++){
            output_preds[i + b * input_len] = i + base;
            input_preds[i + b * input_len] = i + base * 10;
        }
    }
    // tree.getOutputIds(output_preds.get(), output_ids.get(), medusa_head_num);
    tree.getBatchedOutputIds(output_preds.get(), output_ids.get(), output_each_path_len.get(), medusa_head_num, batch_num);
    tree.getBatchedOutputIds(input_preds.get(), input_ids.get(), input_each_path_len.get(), medusa_head_num, batch_num);
    
    auto display_ids = [&](int* ids, int* lens, std::string name){
        std::cout << "[debug] " << name << "ids." << std::endl;
        for(int b = 0; b < batch_num; b++){
            std::cout << "---------------------------" << std::endl; 
            std::cout << "batch " << b << std::endl;
            for(int i = 0; i < path_num; i++){
                for(int j = 0; j < (1 + medusa_head_num); j++){
                    std::cout << ids[i * (1 + medusa_head_num) + j + b * (path_num * (1 + medusa_head_num))] << " ";
                }
                std::cout << std::endl;
                std::cout << "lens = " << lens[i] << std::endl;
            }
        }
    };
    display_ids(output_ids.get(), output_each_path_len.get(), "output");
    // display_ids(input_ids.get(), input_each_path_len.get(), "input");

    const int top_k = 10;
    std::unique_ptr<int[]> medusa_preds(new int[batch_num * medusa_head_num * top_k]);
    int cnt = 0;
    for(int b = 0; b < batch_num; b++){
        for(int i = 0; i < medusa_head_num; i++){
            for(int j = 0; j < top_k; j++){
                medusa_preds[b * medusa_head_num * top_k + i * top_k + j] = cnt++ ;
            }
        }
    }
    std::unique_ptr<int[]> pseudo_inputs(new int[batch_num * len]);
    std::unique_ptr<int[]> max_match_count(new int[batch_num * len]{3, 2, 3, 2});
    std::unique_ptr<int[]> max_match_idx(new int[batch_num * len]{0, 1, 2, 4});

    // tree.getPseudoIdsFromTree(medusa_preds.get(), medusa_head_num, top_k, output_ids.get(), max_match_count, max_match_idx, pseudo_inputs.get());
    // tree.getBatchedPseudoIdsFromTree(medusa_preds.get(), medusa_head_num, top_k, output_ids.get(), max_match_count.get(), max_match_idx.get(), pseudo_inputs.get(), batch_num);
    // std::cout << "[debug] pseudo_inputs: " << std::endl;
    // for(int b = 0; b < batch_num; b++){
    //     std::cout << "---------------------------" << std::endl;
    //     std::cout << "batch : " << b << std::endl;
    //     for(int i = 0; i < len; i++){
    //         std::cout << pseudo_inputs[b * len + i] << " ";
    //     }std::cout << std::endl;
    // }
    return 0;
}