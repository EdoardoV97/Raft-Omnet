#include <vector>

using std::vector;
using namespace omnetpp;

struct log_entry
{
    char var; // only 1 var
    int value;
    int term;
    int logIndex;
    vector<int> configuration;
};

struct append_entry_timer
{
    log_entry entry;
    int destination;
    int prevLogIndex;
    int prevLogTerm;
    cMessage *timeoutEvent;
};

enum serverState
{
    LEADER = 0,
    FOLLOWER = 1,
    CANDIDATE = 2,
};
