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

    return 0;
}