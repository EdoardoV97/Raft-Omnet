//
// A client computer which periodically connects to the
// server for data exchange.
//
simple Client
{
    parameters:
        double lowCommandTimeout @unit(s) = default(0.03s);
        double highCommandTimeout @unit(s) = default(0.05s);
        double requestTimeout @unit(s) @prompt("Connection timeout") = default(20s);
        @display("i=device/laptop");
    gates:
        inout port;
}


