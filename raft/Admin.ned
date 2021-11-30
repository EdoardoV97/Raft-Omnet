simple Admin
{
    parameters:
        int numberOfNewServers = default(2);
        int numberOfserversToRemove = default(1);
        double resendTimer @unit(s) = default(1s);
        volatile double changeConfigTime @unit(s) = default(5s);
        @display("i=device/pc2");
    gates:
        inout port;
}