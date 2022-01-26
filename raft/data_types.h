#include <vector>

using std::vector;
using namespace omnetpp;

struct latest_client_response
{
    int clientAddress;
    int latestSequenceNumber = -1;
    int latestReponseToClient = -1;
    int currentSequenceNumber = -1;
};

struct cluster_configuration
{
    vector<int> servers;
};

struct lastRPC
{
    bool success = false;
    int sequenceNumber = -1;
    bool isHeartbeat = false;
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
    vector<latest_client_response> clientsData;
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
    int value = -1; // Convention to indicate snapshot file void
    // Last known config
    vector<int> cOld;
    vector<int> cNew;
    // Latest response to Client
    vector<latest_client_response> clientsData;
};

struct install_snapshot_timer
{
    snapshot_file snapshot;
    int destination;
    int lastIncludedIndex;
    int lastIncludedTerm;
    cMessage *timeoutEvent;
};
