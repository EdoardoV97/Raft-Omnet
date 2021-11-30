//
// Model of the network, consisting of serveral servers(the cluster), a client, and a switch that connect them
//
network Raft
{
    parameters:
        int numServers @prompt("Number of servers") = default(3);
        int numClients @prompt("Number of clients") = default(2);
    submodules:
        admin: Admin{
            parameters:
                @display("p=70,40");
        }
        client[numClients]: Client {
            parameters:
                @display("p=210,70,m,10,80");
        }
        switch: Switch {
            parameters:
                @display("p=210,170");
            gates:
                port[numServers+numClients+1]; // 1 extra port for the admin
        }
        server[numServers]: Server {
            parameters:
                @display("p=70,270,m,10,80");
        }
    connections:
        admin.port <--> switch.port[0];
        for i=0..numClients-1 {
            client[i].port <--> {  delay = 10ms; } <--> switch.port[i+1];
        }
        for i=0..numServers-1 {
            server[i].port <--> {  delay = 5ms; } <--> switch.port[numClients+1+i];
        }
}