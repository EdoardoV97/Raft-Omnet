#include "RPCPacket_m.h"
#include <algorithm>

using namespace omnetpp;
using std::vector;

/**
 * The basic server of the cluster; see NED file for more info
 */
class Server : public cSimpleModule
{
  public: 
    virtual ~Server();
  private:
    const int READ = 0;
    const int WRITE = 1;

    serverState status = FOLLOWER;
    
    cMessage *sendHearthbeat;
    cMessage *electionTimeoutEvent;
    vector<append_entry_timer> appendEntryTimers;
    
    int myAddress;   // This is the server ID
    int receiverAddress; // This is receiver server ID
    
    int votes = 0; // This is the number of received votes by servers in the configuration (meaninful when status = candidate). It refers to member in the "old" configuration when a membership change in occurring.
    int votesNewConfig = 0; // This is the number of received votes by servers in the "new" configuration (meaninful when status = candidate) when a membership change in occurring.

    int leaderAddress = -1; // This is the leader ID
    int acks = 0; // This is the number of acks (if membership change: acks from configuration)
    int acksNewConf = 0; // This is the number of akcs of the newConfiguration in case of membership change occurring
    bool countingFeedback = false;
    bool withAck = false;
    int heartbeatSeqNum = 0; // This is to allow acks for heartbeat for read-only operations
    bool waitingNoOp = false;
    vector<int> pendingReadClients;

    int adminAddress;  // This is the admin address ID

    // Pointers to handle RPC mexs
    RPCAppendEntriesPacket *appendEntriesRPC = nullptr;
    RPCAppendEntriesResponsePacket *appendEntriesResponseRPC = nullptr;
    RPCRequestVotePacket *requestVoteRPC = nullptr;
    RPCRequestVoteResponsePacket *requestVoteResponseRPC = nullptr;
    RPCInstallSnapshotPacket *installSnapshotRPC = nullptr;
    RPCClientCommandResponsePacket *clientCommandResponseRPC = nullptr;
    RPCAckPacket *ACK;

    // State Machine of the server
    int x = 0;
    vector<int> configuration;
    vector<int> newConfiguration;
    vector<latest_client_response> latestClientResponses;

    // Persistent state --> Updated on stable storage before responding to RPCs
    int currentTerm = 0;
    int votedFor = -1;
    vector<log_entry> log;

    // Volatile state --> Reinitialize after crash
    int commitIndex = 1; // E se il primo log lo committiamo sempre noi??
    int lastApplied = 1;

    // Volatile state on leaders --> Reinitialized after election
    vector<int> nextIndex;
    vector<int> matchIndex;

    vector<int> nextIndexNewConfig;
    vector<int> matchIndexNewConfig;

    void initializeConfiguration();

  protected:
    virtual void initialize() override;
    virtual void handleMessage(cMessage *msg) override;
    void appendNewEntry(log_entry newEntry);
    int getIndex(vector<int> v, int K);
    void appendNewEntryTo(log_entry newEntry, int destAddress, int index);
    bool majority(int N);
    void applyCommand(log_entry entry);
    void becomeLeader();
    void becomeCandidate();
    void becomeFollower(RPCPacket *pkGeneric);
    void updateTerm(RPCPacket *pkGeneric);
    int getClientIndex(int clientAddress);
    void updateLatestClientSequenceNumber(int clientAddress, int sequenceNumber);
    void sendAck(int destAddress, int seqNum);
    void sendResponseToClient(int type, int clientAddress);
    void startReadOnlyLeaderCheck();
};

Define_Module(Server);


// Destructor
Server::~Server()
{
  cancelAndDelete(sendHearthbeat);
  //TODO: eliminate others
}

void Server::initialize()
{
  // Create self messages needed for timeouts
  sendHearthbeat = new cMessage("send-hearthbeat");
  electionTimeoutEvent = new cMessage("election-timeout-event");

  // Initialize addresses
  myAddress = gate("port$i")->getPreviousGate()->getId(); // Return index of this server gate port in the Switch
  adminAddress = gate("port$o")->getNextGate()->getOwnerModule()->gate("port$o", 0)->getId();

  WATCH(adminAddress);
  WATCH(myAddress);
  WATCH_VECTOR(configuration);
  WATCH_VECTOR(newConfiguration);
  WATCH(x);
  WATCH(currentTerm);
  WATCH(votedFor);
  //WATCH_RW(log);
  WATCH(commitIndex);
  WATCH(lastApplied);
  
  WATCH_VECTOR(nextIndex);
  WATCH_VECTOR(matchIndex);

  WATCH(status);

  initializeConfiguration();
  //Pushing the initial configuration in the log
  log_entry firstEntry;
  firstEntry.var = 'x';
  firstEntry.value = x;
  firstEntry.term = currentTerm;
  firstEntry.logIndex = 0;
  //firstEntry.cOld.assign(configuration.begin(), configuration.end());
  log.push_back(firstEntry);
  scheduleAt(simTime() +  uniform(SimTime(par("lowElectionTimeout")), SimTime(par("highElectionTimeout"))), electionTimeoutEvent);
}

