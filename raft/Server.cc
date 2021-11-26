#include "RPCPacket_m.h"
#include <algorithm>
//#include <vector>

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
    serverState status = FOLLOWER;
    
    cMessage *sendHearthbeat;
    cMessage *electionTimeoutEvent;
    vector<append_entry_timer> appendEntryTimers;
    
    int myAddress;   // This is the server ID
    int receiverAddress; // This is receiver server ID
    
    int votes = 0; // This is the number of received votes (meaninful when status = candidate)
    int leaderAddress; // This is the leader ID

    int clientAddress; // This is the client's address

    // Pointers to handle RPC mexs
    RPCAppendEntriesPacket *appendEntriesRPC = nullptr;
    RPCAppendEntriesResponsePacket *appendEntriesResponseRPC = nullptr;
    RPCRequestVotePacket *requestVoteRPC = nullptr;
    RPCRequestVoteResponsePacket *requestVoteResponseRPC = nullptr;
    RPCInstallSnapshotPacket *installSnapshotRPC = nullptr;
    RPCClientCommandResponsePacket *clientCommandResponseRPC = nullptr;

    // State Machine of the server
    int x = 0;
    vector<int> configuration;

    // Persistent state --> Updated on stable storage before responding to RPCs
    int currentTerm = 0;
    int votedFor = -1;
    vector<log_entry> log;

    // Volatile state --> Reinitialize after crash
    int commitIndex = 0; // E se il primo log lo committiamo sempre noi??
    int lastApplied = 0;

    // Volatile state on leaders --> Reinitialized after election
    vector<int> nextIndex;
    vector<int> matchIndex;

    void updateConfiguration();

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
  sendHearthbeat = new cMessage("send-hearthbeat");
  electionTimeoutEvent = new cMessage("election-timeout-event");
  myAddress = gate("port$o")->getNextGate()->getIndex(); // Return index of this server gate port in the Switch
  
  // TODO: Initialize the client address
  // TODO: Initialize the admin address
  updateConfiguration();
  //Pushing the initial configuration in the log
  log_entry firstEntry;
  firstEntry.var = 'x';
  firstEntry.value = x;
  firstEntry.term = currentTerm;
  firstEntry.logIndex = 0;
  firstEntry.configuration.assign(configuration.begin(), configuration.end());
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

    appendEntriesRPC = new RPCAppendEntriesPacket("RPC_APPEND_ENTRIES", RPC_APPEND_ENTRIES);
    appendEntriesRPC->setTerm(currentTerm);
    appendEntriesRPC->setLeaderId(myAddress);
    appendEntriesRPC->setEntry(emptyEntry);
    appendEntriesRPC->setLeaderCommit(commitIndex);
    appendEntriesRPC->setSrcAddress(myAddress);
    appendEntriesRPC->setIsBroadcast(true);
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
      newEntry.configuration.assign(appendEntryTimers[i].entry.configuration.begin(), appendEntryTimers[i].entry.configuration.end());
      //Resend the message
      appendEntriesRPC = new RPCAppendEntriesPacket("RPC_APPEND_ENTRIES", RPC_APPEND_ENTRIES);
      appendEntriesRPC->setTerm(currentTerm); //giusto?
      appendEntriesRPC->setLeaderId(myAddress);
      appendEntriesRPC->setPrevLogIndex(appendEntryTimers[i].prevLogIndex); 
      appendEntriesRPC->setPrevLogTerm(appendEntryTimers[i].prevLogTerm);
      appendEntriesRPC->setEntry(newEntry);
      appendEntriesRPC->setLeaderCommit(commitIndex);
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
      if((pk->getTerm() < currentTerm) || log[pk->getPrevLogIndex()].term != pk->getPrevLogTerm()){ //TODO attento a null pointer exception
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
        else{ //I am not a candidate, remain follower
          cancelEvent(electionTimeoutEvent);
          leaderAddress = pk->getLeaderId();

          if(pk->getLeaderCommit() > commitIndex){
            if(pk->getLeaderCommit() < pk->getEntry().logIndex){
              commitIndex = pk->getLeaderCommit();
            } else{
              commitIndex = pk->getEntry().logIndex;
              }
          }
          scheduleAt(simTime() +  uniform(SimTime(par("lowElectionTimeout")), SimTime(par("highElectionTimeout"))), electionTimeoutEvent);
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
      if((votedFor == -1 || votedFor == pk->getCandidateId()) && (log.back().term <= pk->getLastLogTerm() || (log.back().term == pk->getLastLogTerm() && log.back().logIndex <= pk->getLastLogIndex()))){
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

      if(pk->getSuccess() == false){
        int position = getIndex(configuration, pk->getSrcAddress());
        nextIndex[position]--;
        log_entry newEntry;
        newEntry = log[nextIndex[position]];  
        newEntry.configuration.assign(log[nextIndex[position]].configuration.begin(), log[nextIndex[position]].configuration.end());
        appendNewEntryTo(newEntry, receiverAddress, position);
      }
      else{
        int position = getIndex(configuration, pk->getSrcAddress());
        matchIndex[position] = nextIndex[position];
        nextIndex[position]++; 
        if(nextIndex[position] <= log.size()){
          log_entry newEntry;
          newEntry = log[nextIndex[position]];
          newEntry.configuration.assign(log[nextIndex[position]].configuration.begin(), log[nextIndex[position]].configuration.end());
          appendNewEntryTo(newEntry, receiverAddress, position);
        }

        for (int newCommitIndex = commitIndex + 1; newCommitIndex < log.size(); newCommitIndex++){
          if(majority(newCommitIndex) == true && log[newCommitIndex].term == currentTerm){
            commitIndex = newCommitIndex;
          }
        }
        if(commitIndex > lastApplied){
          lastApplied++;
          applyCommand(log[lastApplied]);
          //Send response to the client
          clientCommandResponseRPC = new RPCClientCommandResponsePacket("RPC_CLIENT_COMMAND_RESPONSE", RPC_CLIENT_COMMAND_RESPONSE);
          requestVoteResponseRPC->setSrcAddress(myAddress);
          requestVoteResponseRPC->setDestAddress(clientAddress);
          send(clientCommandResponseRPC, "port$o");
        }
      }

    }
    //else nothing?
  }
  break;
  case RPC_REQUEST_VOTE_RESPONSE:
  {
    RPCRequestVoteResponsePacket *pk = check_and_cast<RPCRequestVoteResponsePacket *>(pkGeneric);
    //If i am still candidate
    if(status == CANDIDATE){
      if (pk->getVoteGranted() == true){
        votes++;
      }
      // If majority, become leader sending heartbeat to all other.
      if(votes > configuration.size()/2){
        becomeLeader();
      }
    }
  }
  break;
  case RPC_CLIENT_COMMAND:
  {
    RPCClientCommandPacket *pk = check_and_cast<RPCClientCommandPacket *>(pkGeneric);
    if(status == LEADER){ //Process incoming command from a client
      log_entry newEntry;
      newEntry.var = pk->getVar();
      newEntry.value = pk->getValue();
      newEntry.term = currentTerm;
      newEntry.logIndex = log.size();
      log.push_back(newEntry);
      
      cancelEvent(sendHearthbeat);
      appendNewEntry(newEntry);
      scheduleAt(simTime() + par("hearthBeatTime"), sendHearthbeat);
    }
    else{ //Forward to leader
      pk->setDestAddress(leaderAddress);
      send(pk, "port$o");
    }
  }
  break;
  default:
    break;
  }

  delete pkGeneric;
    
}

