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
    cMessage *sendHearthbeat, *temp;
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

    void updateConfiguration();

  protected:
    virtual void initialize() override;
    virtual void handleMessage(cMessage *msg) override;
};

Define_Module(Server);


// Destructor
Server::~Server()
{
  cancelAndDelete(sendHearthbeat);
  cancelAndDelete(temp);
}

void Server::updateConfiguration()
{
    cModule *Switch = gate("port$i")->getPreviousGate()->getOwnerModule();
    int moduleAddress;
    for (int i = 2; i < Switch->gateSize("port$o"); i++){
        if (Switch->gate("port$o", i)->isConnected())
        {
          moduleAddress = Switch->gate("port$o", i)->getId();
          //EV << "Added ID: " << moduleAddress << " to configuration Vector" << endl;
          configuration.push_back(moduleAddress);
        }
    }
}


void Server::initialize()
{
  //matchIndex.assign(par("numServer"), 0);
  sendHearthbeat = new cMessage("send-hearthbeat");
  temp = new cMessage("temp");

  // My address is the out port of the switch corresponding to the input port of this module
  myAddress = gate("port$i")->getPreviousGate()->getId();
  WATCH(myAddress);
  EV << "My address is " << myAddress << endl;

  WATCH_VECTOR(configuration);
  updateConfiguration();


  if (par("isLeader").boolValue() == true) { 
    scheduleAt(simTime(), sendHearthbeat);
    //scheduleAt(simTime() + par("processingTime"), temp);
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
  else if (msg == temp){
    receiverAddress = gate("port$i")->getPreviousGate()->getOwnerModule()->gate("port$o", 1)->getId();
    RPCnewConfigurationCommitted *committed = new RPCnewConfigurationCommitted("RPC_NEW_CONFIG_COMMITTED", RPC_NEW_CONFIG_COMMITTED);
    send(committed, receiverAddress);
  }
  
  RPCPacket *pk = check_and_cast<RPCPacket *>(msg);

    if (pk->getKind() == RPC_APPEND_ENTRIES) {
      delete pk;
    }
}