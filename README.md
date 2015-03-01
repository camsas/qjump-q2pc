q2pc
====

A 2-phase commit system that runs over Q-Jump (or not). Can be configured to run over TCP, UPD+retires, UDP without retries or UDP+Broadcast (assuming QJUmp).

Building
========

Requirements
------------
- cake build system
- libchaste C library
- clang or gcc

Building q2pc requires the Cake build system, available from 
https://github.com/Zomojo/Cake.git

Follow the instructions in INSTALL to configure cake

q2pc also requires the libchaste C library, which is available from
https://github.com/mgrosvenor/libchaste


Building
--------

To build q2pc, make sure that the symbolic link to libchaste (in the deps directory) is pointing to the right place and run ./build.sh from the root directory. 

To build a release version run "./build.sh --variant=release"

Running
=======

q2pc supports the following options

|Mode     | Type          | Short|Long Option    | Description                                                                  |
|---------|---------------|------|---------------|------------------------------------------------------------------------------|
|Optional | Integer |-s  |--server        |  Put q2pc in server mode, specify the number of clients [0]  |
|Optional | Integer |-T  |--threads       |  The number of threads to use [1]  |
|Optional | String  |-c  |--client        |  Put q2pc in client mode, specify server address in x.x.x.x format [(null)]  |
|Optional | Integer |-C  |--id            |  The client ID to use for this client (must be >0) [-1]  |
|Flag     | Boolean |-u  |--udp-ln        |  Use Linux based UDP transport [default]   |
|Flag     | Boolean |-t  |--tcp-ln        |  Use Linux based TCP transport   |
|Flag     | Boolean |-r  |--rdp-ln        |  Use Linux based UDP transport with reliability   |
|Flag     | Boolean |-q  |--udp-qj        |  Use broadcast based UDP transport over Q-Jump   |
|Optional | Integer |-p  |--port          |  Port to use for all transports [7331]  |
|Optional | String  |-B  |--broadcast     |  The broadcast IP address to use in UDP mode ini x.x.x.x format [127.0.0.0]  |
|Optional | String  |-i  |--iface         |  The interface name to use [eth4]  |
|Optional | Integer |-m  |--message-size  |  Size of the messages to use [128]  |
|Flag     | Boolean |-n  |--no-colour     |  Turn off colour log output   |
|Flag     | Boolean |-0  |--log-stdout    |  Log to standard out   |
|Flag     | Boolean |-1  |--log-stderr    |  Log to standard error [default]   |
|Optional | String  |-F  |--log-file      |  Log to the file supplied [(null)]   |
|Optional | Integer |-v  |--log-level     |  Log level verbosity (0 = lowest, 6 = highest) [3]  |
|Optional | Integer |-w  |--wait          |  How long to wait for client/server delay (us) [2000000]  |
|Optional | Integer |-o  |--rto           |  How long to wait before retransmitting a request (us) [200000]  |
|Optional | Integer |-R  |--report-int    |  reporting interval for statistics [100]  |
|Optional | Integer |-S  |--stats-len     |  length of stats to keep [1000]  |
|Flag     | Boolean |-h  |--help          |  Print this help message   |




