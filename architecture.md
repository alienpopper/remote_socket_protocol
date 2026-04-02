Often during debugging, pentesting, and hardware testing, it is necessary to hook up subsystems on different subnets.  This had traditionally been done with SSH tunnels, VPNs, necat, and other ad-hoc tools.  Those tools often break or misbehave during testing and then valuable work time is replaced with debugging those tools.  This project aims to replace those tools with a more stable infrastructure.

The overall architecture has several pieces:  
- The route and monitor services (RM)
- The client services (CS)
- The resource services (RS)
- The endorsement services (ES)
- The name services (NS) 
Generically all participants are referred to as Nodes.

The Client Services
------------------------
The client services are what interact with external software.  A client is the software that has the intent.  High level description of some typical CS tasks are "Listen on a port on the internet", "Connect to an ip/port on the Internet", "Show me the list of connected clients", "tell me if this server is available".

The Route and Monitor Services
------------------------
The Route and Monitor services are the glue that hooks together the systems.  RM generally runs on the Internet or some other central place.  Clients from labs and development environments connect to it.   Servers from labs and development environments connect to it.  Other RM servers connect to each other.  When a client wants to use a resource (for example connect to an SSH server in a hardware lab), the client first establishes a connection to the RM.  The RM can then forward the appropriate messages to the appropriate provider of that service.

Main jobs of the RM:
-handle queries about the health and availability.  
-authenticate that clients are allowed to use the service
-provide detailed logs about client usage
-provide detailed logs about service usage
-trace and debug connections
-forcibly disconnect clients and resource
-rate limit and throttle 

The Resource Services
------------------------
The RSs provide access to the clients.  An RS may be willing to proxy connections into a lab network.  It may make SSH available from a lab machine to clients.  It may act as a general proxy so a client in a restricted environment may access resources on the network.

The Endorsement Services
------------------------
The endorsement services are used to assign properties and authentications to nodes.  For example, to access the ssh servers in lab12, you may need an endorsement from the lab12 ES. Any type of information can be bound via an ES.  For example, it's perfectly valid to assign an endorsement for if someone has performed the lab training.

The name services
-----------------------
The name services are used to lookup nodes.  This is an important part of failover and redundancy.  Often clients will connect to an RM and then try to interact with a RS by name instead of nodeID.  

A common operation during patching and maintenance is to change the value in a NS redirecting all CS to the new server before shutting down the old server.

Names are cryptographically bound so there should never be a name collision between two name services.


The Security Model
------------------------
This protocol is provides authentication and integrity for the protocol itself.  It does not provide confidentiality for anything.  It does not provide any guarantees for the protocols running over the remote socket protocol.  Protocols should view it as an insecure transport mechanism similar to transmitting over the open Internet.  Most modern protocols provide authentication and confidentiality.  For those that don't they have already accepted the risk and running over this infrastructure doesn't change that risk.  The security mechanisms in RSP are there to protect the RSP infrastructure and not the data flowing over it.

The security model is distributed.  The RM provides authN/authZ that gates nodes attaching to the network.  This is to stop random outsiders from probing the services and to stop denial-of-service type situations.  The individual services provide authN/authZ for clients accessing those services.

All messages transiting the network are authenticated and integrity protected, but transmitted in cleartext.  All nodes should always know who they are doing actions on behalf of on a message-by-message basis. 

The network provides security on a message-by-message basis. There is no concept of a stream with security properties applied to the stream.  All nodes are are allowed to reboot/reconnect/reconfigure in real time and as long as this doesn't impact the tunneled protocol the session should continue.  A client is allowed to connected to one RM, start a session, and then switch that session to another RM or a different transport protocol.  The per-message protections need to be strong enough to make this OK.


Message Security
------------------------
On the message layer, there are three possible security models:
1) The message is directly signed by the CS/RS
2) The message has a MIC using a symmetric key that the CS/RS have negotiated beforehand.
3) Data is streamed over connected sockets and has no integrity protections

Directly signed messages are used during negotiation or for short lived sessions.