void Server::handleMessage(cMessage *msg)
{
  if(msg == sendHearthbeat){
    // Send an empty RPCAppendEntries(= hearthbeat), to all followers
    EV << "Sending hearthbeat to followers\n";
    log_entry emptyEntry;
    emptyEntry.term = -1; //convention to explicit heartbeat 
    clients_data temp;
    temp.responses.assign(latestClientResponses.begin(), latestClientResponses.end());


    appendEntriesRPC = new RPCAppendEntriesPacket("RPC_APPEND_ENTRIES", RPC_APPEND_ENTRIES);
    appendEntriesRPC->setTerm(currentTerm);
    appendEntriesRPC->setLeaderId(myAddress);
    appendEntriesRPC->setEntry(emptyEntry);
    appendEntriesRPC->setLeaderCommit(commitIndex);
    appendEntriesRPC->setClientsData(temp);
    appendEntriesRPC->setSrcAddress(myAddress);
    appendEntriesRPC->setIsBroadcast(true);
    if(withAck == true){
      appendEntriesRPC->setHeartbeatSeqNum(heartbeatSeqNum);
      withAck = false;
    }else{
      appendEntriesRPC->setHeartbeatSeqNum(-1);
    }
    send(appendEntriesRPC, "port$o");

    scheduleAt(simTime() + par("hearthBeatTime"), sendHearthbeat);
    return;
  }

  if (msg == electionTimeoutEvent){
    EV << "Starting a new leader election, i am a candidate\n";
    becomeCandidate();
    requestVoteRPC = new RPCRequestVotePacket("RPC_REQUEST_VOTE", RPC_REQUEST_VOTE);
    requestVoteRPC->setTerm(currentTerm);
    requestVoteRPC->setCandidateId(myAddress);
    requestVoteRPC->setLastLogIndex(log.back().logIndex);
    requestVoteRPC->setLastLogTerm(log.back().term);
    requestVoteRPC->setSrcAddress(myAddress);
    requestVoteRPC->setIsBroadcast(true);
    send(requestVoteRPC, "port$o");
    return;
  }
  
  // Retry append entries if an appendEntryTimer is fired 
  for (int i = 0; i < appendEntryTimers.size() ; i++){
    if (msg == appendEntryTimers[i].timeoutEvent){
      log_entry newEntry = appendEntryTimers[i].entry;
      newEntry.cOld.assign(appendEntryTimers[i].entry.cOld.begin(), appendEntryTimers[i].entry.cOld.end());
      newEntry.cNew.assign(appendEntryTimers[i].entry.cNew.begin(), appendEntryTimers[i].entry.cNew.end());
      clients_data temp;
      temp.responses.assign(latestClientResponses.begin(), latestClientResponses.end());
      //Resend the message
      appendEntriesRPC = new RPCAppendEntriesPacket("RPC_APPEND_ENTRIES", RPC_APPEND_ENTRIES);
      appendEntriesRPC->setTerm(currentTerm);
      appendEntriesRPC->setLeaderId(myAddress);
      appendEntriesRPC->setPrevLogIndex(appendEntryTimers[i].prevLogIndex); 
      appendEntriesRPC->setPrevLogTerm(appendEntryTimers[i].prevLogTerm);
      appendEntriesRPC->setEntry(newEntry);
      appendEntriesRPC->setLeaderCommit(commitIndex);
      appendEntriesRPC->setClientsData(temp);
      appendEntriesRPC->setSrcAddress(myAddress);
      appendEntriesRPC->setIsBroadcast(false);
      appendEntriesRPC->setDestAddress(appendEntryTimers[i].destination);
      send(appendEntriesRPC, "port$o");
      //Reset timer
      scheduleAt(simTime() + par("resendTimeout"), appendEntryTimers[i].timeoutEvent);
      return;
    }
    
  }
    
  
  RPCPacket *pkGeneric = check_and_cast<RPCPacket *>(msg);

  updateTerm(pkGeneric);

  switch (pkGeneric->getKind())
  {
  case RPC_APPEND_ENTRIES:
  {
    RPCAppendEntriesPacket *pk = check_and_cast<RPCAppendEntriesPacket *>(pkGeneric);
    // If is NOT heartbeat
    if (pk->getEntry().term != -1){
      cancelEvent(electionTimeoutEvent);
      receiverAddress = pk->getSrcAddress();
      //1) Reply false if term < currentTerm 2)Reply false if log doesn’t contain an entry at prevLogIndex whose term matches prevLogTerm 
      // To avoid out of bound access
      bool partialEval = false;
      if(pk->getPrevLogIndex() < log.size()){
        if(log[pk->getPrevLogIndex()].term != pk->getPrevLogTerm()){
          partialEval = true;
        }
      }else{partialEval = true;}      
      if((pk->getTerm() < currentTerm) || partialEval){
        appendEntriesResponseRPC = new RPCAppendEntriesResponsePacket("RPC_APPEND_ENTRIES_RESPONSE", RPC_APPEND_ENTRIES_RESPONSE);
        appendEntriesResponseRPC->setSuccess(false);
        appendEntriesResponseRPC->setTerm(currentTerm);
        appendEntriesResponseRPC->setSrcAddress(myAddress);
        appendEntriesResponseRPC->setDestAddress(receiverAddress);
        send(appendEntriesResponseRPC, "port$o");
        }
      else {
        //3)If an existing entry conflicts with a new one (same index but different terms), delete the existing entry and all that follow it.
        if (log[pk->getEntry().logIndex].logIndex == pk->getEntry().logIndex && log[pk->getEntry().logIndex].term != pk->getEntry().term){
          log.resize(pk->getEntry().logIndex);
        }
        //4)Append any new entries not already in the log.
        log.push_back(pk->getEntry());
        //5)If leaderCommit > commitIndex, set commitIndex = min (leaderCommit, index of last new entry).
        if(pk->getLeaderCommit() > commitIndex){
          if(pk->getLeaderCommit() < pk->getEntry().logIndex){
            commitIndex = pk->getLeaderCommit();
          } else{
            commitIndex = pk->getEntry().logIndex;
          }
        }
        latestClientResponses.assign(pk->getClientsData().responses.begin(), pk->getClientsData().responses.end());
        // If it's a membership change entry
        if(pk->getEntry().var == 'C'){
          // If it is the entry of the first phase (Cold,new)
          if(!pk->getEntry().cOld.empty()){
            configuration.assign(pk->getEntry().cOld.begin(), pk->getEntry().cOld.end()); // It is necessary only for servers of the new configuration to learn the old configuration
            newConfiguration.assign(pk->getEntry().cNew.begin(), pk->getEntry().cNew.end());
          }
          // If it is the entry of the second phase (Cnew)
          else{
            configuration = newConfiguration; // va fatto solo in quelle incluse in Cnew?
          }
        } 
        //Reply true
        appendEntriesResponseRPC = new RPCAppendEntriesResponsePacket("RPC_APPEND_ENTRIES_RESPONSE", RPC_APPEND_ENTRIES_RESPONSE);
        appendEntriesResponseRPC->setSuccess(true);
        appendEntriesResponseRPC->setTerm(currentTerm);
        appendEntriesResponseRPC->setSrcAddress(myAddress);
        appendEntriesResponseRPC->setDestAddress(receiverAddress);
        send(appendEntriesResponseRPC, "port$o");
      }
      scheduleAt(simTime() +  uniform(SimTime(par("lowElectionTimeout")), SimTime(par("highElectionTimeout"))), electionTimeoutEvent);
    }
    else{  // Heartbeat case
      //If i am candidate
      if(status == CANDIDATE){
          if(pk->getTerm() == currentTerm){ //the > case is already tested with updateTerm() before the switch
            becomeFollower(pk);
          }
          //otherwise the electionTimeoutEvent remains valid
        }
        else{ //I am not a candidate, and i am a follower remain follower, otherwise if i am a leader i ignore heartbeat
          if (status == FOLLOWER){
          cancelEvent(electionTimeoutEvent);
          leaderAddress = pk->getLeaderId();
          if(pk->getLeaderCommit() > commitIndex){
            if(pk->getLeaderCommit() < pk->getEntry().logIndex){
              commitIndex = pk->getLeaderCommit();
            } else{
              commitIndex = pk->getEntry().logIndex;
            }
          }
          if(pk->getHeartbeatSeqNum() != -1){
            sendAck(pk->getSrcAddress(), pk->getHeartbeatSeqNum());
          }
          latestClientResponses.assign(pk->getClientsData().responses.begin(), pk->getClientsData().responses.end());
          scheduleAt(simTime() +  uniform(SimTime(par("lowElectionTimeout")), SimTime(par("highElectionTimeout"))), electionTimeoutEvent);
         }
        }
    }

    for(int i=lastApplied ; commitIndex > i ; i++){
      lastApplied++;
      applyCommand(log[lastApplied]);
    }
  }
  break;
  case RPC_REQUEST_VOTE:
  {
    RPCRequestVotePacket *pk = check_and_cast<RPCRequestVotePacket *>(pkGeneric);
    receiverAddress = pk->getSrcAddress();
    //1)Reply false if term < currentTerm.
    if (pk->getTerm() < currentTerm){
      requestVoteResponseRPC = new RPCRequestVoteResponsePacket("RPC_REQUEST_VOTE_RESPONSE", RPC_REQUEST_VOTE_RESPONSE);
      requestVoteResponseRPC->setVoteGranted(false);
      requestVoteResponseRPC->setTerm(currentTerm);
      requestVoteResponseRPC->setSrcAddress(myAddress);
      requestVoteResponseRPC->setDestAddress(receiverAddress);
      send(requestVoteResponseRPC, "port$o");
    } else{
      //2)If votedFor is null or candidateId, and candidate’s log is at least as up-to-date as receiver’s log, grant vote.
      if((votedFor == -1 || votedFor == pk->getCandidateId()) && (log.back().term < pk->getLastLogTerm() || (log.back().term == pk->getLastLogTerm() && log.back().logIndex <= pk->getLastLogIndex()))){
        cancelEvent(electionTimeoutEvent);
        votedFor = pk->getCandidateId();
        requestVoteResponseRPC = new RPCRequestVoteResponsePacket("RPC_REQUEST_VOTE_RESPONSE", RPC_REQUEST_VOTE_RESPONSE);
        requestVoteResponseRPC->setVoteGranted(true);
        requestVoteResponseRPC->setTerm(currentTerm);
        requestVoteResponseRPC->setSrcAddress(myAddress);
        requestVoteResponseRPC->setDestAddress(receiverAddress);
        send(requestVoteResponseRPC, "port$o");
        scheduleAt(simTime() +  uniform(SimTime(par("lowElectionTimeout")), SimTime(par("highElectionTimeout"))), electionTimeoutEvent);
      }
      else{
        requestVoteResponseRPC = new RPCRequestVoteResponsePacket("RPC_REQUEST_VOTE_RESPONSE", RPC_REQUEST_VOTE_RESPONSE);
        requestVoteResponseRPC->setVoteGranted(false);
        requestVoteResponseRPC->setTerm(currentTerm);
        requestVoteResponseRPC->setSrcAddress(myAddress);
        requestVoteResponseRPC->setDestAddress(receiverAddress);
        send(requestVoteResponseRPC, "port$o");
      }
    }
  }
  break;
  case RPC_APPEND_ENTRIES_RESPONSE:
  {
    RPCAppendEntriesResponsePacket *pk = check_and_cast<RPCAppendEntriesResponsePacket *>(pkGeneric);
    if(status == LEADER){
      receiverAddress = pk->getSrcAddress();
      
      for(int i=0; i < appendEntryTimers.size() ; i++){
        if (receiverAddress == appendEntryTimers[i].destination){
          cancelEvent(appendEntryTimers[i].timeoutEvent);
          appendEntryTimers.erase(appendEntryTimers.begin() + i);
          break;
        }
      }
      // If a very late message coming from someone from before a membership change already completed (which didn't include him in the new configuration)
      if (getIndex(configuration, pk->getSrcAddress()) == -1 && getIndex(newConfiguration, pk->getSrcAddress()) == -1)
      {
        break;
      }
      

      if(pk->getSuccess() == false){
        int position;
        int nextEntryIndex;
        log_entry newEntry;
        // If membership change is occurring
        if(configuration != newConfiguration){
          if(getIndex(configuration, pk->getSrcAddress()) != -1){
            position = getIndex(configuration, pk->getSrcAddress());
            nextEntryIndex = --nextIndex[position]; // first decrement and then save in the variable
            // If it is also in newConfiguration manage to decrement also in nextIndexNewConfig
            if(getIndex(newConfiguration, pk->getSrcAddress()) != -1){
              --nextIndexNewConfig[getIndex(newConfiguration, pk->getSrcAddress())];
            }        
          }
          else{
            position = getIndex(newConfiguration, pk->getSrcAddress());
            nextEntryIndex = --nextIndexNewConfig[position];
          }
        }
        else{ // Not membership change occurring
          position = getIndex(configuration, pk->getSrcAddress());
          nextEntryIndex = --nextIndex[position];
        }
        newEntry = log[nextEntryIndex];
        if(newEntry.var == 'C'){  
          newEntry.cOld.assign(newEntry.cOld.begin(), newEntry.cOld.end());
          newEntry.cNew.assign(newEntry.cNew.begin(), newEntry.cNew.end());
        }
        appendNewEntryTo(newEntry, receiverAddress, position);
      }
      else{
        int position;
        int nextEntryIndex;
        // If membership change is occurring
        if(configuration != newConfiguration){
          if(getIndex(configuration, pk->getSrcAddress()) != -1){
            position = getIndex(configuration, pk->getSrcAddress());
            matchIndex[position] = nextIndex[position];
            nextEntryIndex = ++nextIndex[position];
            
            if(getIndex(newConfiguration, pk->getSrcAddress()) != -1){
              ++nextIndexNewConfig[getIndex(newConfiguration, pk->getSrcAddress())];
            }

            if(nextEntryIndex <= log.size()){
              log_entry newEntry;
              newEntry = log[nextEntryIndex];
              if(newEntry.var == 'C'){  
                newEntry.cOld.assign(newEntry.cOld.begin(), newEntry.cOld.end());
                newEntry.cNew.assign(newEntry.cNew.begin(), newEntry.cNew.end());
              }
              appendNewEntryTo(newEntry, receiverAddress, position);
            }
          }
          else{
            position = getIndex(newConfiguration, pk->getSrcAddress());
            matchIndexNewConfig[position] = nextIndexNewConfig[position];
            nextEntryIndex = ++nextIndexNewConfig[position]; 
            if(nextIndexNewConfig[position] <= log.size()){
              log_entry newEntry;
              newEntry = log[nextEntryIndex];
              if(newEntry.var == 'C'){  
                newEntry.cOld.assign(newEntry.cOld.begin(), newEntry.cOld.end());
                newEntry.cNew.assign(newEntry.cNew.begin(), newEntry.cNew.end());
              }
              appendNewEntryTo(newEntry, receiverAddress, position);
            }
          }
        }
        else{ // Not membership change occurring
          position = getIndex(configuration, pk->getSrcAddress());
          matchIndex[position] = nextIndex[position];
          nextEntryIndex = ++nextIndex[position]; 
          if(nextIndex[position] <= log.size()){
            log_entry newEntry;
            newEntry = log[nextEntryIndex];
            if(newEntry.var == 'C'){  
              newEntry.cOld.assign(newEntry.cOld.begin(), newEntry.cOld.end());
              newEntry.cNew.assign(newEntry.cNew.begin(), newEntry.cNew.end());
            }
            appendNewEntryTo(newEntry, receiverAddress, position);
          }
        }

        for (int newCommitIndex = commitIndex + 1; newCommitIndex < log.size(); newCommitIndex++){
          if(majority(newCommitIndex) == true && log[newCommitIndex].term == currentTerm){
            commitIndex = newCommitIndex;
          }
        }
        for(int i=lastApplied; commitIndex > i; i++){
          lastApplied++;
          applyCommand(log[lastApplied]);
          if(log[lastApplied].term == currentTerm){
            if(log[lastApplied].var == 'N'){
              if(waitingNoOp == true){
                waitingNoOp = false;
                startReadOnlyLeaderCheck();
              }
            }
            else{
              if(log[lastApplied].var == 'C'){ 
                if(!log[lastApplied].cOld.empty()){ //Cold,new case, trigger the Cnew append
                  log_entry newEntry;
                  newEntry.term = currentTerm;
                  newEntry.logIndex = log.size();
                  newEntry.clientAddress = log[lastApplied].clientAddress;
                  newEntry.var = 'C';
                  newEntry.value = log[lastApplied].value;
                  // implicitly newEntry.cOld is empty (Convention: it means that it is the Cnew entry)
                  newEntry.cNew.assign(pk->getClusterConfig().servers.begin(), pk->getClusterConfig().servers.end());
                  appendNewEntry(newEntry);
                }
                else{ //If Cnew is now committed (second phase is terminated)
                  // If 
                  if(getIndex(newConfiguration, myAddress) == -1){
                    becomeFollower();
                  }
                  configuration = newConfiguration; // TODO va messo?
                }
              }
              else{ // Write response case
                sendResponseToClient(WRITE, log[lastApplied].clientAddress);
              }
            }
          }
        }
      }
    }
  }
    break;
  case RPC_REQUEST_VOTE_RESPONSE:
  {
    RPCRequestVoteResponsePacket *pk = check_and_cast<RPCRequestVoteResponsePacket *>(pkGeneric);
    //If i am still candidate
    if(status == CANDIDATE){
      // If the vote is granted
      if (pk->getVoteGranted() == true){
        // If there is a membership change is NOT occurring
        if (newConfiguration == configuration){
          votes++;
        }
        else{ // If a membership change IS occurring
          // If the vote is granted by a server in configuration
          if(getIndex(configuration, pk->getSrcAddress()) != -1){
            votes++;
          }
          // If the vote is granted by a server in newConfiguration (note: a server can be in both new and old configurations)
          if(getIndex(newConfiguration, pk->getSrcAddress()) != -1){
            votesNewConfig++;
          }
        }
      }
      // Check majority of votes for configuration
      if(votes > configuration.size()/2){
        //If a membership change is NOT occurring it is sufficient
        if(newConfiguration == configuration){
          becomeLeader();
        }
        else{ // If a membership change IS taking place i must also check the other majority
          if(votesNewConfig > newConfiguration.size()/2){
            becomeLeader();
          }
        }
      }
    }
  }
  break;
  case RPC_CLIENT_COMMAND:
  {
    RPCClientCommandPacket *pk = check_and_cast<RPCClientCommandPacket *>(pkGeneric);
    //Process incoming command from a client
    if(status == LEADER){ 
      // If request already served
      for(int i=0; i < latestClientResponses.size(); i++){
        if(latestClientResponses[i].clientAddress == pk->getSrcAddress() && latestClientResponses[i].latestSequenceNumber == pk->getSequenceNumber()){
          clientCommandResponseRPC = new RPCClientCommandResponsePacket("RPC_CLIENT_COMMAND_RESPONSE", RPC_CLIENT_COMMAND_RESPONSE);
          clientCommandResponseRPC->setSequenceNumber(latestClientResponses[i].latestSequenceNumber);
          clientCommandResponseRPC->setValue(latestClientResponses[i].latestReponseToClient); // If it was not a read, it is not a problem, the client simply would ignore this field by himself which has no meaning
          clientCommandResponseRPC->setSrcAddress(myAddress);
          clientCommandResponseRPC->setDestAddress(latestClientResponses[i].clientAddress);
          send(clientCommandResponseRPC, "port$o");
          break;
        }
      }
      // Keep track of eventual new clients
      if(getClientIndex(pk->getSrcAddress()) == -1){
        latest_client_response temp;
        temp.clientAddress = pk->getSrcAddress();
        temp.latestSequenceNumber = pk->getSequenceNumber()-1; //Thus when replying simply do ++
        latestClientResponses.push_back(temp);
      }
      
      if(pk->getType() == WRITE){
        log_entry newEntry;
        newEntry.term = currentTerm;
        newEntry.logIndex = log.size();
        newEntry.clientAddress = pk->getSrcAddress();
        newEntry.var = pk->getVar();
        newEntry.value = pk->getValue();
        if(pk->getVar() == 'C'){
          newEntry.cOld.assign(configuration.begin(), configuration.end());
          newEntry.cNew.assign(pk->getClusterConfig().servers.begin(), pk->getClusterConfig().servers.end());
          newConfiguration.assign(pk->getClusterConfig().servers.begin(), pk->getClusterConfig().servers.end());
          // Initializing nextIndexNewConfig and matchIndexNewConfig to manage servers in the newConfiguration
          nextIndexNewConfig.resize(newConfiguration.size(), log.back().logIndex + 1);
          matchIndexNewConfig.resize(newConfiguration.size(), 0);
        }
        log.push_back(newEntry);
        cancelEvent(sendHearthbeat);
        appendNewEntry(newEntry);
        scheduleAt(simTime() + par("hearthBeatTime"), sendHearthbeat);      
      }
      else{ // READ case
        if(pendingReadClients.empty() == true){
          pendingReadClients.push_back(pk->getSrcAddress());
          
          if(log[commitIndex].term == currentTerm){ // If already committed an entry in this term (e.g., at least the initial no_op already committed), an alternative is to check if the index of the last no_op in the log is <= commitIndex (and it's term == currentTerm)
            startReadOnlyLeaderCheck();
          }
          else{
            waitingNoOp = true; // To signal the interest on the no_op commit
          }
        }
        else{
          if(getIndex(pendingReadClients, pk->getSrcAddress()) == -1){ // If the client has resend the request because ha not received yet a response
            pendingReadClients.push_back(pk->getSrcAddress());
          }
        }
      }
    }
    else{ //Redirect the client to the last known leader
      clientCommandResponseRPC = new RPCClientCommandResponsePacket("RPC_CLIENT_COMMAND_RESPONSE", RPC_CLIENT_COMMAND_RESPONSE);
      clientCommandResponseRPC->setRedirect(true);
      clientCommandResponseRPC->setLastKnownLeader(leaderAddress); //the client will check if it is -1; thus no Leader because startup of the cluster
      clientCommandResponseRPC->setSrcAddress(myAddress);
      clientCommandResponseRPC->setDestAddress(pk->getSrcAddress());
      send(clientCommandResponseRPC, "port$o"); 
    }
  }
    break;
  case RPC_ACK:
  {
    RPCAckPacket *pk = check_and_cast<RPCAckPacket *>(pkGeneric);
    if(status == LEADER){
      if(countingFeedback == true){
        if(heartbeatSeqNum == pk->getSequenceNumber()){
          // If not membership change occurring
          if(configuration == newConfiguration){
            acks++;
          }
          else{ // If membership change occurring
            if(getIndex(configuration, pk->getSrcAddress()) != -1){
              acks++;
            } 
            if(getIndex(newConfiguration, pk->getSrcAddress()) != -1){
              acksNewConf++;
            } 
          }
        }
        // If majority of heartbeat exchanged, is possible to finalize the pending read-only request (NO membership change occurring)
        if(configuration == newConfiguration){
          if(acks > configuration.size()/2){
            countingFeedback = false;
            for (int i = 0; i < pendingReadClients.size(); i++){
              sendResponseToClient(READ, pendingReadClients[i]);
            }
            pendingReadClients.clear();
          }
        }
        else{ // Membership change occurring, acks must be received by both majority
          if(acks > configuration.size()/2 && acksNewConf > newConfiguration.size()/2){
            countingFeedback = false;
            for (int i = 0; i < pendingReadClients.size(); i++){
              sendResponseToClient(READ, pendingReadClients[i]);
            }
            pendingReadClients.clear();
          }
        }
      }
    }
  }
    break;
  default:
    break;
  }

  delete pkGeneric;
    
}

