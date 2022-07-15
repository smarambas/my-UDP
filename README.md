# my-UDP

This was my attempt at creating a client-server application for file transfer, using a TCP like protocol build on top of the UDP protocol, using the C POSIX library.
To ensure a reliable transmission I used the Selective Repeat protocol.
The loss of packets is simulated by setting the packet loss probability.
The retransmission timeouts can be fixed or dynamic (depending on network simulated delays).
Both the client and the server are implemented as multithreaded processes, where the various threads manage the sending and receiving of packets.
To manage concurrency I userd POSIX mutexes.

The protocol works but the performances degrade as the number of threads increases, probably because I made some mistakes in managing the mutexes.
