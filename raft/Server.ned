//
simple Server
{
    parameters:
        int maxLogSizeBeforeSnapshot = default(20);
        double errorRateThreshold = default(1.0);
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


