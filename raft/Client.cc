#include "RPCPacket_m.h"
#include <algorithm>
#include "Admin.h"

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
    
      class Admin *Admin;
      cMessage *sendWrite, *sendRead, *requestTimeoutRead, *requestTimeoutWrite;
      RPCClientCommandPacket *clientCommandRPC;
      // Convention: READ = 0, WRITE = 1;
      const int READ = 0;
      const int WRITE = 1;

      void chooseNextRandomOp();
      void initializeConfiguration();
      int chooseRandomServer();
    protected: 
    virtual void initialize() override;
    virtual void handleMessage(cMessage *msg) override;
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
  Admin = check_and_cast<class Admin *>(gate("port$i")->getPreviousGate()->getOwnerModule()->gate("port$o", 1)->getOwnerModule());

  initializeConfiguration();

  sendWrite = new cMessage("sendWrite");
  sendRead = new cMessage("sendRead");
  requestTimeoutRead = new cMessage("requestTimeoutRead");
  requestTimeoutWrite = new cMessage("requestTimeoutWrite");

  chooseNextRandomOp();
}

void Client::handleMessage(cMessage *msg){

  if (msg == sendRead){
    receiverAddress = chooseRandomServer();
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
    receiverAddress = chooseRandomServer();
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
    scheduleAt(simTime(), sendRead);


  RPCPacket *pk = check_and_cast<RPCPacket *>(msg);
  if (pk->getKind() == RPC_CONFIG_CHANGED){
    // Update config in response to Admin mex
    configuration.clear();
    configuration.assign(Admin->configuration.begin(), Admin->configuration.end());
  }
  else if (pk->getKind() == RPC_CLIENT_COMMAND_RESPONSE){
    cancelEvent(requestTimeoutRead);
    cancelEvent(requestTimeoutWrite);

    // Print value received back if it is a read?
    chooseNextRandomOp();
  }
  delete pk;
}


void Client::chooseNextRandomOp(){
  sequenceNumber++;

  // Produce a random integer in the range [0,2)
  int randomOp = intrand(2);
  if(randomOp == READ)
    scheduleAt(simTime() + uniform(SimTime(par("lowCommandTimeout")), SimTime(par("highCommandTimeout"))), sendRead);
  else{
    scheduleAt(simTime() + uniform(SimTime(par("lowCommandTimeout")), SimTime(par("highCommandTimeout"))), sendWrite);
    value++;
  }
}

int Client::chooseRandomServer(){
  int randomServer = intrand(configuration.size());
  return configuration[randomServer];
}

void Client::initializeConfiguration(){
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