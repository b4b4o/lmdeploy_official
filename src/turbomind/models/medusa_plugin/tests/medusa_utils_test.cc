#include "src/turbomind/models/medusa_plugin/medusa_utils.h"

int main(){
    turbomind::MedusaPathTree tree;
    std::vector<std::vector<int>> path_tuples{{1, 1, 1}, {1, 2}, {2, 1, 1}, {2, 1, 2}, {2, 3}};
    tree.insert(path_tuples);
    tree.dbg();

    return 0;
}