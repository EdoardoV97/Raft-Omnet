#include <vector>

using std::vector;
using namespace omnetpp;

struct latest_client_response
{
    int clientAddress;
    int latestSequenceNumber;
    int latestReponseToClient;
};

struct clients_data
{
    vector<latest_client_response> responses;
};

struct cluster_configuration
{
    vector<int> servers;
};


struct log_entry
{
    char var; // only 1 var for the state machine 'x'
    int value;
    int term;
    int logIndex;
    int clientAddress;
    vector<int> cOld;
    vector<int> cNew;
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
    NON_VOTING = 3,
};

struct snapshot_file 
{
    int lastIncludedIndex;
    int lastIncludedTerm;
    // State machine state
    char var;
    int value;
    // Last known config
    vector<int> oldConfiguration;
    vector<int> newConfiguration;
};