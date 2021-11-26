simple Admin
{
    parameters:
        int numberOfNewServers = default(2);
        int numberOfserversToRemove = default(1);
        volatile double delay @unit(s) = default(0.001s);
        volatile double changeConfigTime @unit(s) = default(5s);
        @display("i=device/pc2");
    gates:
        inout port;
}