void Server::appendNewEntry(log_entry newEntry){
  clients_data temp;
  temp.responses.assign(latestClientResponses.begin(), latestClientResponses.end());

  appendEntriesRPC = new RPCAppendEntriesPacket("RPC_APPEND_ENTRIES", RPC_APPEND_ENTRIES);
  appendEntriesRPC->setTerm(currentTerm);
  appendEntriesRPC->setLeaderId(myAddress);
  appendEntriesRPC->setPrevLogIndex(log.back().logIndex);
  appendEntriesRPC->setPrevLogTerm(log.back().term); 
  appendEntriesRPC->setEntry(newEntry);
  appendEntriesRPC->setLeaderCommit(commitIndex);
  appendEntriesRPC->setClientsData(temp);
  appendEntriesRPC->setSrcAddress(myAddress);
  appendEntriesRPC->setIsBroadcast(false);
  
  //Send to all followers in the configuration
  for (int i = 0; i < configuration.size(); i++){
    // If to avoid sending to myself
    if(configuration[i] != myAddress){
      append_entry_timer newTimer;
      newTimer.destination = configuration[i];
      newTimer.prevLogIndex = log.back().logIndex;
      newTimer.prevLogTerm = log.back().term;
      newTimer.timeoutEvent = new cMessage("append-entry-timeout-event");
      newTimer.entry = newEntry; // Sufficient to copy var, value, term, logIndex
      newTimer.entry.cOld.assign(newEntry.cOld.begin(), newEntry.cOld.end()); // To deep copy
      newTimer.entry.cNew.assign(newEntry.cNew.begin(), newEntry.cNew.end());
      appendEntriesRPC->setDestAddress(configuration[i]);
      send(appendEntriesRPC, "port$o");
      
      appendEntryTimers.push_back(newTimer);
      // Reset the timer to wait before retry sending (indefinitely) the append entries for a particular follower
      scheduleAt(simTime() + par("resendTimeout"), newTimer.timeoutEvent);
    }
  }
  // If a membership change is occurring
  if(configuration != newConfiguration){
    // Send to all followers in newConfiguration
    for (int i = 0; i < newConfiguration.size(); i++){
      // If to avoid sending to myself and avoid sending twice if a server is in both configuration and newConfiguration
      if(newConfiguration[i] != myAddress && getIndex(configuration, newConfiguration[i])){
        append_entry_timer newTimer;
        newTimer.destination = newConfiguration[i];
        newTimer.prevLogIndex = log.back().logIndex;
        newTimer.prevLogTerm = log.back().term;
        newTimer.timeoutEvent = new cMessage("append-entry-timeout-event");
        newTimer.entry = newEntry; // Sufficient to copy var, value, term, logIndex
        newTimer.entry.cOld.assign(newEntry.cOld.begin(), newEntry.cOld.end()); // To perform deep copy
        newTimer.entry.cNew.assign(newEntry.cNew.begin(), newEntry.cNew.end());
        appendEntriesRPC->setDestAddress(newConfiguration[i]);
        send(appendEntriesRPC, "port$o");
        
        appendEntryTimers.push_back(newTimer);
        // Reset the timer to wait before retry sending (indefinitely) the append entries for a particular follower
        scheduleAt(simTime() + par("resendTimeout"), newTimer.timeoutEvent);
      }
    }
  }
  return;
}

