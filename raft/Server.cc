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
    cMessage *minElectionTimeoutEvent;
    cMessage *crashTimeoutEvent;
    cMessage *reviveTimeoutEvent;
    vector<append_entry_timer> appendEntryTimers;
    vector<install_snapshot_timer> installSnapshotTimers;
    
    int myAddress;   // This is the server ID
    int receiverAddress; // This is receiver server ID (for utility)
    int adminAddress;  // This is the admin address ID
    
    int votes = 0; // This is the number of received votes by servers in the configuration (meaninful when status = candidate). It refers to member in the "old" configuration when a membership change is occurring.
    int votesNewConfig = 0; // This is the number of received votes by servers in the "new" configuration (meaninful when status = candidate) when a membership change is occurring.

    int leaderAddress = -1; // This is the leader ID
    int acks = 0; // This is the number of acks (if membership change: acks from configuration)
    int acksNewConf = 0; // This is the number of akcs of the newConfiguration in case of membership change occurring
    bool countingFeedback = false;
    int heartbeatSeqNum = 0; // This is to allow acks for heartbeat for read-only operations
    bool waitingNoOp = false;
    vector<int> pendingReadClients;
    bool believeCurrentLeaderExists = false;
    bool newServersCanVote = true;
    bool iAmCrashed = false;
    vector<lastRPC> RPCs; // Track the latest RPC ("sequenceNumber" and "success", to reproduce blocking RPC) for all messages to other servers (meaningful for leader and candidate status).
    vector<lastRPC> RPCsNewConfig;

    // Pointers to handle RPC mexs
    RPCAppendEntriesPacket *appendEntriesRPC = nullptr;
    RPCAppendEntriesResponsePacket *appendEntriesResponseRPC = nullptr;
    RPCRequestVotePacket *requestVoteRPC = nullptr;
    RPCRequestVoteResponsePacket *requestVoteResponseRPC = nullptr;
    RPCInstallSnapshotPacket *installSnapshotRPC = nullptr;
    RPCInstallSnapshotResponsePacket *installSnapshotResponseRPC = nullptr;

    RPCClientCommandResponsePacket *clientCommandResponseRPC = nullptr;
    //RPCAckPacket *ACK = nullptr;

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
    int commitIndex = 0;
    int lastApplied = 0;

    // Volatile state on leaders --> Reinitialized after election
    vector<int> nextIndex;
    vector<int> matchIndex;

    vector<int> nextIndexNewConfig;
    vector<int> matchIndexNewConfig;

    // Snapshot file
    snapshot_file snapshot;

  protected:
    virtual void refreshDisplay() const override;
    virtual void initialize() override;
    virtual void handleMessage(cMessage *msg) override;
    void initializeConfiguration();
    void appendNewEntry(log_entry newEntry, bool onlyToNewServers);
    int getIndex(vector<int> v, int K);
    void appendNewEntryTo(log_entry newEntry, int destAddress, int serverIndex);
    bool majority(int N);
    void applyCommand(log_entry entry);
    void replayLog();
    void becomeLeader();
    void becomeCandidate();
    void becomeFollower(RPCPacket *pkGeneric);
    void updateTerm(RPCPacket *pkGeneric);
    int getClientIndex(int clientAddress);
    void sendAck(int destAddress, int seqNum);
    void sendResponseToClient(int type, int clientAddress);
    void startReadOnlyLeaderCheck();
    bool checkNewServersAreUpToDate();
    void sendHeartbeatToFollower();
    void sendRequestVote();
    void takeSnapshot();
    // Return position in log, if entryIndex is found. Otherwise -1
    int getEntryIndexPositionInLog(int entryIndex);
    void sendSnapshot(int destAddress);
    void sendSnapshotResponse(int destAddress, int seqNum);
    bool checkValidRPCResponse(int sender, int SN);
    bool checkHeartbeatResponse(int sender);
    void applySnapshot();
};

Define_Module(Server);


// Destructor
Server::~Server()
{
  cancelAndDelete(sendHearthbeat);
  cancelAndDelete(electionTimeoutEvent);
  cancelAndDelete(minElectionTimeoutEvent);
  for (int i = 0; i < appendEntryTimers.size() ; i++){
    cancelAndDelete(appendEntryTimers[i].timeoutEvent);
  }
  for (int i = 0; i < installSnapshotTimers.size() ; i++){
    cancelAndDelete(installSnapshotTimers[i].timeoutEvent);
  }
  cancelAndDelete(reviveTimeoutEvent);
  cancelAndDelete(crashTimeoutEvent);
}

void Server::initialize()
{
  // Create self messages needed for timeouts
  sendHearthbeat = new cMessage("send-hearthbeat");
  electionTimeoutEvent = new cMessage("election-timeout-event");
  minElectionTimeoutEvent = new cMessage("min-election-timeout-event");
  crashTimeoutEvent = new cMessage("crash-timeout-event");
  reviveTimeoutEvent = new cMessage("revive-timeout-event");

  // Initialize addresses
  myAddress = gate("port$i")->getPreviousGate()->getId(); // Return index of this server gate port in the Switch
  adminAddress = gate("port$o")->getNextGate()->getOwnerModule()->gate("port$o", 0)->getId();

  WATCH(adminAddress);
  WATCH(myAddress);
  WATCH(leaderAddress);
  WATCH_VECTOR(configuration);
  WATCH_VECTOR(newConfiguration);
  WATCH(x);
  WATCH(currentTerm);
  WATCH(votedFor);
  WATCH(commitIndex);
  WATCH(lastApplied);
  WATCH_VECTOR(nextIndex);
  WATCH_VECTOR(matchIndex);
  WATCH_VECTOR(matchIndexNewConfig);
  WATCH_VECTOR(nextIndexNewConfig);
  WATCH(status);
  WATCH(newServersCanVote);
  WATCH(log);
  WATCH(RPCs);
  WATCH(RPCsNewConfig);
  WATCH(votes);
  WATCH_VECTOR(pendingReadClients);
  WATCH(appendEntryTimers);
  WATCH(snapshot);
  WATCH(installSnapshotTimers);
  WATCH(latestClientResponses);

  // Initialize the initial configuration
  initializeConfiguration();
  newConfiguration.assign(configuration.begin(), configuration.end());
  // Initialize the RPCs
  RPCs.resize(configuration.size());
  RPCsNewConfig.resize(configuration.size());

  

  //Pushing the initial configuration in the log   //TODO anche nei nuovi serve ancora?
  log_entry firstEntry;
  firstEntry.var = 'C';
  firstEntry.term = currentTerm;
  firstEntry.logIndex = 0;
  firstEntry.cNew.assign(configuration.begin(), configuration.end());
  log.push_back(firstEntry);

  scheduleAt(simTime() +  uniform(SimTime(par("lowCrashTimeout")), SimTime(par("highCrashTimeout"))), crashTimeoutEvent);

  if (par("instantieatedAtRunTime"))
  {
    status = NON_VOTING;
    configuration.clear();
    newConfiguration.clear();
    return;
  }
  scheduleAt(simTime() +  uniform(SimTime(par("lowElectionTimeout")), SimTime(par("highElectionTimeout"))), electionTimeoutEvent);
}

