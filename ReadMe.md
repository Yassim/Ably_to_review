# Ably Code challange.

## Building.
`build.sh` and `build.bat` have been supplied to try and simplify building.

On linux, g++ 8 or higher. (tested with 9.3 (on Ubuntu 20.04 TLS on WSL))
```
gcc -std=c++17 Ably.cc -lstdc++ -lpthread -o Ably
```

On Windows, a .vcproj is supplied.
Tested with `Microsoft Visual Studio Community 2017` with `Microsoft Visual C++ 2017`

## Running
The command line format is as follows

`> Ably (server|client) [-uuid string] [-n 1..65525] [-port 1..65525] [-v][-flaky_connection 1..large] [-flaky_data 1..large]`

`client` or `server` tells the application which mode to run in.

### Common Args
* `-port $number` indicates the port the service is to run on, or connect to. default value is 9000.
* `-v` adds trace level logging to the output.
* `-flaky_(connection|data) $number` is used for fault injection and is described in [Fault injection](#Fault-injection).

### Server Args
The server side only respects the Common Args.

`> Ably server`

`> Ably server -port 9010`

### Client
The Client side respects the Common Args in addition to `-uuid` and `-n`.
* `-uuid` is the unique identifier the server is to know this connection by. default is a randomly generated uuid of 40 characters.
* `-n` how many ints are requested. default is a number between 1 and 65535.

Clients will only connect to `localhost`.

`> Ably client`

`> Ably client -uuid test`

`> Ably client -n 5`

`> Ably client -uuid test -n 15`


## Protocol design
This implementation relies on the fact that both ends are on the same architecture.
Its using type-punning (no real serialization), not even any integer network <-> host translation.

Here are the packet descriptions.
```cpp
namespace Protocol {
struct LoginRequest
{
    char uuid[40];
    uint32_t N;

    // non 0 for restart
    uint32_t packets_seen;
};

struct LoginConfirmed
{
    // non 0 for restart
    uint32_t sending_from;

    // same a N in LoginRequest
    uint32_t sending_total;
};

struct DataPacket
{
    uint32_t payload;
};

struct DataComplete
{
    uint32_t checksum;
};
}; // namespace Protocall
```
### Happy path
```
client --> LoginRequest( uuid: test, N: 10, packets_seen: 0)   --> server
client <-- LoginConfirmed( sending_from: 0, sending_total: 10) <-- server
client <-- 1 uint          <-- server (limited to 1 per second, 10 packets sent)
client <-- checksum        <-- server
         connections closed
```

### Connection Restart
```
client --> LoginRequest( uuid: test, N: 10, packets_seen: 0)   --> server
client <-- LoginConfirmed( sending_from: 0, sending_total: 10) <-- server
client <-- 1 uint          <-- server (limited to 1 per second, < 10 packets sent)
         Example: client drops connection after 5 packets

client waits 3 seconds.

client --> LoginRequest( uuid: test, N: 10, packets_seen: 5)   --> server
client <-- LoginConfirmed( sending_from: 5, sending_total: 10) <-- server
client <-- 1 uint          <-- server (limited to 1 per second, 5 packets sent)
client <-- checksum        <-- server
         connections closed
```

## Implementation notes
Simplicity of delivery, others being able to build and test, was a major factor.
There is no dependencies other than the standard lib.
This is for a number of reasons.
* I was unable to find a simple raw tcp lib. (most do far more than required and)
* Even finding a small logging lib was problematic.
* Adding Protobuff or such without a build system to manage it is difficult as well as it being a large lib.

The server stores its session state in 2 locations. 1 being shared, the other local to the process and the active sockets and worker threads.
The Server::SharedState is intended to be a api that could be implemented using a database, or some sort of multi-node cache.

```cpp
namespace Server {
class SharedState
{
    struct ConnectionState;// see imp for details

    void RegisterNewTransmission(const string& id,
                                const vector<uint32_t>& payload);
    ConnectionState GetTransmission(const string& id);
    void SetTransmissionLastSent(const string& id, uint32_t last_sent);
    void EraseTransmission(const string& id);
    void RemoveExpiredSessions();
};
```

For simplicity, each new connection creates a new thread to handle the transmission. It uses a thread sleep between data packets for the rate limiting factor.

For robustness, sessions on the server side are always allowed to expire instead of being removed on the data has been sent. In local testing I saw that it was possible for the server to send 1-2 packets before realising that the client was gone. If this was as it was sending the last number or the checksum, then the client would expect to reconnect, but the server had nothing to resume. letting it expire naturally, leads to the client being able to complete the transfer if it had previously dropped before it received the final information.

## Testing 
### Fault injection
Both client and server have 2 fault injection arguments.

`-flaky_connection N` where N is a maximum for an random number generator. The higher the number, the less likely the fault.
if the rng rolls a 1, then the connection is dropped, and the client will have to reconnect to resume the download.

Example
```
./Ably client -uuid test -n 15 -flaky_connection 5
[MSG] Simple Int stream server
[ERR] !!! FLAKY CONNECTION ACTIVE.. 1 in 5
[INF] connecting as "test" , packets requested  15
[INF] to process from 0 of a total 15
[ERR] !!! INJECTING FLAKY CONNECTION
[ERR] (test) Fault injecting connection fail
[INF] Connection failure, retry in:
[INF] 3
[INF] 2
[INF] 1
[INF] Attempting reconnect
[INF] to process from 7 of a total 15
[ERR] !!! INJECTING FLAKY CONNECTION
[ERR] (test) Fault injecting connection fail
[INF] Connection failure, retry in:
[INF] 3
[INF] 2
[INF] 1
[INF] Attempting reconnect
[INF] to process from 11 of a total 15
[ERR] !!! INJECTING FLAKY CONNECTION
[ERR] (test) Fault injecting connection fail
[INF] Connection failure, retry in:
[INF] 3
[INF] 2
[INF] 1
[INF] Attempting reconnect
[INF] to process from 13 of a total 15
[INF] local checksum 226435210 , remote checksum 226435210
[MSG] Result Success
```

`-flaky_data N` where N is a maximum for an random number generator. The higher the number, the less likely the fault.
if the rng rolls a 1, then payload number is altered by a random value, which forces the checksum to fail.

Example

```
> ./Ably client -uuid test -n 5 -flaky_data 15
[MSG] Simple Int stream server
[ERR] !!! FLAKY DATA ACTIVE.. 1 in 15
[INF] connecting as "test" , packets requested  5
[INF] to process from 0 of a total 5
[ERR] !!! INJECTING FLAKY DATA
[INF] local checksum 1805262643 , remote checksum 1805320145
[MSG] Result Corrupted

> ./Ably client -uuid test -n 5 -flaky_data 15
[MSG] Simple Int stream server
[ERR] !!! FLAKY DATA ACTIVE.. 1 in 15
[INF] connecting as "test" , packets requested  5
[INF] to process from 0 of a total 5
[INF] local checksum 1805320145 , remote checksum 1805320145
[MSG] Result Success
```
