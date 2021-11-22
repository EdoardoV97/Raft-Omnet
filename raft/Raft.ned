//
// Model of the network, consisting of serveral servers(the cluster), a client, and a switch that connect them
//
network Raft
{
    parameters:
        int numServers @prompt("Number of servers") = default(5);
    submodules:
        client: Client {
            parameters:
                timeout = 5s;
                @display("p=210,70");
        }
        switch: Switch {
            parameters:
                delay = 0.005s; // Time switch need to process a mex
                queueMaxLen = 100; // buffer max 100 packets
                @display("p=210,170");
            gates:
                port[numServers+1];
        }
        server[numServers]: Server {
            parameters:
                @display("p=70,270,m,10,80");
        }
    connections:
        for i=0..numServers-1 {
            server[i].port <--> {  delay = 10ms; } <--> switch.port[i];
        }
        client.port <--> {  delay = 10ms; } <--> switch.port[numServers];
}


