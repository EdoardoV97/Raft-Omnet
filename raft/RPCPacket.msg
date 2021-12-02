cplusplus {{
#include "data_types.h"
}}

struct log_entry;
struct clients_data;
struct cluster_configuration;

enum RPCPacketType
{
    RPC_APPEND_ENTRIES = 0;
    RPC_REQUEST_VOTE = 1;
    RPC_INSTALL_SNAPSHOT = 2;
    RPC_APPEND_ENTRIES_RESPONSE = 3;
    RPC_REQUEST_VOTE_RESPONSE = 4;
    RPC_CLIENT_COMMAND = 5;
    RPC_CLIENT_COMMAND_RESPONSE = 6;
    RPC_ACK = 7;
    RPC_INSTALL_SNAPSHOT_RESPONSE = 8;
};

packet RPCPacket
{
    int srcAddress;
    int destAddress;
    bool isBroadcast = false;
    string displayString = "b=10,10,oval,kind,kind,1";
    int sequenceNumber;
};

packet RPCClientCommandPacket extends RPCPacket{
    char var;
    int value;
    int type; // Convention: READ = 0, WRITE = 1;
    cluster_configuration clusterConfig;
}

packet RPCClientCommandResponsePacket extends RPCPacket{
    int value;
    bool redirect = false;
    int lastKnownLeader;
}

packet RPCAppendEntriesPacket extends RPCPacket
{
    int term;                           // leader's term
    int leaderId;                       // leader identifier so follower can redirect clients --> index of leader gate port in the Switch
    int prevLogIndex = 0;               // index of log entry immediately preceding new ones          
    int prevLogTerm = 0;                // term of prevLogIndex entry
    log_entry entry;                    // log entries to store (empty for heartbeat);
    int leaderCommit;                   // leader’s commitIndex
    bool withAck;
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
    int term;               // leader's term
    int leaderId;           // leader identifier so follower can redirect clients --> index of leader gate port in the Switch
    int lastIncludedIndex;
    int lastIncludedTerm;
};

packet RPCInstallSnapshotResponsePacket extends RPCPacket
{
    int term ;      // currentTerm, for leader to update itself
}

packet RPCAckPacket extends RPCPacket{};