void Server::handleMessage(cMessage *msg)
{
  if (msg == crashTimeoutEvent){
    bubble("I am crashed");
    cancelEvent(sendHearthbeat);
    cancelEvent(electionTimeoutEvent);
    cancelEvent(minElectionTimeoutEvent);
    for (int i = 0; i < appendEntryTimers.size() ; i++){
      cancelAndDelete(appendEntryTimers[i].timeoutEvent);
    }
    for (int i = 0; i < installSnapshotTimers.size() ; i++){
      cancelAndDelete(installSnapshotTimers[i].timeoutEvent);
    }
    appendEntryTimers.clear();
    installSnapshotTimers.clear();
    iAmCrashed = true;
    scheduleAt(simTime() + par("reviveTimeout"), reviveTimeoutEvent);
    return;
  }

  if(msg == reviveTimeoutEvent){
    bubble("I am resurrecting");
    // Reset all raft's original volatile variables 
    x = 0;
    configuration.clear();
    newConfiguration.clear();
    latestClientResponses.clear();
    commitIndex = 1;
    lastApplied = 1;
    nextIndex.clear();
    matchIndex.clear();
    nextIndexNewConfig.clear();
    matchIndexNewConfig.clear();
    // Reset all other utility volatile variables 
    votes = 0;
    votesNewConfig = 0; 
    leaderAddress = -1;
    acks = 0; 
    acksNewConf = 0;
    countingFeedback = false;
    heartbeatSeqNum = 0;
    waitingNoOp = false;
    pendingReadClients.clear();
    believeCurrentLeaderExists = false;
    newServersCanVote = true;
    RPCs.clear();
    RPCsNewConfig.clear();
    //The replay of all the log will be triggered automatically from the protocol
    iAmCrashed = false;

    // Reset to follower state only if it was NOT a NON_VOTING server.
    // "Status" is for the check even if it is supposed to be not saved after a crash, but only for simplicity of simulating
    // in fact, we can suppose that a NON_VOTING server will be recovered after a crash and turned in the same NON_VOTING state for coherence by an "admin"
    if(status != NON_VOTING){
      getDisplayString().setTagArg("i", 1, "");
      status = FOLLOWER;
      //scheduleAt(simTime() + par("lowElectionTimeout"), minElectionTimeoutEvent);
      scheduleAt(simTime() +  uniform(SimTime(par("lowElectionTimeout")), SimTime(par("highElectionTimeout"))), electionTimeoutEvent);
      scheduleAt(simTime() +  uniform(SimTime(par("lowCrashTimeout")), SimTime(par("highCrashTimeout"))), crashTimeoutEvent); 
    }
    
    // If a snapshot is available
    if (snapshot.value != -1){applySnapshot();}
    replayLog();
    return;
  }
  
  if(iAmCrashed){
    delete(msg);
    return;
  }
  
  if(msg == sendHearthbeat){
    sendHeartbeatToFollower();
    return;
  }

  if(msg == electionTimeoutEvent){
    if(status == NON_VOTING){return;}
    
    EV << "Starting a new leader election, i am a candidate\n";
    becomeCandidate();
    sendRequestVote();
    return;
  }
  
  if(msg == minElectionTimeoutEvent){
    believeCurrentLeaderExists = false;
    return;
  }


  // Retry append entries if an appendEntryTimer is fired 
  for (int i = 0; i < appendEntryTimers.size() ; i++){
    if (msg == appendEntryTimers[i].timeoutEvent){

      // Re-craft the entry
      log_entry newEntry = appendEntryTimers[i].entry;
      newEntry.cOld.assign(appendEntryTimers[i].entry.cOld.begin(), appendEntryTimers[i].entry.cOld.end());
      newEntry.cNew.assign(appendEntryTimers[i].entry.cNew.begin(), appendEntryTimers[i].entry.cNew.end());
      newEntry.clientsData.assign(appendEntryTimers[i].entry.clientsData.begin(), appendEntryTimers[i].entry.clientsData.end());

      //Create the message
      appendEntriesRPC = new RPCAppendEntriesPacket("RPC_APPEND_ENTRIES", RPC_APPEND_ENTRIES);
      appendEntriesRPC->setTerm(currentTerm);
      appendEntriesRPC->setLeaderId(myAddress);
      appendEntriesRPC->setPrevLogIndex(appendEntryTimers[i].prevLogIndex); 
      appendEntriesRPC->setPrevLogTerm(appendEntryTimers[i].prevLogTerm);
      appendEntriesRPC->setEntry(newEntry);
      appendEntriesRPC->setLeaderCommit(commitIndex);
      appendEntriesRPC->setSrcAddress(myAddress);
      appendEntriesRPC->setDestAddress(appendEntryTimers[i].destination);
      
      if(getIndex(configuration, appendEntryTimers[i].destination) != -1){
        RPCs[getIndex(configuration, appendEntryTimers[i].destination)].sequenceNumber++;
        RPCs[getIndex(configuration, appendEntryTimers[i].destination)].success = false;
        RPCs[getIndex(configuration, appendEntryTimers[i].destination)].isHeartbeat = false; 
        appendEntriesRPC->setSequenceNumber(RPCs[getIndex(configuration, appendEntryTimers[i].destination)].sequenceNumber);
      }
      if(configuration != newConfiguration && getIndex(newConfiguration, appendEntryTimers[i].destination) != -1){
        RPCsNewConfig[getIndex(newConfiguration, appendEntryTimers[i].destination)].sequenceNumber++;
        RPCsNewConfig[getIndex(newConfiguration, appendEntryTimers[i].destination)].success = false;
        RPCsNewConfig[getIndex(newConfiguration, appendEntryTimers[i].destination)].isHeartbeat = false;
        appendEntriesRPC->setSequenceNumber(RPCsNewConfig[getIndex(newConfiguration, appendEntryTimers[i].destination)].sequenceNumber);
      }
      // Send the message only if the destination is still in a configuration
      if(getIndex(configuration, appendEntryTimers[i].destination) != -1 || (configuration != newConfiguration && getIndex(newConfiguration, appendEntryTimers[i].destination) != -1)){
        send(appendEntriesRPC, "port$o");
      }
      else{
        cancelAndDelete(appendEntryTimers[i].timeoutEvent);
        appendEntryTimers.erase(appendEntryTimers.begin() + i);
        return;
      }
      
      //Reset timer
      scheduleAt(simTime() + par("resendTimeout"), appendEntryTimers[i].timeoutEvent);
      return;
    }
    
  }
    
  // Retry install snapshot if an installSnapshotTimer is fired 
  for (int i = 0; i < installSnapshotTimers.size() ; i++){
    if (msg == installSnapshotTimers[i].timeoutEvent){

      // Re-craft the snapshot
      snapshot_file newSnapshot = installSnapshotTimers[i].snapshot;
      newSnapshot.cOld.assign(installSnapshotTimers[i].snapshot.cOld.begin(), installSnapshotTimers[i].snapshot.cOld.end());
      newSnapshot.cNew.assign(installSnapshotTimers[i].snapshot.cNew.begin(), installSnapshotTimers[i].snapshot.cNew.end());
      newSnapshot.clientsData.assign(installSnapshotTimers[i].snapshot.clientsData.begin(), installSnapshotTimers[i].snapshot.clientsData.end());

      //Create the message
      installSnapshotRPC = new RPCInstallSnapshotPacket("RPC_INSTALL_SNAPSHOT", RPC_INSTALL_SNAPSHOT);
      installSnapshotRPC->setTerm(currentTerm);
      installSnapshotRPC->setLeaderId(myAddress);
      installSnapshotRPC->setLastIncludedIndex(installSnapshotTimers[i].lastIncludedIndex); 
      installSnapshotRPC->setLastIncludedTerm(installSnapshotTimers[i].lastIncludedTerm);
      installSnapshotRPC->setSnapshot(newSnapshot);
      installSnapshotRPC->setSrcAddress(myAddress);
      installSnapshotRPC->setDestAddress(installSnapshotTimers[i].destination);
      
      if(getIndex(configuration, installSnapshotTimers[i].destination) != -1){
        RPCs[getIndex(configuration, installSnapshotTimers[i].destination)].sequenceNumber++;
        RPCs[getIndex(configuration, installSnapshotTimers[i].destination)].success = false;
        RPCs[getIndex(configuration, installSnapshotTimers[i].destination)].isHeartbeat = false; 
        installSnapshotRPC->setSequenceNumber(RPCs[getIndex(configuration, installSnapshotTimers[i].destination)].sequenceNumber);
      }
      if(configuration != newConfiguration && getIndex(newConfiguration, installSnapshotTimers[i].destination) != -1){
        RPCsNewConfig[getIndex(newConfiguration, installSnapshotTimers[i].destination)].sequenceNumber++;
        RPCs[getIndex(configuration, installSnapshotTimers[i].destination)].success = false;
        RPCsNewConfig[getIndex(newConfiguration, installSnapshotTimers[i].destination)].isHeartbeat = false;
        installSnapshotRPC->setSequenceNumber(RPCsNewConfig[getIndex(newConfiguration, installSnapshotTimers[i].destination)].sequenceNumber);
      }
      // Send the message only if the destination is still in a configuration
      if(getIndex(configuration, installSnapshotTimers[i].destination) != -1 || (configuration != newConfiguration && getIndex(newConfiguration, installSnapshotTimers[i].destination) != -1)){
        send(installSnapshotRPC, "port$o");
      }
      else{
        cancelAndDelete(installSnapshotTimers[i].timeoutEvent);
        installSnapshotTimers.erase(installSnapshotTimers.begin() + i);
        return;
      }
      
      
      //Reset timer
      scheduleAt(simTime() + par("resendTimeout"), installSnapshotTimers[i].timeoutEvent);
      return;
    }
  }

  RPCPacket *pkGeneric = check_and_cast<RPCPacket *>(msg);

  // Simulate packet dropping on the receiver
  if(dblrand() >= par("errorRateThreshold").doubleValue()){
    delete(pkGeneric);
    bubble("Channel Failure!");
    return;
  }

  updateTerm(pkGeneric);

  switch (pkGeneric->getKind())
  {
  case RPC_APPEND_ENTRIES:
  {
    RPCAppendEntriesPacket *pk = check_and_cast<RPCAppendEntriesPacket *>(pkGeneric);
    // If is NOT heartbeat
    if (pk->getEntry().term != -1){
      cancelEvent(minElectionTimeoutEvent);
      believeCurrentLeaderExists = true;
      cancelEvent(electionTimeoutEvent);
      receiverAddress = pk->getSrcAddress();

      //1) Reply false if term < currentTerm 2)Reply false if log doesn’t contain an entry at prevLogIndex whose term matches prevLogTerm 
      bool partialEval = false;
      int index;

      // Since every Server when it borns pushes one entry in the log, the log can be empty only if a snapshot has been taken
      if(!log.empty()){
        // Check that prevLogIndex is not smaller than Index of first entry in the log. Else means that we need to check the snapshot
        index = getEntryIndexPositionInLog(pk->getPrevLogIndex());
        if (index != -1){
          if(log[index].term != pk->getPrevLogTerm()){
            partialEval = true;
          }
        }
        else{
          // If snapshot exists
          if(snapshot.value != -1){
            if (pk->getPrevLogIndex() != snapshot.lastIncludedIndex){
              partialEval = true;
            }
            else{
              if (pk->getPrevLogTerm() != snapshot.lastIncludedTerm){
                partialEval = true;
              }
            }
          }
          else{
            partialEval = true;
          }
        }
      }
      // Log is empty, so we are sure snapshot exists
      else{
        if (pk->getPrevLogIndex() != snapshot.lastIncludedIndex){
          partialEval = true;
        }
        else{
          if (pk->getPrevLogTerm() != snapshot.lastIncludedTerm){
            partialEval = true;
          }
        }
      }     

      // If term received smaller than currentTerm or partialeval = true, reply false
      if((pk->getTerm() < currentTerm) || partialEval){
        appendEntriesResponseRPC = new RPCAppendEntriesResponsePacket("RPC_APPEND_ENTRIES_RESPONSE", RPC_APPEND_ENTRIES_RESPONSE);
        appendEntriesResponseRPC->setSuccess(false);
        appendEntriesResponseRPC->setTerm(currentTerm);
        appendEntriesResponseRPC->setSrcAddress(myAddress);
        appendEntriesResponseRPC->setDestAddress(receiverAddress);
        appendEntriesResponseRPC->setSequenceNumber(pk->getSequenceNumber());
        send(appendEntriesResponseRPC, "port$o");
      }
      else {


        //3)If an existing entry conflicts with a new one (same index but different terms), delete the existing entry and all that follow it.
        index = getEntryIndexPositionInLog(pk->getEntry().logIndex);
        if (index != -1){ // This also check log is not empty
          if (log[index].logIndex == pk->getEntry().logIndex && log[index].term != pk->getEntry().term){
            log.resize(index);
          }
        }


        //4)Append any new entries, if not already in the log.
        if (!log.empty()){
          if(log.back().logIndex != pk->getEntry().logIndex){
            log.push_back(pk->getEntry());
          }
        }
        else{ log.push_back(pk->getEntry()); }


        //5)If leaderCommit > commitIndex, set commitIndex = min (leaderCommit, index of last new entry).
        if(pk->getLeaderCommit() > commitIndex){
          if(pk->getLeaderCommit() < pk->getEntry().logIndex){
            commitIndex = pk->getLeaderCommit();
          } else{
            commitIndex = pk->getEntry().logIndex;
          }
        }
        latestClientResponses.assign(pk->getEntry().clientsData.begin(), pk->getEntry().clientsData.end());
        // If it's a membership change entry
        if(pk->getEntry().var == 'C'){
          // If it is the entry of the first phase (Cold,new)
          if(!pk->getEntry().cOld.empty()){
            configuration.assign(pk->getEntry().cOld.begin(), pk->getEntry().cOld.end()); // It is necessary only for servers of the new configuration to learn the old configuration
            newConfiguration.assign(pk->getEntry().cNew.begin(), pk->getEntry().cNew.end());
            
            if (status == NON_VOTING){
              becomeFollower(pkGeneric);
              getDisplayString().setTagArg("i", 1, "");
            } 
          }
          // If it is the entry of the second phase (Cnew)
          else{
            configuration.assign(newConfiguration.begin(), newConfiguration.end()); // va fatto solo in quelle incluse in Cnew?
          }
        } 

        //If we arrive here, Reply true
        appendEntriesResponseRPC = new RPCAppendEntriesResponsePacket("RPC_APPEND_ENTRIES_RESPONSE", RPC_APPEND_ENTRIES_RESPONSE);
        appendEntriesResponseRPC->setSuccess(true);
        appendEntriesResponseRPC->setTerm(currentTerm);
        appendEntriesResponseRPC->setSrcAddress(myAddress);
        appendEntriesResponseRPC->setDestAddress(receiverAddress);
        appendEntriesResponseRPC->setSequenceNumber(pk->getSequenceNumber());
        send(appendEntriesResponseRPC, "port$o");
      }
      scheduleAt(simTime() + par("lowElectionTimeout"), minElectionTimeoutEvent);
      scheduleAt(simTime() +  uniform(SimTime(par("lowElectionTimeout")), SimTime(par("highElectionTimeout"))), electionTimeoutEvent);
    }
    else{  // Heartbeat case
      //If i am candidate
      if(status == CANDIDATE){
          sendAck(pk->getSrcAddress(), pk->getSequenceNumber());
          // (Note: otherwise the electionTimeoutEvent remains valid)
        }
        else{ //I am not a candidate, and i am a follower remain follower, otherwise if i am a leader i ignore heartbeat
          if (status == FOLLOWER){
            cancelEvent(minElectionTimeoutEvent);
            cancelEvent(electionTimeoutEvent);
            leaderAddress = pk->getLeaderId();
            believeCurrentLeaderExists = true;
            // If leaderCommit > commitIndex, set commitIndex = min (leaderCommit, index of last new entry = which here is not meaningful, thus use the last entry in the log).
            if(pk->getLeaderCommit() > commitIndex){
              if(pk->getLeaderCommit() < log.back().logIndex){
                commitIndex = pk->getLeaderCommit();
              } else{
                commitIndex = log.back().logIndex;
              }
            }
            sendAck(pk->getSrcAddress(), pk->getSequenceNumber());
            
            //latestClientResponses.assign(pk->getEntry().clientsData.begin(), pk->getEntry().clientsData.end());
            scheduleAt(simTime() + par("lowElectionTimeout"), minElectionTimeoutEvent);
            scheduleAt(simTime() +  uniform(SimTime(par("lowElectionTimeout")), SimTime(par("highElectionTimeout"))), electionTimeoutEvent);
          }
          else{sendAck(pk->getSrcAddress(), pk->getSequenceNumber());}
        }
    }

    // Apply all entries through committ index
    int index;
    for(int i=lastApplied ; i < commitIndex; i++){
      lastApplied++;
      index = getEntryIndexPositionInLog(lastApplied);
      if (index != -1){
        applyCommand(log[index]);
      }
    }

    // Take snapshot if needed
    if (log.size() >= (int)par("maxLogSizeBeforeSnapshot")) {takeSnapshot();}
  }
  break;
  case RPC_REQUEST_VOTE:
  {
    if(status == NON_VOTING){break;}
    
    RPCRequestVotePacket *pk = check_and_cast<RPCRequestVotePacket *>(pkGeneric);
    receiverAddress = pk->getSrcAddress();
    
    //1)Reply false if term < currentTerm.
    if (pk->getTerm() < currentTerm){
      requestVoteResponseRPC = new RPCRequestVoteResponsePacket("RPC_REQUEST_VOTE_RESPONSE", RPC_REQUEST_VOTE_RESPONSE);
      requestVoteResponseRPC->setVoteGranted(false);
      requestVoteResponseRPC->setTerm(currentTerm);
      requestVoteResponseRPC->setSrcAddress(myAddress);
      requestVoteResponseRPC->setDestAddress(receiverAddress);
      requestVoteResponseRPC->setSequenceNumber(pk->getSequenceNumber());
      send(requestVoteResponseRPC, "port$o");
    } else{
      //2)If votedFor is null or candidateId, and candidate’s log is at least as up-to-date as receiver’s log, grant vote.
      if((votedFor == -1 || votedFor == pk->getCandidateId()) && (log.back().term < pk->getLastLogTerm() || (log.back().term == pk->getLastLogTerm() && log.back().logIndex <= pk->getLastLogIndex()))){
        if(!believeCurrentLeaderExists){
          cancelEvent(electionTimeoutEvent);
          votedFor = pk->getCandidateId();
          requestVoteResponseRPC = new RPCRequestVoteResponsePacket("RPC_REQUEST_VOTE_RESPONSE", RPC_REQUEST_VOTE_RESPONSE);
          requestVoteResponseRPC->setVoteGranted(true);
          requestVoteResponseRPC->setTerm(currentTerm);
          requestVoteResponseRPC->setSrcAddress(myAddress);
          requestVoteResponseRPC->setDestAddress(receiverAddress);
          requestVoteResponseRPC->setSequenceNumber(pk->getSequenceNumber());
          send(requestVoteResponseRPC, "port$o");
          scheduleAt(simTime() +  uniform(SimTime(par("lowElectionTimeout")), SimTime(par("highElectionTimeout"))), electionTimeoutEvent);
        }
      }
      else{
        requestVoteResponseRPC = new RPCRequestVoteResponsePacket("RPC_REQUEST_VOTE_RESPONSE", RPC_REQUEST_VOTE_RESPONSE);
        requestVoteResponseRPC->setVoteGranted(false);
        requestVoteResponseRPC->setTerm(currentTerm);
        requestVoteResponseRPC->setSrcAddress(myAddress);
        requestVoteResponseRPC->setDestAddress(receiverAddress);
        requestVoteResponseRPC->setSequenceNumber(pk->getSequenceNumber());
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
      // Check if it is a valid RPC response (the one expected and if it was not already received)
      if(!checkValidRPCResponse(receiverAddress, pk->getSequenceNumber())){break;}
      
      // Check if the response is an heartbeat response
      if (checkHeartbeatResponse(receiverAddress)){
        // Check if i am counting the heartbeat responses (for read-only)
        if(countingFeedback == true){
          // If the sender (receiverAddress) is in configuration
          if(getIndex(configuration, receiverAddress) != -1){
            acks++;
          }
          // If the sender (receiverAddress) is in (or also) newConfiguration during a membership change
          if(configuration != newConfiguration && getIndex(newConfiguration, receiverAddress) != -1){
            acksNewConf++;
          }
          
          // If majority of heartbeat exchanged, is possible to finalize the pending read-only request (NO membership change occurring)
          if(configuration == newConfiguration || newServersCanVote == false){
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
        break;
      }

      for(int i=0; i < appendEntryTimers.size() ; i++){
        if (receiverAddress == appendEntryTimers[i].destination){
          cancelAndDelete(appendEntryTimers[i].timeoutEvent);
          appendEntryTimers.erase(appendEntryTimers.begin() + i);
          break;
        }
      }

      if(pk->getSuccess() == false){
        int position;
        int index;
        log_entry newEntry;

        // If server in configuration
        if(getIndex(configuration, receiverAddress) != -1){
          position = getIndex(configuration, receiverAddress);
          index = --nextIndex[position]; // first decrement and then save in the variable
        }

        // If server in newConfiguration during a membership change
        if(configuration != newConfiguration && getIndex(newConfiguration, receiverAddress) != -1){
          position = getIndex(newConfiguration, receiverAddress);
          index = --nextIndexNewConfig[position];
        }

        // Check if index of entry to send is in log of LEADER, otherwise mean that we need to send a Snapshot to the follower to update him
        index = getEntryIndexPositionInLog(index);
        if (index == -1 ){
          if(snapshot.value != -1){
            sendSnapshot(receiverAddress);
          }
          break;
        }

        // Send the entry
        newEntry = log[index];
        newEntry.clientsData.assign(log[index].clientsData.begin(), log[index].clientsData.end());
        if(newEntry.var == 'C'){  
          newEntry.cOld.assign(newEntry.cOld.begin(), newEntry.cOld.end());
          newEntry.cNew.assign(newEntry.cNew.begin(), newEntry.cNew.end());
        }
        appendNewEntryTo(newEntry, receiverAddress, position);
      }
      else{ //Success == true
        int position;
        int index;

        if(getIndex(configuration, receiverAddress) != -1){
          position = getIndex(configuration, receiverAddress);
          // Update matchIndex and nextIndex
          matchIndex[position] = nextIndex[position];
          index = ++nextIndex[position];
        }


        // Configuration change and follower also in new config
        if(configuration != newConfiguration && getIndex(newConfiguration, receiverAddress) != -1){
          position = getIndex(newConfiguration, receiverAddress);  
          // Update matchIndex and nextIndex
          matchIndexNewConfig[position] = nextIndexNewConfig[position];
          index = ++nextIndexNewConfig[position];
        }


        // If nextIndex is not in any log entry ==> LEADER may have delete the entry cause of snapshotting
        if(getEntryIndexPositionInLog(index) == -1){
          // If nextIndex is in snapshot, send snapshot. Else means that FOLLOWER is up to date
          if(snapshot.value != -1 && index <= snapshot.lastIncludedIndex) {sendSnapshot(receiverAddress);}
          else{
            // Case of membership change and the address come from an only NEW server which now is up to date
            if(!newServersCanVote && configuration != newConfiguration && getIndex(newConfiguration, receiverAddress) != -1 && getIndex(configuration, receiverAddress) == -1){
              if(checkNewServersAreUpToDate()){ // Now trigger the Cold,new append
                bubble("Creating C_old,new");
                log_entry newEntry;
                newEntry.term = currentTerm;
                newEntry.logIndex = log.back().logIndex + 1;
                newEntry.clientAddress = adminAddress;
                newEntry.var = 'C';
                newEntry.cOld.assign(configuration.begin(), configuration.end());
                newEntry.cNew.assign(newConfiguration.begin(), newConfiguration.end());
                newEntry.clientsData.assign(latestClientResponses.begin(), latestClientResponses.end());
                log.push_back(newEntry);
                matchIndex[getIndex(configuration, myAddress)]++;
                if (getIndex(newConfiguration, myAddress) != -1){
                  matchIndexNewConfig[getIndex(newConfiguration, myAddress)]++;
                }
                newServersCanVote = true;
                cancelEvent(sendHearthbeat);
                appendNewEntry(newEntry, false);
                scheduleAt(simTime() + par("hearthBeatTime"), sendHearthbeat); 
              }
            }
          }
        }
        else{
          index = getEntryIndexPositionInLog(index);
          log_entry newEntry;
          newEntry = log[index];
          newEntry.clientsData.assign(log[index].clientsData.begin(), log[index].clientsData.end());
          if(newEntry.var == 'C'){  
            newEntry.cOld.assign(newEntry.cOld.begin(), newEntry.cOld.end());
            newEntry.cNew.assign(newEntry.cNew.begin(), newEntry.cNew.end());
          }
          appendNewEntryTo(newEntry, receiverAddress, position);
        }

        // commitIndex update
        for (int newCommitIndex = commitIndex + 1; !log.empty() && newCommitIndex <= log.back().logIndex; newCommitIndex++){
          index = getEntryIndexPositionInLog(newCommitIndex);
          if(majority(newCommitIndex) == true && log[index].term == currentTerm){
            commitIndex = newCommitIndex;
          }
        }

        // lastApplied update and command apply
        for(int i=lastApplied; i < commitIndex; i++){
          lastApplied++;
          index = getEntryIndexPositionInLog(lastApplied);
          applyCommand(log[index]);
          if(log[index].term == currentTerm){
            if(log[index].var == 'N'){
              if(waitingNoOp == true){
                waitingNoOp = false;
                startReadOnlyLeaderCheck();
              }
            }
            else{ //If Cnew is now committed
              if(log[index].var == 'C'){
                if(log[index].cOld.empty()){
                  configuration.assign(newConfiguration.begin(), newConfiguration.end());
                  nextIndex.assign(nextIndexNewConfig.begin(), nextIndexNewConfig.end());
                  matchIndex.assign(matchIndexNewConfig.begin(), matchIndexNewConfig.end());
                  RPCs.assign(RPCsNewConfig.begin(), RPCsNewConfig.end());
                  // If the leader is not in Cnew become follower
                  if(getIndex(newConfiguration, myAddress) == -1){
                    becomeFollower(pk);
                  }
                  sendResponseToClient(WRITE, log[index].clientAddress); // This is the response to the Admin
                }
              }
              else{ 
                // Write response case
                sendResponseToClient(WRITE, log[index].clientAddress);
              }
            }
          }
          // Cold,new case: now trigger the Cnew append
          if(log[index].var == 'C' && !log[index].cOld.empty() && configuration != newConfiguration){ 
            bubble("Creating C_new");
            log_entry newEntry;
            newEntry.term = currentTerm;
            newEntry.logIndex = log.back().logIndex + 1;
            newEntry.clientAddress = log[index].clientAddress;
            newEntry.var = 'C';
            newEntry.value = log[index].value;
            newEntry.clientsData.assign(latestClientResponses.begin(), latestClientResponses.end());
            // Update matchIndex to count leader vote
            matchIndex[getIndex(configuration, myAddress)]++;
            // Update also in the matchIndexNewConfig if leader is also in that new configuration.
            if (getIndex(newConfiguration, myAddress) != -1){
              matchIndexNewConfig[getIndex(newConfiguration, myAddress)]++;
            }
            // implicitly newEntry.cOld is empty (Convention: it means that it is the Cnew entry)
            newEntry.cNew.assign(newConfiguration.begin(), newConfiguration.end());
            log.push_back(newEntry);
            appendNewEntry(newEntry, false);
          }
        }
      }
    }
  }
  break;
  case RPC_REQUEST_VOTE_RESPONSE:
  {
    RPCRequestVoteResponsePacket *pk = check_and_cast<RPCRequestVoteResponsePacket *>(pkGeneric);
    int sender = pk->getSrcAddress();
    //If i am still candidate
    if(status == CANDIDATE){
      // Check if it is a valid RPC response (the one expected and if it was not already received)
      if(!checkValidRPCResponse(sender, pk->getSequenceNumber())){break;}
      
      // If the vote is granted
      if (pk->getVoteGranted() == true){
        bubble("Vote received");
        // If the vote is granted by a server in configuration
        if(getIndex(configuration, sender) != -1){
          votes++;
        }

        // If the vote is granted by a server in new configuration (in membership change case)
        if(configuration != newConfiguration && getIndex(newConfiguration, sender) != -1){
          votesNewConfig++;
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
  }
  break;
  case RPC_CLIENT_COMMAND:
  {
    RPCClientCommandPacket *pk = check_and_cast<RPCClientCommandPacket *>(pkGeneric);
    bool ret=false;
    //Process incoming command from a client
    if(status == LEADER){ 
      // If request already served
      for(int i=0; i < latestClientResponses.size(); i++){
        if(latestClientResponses[i].clientAddress == pk->getSrcAddress()){
          if(latestClientResponses[i].latestSequenceNumber == pk->getSequenceNumber()){
            clientCommandResponseRPC = new RPCClientCommandResponsePacket("RPC_CLIENT_COMMAND_RESPONSE", RPC_CLIENT_COMMAND_RESPONSE);
            clientCommandResponseRPC->setSequenceNumber(latestClientResponses[i].latestSequenceNumber);
            clientCommandResponseRPC->setValue(latestClientResponses[i].latestReponseToClient); // If it was not a read, it is not a problem, the client simply would ignore this field by himself which has no meaning
            clientCommandResponseRPC->setSrcAddress(myAddress);
            clientCommandResponseRPC->setDestAddress(latestClientResponses[i].clientAddress);
            send(clientCommandResponseRPC, "port$o");
            ret = true;
            break; // exit from the for
          }
          else{
            // If command was already received but not yet completed, simply ignore
            if (latestClientResponses[i].currentSequenceNumber == pk->getSequenceNumber()){
              ret = true;
              break; // exit from the for
            }
            else{
              latestClientResponses[i].currentSequenceNumber = pk->getSequenceNumber();
            }
          }
        }    
      }
      if(ret){break;} // exit from the switch
      
      // Keep track of eventual new clients
      if(getClientIndex(pk->getSrcAddress()) == -1){
        latest_client_response temp;
        temp.clientAddress = pk->getSrcAddress();
        temp.latestSequenceNumber = pk->getSequenceNumber()-1;
        temp.currentSequenceNumber = pk->getSequenceNumber();
        latestClientResponses.push_back(temp);
      }


      // Command received == WRITE
      if(pk->getType() == WRITE){
        if(pk->getVar() == 'C'){ // If a config change mex
          newConfiguration.assign(pk->getClusterConfig().servers.begin(), pk->getClusterConfig().servers.end());
          // Initializing nextIndexNewConfig and matchIndexNewConfig to manage servers in the newConfiguration
          nextIndexNewConfig.resize(newConfiguration.size(), log.back().logIndex);
          matchIndexNewConfig.resize(newConfiguration.size(), 0);
          RPCsNewConfig.clear();
          RPCsNewConfig.resize(newConfiguration.size());
          // We need to copy in matchIndexNewConfig the indexes of the servers in config that are also in newConfig, and copy also the RPC tracked data
          for (int i=0; i < newConfiguration.size(); i++){
            int serverInBothConfig = getIndex(configuration, newConfiguration[i]);
            if (serverInBothConfig != -1){ // Server is both in config and in newConfig
              matchIndexNewConfig[i] = matchIndex[serverInBothConfig];
              nextIndexNewConfig[i] = nextIndex[serverInBothConfig];
              RPCsNewConfig[i].sequenceNumber = RPCs[i].sequenceNumber;
              RPCsNewConfig[i].success = RPCs[i].success;
              RPCsNewConfig[i].isHeartbeat = RPCs[i].isHeartbeat;
            }
          }
          for (int i = 0; i < RPCsNewConfig.size(); i++){
            RPCsNewConfig[i].success = true; // To allow the first appendNewEntry to proceed like expected
          } 
          // Start bringing up to date NEW (only) servers
          newServersCanVote = false;
          if (!log.empty()){
            log_entry newEntry;
            newEntry = log.back();
            newEntry.cOld.assign(log.back().cOld.begin(), log.back().cOld.end());
            newEntry.cNew.assign(log.back().cNew.begin(), log.back().cNew.end());
            newEntry.clientsData.assign(log.back().clientsData.begin(), log.back().clientsData.end());
            appendNewEntry(newEntry, true);
          }
          else{
            // Snapshot need to be send only to new servers
            for (int i=0; i< newConfiguration.size(); i++){
              // If server is not in configuration, means it is a new added one
              if (getIndex(configuration, newConfiguration[i]) == -1){
                sendSnapshot(newConfiguration[i]);
              }
            }
          }
          break;
        }

        // If it is a normal client request 
        log_entry newEntry;
        newEntry.term = currentTerm;
        newEntry.logIndex = log.back().logIndex + 1;
        newEntry.clientAddress = pk->getSrcAddress();
        newEntry.var = pk->getVar();
        newEntry.value = pk->getValue();
        newEntry.clientsData.assign(latestClientResponses.begin(), latestClientResponses.end());
        log.push_back(newEntry);
        matchIndex[getIndex(configuration, myAddress)]++;
        
        if(configuration != newConfiguration && getIndex(newConfiguration, myAddress) != -1){
          matchIndexNewConfig[getIndex(newConfiguration, myAddress)]++;
        }
        
        cancelEvent(sendHearthbeat);
        appendNewEntry(newEntry, false);
        scheduleAt(simTime() + par("hearthBeatTime"), sendHearthbeat);
        
        // Take snapshot if needed
        if (log.size() >= (int)par("maxLogSizeBeforeSnapshot")) {takeSnapshot();}      
      }
      else{ // READ case
        if(pendingReadClients.empty() == true){
          pendingReadClients.push_back(pk->getSrcAddress());
          
          int index = getEntryIndexPositionInLog(commitIndex);
          if(log[index].term == currentTerm){ // If already committed an entry in this term (e.g., at least the initial no_op already committed), an alternative is to check if the index of the last no_op in the log is <= commitIndex (and it's term == currentTerm)
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
      if(pk->getSrcAddress() == adminAddress){
        break;
      }
      clientCommandResponseRPC = new RPCClientCommandResponsePacket("RPC_CLIENT_COMMAND_RESPONSE", RPC_CLIENT_COMMAND_RESPONSE);
      clientCommandResponseRPC->setRedirect(true);
      clientCommandResponseRPC->setLastKnownLeader(leaderAddress); //the client will check if it is -1; thus no Leader because startup of the cluster
      clientCommandResponseRPC->setSrcAddress(myAddress);
      clientCommandResponseRPC->setDestAddress(pk->getSrcAddress());
      clientCommandResponseRPC->setSequenceNumber(pk->getSequenceNumber());
      send(clientCommandResponseRPC, "port$o"); 
    }
  }
  break;
  case RPC_INSTALL_SNAPSHOT:
  {
    RPCInstallSnapshotPacket *pk = check_and_cast<RPCInstallSnapshotPacket *>(pkGeneric);
    receiverAddress = pk->getSrcAddress();
    int seqNum = pk->getSequenceNumber();

    // If term in packet < currentTerm, reply immediately
    if (pk->getTerm() < currentTerm){
      sendSnapshotResponse(receiverAddress, seqNum);
      break;
    }

    // 0) Update the leader address
    leaderAddress = pk->getLeaderId();


    // 1) Save received snapshot file, discarding existing snapshot if index smaller 
    if (pk->getLastIncludedIndex() > snapshot.lastIncludedIndex)  {snapshot = pk->getSnapshot();}


    // 2) If existing log entry has same index and term as snapshot's last included entry, then entries covered by snapshot are deleted,
    //    but entries following the snapshot are still valid and must be retained. Reply after resizing
    int index = getEntryIndexPositionInLog(pk->getLastIncludedIndex());
    if (index != -1){
      if (log[index].term == pk->getLastIncludedTerm()){
        log.erase(log.begin(), log.begin() + index);
        sendSnapshotResponse(receiverAddress, seqNum);
        break;
      }
    }


    // 3) Discard the entire log
    log.clear();


    // 4) If we arrive here, received snapshot has the most up-to-date info.
    //    So, reset state machine using snapshot contents(and load snapshot's cluster configuration, and client's data)
    x = pk->getSnapshot().value;
    configuration.assign(pk->getSnapshot().cOld.begin(), pk->getSnapshot().cOld.end());
    newConfiguration.assign(pk->getSnapshot().cNew.begin(), pk->getSnapshot().cNew.end());
    latestClientResponses.assign(pk->getSnapshot().clientsData.begin(), pk->getSnapshot().clientsData.end());
    sendSnapshotResponse(receiverAddress, seqNum);
  }
  break;
  case RPC_INSTALL_SNAPSHOT_RESPONSE:
  {
    RPCInstallSnapshotResponsePacket *pk = check_and_cast<RPCInstallSnapshotResponsePacket *>(pkGeneric);
    if(status == LEADER){
      int receiverAddress = pk->getSrcAddress();
      if(!checkValidRPCResponse(receiverAddress, pk->getSequenceNumber())){break;}
      
      bool snapshotIsChanged = false; 

      for(int i=0; i < installSnapshotTimers.size() ; i++){
        if (receiverAddress == installSnapshotTimers[i].destination){
          cancelAndDelete(installSnapshotTimers[i].timeoutEvent);
          installSnapshotTimers.erase(installSnapshotTimers.begin() + i);
          if(installSnapshotTimers[i].lastIncludedIndex != snapshot.lastIncludedIndex){
            snapshotIsChanged = true;
          }
          break;
        }
      }

      // Check if there are other entries to send after the snapshot
      int position;
      int index;

      if (getIndex(configuration, receiverAddress) != -1)
      {
        position = getIndex(configuration, receiverAddress);
        // Update matchIndex and nextIndex
        if(snapshotIsChanged){
          matchIndex[position] = nextIndex[position];
          index = ++nextIndex[position];
        }
        else{
          matchIndex[position] = snapshot.lastIncludedIndex;
          nextIndex[position] = snapshot.lastIncludedIndex + 1;
          index = nextIndex[position];
        }
      }

      // Configuration change and follower also in new config
      if (configuration != newConfiguration && getIndex(newConfiguration, receiverAddress) != -1)
      {
        position = getIndex(newConfiguration, receiverAddress);
        // Update matchIndex and nextIndex
        if(snapshotIsChanged){
          matchIndexNewConfig[position] = nextIndexNewConfig[position];
          index = ++nextIndexNewConfig[position];
        }
        else{
          matchIndexNewConfig[position] = snapshot.lastIncludedIndex;
          nextIndexNewConfig[position] = snapshot.lastIncludedIndex + 1;
          index = nextIndexNewConfig[position];
        }
      }

      // If nextIndex is not in any log entry ==> LEADER may have delete the entry cause of snapshotting
      if (getEntryIndexPositionInLog(index) == -1)
      {
        // If nextIndex is in snapshot, send snapshot. Else means that FOLLOWER is up to date
        if (snapshot.value != -1 && index <= snapshot.lastIncludedIndex)
        {
          sendSnapshot(receiverAddress);
        }
        else
        {
          // Case of membership change and the address come from an only NEW server which now is up to date
          if (!newServersCanVote && configuration != newConfiguration && getIndex(newConfiguration, receiverAddress) != -1 && getIndex(configuration, receiverAddress) == -1)
          {
            if (checkNewServersAreUpToDate())
            { // Now trigger the Cold,new append
              bubble("Creating C_old,new");
              log_entry newEntry;
              newEntry.term = currentTerm;
              newEntry.logIndex = log.back().logIndex + 1;
              newEntry.clientAddress = adminAddress;
              newEntry.var = 'C';
              newEntry.cOld.assign(configuration.begin(), configuration.end());
              newEntry.cNew.assign(newConfiguration.begin(), newConfiguration.end());
              newEntry.clientsData.assign(latestClientResponses.begin(), latestClientResponses.end());
              log.push_back(newEntry);
              matchIndex[getIndex(configuration, myAddress)]++;
              if (getIndex(newConfiguration, myAddress) != -1)
              {
                matchIndexNewConfig[getIndex(newConfiguration, myAddress)]++;
              }
              newServersCanVote = true;
              cancelEvent(sendHearthbeat);
              appendNewEntry(newEntry, false);
              scheduleAt(simTime() + par("hearthBeatTime"), sendHearthbeat);
            }
          }
        }
      }
      else
      {
        index = getEntryIndexPositionInLog(index);
        log_entry newEntry;
        newEntry = log[index];
        newEntry.clientsData.assign(log[index].clientsData.begin(), log[index].clientsData.end());
        if (newEntry.var == 'C')
        {
          newEntry.cOld.assign(newEntry.cOld.begin(), newEntry.cOld.end());
          newEntry.cNew.assign(newEntry.cNew.begin(), newEntry.cNew.end());
        }
        appendNewEntryTo(newEntry, receiverAddress, position);
      }
    }
  }
  default:
  break;
  }

  delete pkGeneric;  
}

bool Server::checkValidRPCResponse(int sender, int SN){
  bool result = false;

  // Check if server is one of configuration (even if membership change or not) and the sequence numbers matches and not yet received
  if(getIndex(configuration, sender) != -1 && RPCs[getIndex(configuration, sender)].sequenceNumber == SN && RPCs[getIndex(configuration, sender)].success == false){
    RPCs[getIndex(configuration, sender)].success = true;
    result = true;
  }
  // Check if server is one of newConfiguration if a membership change is occurring and the sequence numbers matches and not yet received (Note: if a server i both in "configuration" and "newConfiguration", it will pass this "if" and the one above exactly with same behaviour beacuse RPCs and RPCsNewConfig for him will be equals)
  if(configuration != newConfiguration && getIndex(newConfiguration, sender) != -1 && RPCsNewConfig[getIndex(newConfiguration, sender)].sequenceNumber == SN && RPCsNewConfig[getIndex(newConfiguration, sender)].success == false){
    RPCsNewConfig[getIndex(newConfiguration, sender)].success = true;
    result = true;
  }
  return result;
}

bool Server::checkHeartbeatResponse(int sender){
  bool result = false;

  // Check if server is one of configuration (even if membership change or not) and the sequence numbers matches and not yet received
  if(getIndex(configuration, sender) != -1 && RPCs[getIndex(configuration, sender)].isHeartbeat){
    result = true;
  }
  // Check if server is one of newConfiguration if a membership change is occurring and the sequence numbers matches and not yet received (Note: if a server i both in "configuration" and "newConfiguration", it will pass this "if" and the one above exactly with same behaviour beacuse RPCs and RPCsNewConfig for him will be equals)
  if(configuration != newConfiguration && getIndex(newConfiguration, sender) != -1 && RPCsNewConfig[getIndex(newConfiguration, sender)].isHeartbeat){
    result = true;
  }
  return result;
}

void Server::appendNewEntry(log_entry newEntry, bool onlyToNewServers){
  int index;
  RPCPacket *pk_copy;

  appendEntriesRPC = new RPCAppendEntriesPacket("RPC_APPEND_ENTRIES", RPC_APPEND_ENTRIES);
  appendEntriesRPC->setTerm(currentTerm);
  appendEntriesRPC->setLeaderId(myAddress);

  // I Need to have at least 2 entries, to take prevLogIndex from log
  if (log.size() > 1){
    index = getEntryIndexPositionInLog(log.back().logIndex);
    appendEntriesRPC->setPrevLogIndex(log.back().logIndex - 1);
    appendEntriesRPC->setPrevLogTerm(log[index - 1].term); 
  }
  else{ // Take from snapshot
    appendEntriesRPC->setPrevLogIndex(snapshot.lastIncludedIndex);
    appendEntriesRPC->setPrevLogTerm(snapshot.lastIncludedTerm); 
  }

  appendEntriesRPC->setEntry(newEntry);
  appendEntriesRPC->setLeaderCommit(commitIndex);
  appendEntriesRPC->setSrcAddress(myAddress);
  

  if (onlyToNewServers == false){
    //Send to all followers in the configuration
    for (int i = 0; i < configuration.size(); i++){
      // If to avoid sending to myself and to send only if the follower has no pending RPCs
      if(configuration[i] != myAddress){
        append_entry_timer newTimer;
        newTimer.destination = configuration[i];

        // I Need to have at least 2 entries, to take prevLogIndex from log
        if (log.size() > 1){
          index = getEntryIndexPositionInLog(log.back().logIndex);
          newTimer.prevLogIndex = log.back().logIndex -1;
          newTimer.prevLogTerm = log[index-1].term;
        }
        else{ // Take from snapshot
          newTimer.prevLogIndex = snapshot.lastIncludedIndex;
          newTimer.prevLogTerm = snapshot.lastIncludedTerm;
        }

        newTimer.timeoutEvent = new cMessage("append-entry-timeout-event");
        newTimer.entry = newEntry; // Sufficient to copy var, value, term, logIndex
        newTimer.entry.cOld.assign(newEntry.cOld.begin(), newEntry.cOld.end()); // To deep copy
        newTimer.entry.cNew.assign(newEntry.cNew.begin(), newEntry.cNew.end());
        newTimer.entry.clientsData.assign(newEntry.clientsData.begin(), newEntry.clientsData.end());
        
        // To check if there is not already an append entry "programmed"
        bool timerAlreadyPresent = false;
        for(int k=0; k < appendEntryTimers.size() ; k++){
          if (configuration[i] == appendEntryTimers[k].destination){
            timerAlreadyPresent = true;
            break;
          }
        }

        if(RPCs[i].success == true && !timerAlreadyPresent){
          appendEntriesRPC->setDestAddress(configuration[i]);

          // Increment sequence number
          RPCs[i].sequenceNumber++;
          RPCs[i].success = false;
          RPCs[i].isHeartbeat = false;
          appendEntriesRPC->setSequenceNumber(RPCs[i].sequenceNumber);
          // Also in RPCsNewConfig if the follower is also in newConfiguration
          if(configuration != newConfiguration && getIndex(newConfiguration, configuration[i]) != -1){
            RPCsNewConfig[getIndex(newConfiguration, configuration[i])].sequenceNumber++;
            RPCsNewConfig[getIndex(newConfiguration, configuration[i])].success = false;
            RPCsNewConfig[getIndex(newConfiguration, configuration[i])].isHeartbeat = false;
          }
          
          pk_copy = appendEntriesRPC->dup();
          if(newEntry.var=='N'){
            pk_copy->setDisplayString("b=10,10,rect,kind,kind,1");
          }
          send(pk_copy, "port$o");
          
          appendEntryTimers.push_back(newTimer);
          // Reset the timer to wait before retry sending (indefinitely) the append entries for a particular follower
          scheduleAt(simTime() + par("resendTimeout"), newTimer.timeoutEvent);
        }
        else{
          // If is going on an heartbeat RPC, set a timer that will trigger an append entry in the future when the heartbeat RPC will be possible terminated
          if (RPCs[i].isHeartbeat){         
            if(!timerAlreadyPresent){
              appendEntryTimers.push_back(newTimer);
              // Reset the timer to wait before retry sending (indefinitely) the append entries for a particular follower
              scheduleAt(simTime() + par("hearthBeatTime"), newTimer.timeoutEvent); // hearthBeatTime -> to avoid overlapping with a read-only leader check occurring
            }
          }
        }
      }
    }
  }
  
  // If a membership change is occurring
  if(configuration != newConfiguration){
    // Send to all followers in newConfiguration
    for (int i = 0; i < newConfiguration.size(); i++){
      // If to avoid sending to myself and avoid sending twice (if a server is in both configuration and newConfiguration) and only if it has no pending RPCs
      if(newConfiguration[i] != myAddress && getIndex(configuration, newConfiguration[i]) == -1){
        append_entry_timer newTimer;
        newTimer.destination = newConfiguration[i];

        // I Need to have at least 2 entries, to take prevLogIndex from log
        if (log.size() > 1){
          index = getEntryIndexPositionInLog(log.back().logIndex);
          newTimer.prevLogIndex = log.back().logIndex -1;
          newTimer.prevLogTerm = log[index-1].term;
        }
        else{ // Take from snapshot
          newTimer.prevLogIndex = snapshot.lastIncludedIndex;
          newTimer.prevLogTerm = snapshot.lastIncludedTerm;
        }

        newTimer.timeoutEvent = new cMessage("append-entry-timeout-event");
        newTimer.entry = newEntry; // Sufficient to copy var, value, term, logIndex
        newTimer.entry.cOld.assign(newEntry.cOld.begin(), newEntry.cOld.end()); // To perform deep copy
        newTimer.entry.cNew.assign(newEntry.cNew.begin(), newEntry.cNew.end());
        newTimer.entry.clientsData.assign(newEntry.clientsData.begin(), newEntry.clientsData.end());

        // To check if there is not already an append entry "programmed"
        bool timerAlreadyPresent = false;        
        for(int k=0; k < appendEntryTimers.size() ; k++){
          if (newConfiguration[i] == appendEntryTimers[k].destination){
            timerAlreadyPresent = true;
            break;
          }
        }

        if(RPCsNewConfig[i].success == true && !timerAlreadyPresent){
          appendEntriesRPC->setDestAddress(newConfiguration[i]);

          // Increment sequence number
          RPCsNewConfig[i].sequenceNumber++;
          RPCsNewConfig[i].success = false;
          RPCsNewConfig[i].isHeartbeat = false;
          appendEntriesRPC->setSequenceNumber(RPCsNewConfig[i].sequenceNumber);

          EV << "New Entry: Index=" << newEntry.logIndex
          << " Term=" << newEntry.term
          << " value" << newEntry.value
          << endl;
          pk_copy = appendEntriesRPC->dup();
          if(newEntry.var=='N'){
            pk_copy->setDisplayString("b=10,10,rect,kind,kind,1");
          }
          send(pk_copy, "port$o");
          
          appendEntryTimers.push_back(newTimer);
          // Reset the timer to wait before retry sending (indefinitely) the append entries for a particular follower
          scheduleAt(simTime() + par("resendTimeout"), newTimer.timeoutEvent);
        }else{
          // If is going on an heartbeat RPC, set a timer that will trigger an append entry in the future when the heartbeat RPC will be possible terminated
          if (RPCsNewConfig[i].isHeartbeat){
            if(!timerAlreadyPresent){
              appendEntryTimers.push_back(newTimer);
              // Reset the timer to wait before retry sending (indefinitely) the append entries for a particular follower
              scheduleAt(simTime() + par("hearthBeatTime"), newTimer.timeoutEvent); // hearthBeatTime -> to avoid overlapping with a read-only leader check occurring
            }
          }
        }
      }
    }
  }
  delete(appendEntriesRPC);
  return;
}

void Server::appendNewEntryTo(log_entry newEntry, int destAddress, int serverIndex){
  int index;

  appendEntriesRPC = new RPCAppendEntriesPacket("RPC_APPEND_ENTRIES", RPC_APPEND_ENTRIES);
  appendEntriesRPC->setTerm(currentTerm);
  appendEntriesRPC->setLeaderId(myAddress);
  appendEntriesRPC->setEntry(newEntry);
  appendEntriesRPC->setLeaderCommit(commitIndex);
  appendEntriesRPC->setSrcAddress(myAddress);

  // Update Sequence Number of receiver
  if(getIndex(configuration, destAddress) != -1){
    RPCs[getIndex(configuration, destAddress)].sequenceNumber++;
    RPCs[getIndex(configuration, destAddress)].success = false;
    RPCs[getIndex(configuration, destAddress)].isHeartbeat = false;
    appendEntriesRPC->setSequenceNumber(RPCs[getIndex(configuration, destAddress)].sequenceNumber);
  }
  if(configuration != newConfiguration && getIndex(newConfiguration, destAddress) != -1){
    RPCsNewConfig[getIndex(newConfiguration, destAddress)].sequenceNumber++;
    RPCsNewConfig[getIndex(newConfiguration, destAddress)].success = false;
    RPCsNewConfig[getIndex(newConfiguration, destAddress)].isHeartbeat = false;
    appendEntriesRPC->setSequenceNumber(RPCsNewConfig[getIndex(newConfiguration, destAddress)].sequenceNumber);
  }

  int tempPrevLogIndex;
  int tempPrevLogTerm;
  // If NOT membership change occurring OR if it is occurring but the destination is in configuration
  if((configuration == newConfiguration) || (configuration != newConfiguration && getIndex(configuration, destAddress) != -1)){
    index = getEntryIndexPositionInLog(nextIndex[serverIndex]);
    // If index == 0 means that we must take from the snapshot the prevLogIndex and prevLogTerm, because the previous entry is not more in the log
    if(index == 0){
      tempPrevLogIndex = snapshot.lastIncludedIndex;
      tempPrevLogTerm = snapshot.lastIncludedTerm;
    }
    else{
      tempPrevLogIndex = log[index-1].logIndex; // -1 because the previous
      tempPrevLogTerm = log[index-1].term;
    }
    
  }
  else{ // A membership change is occurring and the destination is ONLY in newConfiguration (by exclusion from the IF case above)
    index = getEntryIndexPositionInLog(nextIndexNewConfig[serverIndex]);
    if(index == 0){
      tempPrevLogIndex = snapshot.lastIncludedIndex;
      tempPrevLogTerm = snapshot.lastIncludedTerm; 
    }
    else{
      tempPrevLogIndex = log[index-1].logIndex; // -1 because the previous
      tempPrevLogTerm = log[index-1].term;
    }
  }

  appendEntriesRPC->setPrevLogIndex(tempPrevLogIndex);
  appendEntriesRPC->setPrevLogTerm(tempPrevLogTerm);

  // Create the associated timer to eventually resend the message if no response come back.
  append_entry_timer newTimer;
  newTimer.destination = destAddress;
  newTimer.prevLogIndex = tempPrevLogIndex;
  newTimer.prevLogTerm = tempPrevLogTerm;
  newTimer.timeoutEvent = new cMessage("append-entry-timeout-event");
  newTimer.entry = newEntry;
  newTimer.entry.cOld.assign(newEntry.cOld.begin(), newEntry.cOld.end());
  newTimer.entry.cNew.assign(newEntry.cNew.begin(), newEntry.cNew.end());
  newTimer.entry.clientsData.assign(newEntry.clientsData.begin(), newEntry.clientsData.end());
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
  int total, index;
  int counter1 = 0;
  int counter2 = 0;
  // If NOT membership change occurring
  if(configuration == newConfiguration || newServersCanVote == false){
    total = configuration.size();
    for (int i = 0; i < matchIndex.size(); i++){
      if(N <= matchIndex[i]){
        counter1++;
      }
    }
    return counter1 > (total/2);
  }
  else{ // Membership change occurring
    // If trying to commit Cnew use only Cnew majority.
    index = getEntryIndexPositionInLog(N);
    if(log[index].var == 'C' && log[index].cOld.empty() && !log[index].cNew.empty()){
      EV << "Trying to committ Cnew, using Cnew majority" << endl;
      total = newConfiguration.size();
      for (int i = 0; i < matchIndexNewConfig.size(); i++){
        if(N <= matchIndexNewConfig[i]){
          counter1++;
        }
      }
      return counter1 > (total/2);
    }
    else{ // Trying to commit Cold,new and other entries after (before Cnew) considering disjoint majorities.
      EV << "Trying to committ Cold,new, using disjoint majority" << endl;
      total = configuration.size();
      int total2 = newConfiguration.size();
      for (int i = 0; i < matchIndex.size(); i++){
        if(N <= matchIndex[i]){
          counter1++;
        }
      }
      for (int i = 0; i < matchIndexNewConfig.size(); i++){
        if(N <= matchIndexNewConfig[i]){
          counter2++;
        }
      }
      // To have majority i need disjoint agreement
      EV << "Total:" << total << " Total2:" << total2 << " Counter1:" << counter1 << " Counter2:" << counter2 << endl;
      return (counter1 > (total/2)) && (counter2 > (total2/2));
    }
  }
}

void Server::applyCommand(log_entry entry){
  switch (entry.var){
  case 'x':
    x = entry.value;
    break;
  case 'N': //no_op entry
    break;
  case 'C': // membership change entry (Cold,new or Cnew)
    break;
  default:
    break;
  }
  latestClientResponses.assign(entry.clientsData.begin(), entry.clientsData.end());
  return;
}

void Server::replayLog(){
  for (int i = 0; i < log.size(); i++){
    log_entry entry = log[i];
    //applyCommand(entry);
    if (entry.var == 'C'){
      if(!entry.cOld.empty()){ // Cold,new case
        configuration.assign(entry.cOld.begin(), entry.cOld.end());
        newConfiguration.assign(entry.cNew.begin(), entry.cNew.end());

        RPCsNewConfig.clear();
        RPCsNewConfig.resize(newConfiguration.size());

        for (int i=0; i < newConfiguration.size(); i++){
          int serverInBothConfig = getIndex(configuration, newConfiguration[i]);
          if (serverInBothConfig != -1){ // Server is both in config and in newConfig
            RPCsNewConfig[i].sequenceNumber = RPCs[i].sequenceNumber;
            RPCsNewConfig[i].success = RPCs[i].success;
          }
        }
      }
      else{ // Cnew case
        newConfiguration.assign(entry.cNew.begin(), entry.cNew.end());
        configuration.assign(newConfiguration.begin(), newConfiguration.end());
        RPCs.assign(RPCsNewConfig.begin(), RPCsNewConfig.end());
      }
    }
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
      votedFor = -1;
      if(status == CANDIDATE || status == LEADER){
        becomeFollower(pk);
      }
    }
    else{
      if (pk->getTerm() == currentTerm && status == CANDIDATE){
        becomeFollower(pk);
      }
    }
  }
    break;
  case RPC_REQUEST_VOTE:
  {
    RPCRequestVotePacket *pk = check_and_cast<RPCRequestVotePacket *>(pkGeneric);
    if(pk->getTerm() > currentTerm && !believeCurrentLeaderExists){
      currentTerm = pk->getTerm();
      votedFor = -1;
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
      votedFor = -1;
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
      votedFor = -1;
      if(status == CANDIDATE || status == LEADER){
        becomeFollower(pk);
      }
    }
  }
    break;
  case RPC_INSTALL_SNAPSHOT:
  {
    RPCInstallSnapshotPacket *pk = check_and_cast<RPCInstallSnapshotPacket *>(pkGeneric);
    if(pk->getTerm() > currentTerm){
      currentTerm = pk->getTerm();
      votedFor = -1;
      if(status == CANDIDATE || status == LEADER){
        becomeFollower(pk);
      }
    }
  }
    break;
  case RPC_INSTALL_SNAPSHOT_RESPONSE:
  {
    RPCInstallSnapshotResponsePacket *pk = check_and_cast<RPCInstallSnapshotResponsePacket *>(pkGeneric);
    if(pk->getTerm() > currentTerm){
      currentTerm = pk->getTerm();
      votedFor = -1;
      if(status == CANDIDATE || status == LEADER){
        becomeFollower(pk);
      }
    }
  }
  default:
    break;
  }
  return;
}

void Server::becomeCandidate(){
  status = CANDIDATE;
  currentTerm++;
  votedFor = myAddress;

  // If the server is in configuration (even if membership change or not)
  if(getIndex(configuration, myAddress) != -1){
      votes = 1;
  }
  // If membership change and the server is in newConfiguration
  if(configuration != newConfiguration && getIndex(newConfiguration, myAddress) != -1){
    votesNewConfig = 1;
  }
  scheduleAt(simTime() +  uniform(SimTime(par("lowElectionTimeout")), SimTime(par("highElectionTimeout"))), electionTimeoutEvent);
}

void Server::becomeLeader(){
  cancelEvent(minElectionTimeoutEvent);
  believeCurrentLeaderExists = true;
  cancelEvent(electionTimeoutEvent);
  status = LEADER;
  leaderAddress = myAddress;
  votes = 0;
  newServersCanVote = true;
  votesNewConfig = 0;
  nextIndex.clear();
  matchIndex.clear();
  nextIndex.resize(configuration.size(), log.back().logIndex + 1);
  matchIndex.resize(configuration.size(), 0);
  RPCs.clear();
  RPCs.resize(configuration.size());
  for (int i = 0; i < RPCs.size(); i++){
    RPCs[i].success = true; // To avoid waiting for other request vote response (useless if now i am leader), mark them as finished
  }
  
  // If i become leader in a membership change phase
  if(configuration != newConfiguration){
    nextIndexNewConfig.resize(newConfiguration.size(), log.back().logIndex + 1);
    matchIndexNewConfig.resize(newConfiguration.size(), 0);
    RPCsNewConfig.clear();
    RPCsNewConfig.resize(newConfiguration.size());
    for (int i = 0; i < RPCsNewConfig.size(); i++){
      RPCsNewConfig[i].success = true; // To avoid waiting for other request vote response (useless if now i am leader), mark them as finished
    }
  }

  // If snapshotFile is NOT empty
  if (snapshot.value != -1){ 
    commitIndex = snapshot.lastIncludedIndex;
    lastApplied = commitIndex;
  }

  log_entry newEntry;
  newEntry.term = currentTerm;
  newEntry.logIndex = log.back().logIndex + 1;
  newEntry.var = 'N';
  newEntry.clientsData.assign(latestClientResponses.begin(), latestClientResponses.end());
  matchIndex[getIndex(configuration, myAddress)] = newEntry.logIndex;

  if(configuration != newConfiguration && getIndex(newConfiguration, myAddress) != -1){
    matchIndexNewConfig[getIndex(newConfiguration, myAddress)] = newEntry.logIndex;
  }
  log.push_back(newEntry);
  appendNewEntry(newEntry, false);

  scheduleAt(simTime() + par("hearthBeatTime"), sendHearthbeat); // Schedule the sendHeartbeat
}

void Server::becomeFollower(RPCPacket *pkGeneric){
  cancelEvent(electionTimeoutEvent);
  cancelEvent(minElectionTimeoutEvent);
  cancelEvent(sendHearthbeat);
  believeCurrentLeaderExists = true;
  for (int i = 0; i < appendEntryTimers.size() ; i++){
    cancelEvent(appendEntryTimers[i].timeoutEvent);
  }
  for (int i = 0; i < installSnapshotTimers.size() ; i++){
    cancelEvent(installSnapshotTimers[i].timeoutEvent);
  }
  if(pkGeneric->getKind() == RPC_APPEND_ENTRIES){
    RPCAppendEntriesPacket *pk = check_and_cast<RPCAppendEntriesPacket *>(pkGeneric);
    leaderAddress = pk->getLeaderId();
  }
  votes = 0;
  votesNewConfig = 0;
  acks = 0;
  nextIndex.clear();
  matchIndex.clear();
  nextIndexNewConfig.clear();
  matchIndexNewConfig.clear();

  if (status != NON_VOTING){
    scheduleAt(simTime() + par("lowElectionTimeout"), minElectionTimeoutEvent);
    scheduleAt(simTime() +  uniform(SimTime(par("lowElectionTimeout")), SimTime(par("highElectionTimeout"))), electionTimeoutEvent);
  }
  status = FOLLOWER;
}

void Server::initializeConfiguration(){
  cModule *Switch = gate("port$i")->getPreviousGate()->getOwnerModule();
  int moduleAddress;
  for (int i = 1; i < Switch->gateSize("port$o"); i++){
    std::string serverString = "server";
    if (Switch->gate("port$o", i)->isConnected()){
      std::string moduleCheck = Switch->gate("port$o", i)->getNextGate()->getOwnerModule()->getFullName();
      if (moduleCheck.find(serverString) != std::string::npos){
        moduleAddress = Switch->gate("port$o", i)->getId();
        //EV << "Added ID: " << moduleAddress << " to configuration Vector" << endl;
        configuration.push_back(moduleAddress);
      }
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

void Server::sendAck(int destAddress, int seqNum){
  appendEntriesResponseRPC = new RPCAppendEntriesResponsePacket("RPC_APPEND_ENTRIES_RESPONSE", RPC_APPEND_ENTRIES_RESPONSE);
  appendEntriesResponseRPC->setSuccess(true);
  appendEntriesResponseRPC->setTerm(currentTerm);
  appendEntriesResponseRPC->setSrcAddress(myAddress);
  appendEntriesResponseRPC->setDestAddress(destAddress);
  appendEntriesResponseRPC->setSequenceNumber(seqNum);
  appendEntriesResponseRPC->setDisplayString("b=7,7,oval,black,black,1");

  send(appendEntriesResponseRPC, "port$o");
  
  
  // ACK->setDisplayString("b=7,7,oval,black,black,1");
  // send(ACK, "port$o"); 
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
  clientCommandResponseRPC->setSequenceNumber(latestClientResponses[getClientIndex(clientAddress)].currentSequenceNumber);
  clientCommandResponseRPC->setSrcAddress(myAddress);
  clientCommandResponseRPC->setDestAddress(clientAddress);
  send(clientCommandResponseRPC, "port$o");
  latestClientResponses[getClientIndex(clientAddress)].latestSequenceNumber = latestClientResponses[getClientIndex(clientAddress)].currentSequenceNumber;
}

void Server::startReadOnlyLeaderCheck(){
  cancelEvent(sendHearthbeat);
  countingFeedback = true;
  acks++;
  if(configuration!=newConfiguration && (getIndex(newConfiguration, myAddress)!=-1)){
    acksNewConf++;
  }
  scheduleAt(simTime(), sendHearthbeat); // Trigger immediately an heartbeat send
}

bool Server::checkNewServersAreUpToDate(){
  vector<int> serversOnlyInNewConf;
  int counter = 0;
  // Finding all servers which are in the "new" configurations but NOT in the "old" one.
  for (int i = 0; i < newConfiguration.size(); i++){
    if(getIndex(configuration, newConfiguration[i]) == -1){ // If server of newConfig is not in configuration, mean it is a new one
      serversOnlyInNewConf.push_back(newConfiguration[i]);
    }
  }

  for (int i = 0; i < serversOnlyInNewConf.size(); i++){
    if(matchIndexNewConfig[getIndex(newConfiguration, serversOnlyInNewConf[i])] == log.back().logIndex){
      counter++;
    }
  }
  
  if (counter == serversOnlyInNewConf.size())
  {
    return true;
  }
  return false;
}

void Server::sendHeartbeatToFollower(){
  // Send an empty RPCAppendEntries(= hearthbeat), to all followers
  //EV << "Sending hearthbeat to followers\n";
  bubble("Sending heartbeat");
  for (int i = 0; i < configuration.size(); i++){
    // If it is not myAddress, and the previous RPC is completed or it was an heartbeat RPC.
    if(configuration[i] != myAddress && (RPCs[i].success == true || RPCs[i].isHeartbeat == true)){
      log_entry emptyEntry;
      emptyEntry.term = -1; //convention to explicit heartbeat
    
      appendEntriesRPC = new RPCAppendEntriesPacket("RPC_APPEND_ENTRIES", RPC_APPEND_ENTRIES);
      appendEntriesRPC->setTerm(currentTerm);
      appendEntriesRPC->setLeaderId(myAddress);
      appendEntriesRPC->setEntry(emptyEntry);
      appendEntriesRPC->setLeaderCommit(commitIndex);
      appendEntriesRPC->setSrcAddress(myAddress);
      appendEntriesRPC->setDestAddress(configuration[i]);
      
      RPCs[i].sequenceNumber++;
      RPCs[i].success = false;
      RPCs[i].isHeartbeat = true;
      appendEntriesRPC->setSequenceNumber(RPCs[i].sequenceNumber);
      if (newConfiguration != configuration && getIndex(newConfiguration, configuration[i]) != -1){
        RPCsNewConfig[getIndex(newConfiguration, configuration[i])].sequenceNumber++;
        RPCsNewConfig[getIndex(newConfiguration, configuration[i])].success = false;
        RPCsNewConfig[getIndex(newConfiguration, configuration[i])].isHeartbeat = true;
      }

      appendEntriesRPC->setDisplayString("b=7,7,oval,blue,black,1");
      send(appendEntriesRPC, "port$o");
    }
  }
  // If a membership change is occurring (consider Cold,new)
  if (newConfiguration != configuration){
    for (int i = 0; i < newConfiguration.size(); i++){
      if(newConfiguration[i] != myAddress && getIndex(configuration, newConfiguration[i]) == -1 && (RPCsNewConfig[i].success == true || RPCsNewConfig[i].isHeartbeat == true)){
        log_entry emptyEntry;
        emptyEntry.term = -1; //convention to explicit heartbeat 
      
        appendEntriesRPC = new RPCAppendEntriesPacket("RPC_APPEND_ENTRIES", RPC_APPEND_ENTRIES);
        appendEntriesRPC->setTerm(currentTerm);
        appendEntriesRPC->setLeaderId(myAddress);
        appendEntriesRPC->setEntry(emptyEntry);
        appendEntriesRPC->setLeaderCommit(commitIndex);
        appendEntriesRPC->setSrcAddress(myAddress);
        appendEntriesRPC->setDestAddress(newConfiguration[i]);
        
        RPCsNewConfig[i].sequenceNumber++;
        RPCsNewConfig[i].success = false;
        RPCsNewConfig[i].isHeartbeat = true;
        appendEntriesRPC->setSequenceNumber(RPCsNewConfig[i].sequenceNumber);
        
        appendEntriesRPC->setDisplayString("b=7,7,oval,blue,black,1");
        send(appendEntriesRPC, "port$o");
      }
    }
  }
  scheduleAt(simTime() + par("hearthBeatTime"), sendHearthbeat);
  return;
}

void Server::sendRequestVote(){
  RPCPacket *pk_copy;

  requestVoteRPC = new RPCRequestVotePacket("RPC_REQUEST_VOTE", RPC_REQUEST_VOTE);
  requestVoteRPC->setTerm(currentTerm);
  requestVoteRPC->setCandidateId(myAddress);
  if (log.empty()){
    requestVoteRPC->setLastLogIndex(snapshot.lastIncludedIndex);
    requestVoteRPC->setLastLogTerm(snapshot.lastIncludedTerm);
  }
  else{
    requestVoteRPC->setLastLogIndex(log.back().logIndex);
    requestVoteRPC->setLastLogTerm(log.back().term);
  }
  requestVoteRPC->setSrcAddress(myAddress);

  for (int i = 0; i < configuration.size(); i++){
    if(configuration[i] != myAddress){

      RPCs[i].sequenceNumber++;
      RPCs[i].success = false;
      RPCs[i].isHeartbeat = false;
      // If membership change occurring check if necessary to increment also the seq num un RPCsNewConfig
      if (configuration != newConfiguration && getIndex(newConfiguration, configuration[i]) != -1){
        RPCsNewConfig[getIndex(newConfiguration, configuration[i])].sequenceNumber++;
        RPCsNewConfig[getIndex(newConfiguration, configuration[i])].success = false;
        RPCsNewConfig[getIndex(newConfiguration, configuration[i])].isHeartbeat = false;
      }

      requestVoteRPC->setSequenceNumber(RPCs[i].sequenceNumber);
      requestVoteRPC->setDestAddress(configuration[i]);
      pk_copy = requestVoteRPC->dup();
      send(pk_copy, "port$o");
    }
  }
  // If a membership change is occurring (consider Cold,new)
  if (newConfiguration != configuration){
    for (int i = 0; i < newConfiguration.size(); i++){
      // If the destination is not myself and if it is not in configuration (to avoid double sending)
      if(newConfiguration[i] != myAddress && getIndex(configuration, newConfiguration[i]) == -1){
        RPCsNewConfig[i].sequenceNumber++;
        RPCsNewConfig[i].success = false;
        RPCsNewConfig[i].isHeartbeat = false;
        requestVoteRPC->setSequenceNumber(RPCsNewConfig[i].sequenceNumber);
        requestVoteRPC->setDestAddress(newConfiguration[i]);
        pk_copy = requestVoteRPC->dup();
        send(pk_copy, "port$o");
      }
    }
  }
  delete(requestVoteRPC);
}

void Server::takeSnapshot(){
  bubble("I am taking a snapshot"); 
  for (int i=0; i<log.size(); i++){
    if(log[i].logIndex == commitIndex){

      // Save index and term of last entry in the log known to be committed
      snapshot.lastIncludedIndex = log[i].logIndex;
      snapshot.lastIncludedTerm = log[i].term;

      // Save state machine state
      snapshot.var = 'x';
      snapshot.value = x;
      snapshot.cOld.assign(configuration.begin(), configuration.end());
      snapshot.cNew.assign(newConfiguration.begin(), newConfiguration.end());
      snapshot.clientsData.assign(latestClientResponses.begin(), latestClientResponses.end());

      // Delete the log till(included) the entry with lastIncludedIndex
      log.erase(log.begin(), log.begin() + i + 1); // begin() + i erase the first i elements of the log. +1 is needed since log positions start from 0
      break; 
    }
  }
}

int Server::getEntryIndexPositionInLog(int entryIndex){
  for (int i=0; i<log.size(); i++){

    // If we found the entryIndex in the log, return the position where we found the match
    if(log[i].logIndex == entryIndex){
      return i;
    }
  }
  // If entryIndex not found, or log is empty because is all in snapshot
  return -1; 
}

void Server::sendSnapshot(int destAddress){
  bubble("I am sending a snapshot"); 
  snapshot_file temp = snapshot;
  temp.cOld.assign(snapshot.cOld.begin(), snapshot.cOld.end());
  temp.cNew.assign(snapshot.cNew.begin(), snapshot.cNew.end());
  temp.clientsData.assign(snapshot.clientsData.begin(), snapshot.clientsData.end());

  installSnapshotRPC = new RPCInstallSnapshotPacket("RPC_INSTALL_SNAPSHOT", RPC_INSTALL_SNAPSHOT);
  installSnapshotRPC->setSrcAddress(myAddress);
  installSnapshotRPC->setDestAddress(destAddress);
  installSnapshotRPC->setTerm(currentTerm);
  installSnapshotRPC->setLeaderId(myAddress);
  installSnapshotRPC->setLastIncludedIndex(snapshot.lastIncludedIndex);
  installSnapshotRPC->setLastIncludedTerm(snapshot.lastIncludedTerm);
  installSnapshotRPC->setSnapshot(temp);

  // Update Sequence Number of receiver
  if(getIndex(configuration, destAddress) != -1){
    RPCs[getIndex(configuration, destAddress)].sequenceNumber++;
    RPCs[getIndex(configuration, destAddress)].success = false;
    RPCs[getIndex(configuration, destAddress)].isHeartbeat = false;
    installSnapshotRPC->setSequenceNumber(RPCs[getIndex(configuration, destAddress)].sequenceNumber);
  }
  if(configuration != newConfiguration && getIndex(newConfiguration, destAddress) != -1){
    RPCsNewConfig[getIndex(newConfiguration, destAddress)].sequenceNumber++;
    RPCsNewConfig[getIndex(newConfiguration, destAddress)].success = false;
    RPCsNewConfig[getIndex(newConfiguration, destAddress)].isHeartbeat = false;
    installSnapshotRPC->setSequenceNumber(RPCsNewConfig[getIndex(newConfiguration, destAddress)].sequenceNumber);
  }
  
  install_snapshot_timer newTimer;
  newTimer.destination = destAddress;
  newTimer.lastIncludedIndex = temp.lastIncludedIndex;
  newTimer.lastIncludedTerm = temp.lastIncludedTerm;
  newTimer.timeoutEvent = new cMessage("install-snapshot-timeout-event");
  newTimer.snapshot = temp;
  newTimer.snapshot.cOld.assign(temp.cOld.begin(), temp.cOld.end());
  newTimer.snapshot.cNew.assign(temp.cNew.begin(), temp.cNew.end());
  newTimer.snapshot.clientsData.assign(temp.clientsData.begin(), temp.clientsData.end());

  installSnapshotTimers.push_back(newTimer);
  scheduleAt(simTime() + par("resendTimeout"), newTimer.timeoutEvent);

  send(installSnapshotRPC, "port$o");
}

void Server::sendSnapshotResponse(int destAddress, int seqNum){
  installSnapshotResponseRPC = new RPCInstallSnapshotResponsePacket("RPC_INSTALL_SNAPSHOT_RESPONSE", RPC_INSTALL_SNAPSHOT_RESPONSE);
  installSnapshotResponseRPC->setSrcAddress(myAddress);
  installSnapshotResponseRPC->setDestAddress(destAddress);
  installSnapshotResponseRPC->setTerm(currentTerm);
  installSnapshotResponseRPC->setSequenceNumber(seqNum);

  send(installSnapshotResponseRPC, "port$o");
}

void Server::applySnapshot(){

  // Update currentTerm
  currentTerm = snapshot.lastIncludedTerm;

  // Update commitIndex, state machine, and lastApplied
  commitIndex = snapshot.lastIncludedIndex;
  switch (snapshot.var)
  {
  case 'x':
    x = snapshot.value;
    break; 
  default:
    break;
  }
  lastApplied = commitIndex;

  // Update clientsData
  latestClientResponses.assign(snapshot.clientsData.begin(), snapshot.clientsData.end());

  // Update configurations
  configuration.assign(snapshot.cOld.begin(), snapshot.cOld.end());
  newConfiguration.assign(snapshot.cNew.begin(), snapshot.cNew.end());
}

void Server::refreshDisplay() const
{
    char buf[50];
    std::string s;
    switch(status){
      case LEADER:{
        s = "LEADER";
      }
      break;
      case FOLLOWER:{
        s = "FOLLOWER";
      }
      break;
      case CANDIDATE:{
        s = "CANDIDATE";
      }
      break;
      case NON_VOTING:{
        s = "NON_VOTING";
      }
      break;
    }
    const char *c = s.c_str();
    sprintf(buf, "s: %s  Term: %d  x: %d, size: %ld", c, currentTerm, x, log.size());
    getDisplayString().setTagArg("t", 0, buf);

    if (status == LEADER){
      getDisplayString().setTagArg("t", 2, "green");
    }
    if (status == FOLLOWER){
      getDisplayString().setTagArg("t", 2, "grey");
      //getDisplayString().setTagArg("i", 1, "");
    }
    if (status == CANDIDATE){
      getDisplayString().setTagArg("t", 2, "red");
    }
    if (iAmCrashed == true){
      getDisplayString().setTagArg("i", 1, "black");
    }
    
}

std::ostream& operator<<(std::ostream& os, const vector<log_entry> log){
  for (int i = 0; i < log.size(); i++){
    os << "{i=" << log[i].logIndex << ",t=" << log[i].term << ",v=" << log[i].value << "} "; // no endl!
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const vector<lastRPC> RPCs){
  for (int i = 0; i < RPCs.size(); i++){
    os << "{SN=" << RPCs[i].sequenceNumber << ",Success=" << RPCs[i].success << ",IsHeartbeat=" << RPCs[i].isHeartbeat << "} "; // no endl!
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const vector<append_entry_timer> appendEntryTimers){
  for (int i = 0; i < appendEntryTimers.size(); i++){
    os << "{D=" << appendEntryTimers[i].destination << ",Event=" << appendEntryTimers[i].timeoutEvent << "} "; // no endl!
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const vector<install_snapshot_timer> installSnapshotTimers){
  for (int i = 0; i < installSnapshotTimers.size(); i++){
    os << "{D=" << installSnapshotTimers[i].destination << ",Event=" << installSnapshotTimers[i].timeoutEvent << "} "; // no endl!
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const snapshot_file snap){
  os << "{lastIndex=" << snap.lastIncludedIndex << ",lastTerm=" << snap.lastIncludedTerm << ",value=" << snap.value << "} "; // no endl!
  return os;
}

std::ostream& operator<<(std::ostream& os, const vector<latest_client_response> latestClientResponses){
  for (int i = 0; i < latestClientResponses.size(); i++){
    os << "{Client=" << latestClientResponses[i].clientAddress << ",LSN=" << latestClientResponses[i].latestSequenceNumber << ",CSN=" << latestClientResponses[i].currentSequenceNumber << ",LResponse=" << latestClientResponses[i].latestReponseToClient << "} "; // no endl!
  }
  return os;
}