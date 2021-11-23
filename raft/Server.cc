#include "RPCPacket_m.h"
//#include <vector>

using namespace omnetpp;
using std::vector;

/**
 * The basic server of the cluster; see NED file for more info
 */
class Server : public cSimpleModule
{
  public: 
    virtual ~Server();
  private:
    bool isLeader = false;
    cMessage *sendHearthbeat;
    int myAddress;   // This is the server ID
    int receiverAddress; // This is receiver server ID

    // Pointers to handle RPC mexs
    RPCAppendEntriesPacket *appendEntriesRPC = nullptr;
    RPCAppendEntriesResponsePacket *appendEntriesResponseRPC = nullptr;
    RPCRequestVotePacket *requestVoteRPC = nullptr;
    RPCRequestVoteResponsePacket *requestVoteResponseRPC = nullptr;
    RPCInstallSnapshotPacket *installSnapshotRPC = nullptr;

    // State Machine of the server
    int x = 0;
    vector<int> configuration;

    // Persistent state --> Updated on stable storage before responding to RPCs
    int currentTerm = 0;
    int votedFor = -1;
    vector<log_entry> log;

    // Volatile state --> Reinitialize after crash
    int commitIndex = 0;
    int lastApplied = 0;

    // Volatile state on leaders --> Reinitialized after election
    vector<int> nextIndex;
    vector<int> matchIndex;

  protected:
    virtual void initialize() override;
    virtual void handleMessage(cMessage *msg) override;
};

Define_Module(Server);


// Destructor
Server::~Server()
{
  cancelAndDelete(sendHearthbeat);
}


void Server::initialize()
{
  //matchIndex.assign(par("numServer"), 0);
  sendHearthbeat = new cMessage("send-hearthbeat");

  myAddress = gate("port$o")->getNextGate()->getIndex(); // Return index of this server gate port in the Switch

  if (par("isLeader").boolValue() == true) { 
    scheduleAt(simTime(), sendHearthbeat);
    isLeader = true;
  }
}

void Server::handleMessage(cMessage *msg)
{
  if(msg == sendHearthbeat){
    // Send an empty RPCAppendEntries(= hearthbeat), to all followers

    EV << "Sending hearthbeat to followers\n";
    appendEntriesRPC = new RPCAppendEntriesPacket("RPC_APPEND_ENTRIES", RPC_APPEND_ENTRIES);
    appendEntriesRPC->setSrcAddress(myAddress);
    appendEntriesRPC->setIsBroadcast(true);
    send(appendEntriesRPC, "port$o");

    scheduleAt(simTime() + par("hearthBeatTime"), sendHearthbeat);
    return;
  }
  
  RPCPacket *pk = check_and_cast<RPCPacket *>(msg);

    if (pk->getKind() == RPC_APPEND_ENTRIES) {
      delete pk;
    }
}

// Fare un metodo per checkare server facenti parte della configurazione corrente(= connessi allo switch)