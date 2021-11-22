//
// A client computer which periodically connects to the
// server for data exchange.
//
simple Client
{
    parameters:
        double timeout @unit(s) @prompt("Connection timeout") = default(20s);
        volatile double connIaTime @unit(s) @prompt("Connection interarrival time") = default(10s);
        volatile double queryIaTime @unit(s) @prompt("Query interarrival time") = default(2s);
        @display("i=device/pc2");
    gates:
        inout port;
}


