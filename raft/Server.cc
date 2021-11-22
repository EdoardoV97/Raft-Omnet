#include "RPCPacket_m.h"

using namespace omnetpp;

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
    int myAddress;
    int receiverAddress;
    RPCAppendEntriesPacket *appendEntriesRPC = nullptr;
    RPCRequestVotePacket *requestVoteRPC = nullptr;
    RPCInstallSnapshotPacket *installSnapshotRPC = nullptr;

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
  sendHearthbeat = new cMessage("send-hearthbeat");

  myAddress = gate("port$o")->getNextGate()->getIndex(); // Return index of the server gate port in the Switch
  //receiverAddress = gate("port$o")->getNextGate()->size()-2;

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
    //appendEntriesRPC->setDestAddress(-1);
    appendEntriesRPC->setIsBroadcast(true);
    appendEntriesRPC->setPayload("hearthbeat");
    send(appendEntriesRPC, "port$o");

    scheduleAt(simTime() + par("hearthBeatTime"), sendHearthbeat);
    return;
  }
  
  RPCPacket *pk = check_and_cast<RPCPacket *>(msg);

    if (pk->getKind() == RPC_APPEND_ENTRIES) {
      delete pk;
    }
}

