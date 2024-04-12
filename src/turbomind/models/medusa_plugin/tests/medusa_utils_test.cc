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

    int input_len = 10;
    int medusa_head_num = 5;
    int path_num = 5;
    std::unique_ptr<int[]> output_preds(new int[input_len]);
    std::unique_ptr<int[]> output_ids(new int[path_num * (1 + medusa_head_num)]);

    for(int i = 0; i < input_len; i++){
        output_preds[i] = 10 + i;
    }
    tree.getOutputIds(output_preds.get(), output_ids.get(), medusa_head_num);
    std::cout << "[debug] outputids." << std::endl;
    for(int i = 0; i < path_num; i++){
        for(int j = 0; j < (1 + medusa_head_num); j++){
            std::cout << output_ids[i * (1 + medusa_head_num) + j] << " ";
        }
        std::cout << std::endl;
    }

    return 0;
}