#include "RPCPacket_m.h"
#include<string>

using namespace omnetpp;
using std::vector;
using std::__cxx11::to_string;
using std::string;

class Admin : public cSimpleModule
{
  public: 
    virtual ~Admin();
  private:
    int myAddress;
    int numberOfNewServers, numberOfServersToRemove;
    int numberOfserversToRemove;

    // Actual configuration
    vector<int> configuration;

    // Servers to purge
    vector<int> toPurge;
    cModule *Switch, *serverToPurge;

    cMessage *changeConfig;
    cGate *newServerPortIN, *newServerPortOUT;
    cGate *newSwitchPortIN, *newSwitchPortOUT;

    // RPC messages
    RPCconfigChangedPacket *configChangedRPC;

    // Methods
    void createNewServer(int index);
    void deleteServer();
    void updateConfiguration();
  protected: 
    virtual void initialize() override;
    virtual void handleMessage(cMessage *msg) override;
};

Define_Module(Admin);

// Destructor
Admin::~Admin()
{
  cancelAndDelete(changeConfig);
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
    delayChannelIN->setDelay(0.01);
    delayChannelOUT->setDelay(0.01);
    newServerPortIN = module->gate("port$i");
    newServerPortOUT = module->gate("port$o");
    newSwitchPortOUT->connectTo(newServerPortIN, delayChannelIN);
    newServerPortOUT->connectTo(newSwitchPortIN, delayChannelOUT);

    // create internals, and schedule it
    module->buildInside();
    module->callInitialize();
}

void Admin::deleteServer()
{
  for (int i = 2; i < Switch->gateSize("port$o"); i++){
        if (Switch->gate("port$o", i)->getId() /*TODO is in toPurge*/ )
        {
          // Get the Server to purge and disconnect port from the switch
          serverToPurge = Switch->gate("port$o", i)->getNextGate()->getOwnerModule();
          serverToPurge->gate("port$o")->disconnect();
          Switch->gate("port$o", i)->disconnect();
          
          // Delete the module
          serverToPurge->callFinish();
          serverToPurge->deleteModule();
        }
    }
}

void Admin::updateConfiguration()
{
    int moduleAddress;
    configuration.clear();
    for (int i = 2; i < Switch->gateSize("port$o"); i++){
        if (Switch->gate("port$o", i)->isConnected())
        {
          moduleAddress = Switch->gate("port$o", i)->getId();
          EV << "Added ID: " << moduleAddress << " to configuration Vector" << endl;
          configuration.push_back(moduleAddress);
        }
    }
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
          scheduleAt(simTime() + par("delay"), changeConfig);
          return;
        }
        else if (numberOfNewServers == 0) {
            // We have added all the new servers, so update the config
            updateConfiguration();
            numberOfNewServers = par("numberOfNewServers");

            // 2) Now new servers has been added, so we can purge the ones we want to remove from config
            for(int i = 0; i < numberOfServersToRemove; i++){
              // Update the config again with server removed, being carefull to not delete new servers just added
              bubble("Removing old servers from config!");
              int position = configuration.size() - 1 - numberOfNewServers - i;
              int purgedAddress = configuration[position];
              toPurge.push_back(purgedAddress);
              EV << "Added ID: " << purgedAddress << " to toPurge Vector" << endl;
              //configuration.erase(configuration.begin() + configuration.size() - 1 - numberOfNewServers);
            }
            // 3) Now we can inform the cluster of the change
            //configChangedRPC = new RPCconfigChangedPacket("RPC_CONFIG_CHANGED", RPC_CONFIG_CHANGED);
            // TODO add the config vector in the RPCconfigChangedPacket
            //send(configChangedRPC, "port$o");
            return;
        }
    }

    // The leader has committed the new configuration, so old servers can be shutted down
    RPCPacket *pk = check_and_cast<RPCPacket *>(msg);
    if (pk->getKind() == RPC_NEW_CONFIG_COMMITTED){
      //deleteServer();
      delete pk;
    }
}