The bulk of the traffic with use a MIC.  The CS/RS start by negotiating symetric keys using directly signed messages and then transition.  It is always legal for a CS/RS to directly sign a message even if a MIC key has been negotiated.

Data streaming is only used when the test requires minimizing the overhead of the transport network.  For example fragmentation/reassembly may be an issue.  Timing of arrival changes may need to be minimized.  This mode should only be used when a man-in-the-middle attack is a acceptable risk.

There is the problem of flooding the network with MIC "signed" packets.  For this reason, when the RM establishes a new session with a node, it verifies the nodeID explicitly.  It also verifies the nodeID again any time a MIC failure is reported by another node back to the session.

Message Format
------------------------
Different environments prefer different messages structures.  Firmware generally prefers C-Style structure stacking.  Web generally prefers JSON.  Other environments may prefer XML.  Often the correct tool isn't used because it's a pain to implement a message stack in that language.  It's unlikely that a firmware test environment will implement a JSON parser just to interact with an outside service.

The remote_socket_protocol tries to abstract away message encoding so that the test harness can use whatever encoding is easiest.  Messages are defined in an abstract way that can be encoded by almost any data encoder.

When the CS/RS establishes a new session with an RM, the session starts out in a simple ASCII text protocol.  When both sides agree on an encoding, the session transitions to that encoding.  

Internally the RM and all RM<->RM communications is based on Google Protocol Buffers (protobufs). And RM<->RM links do no negotiate an encoding. The RM is responsible for translating the edge encoding to/from a protobuf right before it is sent to an RS/CS.  Internally, all message transit the RM nodes as protobufs.  The CS/RS are not aware of the encoding chosen by the opposite end of the connection.  Encoding used is not a discoverable property.

ASCII handshake protocol
------------------------

Normally an RM listens on a TCP port.  (UDP or other protocols may be supported in the future).  When an RS/CS establishes a connection, the server is the first to transmit.

```
RSP version 0.1.21-alpha2\r\n
encodings:xml, json, protobuf, C\r\n
asymmetric: RSA, EC256\r\n
MIC: HMAC-SHA256-AES256\r\n
\r\n
```

Like HTTP each line is terminated with a \r\n and the end of the server messages ends with a \r\n\r\n.

After the server advertisement, the client responds with a similar encoding message.

```
encoding: protobuf\r\n
\r\n
```

The only required part of the client response is the encoding.  It is acceptable for the client to not parse the server advertisement and blindly transmit the encoding.  This allows devices with limited resources to connect use the service.

The server then finishes the handshake with the server success message.

```
0error: some_long_error_message\r\n
\r\n
```

or

```
1sucess: some_long_success_message\r\n
\r\n
```

It is acceptable for low resource client to only parser the first byte for success/failure and then throw away all the bytes until the \r\n\r\n.

The session then immediate transitions to the chosen encoding.  The message framing is unique to each encoding and there is no universal way to determine the start/end/length of a message across the encodings.  The client is required to speak the specific encoding.

On failure, the server immediately closes the socket and terminates the client session after sending the error.  The client is required to reconnect to try again.

Identities and Endorsements
------------------------
Each node is identified with a public/private keypair (node identity or NKP).  When referring to a node, a compact hash of the public key is used (nodeID).  NodeIDs are a primary unit that are used for routing, debugging, and most other tasks that aren't directly involved in authN/authZ.

Having each node maintain a full list of all authorized participants can be problematic.  Identities are often transmitted along with an Endorsement.  An endorsement cryptographically binds a property to an identity.  For example an endorsement could be used to prove that a node has been approved by a central authority or it could be used to assign a billing group.  

Endorsements contain the nodeID of the ES, the nodeID of the CS, a value, and an expiration time. 


Logging and Debugging
-----------------------
One of the primary features of the remote_socket_protocol is it is supposed to make logging and debugging easy.  This is accomplished by several mechanisms:

- Tracing header attached to a message
- Logging and debugging messages sent to the RM and collected by anyone interested
- Active interrogation of the various nodes and services
- Counters embedded in  the various nodes

A tracing header is a way to request detailed logging of the message handling during processing.  To request header tracing, 