void Server::appendNewEntryTo(log_entry newEntry, int destAddress, int index){
  clients_data temp;
  temp.responses.assign(latestClientResponses.begin(), latestClientResponses.end());

  appendEntriesRPC = new RPCAppendEntriesPacket("RPC_APPEND_ENTRIES", RPC_APPEND_ENTRIES);
  appendEntriesRPC->setTerm(currentTerm);
  appendEntriesRPC->setLeaderId(myAddress);
  // If NOT membership change occurring OR if it is occurring but the destination is in configuration
  if((configuration == newConfiguration) || (configuration != newConfiguration && getIndex(configuration, destAddress) != -1)){
    appendEntriesRPC->setPrevLogIndex(log[nextIndex[index]-1].logIndex); // -1 because the previous
    appendEntriesRPC->setPrevLogTerm(log[nextIndex[index]-1].term);
  }
  else{ // A membership change is occurring and the destination is ONLY in newConfiguration (by exclusion from the IF case above)
    appendEntriesRPC->setPrevLogIndex(log[nextIndexNewConfig[index]-1].logIndex); // -1 because the previous
    appendEntriesRPC->setPrevLogTerm(log[nextIndexNewConfig[index]-1].term);   
  }
  appendEntriesRPC->setEntry(newEntry);
  appendEntriesRPC->setLeaderCommit(commitIndex);
  appendEntriesRPC->setClientsData(temp);
  appendEntriesRPC->setSrcAddress(myAddress);
  appendEntriesRPC->setIsBroadcast(false);

  append_entry_timer newTimer;
  newTimer.destination = destAddress;
  newTimer.timeoutEvent = new cMessage("append-entry-timeout-event");
  newTimer.entry = newEntry;
  newTimer.entry.cOld.assign(newEntry.cOld.begin(), newEntry.cOld.end());
  newTimer.entry.cNew.assign(newEntry.cNew.begin(), newEntry.cNew.end());
  appendEntriesRPC->setDestAddress(destAddress);
  send(appendEntriesRPC, "port$o");
    
  appendEntryTimers.push_back(newTimer);
  // Reset the timer to wait before retry sending (indefinitely) the append entries for a particular follower
  scheduleAt(simTime() + par("resendTimeout"), newTimer.timeoutEvent);
}

