#include "RPCPacket_m.h"

using namespace omnetpp;

#define STACKSIZE    16384

/**
 * Simulates a switch between clients and server; see NED file for more info
 */
class Switch : public cSimpleModule
{
  private:
    RPCPacket *pk_copy;
  public:
    Switch() : cSimpleModule(STACKSIZE) {}
    virtual void activity() override;
};

Define_Module(Switch);

void Switch::activity()
{
    int queueMaxLen = (int)par("queueMaxLen");
    int destAddress;
    cQueue queue("queue");
    for ( ; ; ) {
        // receive msg
        cMessage *msg;
        if (!queue.isEmpty())
            msg = (cMessage *)queue.pop();
        else
            msg = receive();

        // model processing delay; packets that arrive meanwhile are queued
        waitAndEnqueue(par("delay"), &queue);


        RPCPacket *pk = check_and_cast<RPCPacket *>(msg);
        // Send in broadcast
        int source = pk->getSrcAddress();
        if (pk->getIsBroadcast() == true){
            EV << "Relaying msg received in broadcast to the Cluster\n";
            for (int i = 1; i < gateSize("port$o"); i++){
                if (gate("port$o", i)->isConnected()){
                    std::string serverString = "server";
                    std::string moduleCheck = gate("port$o", i)->getNextGate()->getOwnerModule()->getFullName();
                    // We need to check if receiver is a server, otherwise don't send
                    if (moduleCheck.find(serverString) != std::string::npos)
                    {
                        // Check that receiver isn't the sender himself
                        if (gate("port$o", i)->getId() != source){
                                pk_copy = pk->dup();
                                send(pk_copy, "port$o", i);
                        }
                    }
                }
            }
            delete pk;
        }
        // Send to specific address
        else{
            destAddress = pk->getDestAddress();
            const char* destName = gate(destAddress)->getNextGate()->getOwnerModule()->getFullName();
            EV << "Relaying msg received to " << destName << "  (addr = " << destAddress << ")\n";
            send(msg, destAddress);
        }
    }
}