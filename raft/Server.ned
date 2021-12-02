//
simple Server
{
    parameters:
        int maxLogSizeBeforeSnapshot;
        volatile bool instantieatedAtRunTime = default(false);
        volatile double hearthBeatTime @unit(s);
        volatile double lowElectionTimeout @unit(s);
        volatile double highElectionTimeout @unit(s);
        volatile double resendTimeout @unit(s);
        volatile double lowCrashTimeout @unit(s);
        volatile double highCrashTimeout @unit(s);
        volatile double reviveTimeout @unit(s);
        @display("i=device/server");
    gates:
        inout port;
}