int Server::getIndex(vector<int> v, int K){
  auto it = find(v.begin(), v.end(), K);
  // If element was found
  if (it != v.end()){
    // calculating the index of K
    int index = it - v.begin();
    return index;
  }
  // If the element is not present in the vector
  return -1;
}

bool Server::majority(int N){
  // If NOT membership change occurring
  if(configuration == newConfiguration){
    int total = configuration.size();
    int counter = 0;
    for (int i = 0; i < matchIndex.size(); i++){
      if(N >= matchIndex[i]){
        counter++;
      }
    }
    return counter > (total/2);
  }
  else{ // Membership change occurring
    int total1 = configuration.size();
    int total2 = newConfiguration.size();
    int counter1 = 0;
    int counter2 = 0;
    for (int i = 0; i < matchIndex.size(); i++){
      if(N >= matchIndex[i]){
        counter1++;
      }
    }
    for (int i = 0; i < matchIndexNewConfig.size(); i++){
      if(N >= matchIndexNewConfig[i]){
        counter2++;
      }
    }
    // To have majority i need disjoint agreement
    return counter1 > (total1/2) && counter2 > (total2/2);
  }
}

void Server::applyCommand(log_entry entry){
  if(entry.value == -2 || entry.var == 'C'){ //no_op entry
    return;
  }

  switch (entry.var){
  case 'x':
    x = entry.value;
    break;
  
  default:
    break;
  }
  return;
}

