//
// A client computer which periodically connects to the
// server for data exchange.
//
simple Client
{
    parameters:
        double lowCommandTimeout @unit(s) = default(0.3s);
        double highCommandTimeout @unit(s) = default(0.5s);
        double requestTimeout @unit(s) @prompt("Request timeout") = default(0.5s);
        @display("i=device/laptop");
    gates:
        inout port;
}


