//
// This file is part of an OMNeT++/OMNEST simulation example.
//
// Copyright (C) 1992-2015 Andras Varga
//
// This file is distributed WITHOUT ANY WARRANTY. See the file
// `license' for details on this and other legal matters.
//


//
// A switch that accepts packets, and sends packets with destination
// address `a' to port out[a]. Incoming packets are queued up in a single
// queue and served `pkRate' packets per second. The queue has a finite
// size (overflow packets are dropped).
//
simple Switch
{
    parameters:
        double delay @unit(s) @prompt("Switch delay");
        @display("i=device/switch_l;q=queue");
    gates:
        inout port[];
}