void Server::updateTerm(RPCPacket *pkGeneric){
  switch (pkGeneric->getKind())
  {
  case RPC_APPEND_ENTRIES:
  {
    RPCAppendEntriesPacket *pk = check_and_cast<RPCAppendEntriesPacket *>(pkGeneric);
    if(pk->getTerm() > currentTerm){
    currentTerm = pk->getTerm();
    if(status == CANDIDATE || status == LEADER){
      becomeFollower(pk);
    }
  }
  }
    break;
  case RPC_REQUEST_VOTE:
  {
    RPCRequestVotePacket *pk = check_and_cast<RPCRequestVotePacket *>(pkGeneric);
    if(pk->getTerm() > currentTerm){
    currentTerm = pk->getTerm();
    if(status == CANDIDATE || status == LEADER){
      becomeFollower(pk);
    }
  }
  }
    break;
  case RPC_APPEND_ENTRIES_RESPONSE:
  {
    RPCAppendEntriesResponsePacket *pk = check_and_cast<RPCAppendEntriesResponsePacket *>(pkGeneric);
    if(pk->getTerm() > currentTerm){
    currentTerm = pk->getTerm();
    if(status == CANDIDATE || status == LEADER){
      becomeFollower(pk);
    }
  }
  }
    break;
  case RPC_REQUEST_VOTE_RESPONSE:
  {
    RPCRequestVoteResponsePacket *pk = check_and_cast<RPCRequestVoteResponsePacket *>(pkGeneric);
    if(pk->getTerm() > currentTerm){
    currentTerm = pk->getTerm();
    if(status == CANDIDATE || status == LEADER){
      becomeFollower(pk);
    }
  }
  }
    break;
  default:
    break;
  }
  return;
}

