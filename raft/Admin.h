#ifndef __ADMIN_H
#define __ADMIN_H

using namespace omnetpp;
using std::vector;
using std::__cxx11::to_string;
using std::string;


class Admin : public cSimpleModule
{
  public: 
    virtual ~Admin();

    // Actual cluster configuration
    vector<int> configuration;
  private:
    int myAddress;
    int numberOfNewServers, numberOfServersToRemove;
    int numberOfserversToRemove;

    int sequenceNumber = 0;

    // Servers to purge
    vector<int> toPurge;
    cModule *Switch, *serverToPurge;

    cMessage *changeConfig, *resendTimer;
    cGate *newServerPortIN, *newServerPortOUT;
    cGate *newSwitchPortIN, *newSwitchPortOUT;

    // RPC messages
    RPCClientCommandPacket *configChangedRPC;

    // Methods
    void createNewServer(int index);
    void deleteServer();
    void updateConfiguration();
    void sendChangeConfig(bool onlyServer);
  protected: 
    virtual void initialize() override;
    virtual void handleMessage(cMessage *msg) override;
};

#endif