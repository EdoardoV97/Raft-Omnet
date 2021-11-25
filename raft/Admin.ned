simple Admin
{
    parameters:
        int numberOfNewServers = default(3);
        int numberOfserversToRemove = default(2);
        volatile double delay @unit(s) = default(0.001s);
        volatile double changeConfigTime @unit(s) = default(0.2s);
        @display("i=device/pc2");
    gates:
        inout port;
}