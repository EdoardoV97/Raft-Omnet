#include "RPCPacket_m.h"
#include <string>
#include "Admin.h"

using namespace omnetpp;
using std::vector;
using std::__cxx11::to_string;
using std::string;


Define_Module(Admin);

// Destructor
Admin::~Admin()
{
  cancelAndDelete(changeConfig);
  cancelAndDelete(resendTimer);
}

void Admin::initialize()
{
    // My address is the out port of the switch corresponding to the input port of this module
    myAddress = gate("port$i")->getPreviousGate()->getId(); 
    Switch = gate("port$i")->getPreviousGate()->getOwnerModule();

    // Get param from configuration
    numberOfNewServers = par("numberOfNewServers");
    numberOfServersToRemove = par("numberOfserversToRemove");

    // Create the self mexs
    changeConfig = new cMessage("changeConfig");
    resendTimer = new cMessage("resendTimer");

    WATCH(myAddress);
    WATCH_VECTOR(configuration);
    WATCH_VECTOR(toPurge);
    updateConfiguration();
    scheduleAt(simTime() + par("changeConfigTime"), changeConfig);
}


void Admin::handleMessage(cMessage *msg)
{
    // Here procedure is:
    // 1) Add the new servers to the config --> they become immediately alive
    // 2) Purge the servers to be removed from the config, still not removing them from the net
    // 3) Inform the cluster the config has changed
    // 4) Once leader committ cNew, Admin can effectively shutdown the old servers

    if(msg == changeConfig){
        if (numberOfNewServers > 0){

          // 1) Add server procedure
          Switch->setGateSize("port", Switch->gateSize("port$o") + 1);
          int index = gate("port$o")->getNextGate()->size() - 1;
          newSwitchPortIN = Switch->gate("port$i", index);
          newSwitchPortOUT = Switch->gate("port$o", index);
          bubble("Adding new server!");
          createNewServer(index-2);
          numberOfNewServers--;

          // Repeat the add server procedure
          scheduleAt(simTime(), changeConfig);
        }
        else if (numberOfNewServers == 0) {
            // We have added all the new servers, so update the config
            updateConfiguration();
            numberOfNewServers = par("numberOfNewServers");

            // 2) Now new servers has been added, so we can purge the ones we want to remove from config
            int oldConfigSize = configuration.size();
            bubble("Removing old servers from config!");
            for(int i = 0; i < numberOfServersToRemove; i++){
              // Update the config again with server removed, being carefull to not delete new servers just added
              int position = oldConfigSize - 1 - numberOfNewServers - i;
              int purgedAddress = configuration[position];
              toPurge.push_back(purgedAddress);
              EV << "Added ID: " << purgedAddress << " to toPurge Vector" << endl;
              configuration.erase(configuration.begin() + configuration.size() - 1 - numberOfNewServers);
              Switch->gate(purgedAddress)->getNextGate()->getOwnerModule()->getDisplayString().setTagArg("i", 1, "red");
            }
            // 3) Now we can inform the clients of the change, and the cluster, knowing that only leader will keep this mex
            //    Note that clients will still try to send mex to servers in toPurge vector, since these servers will be effectively
            //    purged only at step 4)
            sequenceNumber++;
            sendChangeConfig(false);
        }
        return;
    }
    else if (msg == resendTimer){
      // Resend timeout case
      cancelEvent(resendTimer);
      sendChangeConfig(true);
      return;
    }

    // 4) The leader has committed the new configuration, so old servers can be shutted down
    RPCPacket *pk = check_and_cast<RPCPacket *>(msg);
    if (pk->getKind() == RPC_CLIENT_COMMAND_RESPONSE){
      // If sequence number is correct
      RPCClientCommandResponsePacket *response = check_and_cast<RPCClientCommandResponsePacket *>(msg);
      if(sequenceNumber == response->getSequenceNumber()){
        cancelEvent(resendTimer);
        deleteServer();
        bubble("Shutting down old servers");
        toPurge.clear();
        scheduleAt(simTime() + par("changeConfigTime"), changeConfig);
      }
    }
    delete pk;
}


void Admin::createNewServer(int index)
{
    cModuleType *moduleType = cModuleType::get("Server");
    string i = to_string(index);
    string temp = "server[" + i + "]";
    const char *name = temp.c_str();
    cModule *module = moduleType->create(name, getSystemModule());

    cDelayChannel *delayChannelIN = cDelayChannel::create("DelayChannel");
    cDelayChannel *delayChannelOUT = cDelayChannel::create("DelayChannel");
    delayChannelIN->setDelay(0.001);
    delayChannelOUT->setDelay(0.001);
    newServerPortIN = module->gate("port$i");
    newServerPortOUT = module->gate("port$o");
    newSwitchPortOUT->connectTo(newServerPortIN, delayChannelIN);
    newServerPortOUT->connectTo(newSwitchPortIN, delayChannelOUT);

    // create internals, and schedule it
    module->buildInside();
    module->par("instantieatedAtRunTime").setBoolValue(true);
    module->getDisplayString().setTagArg("i", 1, "green");
    module->callInitialize();
}


void Admin::deleteServer()
{
  int serverID;
  for (int i = 1; i < Switch->gateSize("port$o"); i++){
      serverID = Switch->gate("port$o", i)->getId();
      for (int k = 0; k < toPurge.size() ; k++){
        if (serverID == toPurge[k]){
          // Get the Server to purge and disconnect port from the switch
          serverToPurge = Switch->gate("port$o", i)->getNextGate()->getOwnerModule();
          serverToPurge->gate("port$o")->disconnect();
          Switch->gate("port$o", i)->disconnect();
            
          // Delete the Server
          serverToPurge->callFinish();
          serverToPurge->deleteModule();
        }
      }
  }
}


void Admin::updateConfiguration()
{
    int moduleAddress;
    configuration.clear();
    for (int i = 1; i < Switch->gateSize("port$o"); i++){
        if (Switch->gate("port$o", i)->isConnected()){
          std::string serverString = "server";
          std::string moduleCheck = Switch->gate("port$o", i)->getNextGate()->getOwnerModule()->getFullName();
          if(moduleCheck.find(serverString) != std::string::npos){
            moduleAddress = Switch->gate("port$o", i)->getId();
            EV << "Added ID: " << moduleAddress << " to configuration Vector" << endl;
            configuration.push_back(moduleAddress);
          }
        }
    }
}


void Admin::sendChangeConfig(bool onlyServer){
  // This mex need to be send to all: both cluster's servers and clients
  for (int i=1; i < Switch->gateSize("port$o"); i++){
    if (Switch->gate("port$o", i)->isConnected()){
          cluster_configuration newConfig;
          newConfig.servers.assign(configuration.begin(), configuration.end());
          configChangedRPC = new RPCClientCommandPacket("RPC_CLIENT_COMMAND", RPC_CLIENT_COMMAND);
          configChangedRPC->setSrcAddress(myAddress);
          configChangedRPC->setDestAddress(Switch->gate("port$o", i)->getId());
          configChangedRPC->setClusterConfig(newConfig);
          configChangedRPC->setSequenceNumber(sequenceNumber);
          configChangedRPC->setVar('C');
          configChangedRPC->setType(1); // Mark as write
          send(configChangedRPC, "port$o");
    }
  }
  scheduleAt(simTime() + par("resendTimer"), resendTimer);
  return;
}