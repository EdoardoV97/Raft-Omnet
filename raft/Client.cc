#include "RPCPacket_m.h"

using namespace omnetpp;

#define STACKSIZE    16384

/**
 * Client computer; see NED file for more info
 */
class Client : public cSimpleModule
{
  public:
    Client() : cSimpleModule(STACKSIZE) {}
    virtual void activity() override;
};

Define_Module(Client);

void Client::activity()
{
  return;
}

