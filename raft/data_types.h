#include <vector>

using std::vector;

struct log_entry
{
    char *var; // only 1 var
    int value;
    int term;
    int logIndex;
    vector<int> configuration;
};

//enum serverState
//{
//    LEADER = 0;
//    FOLLOWER = 1;
//    CANDIDATE = 2;
//}