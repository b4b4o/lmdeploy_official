#include "src/turbomind/models/medusa_plugin/medusa_utils.h"

int main(){
    turbomind::MedusaPathTree tree;
    std::vector<std::vector<int>> path_tuples{{1, 1, 1}, {1, 2}, {2, 1, 1}, {2, 1, 2}, {2, 3}};
    tree.insert(path_tuples);
    tree.dbg();

    std::string path_filename = "src/turbomind/models/medusa_plugin/medusa_choices.info";
    std::string path_modelname = "vicuna_13b_stage1";

    turbomind::MedusaUtils utils(path_filename, path_modelname);
    utils.displayPathTuples(utils.medusa_path_tuples_);

    return 0;
}