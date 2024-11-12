# Overview

This is an expimental GPGNet proxy server.
The server work only for localhost games, user can run up to 3 game instances on the same PC.

# Usage

    # build 
    make all

    # run the server, dump all UDP packets to log.csv file
    gpgnet-mock --record log.csv

    # fa-test.cmd script run 3 game instatnaces
    #  [1] player1 (host) lobby_port = 6001, proxy_port = 7001
    #  [2] player2 lobby_port = 6002, proxy_port = 7002
    #  [3] player3 lobby_port = 6003, proxy_port = 7003
    # Ensure that WORKDIR is set to the actual FAF installation directory (c:\ProgramData\FAForever\bin\ by default)
    fa-test.cmd

## Usefull tweaks "lua.nx2/userInit.lua"

remove ConExecute function hook

    _G.ConExecute = function(command)
        ...
    end

add lines
    
    ConExecute('net_DebugLevel 3') -- enable debug log
    ConExecute('net_CompressionMethod 0') -- disable compression

# .csv log

The log file using CSV format that also compatible with sqlite3

## Fields

- `ts` - time in milliseconds since the game start
- `src` - source port
- `dst` - destination port
- `type` - packet type
- `ser` - packet serial number
- `irt` - in reply to `ser`
- `seq` - data sequence number
- `expected` - expected sequence number
- `data` - packet payload (hex blob)

### packet type
    
    CON connection request
    ANS anwser to CON
    DAT data packet
    ACK acknowledgement
    KPA keep alive packet
    GBY goodbye
    UNK unknown packet

### packet data

Data field contains byte stream, that later broken down into individual messages

    struct MPMsg {
        u8  type; // message type
        u16 len;  // total message length, including the `type`
        u8  data[len - 1]; // message data
    }

By default message stream compressed using zlib.

    // ForgedAlliance.exe zlib parameters 
    deflateInit2(ctx, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -14 /*windowBits */, 8 /* memLevel */, Z_DEFAULT_STRATEGY)

    # message stream decompression, python example
    import zlib
    z = zlib.decompressobj(wbits=-14)
    z.decompress(bytearray.fromhex("4ae16728c849ac4c2d32626062606000000000ffff"))

Messages that games send even when all players are idle, probably related to simtick.

    // 32 0800 00 6b000000 (type=0x32 len=8 flag=0 simtick=107)
    // 00 7000 1000000 (type=0x00 len=7 value=1)
    // 34 0700 6b000000 (type=0x34 len=7 simtick=107)
    // 33 0700 6b000000 (type=0x33 len=7 simtick=107)

## Import into sqlite3

    # load .cvs into the temporary table named `fa`, then select first 100 DAT packets where port is equal to 6001, then sort by `ser` field
    sqlite3 -cmd "create table fa(ts, src int, dst int, type int, ser int, irt int, seq int, expected int, data blob)" -cmd ".mode tab" -cmd ".import log.csv fa"
    >select * from fa where src = 6001 and type = 'DAT' order by ser limit 100;

    # dump exchange between player1 and player2
    >.output p1p2.csv
    >select * from fa where src = 6001 and dst = 6002 or src = 6002 and dst == 6001;

