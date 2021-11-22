//
simple Server
{
    parameters:
        bool isLeader = default(false);
        volatile double hearthBeatTime @unit(s) = default(0.1s);
        double processingTime @unit(s) @prompt("Time to process a mex") = default(0.2s);
        @display("i=device/server");
    gates:
        inout port;
}


