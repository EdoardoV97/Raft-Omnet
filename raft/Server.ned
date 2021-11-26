//
simple Server
{
    parameters:
        int seed = default(1);
        volatile double hearthBeatTime @unit(s);
        volatile double lowElectionTimeout @unit(s);
        volatile double highElectionTimeout @unit(s);
        volatile double resendTimeout @unit(s);
        @display("i=device/server");
    gates:
        inout port;
}


