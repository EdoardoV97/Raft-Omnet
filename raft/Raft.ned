//
// Model of the network, consisting of serveral servers(the cluster), a client, and a switch that connect them
//
network Raft
{
    parameters:
        int numServers @prompt("Number of servers") = default(3);
    submodules:
        admin: Admin{
            parameters:
                @display("p=90,70");
        }
        client: Client {
            parameters:
                requestTimeout = 5s;
                @display("p=210,70");
        }
        switch: Switch {
            parameters:
                delay = 0.005s;
                queueMaxLen = 100; // buffer max 100 packets
                @display("p=210,170");
            gates:
                port[numServers+2]; // 1 extra port for the client, and 1 extra for the switch
        }
        server[numServers]: Server {
            parameters:
                @display("p=70,270,m,10,80");
        }
    connections:
        for i=0..numServers-1 {
            server[i].port <--> {  delay = 5ms; } <--> switch.port[i+2];
        }
        client.port <--> {  delay = 10ms; } <--> switch.port[0];
        admin.port <--> switch.port[1];
}