// Fare un metodo per checkare server facenti parte della configurazione corrente(= connessi allo switch)

void Server::appendNewEntry(log_entry newEntry){
  
  appendEntriesRPC = new RPCAppendEntriesPacket("RPC_APPEND_ENTRIES", RPC_APPEND_ENTRIES);
  appendEntriesRPC->setTerm(currentTerm);
  appendEntriesRPC->setLeaderId(myAddress);
  appendEntriesRPC->setPrevLogIndex(log.back().logIndex);
  appendEntriesRPC->setPrevLogTerm(log.back().term); 
  appendEntriesRPC->setEntry(newEntry);
  appendEntriesRPC->setLeaderCommit(commitIndex);
  appendEntriesRPC->setSrcAddress(myAddress);
  appendEntriesRPC->setIsBroadcast(false);
  
  //Send to all followers in the configuration
  for (int i = 0; i < configuration.size(); i++){
    append_entry_timer newTimer;
    newTimer.destination = configuration[i];
    newTimer.prevLogIndex = log.back().logIndex;
    newTimer.prevLogTerm = log.back().term;
    newTimer.timeoutEvent = new cMessage("append-entry-timeout-event");
    newTimer.entry = newEntry; // Should be sufficient
    // newTimer.entry.var = newEntry.var;
    // newTimer.entry.value = newEntry.value;
    // newTimer.entry.term = newEntry.term;
    // newTimer.entry.logIndex = newEntry.logIndex;
    newTimer.entry.configuration.assign(newEntry.configuration.begin(), newEntry.configuration.end()); // copy by value ("deep copy")
    
    appendEntriesRPC->setDestAddress(configuration[i]);
    send(appendEntriesRPC, "port$o");
    
    appendEntryTimers.push_back(newTimer);
    // Reset the timer to wait before retry sending (indefinitely) the append entries for a particular follower
    scheduleAt(simTime() + par("resendTimeout"), newTimer.timeoutEvent);
  }
  return;
}

