//
// A client computer which periodically connects to the
// server for data exchange.
//
simple Client
{
    parameters:
        double lowCommandTimeout @unit(s) = default(2s);
        double highCommandTimeout @unit(s) = default(3s);
        double requestTimeout @unit(s) @prompt("Request timeout") = default(0.5s);
        double clientErrorRateThreshold = default(1.0);
        @display("i=device/laptop");
    gates:
        inout port;
}