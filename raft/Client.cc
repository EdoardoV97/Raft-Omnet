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
      int adminAddress;
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
      virtual void refreshDisplay() const override;
      virtual void initialize() override;
      virtual void handleMessage(cMessage *msg) override;
      void chooseNextRandomOp();
      int chooseRandomServer();
      void initializeConfiguration();
      bool isInConfig(int destAddress);
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
  adminAddress = gate("port$o")->getNextGate()->getOwnerModule()->gate("port$o", 0)->getId();

  WATCH_VECTOR(configuration);
  WATCH(myAddress);
  WATCH(value);

  sendWrite = new cMessage("sendWrite");
  sendRead = new cMessage("sendRead");
  requestTimeoutRead = new cMessage("requestTimeoutRead");
  requestTimeoutWrite = new cMessage("requestTimeoutWrite");

  initializeConfiguration();
  chooseNextRandomOp();
}

void Client::handleMessage(cMessage *msg){

  if (msg == sendRead){
    if (isRedirect == false){
      receiverAddress = chooseRandomServer();
    }
    //bubble("READ");
    cancelEvent(requestTimeoutRead);
    clientCommandRPC = new RPCClientCommandPacket("RPC_CLIENT_COMMAND", RPC_CLIENT_COMMAND);
    clientCommandRPC->setDestAddress(receiverAddress);
    clientCommandRPC->setSequenceNumber(sequenceNumber);
    clientCommandRPC->setSrcAddress(myAddress);
    char x = 'x';
    clientCommandRPC->setType(READ);
    send(clientCommandRPC, "port$o");
    scheduleAt(simTime() + par("requestTimeout"), requestTimeoutRead);
    return;
  }
  else if (msg == sendWrite) {
    if (isRedirect == false){
      receiverAddress = chooseRandomServer();
    }
    //bubble("WRITE");
    cancelEvent(requestTimeoutWrite);
    clientCommandRPC = new RPCClientCommandPacket("RPC_CLIENT_COMMAND", RPC_CLIENT_COMMAND);
    clientCommandRPC->setDestAddress(receiverAddress);
    clientCommandRPC->setSequenceNumber(sequenceNumber);
    clientCommandRPC->setSrcAddress(myAddress);
    char x = 'x';
    clientCommandRPC->setVar(x);
    clientCommandRPC->setValue(value);
    clientCommandRPC->setType(WRITE);
    send(clientCommandRPC, "port$o");
    scheduleAt(simTime() + par("requestTimeout"), requestTimeoutWrite);
    return;
  }
  // If timeout without receiving response, try to resend the mex, choosing another server
  else if (msg == requestTimeoutRead){
    bubble("Retry READ");
    cancelEvent(sendRead);
    scheduleAt(simTime(), sendRead);
    return;
  }
  else if (msg == requestTimeoutWrite){
    bubble("Retry WRITE");
    cancelEvent(sendWrite);
    scheduleAt(simTime(), sendWrite);
    return;
  }


  RPCPacket *pk = check_and_cast<RPCPacket *>(msg);

  // Simulate packet dropping on the receiver
  if(dblrand() >= par("clientErrorRateThreshold").doubleValue() && pk->getSrcAddress() != adminAddress){
    delete(pk);
    bubble("Channel Failure!");
    return;
  }

  if (pk->getKind() == RPC_CLIENT_COMMAND){
    // Update config in response to Admin mex
    RPCClientCommandPacket *response = check_and_cast<RPCClientCommandPacket *>(pk);
    configuration.assign(response->getClusterConfig().servers.begin(), response->getClusterConfig().servers.end());
  }
  else if (pk->getKind() == RPC_CLIENT_COMMAND_RESPONSE){
    RPCClientCommandResponsePacket *response = check_and_cast<RPCClientCommandResponsePacket *>(pk);

    if (response->getRedirect() == true){

      // If no known leader is available or is a leader of an old config, drop the packet and retry later
      if (!isInConfig(response->getLastKnownLeader())) {
        delete pk;
        return;
      }
      // If I have been redirect set the receiverAddress to LastKnownLeader, so to avoid pick a random server again
      isRedirect = true;
      receiverAddress = response->getLastKnownLeader();
      if (lastOperation == READ){
        cancelEvent(sendRead);
        scheduleAt(simTime(), sendRead);
      }
      else {
        cancelEvent(sendWrite);
        scheduleAt(simTime(), sendWrite);
      }
    }
    else{
      // I have received a valid response
      // Print value received back if it is a read?
      if(sequenceNumber == response->getSequenceNumber())
      {
        if (lastOperation == READ){
          EV << "READ Request SUCCESS! Received response back for request with SN = " << response->getSequenceNumber() << "  from: " << response->getSrcAddress() << endl;
          EV << "Value read is x = " << response->getValue() << endl;
        }
        else{
          EV << "WRITE Request SUCCESS! Received response back for request with SN = " << response->getSequenceNumber() << "  from: " << response->getSrcAddress() << endl;
        }
        //Now client can issue another request
        cancelEvent(requestTimeoutRead);
        cancelEvent(requestTimeoutWrite);
        chooseNextRandomOp();
      }
      else{
        EV << "Received response back for request with old SN = " << response->getSequenceNumber() << endl;
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
    //EV << "Sending READ command" << endl;
    scheduleAt(simTime() + uniform(SimTime(par("lowCommandTimeout")), SimTime(par("highCommandTimeout"))), sendRead);
  }
  else{
    lastOperation = WRITE;
    //EV << "Sending WRITE command" << endl;
    scheduleAt(simTime() + uniform(SimTime(par("lowCommandTimeout")), SimTime(par("highCommandTimeout"))), sendWrite);
    value = intrand(1001);
  }
  isRedirect = false;
}

int Client::chooseRandomServer(){
  int randomServer = intrand(configuration.size());
  return configuration[randomServer];
}

void Client::initializeConfiguration(){
  cModule *Switch = gate("port$i")->getPreviousGate()->getOwnerModule();
  int moduleAddress;
  for (int i = 1; i < Switch->gateSize("port$o"); i++){
    std::string serverString = "server";
    std::string moduleCheck = Switch->gate("port$o", i)->getNextGate()->getOwnerModule()->getFullName();
    if (Switch->gate("port$o", i)->isConnected()){
      if (moduleCheck.find(serverString) != std::string::npos){
        moduleAddress = Switch->gate("port$o", i)->getId();
        //EV << "Added ID: " << moduleAddress << " to configuration Vector" << endl;
        configuration.push_back(moduleAddress);
      }
    }
  }
}

bool Client::isInConfig(int destAddress){
  for (int i=0; i<configuration.size(); i++){
    if (configuration[i] == destAddress){
      return true;
    }
  }
  return false;
}

void Client::refreshDisplay() const
{
    char buf[40];
    sprintf(buf, "SN: %d  Op: %d", sequenceNumber, lastOperation);
    getDisplayString().setTagArg("t", 0, buf);
}