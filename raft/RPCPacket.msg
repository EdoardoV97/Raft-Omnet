//
// This file is part of an OMNeT++/OMNEST simulation example.
//
// Copyright (C) 1992-2015 Andras Varga
//
// This file is distributed WITHOUT ANY WARRANTY. See the file
// `license' for details on this and other legal matters.
//


enum RPCPacketType
{
    RPC_APPEND_ENTRIES = 0;
    RPC_REQUEST_VOTE = 1;
    RPC_INSTALL_SNAPSHOT = 2;
};

packet RPCPacket
{
    //TODO Insert fields
    int srcAddress;
    int destAddress;
    bool isBroadcast;
};

packet RPCAppendEntriesPacket extends RPCPacket
{
    //TODO Insert fields
    string payload;
};

packet RPCRequestVotePacket extends RPCPacket
{
    //TODO Insert fields
    string payload;
};

packet RPCInstallSnapshotPacket extends RPCPacket
{
    //TODO Insert fields
    string payload;
};

