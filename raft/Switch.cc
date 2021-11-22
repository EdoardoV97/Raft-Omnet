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
    //simtime_t pkDelay = 1 / (double)par("pkRate");
    int queueMaxLen = (int)par("queueMaxLen");
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

        // send msg to destination
        RPCPacket *pk = check_and_cast<RPCPacket *>(msg);
        int source = pk->getSrcAddress();
        if (pk->getIsBroadcast() == true){
            EV << "Relaying msg received in broadcast\n";
            for (int i = 0; i < gateSize("port$o")-1; i++)
                {
                    if (i != source){
                        pk_copy = pk->dup();
                        send(pk_copy, "port$o", i);
                    }
                }
            delete pk;
        }
        else{
            int dest = pk->getDestAddress();
            EV << "Relaying msg received to addr=" << dest << "\n";
            send(msg, "port$o", dest);
        }

        // display status: normal=queue empty, yellow=queued packets; red=queue overflow
        int qLen = queue.getLength();
        if (hasGUI())
            getDisplayString().setTagArg("i", 1, qLen == 0 ? "" : qLen < queueMaxLen ? "gold" : "red");
    }
}