void Server::appendNewEntryTo(log_entry newEntry, int destAddress, int index){
  
  appendEntriesRPC = new RPCAppendEntriesPacket("RPC_APPEND_ENTRIES", RPC_APPEND_ENTRIES);
  appendEntriesRPC->setTerm(currentTerm);
  appendEntriesRPC->setLeaderId(myAddress);
  appendEntriesRPC->setPrevLogIndex(log[nextIndex[index]-1].logIndex); // -1 because the previous
  appendEntriesRPC->setPrevLogTerm(log[nextIndex[index]-1].term);
  appendEntriesRPC->setEntry(newEntry);
  appendEntriesRPC->setLeaderCommit(commitIndex);
  appendEntriesRPC->setSrcAddress(myAddress);
  appendEntriesRPC->setIsBroadcast(false);

  append_entry_timer newTimer;
  newTimer.destination = destAddress;
  newTimer.timeoutEvent = new cMessage("append-entry-timeout-event");
  newTimer.entry = newEntry;
  newTimer.entry.configuration.assign(newEntry.configuration.begin(), newEntry.configuration.end()); // copy by value ("deep copy")
    
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
  int total = configuration.size();
  int counter = 0;
  for (int i = 0; i < matchIndex.size(); i++){
    if(N >= matchIndex[i]){
      counter++;
    }
  }
  return counter > (total/2);
}

void Server::applyCommand(log_entry entry){
  switch (entry.var)
  {
  case 'x':
    x = entry.value;
    break;
  
  default:
    break;
  }
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
  votes = 1;
  scheduleAt(simTime() +  uniform(SimTime(par("lowElectionTimeout")), SimTime(par("highElectionTimeout"))), electionTimeoutEvent);
}

void Server::becomeLeader(){
  cancelEvent(electionTimeoutEvent);
  status = LEADER;
  leaderAddress = myAddress;
  votes = 0; 
  votedFor = -1;
  nextIndex.clear();
  matchIndex.clear();
  nextIndex.resize(configuration.size(), log.back().logIndex + 1);
  matchIndex.resize(configuration.size(), 0);
  scheduleAt(simTime(), sendHearthbeat); // Trigger istantaneously the sendHeartbeat
}

void Server::becomeFollower(RPCPacket *pkGeneric){
  cancelEvent(electionTimeoutEvent);
  status = FOLLOWER;
  if(pkGeneric->getKind() == RPC_APPEND_ENTRIES){
    RPCAppendEntriesPacket *pk = check_and_cast<RPCAppendEntriesPacket *>(pkGeneric);
    leaderAddress = pk->getLeaderId();
  }
  votes = 0;
  votedFor = -1;
  nextIndex.clear();
  matchIndex.clear();
  scheduleAt(simTime() +  uniform(SimTime(par("lowElectionTimeout")), SimTime(par("highElectionTimeout"))), electionTimeoutEvent);
}

void Server::updateConfiguration(){
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