#include "RPCPacket_m.h"
#include <algorithm>

using namespace omnetpp;
using std::vector;

/**
 * Client computer; see NED file for more info
 */
class Client : public cSimpleModule
{
    public:
      virtual ~Client();
    private:
      int myAddress;
      int receiverAddress; // This is the choosen server ID

      // Actual cluster configuration
      vector<int> configuration;

      int value = 0;
      // To identify each client request
      int sequenceNumber = 0;
    
      cMessage *sendWrite, *sendRead, *requestTimeoutRead, *requestTimeoutWrite;
      RPCClientCommandPacket *clientCommandRPC;
      // Convention: READ = 0, WRITE = 1;
      const int READ = 0;
      const int WRITE = 1;
      // Last operation requested by this client
      int lastOperation = -1;
      // To know if client has been redirect to a known leader
      bool isRedirect = false;

    protected: 
      virtual void initialize() override;
      virtual void handleMessage(cMessage *msg) override;
      void chooseNextRandomOp();
      int chooseRandomServer();
  };

Define_Module(Client);

// Destructor
Client::~Client()
{
  cancelAndDelete(sendRead);
  cancelAndDelete(sendWrite);
  cancelAndDelete(requestTimeoutRead);
  cancelAndDelete(requestTimeoutWrite);
}

void Client::initialize(){
  myAddress = gate("port$i")->getPreviousGate()->getId();

  WATCH_VECTOR(configuration);
  WATCH(myAddress);

  sendWrite = new cMessage("sendWrite");
  sendRead = new cMessage("sendRead");
  requestTimeoutRead = new cMessage("requestTimeoutRead");
  requestTimeoutWrite = new cMessage("requestTimeoutWrite");

  chooseNextRandomOp();
}

void Client::handleMessage(cMessage *msg){

  if (msg == sendRead){
    if (isRedirect == false){
      receiverAddress = chooseRandomServer();
    }
    clientCommandRPC = new RPCClientCommandPacket("RPC_CLIENT_COMMAND", RPC_CLIENT_COMMAND);
    clientCommandRPC->setDestAddress(receiverAddress);
    clientCommandRPC->setSequenceNumber(sequenceNumber);
    char x = 'x';
    clientCommandRPC->setVar(x);
    clientCommandRPC->setType(READ);
    send(clientCommandRPC, "port$o");
    scheduleAt(simTime() + par("requestTimeout"), requestTimeoutRead);
    return;
  }
  else if (msg == sendWrite) {
    if (isRedirect == false){
      receiverAddress = chooseRandomServer();
    }
    clientCommandRPC = new RPCClientCommandPacket("RPC_CLIENT_COMMAND", RPC_CLIENT_COMMAND);
    clientCommandRPC->setDestAddress(receiverAddress);
    clientCommandRPC->setSequenceNumber(sequenceNumber);
    char x = 'x';
    clientCommandRPC->setVar(x);
    clientCommandRPC->setValue(value);
    clientCommandRPC->setType(WRITE);
    send(clientCommandRPC, "port$o");
    scheduleAt(simTime() + par("requestTimeout"), requestTimeoutWrite);
    return;
  }
  // If timeout without receiving response, try to resend the mex, choosing another server
  else if (msg == requestTimeoutRead)
    scheduleAt(simTime(), sendRead);
  else if (msg == requestTimeoutWrite)
    scheduleAt(simTime(), sendWrite);



  RPCPacket *pk = check_and_cast<RPCPacket *>(msg);
  if (pk->getKind() == RPC_CLIENT_COMMAND){
    // Update config in response to Admin mex
    RPCClientCommandPacket *response = check_and_cast<RPCClientCommandPacket *>(pk);
    configuration.assign(response->getClusterConfig().servers.begin(), response->getClusterConfig().servers.end());
  }
  else if (pk->getKind() == RPC_CLIENT_COMMAND_RESPONSE){
    RPCClientCommandResponsePacket *response = check_and_cast<RPCClientCommandResponsePacket *>(pk);

    if (response->getRedirect() == true){
      // If I have been redirect set the receiverAddress to LastKnownLeader, so to avoid pick a random server again
      isRedirect = true;
      receiverAddress = response->getLastKnownLeader();
      if (lastOperation == READ){
        scheduleAt(simTime(), sendRead);
      }
      else {
        scheduleAt(simTime(), sendWrite);
      }
    }
    else{
      // I have received a valid response
      // Print value received back if it is a read?
      if(sequenceNumber == response->getSequenceNumber())
      {
        if (lastOperation == READ){
          EV << "READ Request SUCCESS! Received response back for request with SN = " << response->getSequenceNumber() << "  from: " << response->getSrcAddress();
          EV << "Value read is x = " << response->getValue();
        }
        else{
          EV << "WRITE Request SUCCESS! Received response back for request with SN = " << response->getSequenceNumber() << "  from: " << response->getSrcAddress();
        }
        //Now client can issue another request
        cancelEvent(requestTimeoutRead);
        cancelEvent(requestTimeoutWrite);
        chooseNextRandomOp();
      }
      else{
        EV << "Received response back for request with old SN = " << response->getSequenceNumber();
      }
    }
  }
  delete pk;
}


void Client::chooseNextRandomOp(){
  sequenceNumber++;

  // Produce a random integer in the range [0,2)
  int randomOp = intrand(2);
  if(randomOp == READ){
    lastOperation = READ;
    scheduleAt(simTime() + uniform(SimTime(par("lowCommandTimeout")), SimTime(par("highCommandTimeout"))), sendRead);
  }
  else{
    lastOperation = WRITE;
    scheduleAt(simTime() + uniform(SimTime(par("lowCommandTimeout")), SimTime(par("highCommandTimeout"))), sendWrite);
    value++;
  }
  isRedirect = false;
}

int Client::chooseRandomServer(){
  int randomServer = intrand(configuration.size());
  return configuration[randomServer];
}