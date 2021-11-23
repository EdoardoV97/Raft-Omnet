struct log_entry
{
    char *var; // only 1 var
    int value;
    int term;
    int logIndex;
    //TODO Aggiungere vector di ID per la configurazione
};

//enum serverState
//{
//    LEADER = 0;
//    FOLLOWER = 1;
//    CANDIDATE = 2;
//}