void Server::becomeCandidate(){
  status = CANDIDATE;
  currentTerm++;
  votedFor = myAddress;
  // If a membership change is occurring
  if(configuration != newConfiguration){
    // If I am in the new configuration
    if(getIndex(newConfiguration, myAddress) != -1){
      votesNewConfig = 1;
    }
    // If I am in the old configuration
    if(getIndex(configuration, myAddress) != -1){
      votes = 1;
    }
  }
  else{ // Not membership change
    votes = 1;
  }
  scheduleAt(simTime() +  uniform(SimTime(par("lowElectionTimeout")), SimTime(par("highElectionTimeout"))), electionTimeoutEvent);
}

void Server::becomeLeader(){
  cancelEvent(electionTimeoutEvent);
  status = LEADER;
  leaderAddress = myAddress;
  votes = 0;
  votesNewConfig = 0;
  votedFor = -1;
  nextIndex.clear();
  matchIndex.clear();
  nextIndex.resize(configuration.size(), log.back().logIndex + 1);
  matchIndex.resize(configuration.size(), 0);
  // If i become leader in a membership change phase
  if(configuration != newConfiguration){
    nextIndexNewConfig.resize(newConfiguration.size(), log.back().logIndex + 1);
    matchIndexNewConfig.resize(newConfiguration.size(), 0);
  }

  log_entry newEntry;
  newEntry.term = currentTerm;
  newEntry.logIndex = log.size();
  newEntry.var = 'N';
  appendNewEntry(newEntry);

  scheduleAt(simTime() + par("hearthBeatTime"), sendHearthbeat); // Trigger istantaneously the sendHeartbeat
}

