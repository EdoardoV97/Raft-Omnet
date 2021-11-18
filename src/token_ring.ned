simple TRNode {
  parameters:
    bool sendMsgOnInit = default(false);
    volatile double reqPeriod @unit(s) = default(uniform(300s,500s));
    volatile double usePeriod @unit(s) = default(uniform(1s,2s));
        
    @display("i=block/process");
  gates:
    input in;
    output out;
}

channel MyChannel extends ned.DatarateChannel {
//    delay = uniform(90ms,110ms);
//    datarate = normal(1024kbps,1kbps);
    delay = 100ms;
    datarate = 1024kbps;
}

network TokenRing {
  parameters:
    int n = default(5);

  submodules:
    node[n]: TRNode {    }
    
  connections:
    for i=0..n-1 {
            node[i].out --> MyChannel --> node[(i+1)%n].in;
    }
}
