cplusplus {{
#include "data_types.h"
}}

struct log_entry;

enum RPCPacketType
{
    RPC_APPEND_ENTRIES = 0;
    RPC_REQUEST_VOTE = 1;
    RPC_INSTALL_SNAPSHOT = 2;
    RPC_APPEND_ENTRIES_RESPONSE = 3;
    RPC_REQUEST_VOTE_RESPONSE = 4;
    RPC_CLIENT_COMMAND = 5;
    RPC_CLIENT_COMMAND_RESPONSE = 6;
    RPC_CONFIG_CHANGED = 7;
    RPC_NEW_CONFIG_COMMITTED = 8;
};

packet RPCPacket
{
    int srcAddress;
    int destAddress;
    bool isBroadcast = false;  //Va bene come valore di default?
};

packet RPCClientCommandPacket extends RPCPacket{
    char var;
    int value;
    // Convention: READ = 0, WRITE = 1;
    int type;
    int sequenceNumber;
}

packet RPCClientCommandResponsePacket extends RPCPacket{
    int sequenceNumber;
    int value;
    bool redirect = false;
    int lastKnownLeader;
}
// NOTA: si possono specificare valori di default!
packet RPCAppendEntriesPacket extends RPCPacket
{
    int term;                           // leader's term
    int leaderId;                       // leader identifier so follower can redirect clients --> index of leader gate port in the Switch
    int prevLogIndex;                   // index of log entry immediately preceding new ones          
    int prevLogTerm;                    // term of prevLogIndex entry
    log_entry entry;                  // log entries to store (empty for heartbeat);
    int leaderCommit;                   // leader’s commitIndex
};


packet RPCAppendEntriesResponsePacket extends RPCPacket
{
    int term;                           // currentTerm, for leader to update itself
    bool success;                       // true if follower contained entry matching prevLogIndex and prevLogTerm
};

packet RPCRequestVotePacket extends RPCPacket
{
    int term;                           // candidate's term
    int candidateId;                    // candidate identifier requesting vote --> index of candidate gate port in the Switch
    int lastLogIndex;                   // index of candidate’s last log entry
    int lastLogTerm;                    // term of candidate’s last log entry
};

packet RPCRequestVoteResponsePacket extends RPCPacket
{
    int term;                           // currentTerm, for candidate to update itself
    bool voteGranted;                   // true means candidate received vote

};

packet RPCInstallSnapshotPacket extends RPCPacket
{
    //TODO Insert fields
    string payload;
};


packet RPCconfigChangedPacket extends RPCPacket
{
    // TODO: add the config vector in the RPCconfigChangedPacket
    string payload;
};

packet RPCnewConfigurationCommitted extends RPCPacket
{};