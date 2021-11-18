#include <omnetpp.h>

using namespace omnetpp;

class TRNode : public cSimpleModule {
private:
    cMessage *needResEvt;
    cMessage *finishedUsingResEvt;
    cMessage *token;
    bool needRes;
    simtime_t startWaitingTime;
    cOutVector waitingTime;
protected:
    virtual void initialize();
    virtual void handleMessage(cMessage *msg);
    virtual void finalize();
};

Define_Module(TRNode);

void TRNode::initialize() {
    WATCH(needRes);
    needRes = false;
    needResEvt = new cMessage("Need");
    finishedUsingResEvt = new cMessage("Finish");
    scheduleAt(simTime()+par("reqPeriod"), needResEvt);
    if(par("sendMsgOnInit").boolValue()==true) {
        EV << "Creating the token" << endl;
        send(new cMessage("Token"), "out"); 
    }
    waitingTime.setName("WaitingTime");
}

void TRNode::finalize() {
    cancelAndDelete(needResEvt);
    cancelAndDelete(finishedUsingResEvt);
}

void TRNode::handleMessage(cMessage *msg) {
    if(msg==needResEvt) {
        needRes = true;
        startWaitingTime = simTime();
        EV << "I need accessing resource" << endl;
        bubble("I need the resource");
    } else if(msg==finishedUsingResEvt) {
        needRes = false;
        EV << "Finished using the resource" << endl;
        bubble("Finished");
        send(token, "out");
        scheduleAt(simTime()+par("reqPeriod"), needResEvt);
    } else {// this is the token
        if(needRes) {
            waitingTime.record(simTime()-startWaitingTime);
            EV << "I am accessing the resource after waiting for "
               << (simTime()-startWaitingTime)
               << "s" << endl;
            bubble("Accessing the resource");
            scheduleAt(simTime()+par("usePeriod"), finishedUsingResEvt);
            token = msg;
        } else send(msg,"out");
    }
}