void Server::becomeFollower(RPCPacket *pkGeneric){
  cancelEvent(electionTimeoutEvent);
  cancelEvent(sendHearthbeat);
  for (int i = 0; i < appendEntryTimers.size() ; i++){
    cancelEvent(appendEntryTimers[i].timeoutEvent);
  }
  status = FOLLOWER;
  if(pkGeneric->getKind() == RPC_APPEND_ENTRIES){
    RPCAppendEntriesPacket *pk = check_and_cast<RPCAppendEntriesPacket *>(pkGeneric);
    leaderAddress = pk->getLeaderId();
  }
  votes = 0;
  votesNewConfig = 0;
  votedFor = -1;
  acks = 0;
  nextIndex.clear();
  matchIndex.clear();
  nextIndexNewConfig.clear();
  matchIndexNewConfig.clear();
  scheduleAt(simTime() +  uniform(SimTime(par("lowElectionTimeout")), SimTime(par("highElectionTimeout"))), electionTimeoutEvent);
}

void Server::initializeConfiguration(){
    cModule *Switch = gate("port$i")->getPreviousGate()->getOwnerModule();
    int moduleAddress;
    for (int i = 2; i < Switch->gateSize("port$o"); i++){
        if (Switch->gate("port$o", i)->isConnected())
        {
          moduleAddress = Switch->gate("port$o", i)->getId();
          //EV << "Added ID: " << moduleAddress << " to configuration Vector" << endl;
          configuration.push_back(moduleAddress);
        }
    }
}

int Server::getClientIndex(int clientAddress){
  for(int i=0; i<latestClientResponses.size(); i++){
    if(clientAddress == latestClientResponses[i].clientAddress){
    return i;
    }
  }
  return -1;
}

void Server::updateLatestClientSequenceNumber(int clientAddress, int sequenceNumber){
 for(int i=0; i<latestClientResponses.size(); i++){
   if(latestClientResponses[i].clientAddress == clientAddress){
     latestClientResponses[i].latestSequenceNumber = sequenceNumber;
     return;
   }
 }
 return;
}

void Server::sendAck(int destAddress, int seqNum){
  ACK = new RPCAckPacket("RPC_ACK", RPC_ACK);
  ACK->setSequenceNumber(seqNum);
  ACK->setSrcAddress(myAddress);
  ACK->setDestAddress(destAddress);
  send(ACK, "port$o"); 
}

void Server::sendResponseToClient(int type, int clientAddress){
  clientCommandResponseRPC = new RPCClientCommandResponsePacket("RPC_CLIENT_COMMAND_RESPONSE", RPC_CLIENT_COMMAND_RESPONSE);
  if(type == READ){
    clientCommandResponseRPC->setValue(x);
    latestClientResponses[getClientIndex(clientAddress)].latestReponseToClient = x;
  }else{
    clientCommandResponseRPC->setValue(-1); // Convention for write responses
    latestClientResponses[getClientIndex(clientAddress)].latestReponseToClient = -1;
  }
  clientCommandResponseRPC->setSequenceNumber(latestClientResponses[getClientIndex(clientAddress)].latestSequenceNumber + 1);
  clientCommandResponseRPC->setSrcAddress(myAddress);
  clientCommandResponseRPC->setDestAddress(clientAddress);
  send(clientCommandResponseRPC, "port$o");
  latestClientResponses[getClientIndex(clientAddress)].latestSequenceNumber++;
}

void Server::startReadOnlyLeaderCheck(){
  cancelEvent(sendHearthbeat);
  countingFeedback = true;
  withAck = true;
  heartbeatSeqNum++;
  scheduleAt(simTime(), sendHearthbeat); // Trigger immediately an heartbeat send
}