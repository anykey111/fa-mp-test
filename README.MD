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

## Import into sqlite3

    # load .cvs into the temporary table named `fa`, then select first 100 DAT packets where port is equal to 6001, then sort by `ser` field
    sqlite3 -cmd "create table fa(ts, src int, dst int, type int, ser int, irt int, seq int, expected int, data blob)" -cmd ".mode tab" -cmd ".import log.csv fa"
    >select * from fa where port = 6001 and type = 'DAT' order by ser limit 100;

