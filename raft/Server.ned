//
simple Server
{
    parameters:
        bool isLeader = default(false);
        int seed = default(1);
        volatile double hearthBeatTime @unit(s) = default(0.01s);
        volatile double lowElectionTimeout @unit(s) = default(0.03s);
        volatile double highElectionTimeout @unit(s) = default(0.05s);
        volatile double resendTimeout @unit(s) = default(0.005s);
        double processingTime @unit(s) @prompt("Time to process a mex") = default(0.002s);
        @display("i=device/server");
    gates:
        inout port;
}


