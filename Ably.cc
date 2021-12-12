#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <list>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Ws2tcpip.h>
#include <winsock2.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
const int INVALID_SOCKET = 0;
const int SOCKET_ERROR   = -1;

void closesocket(SOCKET s)
{
    close(s);
}
#include <signal.h>
#endif

#include <cassert>

using namespace std;
#include "common.h"
#include "log.h"
#include "tcp_util.h"

namespace Protocal {
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

int g_port_number;
}; // namespace Protocal

namespace FaultInjection {
int g_flaky_connection = 0;
int g_flaky_data       = 0;

bool FlakyConnection()
{
    if (g_flaky_connection) {
        static random_device rng;
        static uniform_int_distribution<int> dist(1, g_flaky_connection);
        if (dist(rng) == 1) {
            LogError("!!! INJECTING FLAKY CONNECTION");
            return true;
        }
    }
    return false;
}

uint32_t FlakyData()
{
    if (g_flaky_data) {
        static random_device rng;
        static uniform_int_distribution<int> dist(1, g_flaky_data);
        if (dist(rng) == 1) {
            LogError("!!! INJECTING FLAKY DATA");
            return dist(rng);
        }
    }
    return 0;
}
}; // namespace FaultInjection

namespace Server {
using Time = std::chrono::time_point<std::chrono::system_clock>;
using namespace std::chrono_literals; // give me the s suffix for numbers. so 5s
                                      // = 5 seconds

class SharedState
{
  public:
    struct ConnectionState
    {
        vector<uint32_t> payload;
        Time last_seen;
        uint32_t last_sent;

        ~ConnectionState() {}
        ConnectionState()
          : payload{}
          , last_sent{ 0 } {};
        ConnectionState(const vector<uint32_t>& payload)
          : payload(payload)
          , last_sent{ 0 }
        {}

        template<class Archive>
        void serialize(Archive& archive)
        {
            archive(last_sent, payload);
        }
    };

    void RegisterNewTransmission(const string& id,
                                 const vector<uint32_t>& payload)
    {
        lock_guard<mutex> scope_guard(lock);

        client_id_2_state.emplace(id, payload);
    }

    ConnectionState GetTransmission(const string& id)
    {
        lock_guard<mutex> scope_guard(lock);

        auto i = client_id_2_state.find(id);
        if (i == end(client_id_2_state)) {
            // log state not found for this id.
            // returning an empty transmission.
            // This will then be treated as a never before seen connection and
            // the payload will be genorated.
            return {};
        }
        return i->second;
    }

    void SetTransmissionLastSent(const string& id, uint32_t last_sent)
    {
        lock_guard<mutex> scope_guard(lock);

        auto& i     = client_id_2_state.at(id);
        i.last_sent = last_sent;
        i.last_seen = chrono::system_clock::now();
    }

    void EraseTransmission(const string& id)
    {
        lock_guard<mutex> scope_guard(lock);

        client_id_2_state.erase(id);
    }

    void RemoveExpiredSessions()
    {
        auto expired = chrono::system_clock::now() - 30s;

        lock_guard<mutex> scope_guard(lock);

        for (auto i = begin(client_id_2_state); i != end(client_id_2_state);) {
            if (i->second.last_seen < expired) {
                LogInfo("(" + i->first + ")", "Session expried, removing");
                i = client_id_2_state.erase(i);
            } else {
                ++i;
            }
        }
    }

  private:
    unordered_map<string, ConnectionState> client_id_2_state;
    mutex lock;
};

struct LocalClientState
{
    string uuid;
    atomic<bool> done;
    TCPStream stream;
    thread process;

    LocalClientState(TCPStream s, SharedState* ss)
      : uuid{ "unkown" }
      , done{ false }
      , stream{ s }
      , process(&LocalClientState::ProcessTransmission, this, ss)
    {}

    ~LocalClientState() { process.join(); }

    void ProcessTransmission(SharedState* server_shared)
    {
        try {
            auto conn = TSerialToStream{ stream };

            // Step 1. Receive login.
            auto login = conn.RecvN<Protocal::LoginRequest>();
            uuid       = string(begin(login.uuid),
                          find(begin(login.uuid), end(login.uuid), '\0'));

            LogInfo("login for", uuid);
            LogInfo("(" + uuid + ")", "requested", login.packets_seen, "to",
                    login.N);

            // Step 2. Get the previous state, if any.
            auto to_transmit = server_shared->GetTransmission(uuid);
            if (to_transmit.payload.size() == 0) {
                // new transmission, or one that had time out and we've
                // forgotten.
                random_device random_src;
                to_transmit.payload.reserve(login.N);
                for (uint32_t i = 0; i < login.N; i++) {
                    to_transmit.payload.emplace_back(random_src());
                }
                to_transmit.last_sent = 0;

                server_shared->RegisterNewTransmission(uuid,
                                                       to_transmit.payload);
            } else {
                LogInfo("(" + uuid + ")", "resumed. Last sent ",
                        to_transmit.last_sent);
            }

            // Step 3. Calc where to start.
            // This is the min of were both ends thought they had gotten to.
            // SendN this to the client. Confirming their log in, and where we
            // are starting from.
            auto sending_from = min(to_transmit.last_sent, login.packets_seen);
            conn.SendN(Protocal::LoginConfirmed{
              sending_from,
              static_cast<uint32_t>(to_transmit.payload.size()) });

            // we test for N missmatch here, so we have told the client our
            // numbers they match in the client either, so both will terminate
            // this session
            if (to_transmit.payload.size() != login.N) {
                LogError("Request N Packet missmatch. server:",
                         to_transmit.payload.size(), "client:", login.N);
                conn.Close();
                done = true;
                return;
            }

            LogInfo("(" + uuid + ")", "will send", sending_from, "to", login.N);

            // Step 4. do the actual stream of data.
            auto time_acc = std::chrono::system_clock::now();
            for (auto pi = sending_from; pi < to_transmit.payload.size();
                 ++pi) {
                auto data_to_send = to_transmit.payload[pi];
                data_to_send += FaultInjection::FlakyData();
                conn.SendN(Protocal::DataPacket{ data_to_send });

                LogTrace("(" + uuid + ")", "sent packet", pi, "value",
                         to_transmit.payload[pi]);

                server_shared->SetTransmissionLastSent(uuid, pi);
                this_thread::sleep_until(time_acc += 1s);

                if (FaultInjection::FlakyConnection()) {
                    LogError("(" + uuid + ")",
                             "Fault injecting connection fail");
                    conn.Close();
                    done = true;
                    return;
                }
            }

            // Step 5. Send the checksum and close everthing down.
            auto checksum = Common::ComputeChecksum(to_transmit.payload);
            LogInfo("(" + uuid + ")", "Payload sent, sending check sum",
                    checksum);

            conn.SendN(Protocal::DataComplete{ checksum });
            conn.Close();

            LogInfo("(" + uuid + ")",
                    "Complete transmission, closed connection.");
        } catch (socket_close_exception e) {
            LogError("(" + uuid + ")", "Socket closed early");
        }
        // Either successful, or some socket error, this thread is done.
        done = true;
    }
};

int main(int , const char** )
{
    SharedState shared;

    LogInfo("Starting server");

    TCPStream conn(Protocal::g_port_number);

    LogInfo("Listening on", Protocal::g_port_number);

    list<LocalClientState> active_clients;

    int trace_counter = 0;
    for (;;) {
        if (!conn.WaitForDataToRecv(1s)) {
            LogTrace("Waiting on connection", trace_counter++);
            shared.RemoveExpiredSessions();

            active_clients.remove_if([](const auto& client) {
                if (client.done) {
                    LogTrace("removing client ", client.uuid);
                    return true;
                }
                return false;
            });

            continue;
        }

        {
            LogInfo("accepting new connection");
            active_clients.emplace_back(conn.Accept(), &shared);
            LogInfo("accepting new connection - done");
        }
    }

    conn.Close();

    return 0;
}
} // namespace Server

namespace Client {
enum class ReturnCode
{
    Success,
    CorruptedDownload,
    ConnectionFailure,
    BadUUID,
    BadRequest,
};

ReturnCode ProcessTransmission(INetStream* stream, const string& uuid, uint32_t N,
                               vector<uint32_t>& out_payload)
{
    try {
        auto conn = TSerialToStream{ *stream };

        // Setp 1. Log in.
        // send who we are, how many ints we want,
        // And if this is a reconnect, how many its we've seen.
        {
            Protocal::LoginRequest r{};
            if (uuid.size() > 40) {
                LogError(uuid.size(),
                         "is to many characters for the uuid. limit is 40");
                return ReturnCode::BadUUID;
            }
            copy(begin(uuid), end(uuid), r.uuid);
            r.N            = N;
            r.packets_seen = out_payload.size();

            conn.SendN(r);
        }

        // Step 2. Wait for loging confirmed.
        auto session = conn.RecvN<Protocal::LoginConfirmed>();
        if (session.sending_total != N) {
            LogError("Request N Packet missmatch. client:", N,
                     "server:", session.sending_total);
            return ReturnCode::BadRequest;
        }

        out_payload.reserve(session.sending_total);

        LogInfo("to process from", session.sending_from, "of a total",
                session.sending_total);

        // Step 3. Recv loop.
        for (auto pi = session.sending_from; pi < session.sending_total; ++pi) {
            auto p = conn.RecvN<Protocal::DataPacket>();
            p.payload += FaultInjection::FlakyData();
            if (pi < out_payload.size()) {
                out_payload[pi] = p.payload;
            } else {
                out_payload.push_back(p.payload);
            }
            LogTrace("packet", pi, ", value of ", p.payload);

            if (FaultInjection::FlakyConnection()) {
                LogError("(" + uuid + ")", "Fault injecting connection fail");
                return ReturnCode::ConnectionFailure;
            }
        }

        // Step 4. Recv the checksum.
        auto complete = conn.RecvN<Protocal::DataComplete>();

        // Step 5. Compute checksum.
        auto checksum = Common::ComputeChecksum(out_payload);
        LogInfo("local checksum", checksum, ", remote checksum",
                complete.checksum);
        return checksum == complete.checksum ? ReturnCode::Success
                                             : ReturnCode::CorruptedDownload;

    } catch (socket_close_exception e) {
        return ReturnCode::ConnectionFailure;
    }
}

int main(int argc, const char** argv)
{
    string uuid = "";
    if (auto arg = Common::GetArg("-uuid", argc, argv); arg) {
        uuid = arg;
    } else {
        uuid = Common::RandomUUID(40);
    }

    auto n = Common::GetIntArg("-n", argc, argv, 0);
    if (!n) {
        random_device rng;
        uniform_int_distribution<int> dist(1, 0xffff);
        n = dist(rng);
    }

    LogInfo("connecting as", quoted(uuid), ", packets requested ", n);

    auto result = ReturnCode::Success;
    vector<uint32_t> payload;

    do {
        if (result == ReturnCode::ConnectionFailure) {
            LogInfo("Connection failure, retry in:");
            for (int i = 3; i > 0; i--) {
                this_thread::sleep_for(1s);
                LogInfo(i);
            }
            LogInfo("Attempting reconnect");
        }
        TCPStream conn("localhost", Protocal::g_port_number);
        result = ProcessTransmission(&conn, uuid, n, payload);
        conn.Close();
    } while (result == ReturnCode::ConnectionFailure);

    LogMessage("Result",
               ((result == ReturnCode::Success) ? "Success" : "Corrupted"));

    return 0;
}
} // namespace Client

int main(int argc, const char** argv)
{
    Log(LogLevel::Message, "Simple Int stream server");
    Log(LogLevel::Trace, vector<string>{ argv, argv + argc });
#ifdef _WIN32
    {
        // start up win sock 2.2 lib
        WSADATA wsa_data;
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
            LogError("Windows Sockets failed to start");
            return 1;
        }
    }
#else
    {
        // ignore sig pipe.
        // if the client goes away and this is not ignored, then the server
        // dissapears (crash, but no segfault).
        // I deal with send to a broken pipe by reassigning an exception and
        // closing as required, so I dont need this
        struct sigaction tmp
        {
            SIG_IGN
        };
        sigaction(SIGPIPE, &tmp, NULL);
    }
#endif

    // Common params
    Protocal::g_port_number = Common::GetIntArg("-port", argc, argv, 9000);
    g_log_level =
      Common::HasArg("-v", argc, argv) ? LogLevel::Trace : LogLevel::Info;

    FaultInjection::g_flaky_connection =
      Common::GetIntArg("-flaky_connection", argc, argv, 0);
    if (FaultInjection::g_flaky_connection) {
        LogError("!!! FLAKY CONNECTION ACTIVE.. 1 in", FaultInjection::g_flaky_connection);
    }
    FaultInjection::g_flaky_data =
      Common::GetIntArg("-flaky_data", argc, argv, 0);
    if (FaultInjection::g_flaky_data) {
        LogError("!!! FLAKY DATA ACTIVE.. 1 in", FaultInjection::g_flaky_data);
    }

    if (0 == strcmp("client", argv[1])) {
        Client::main(argc - 1, argv + 1);
    }
    if (0 == strcmp("server", argv[1])) {
        Server::main(argc - 1, argv + 1);
    }

#ifdef _WIN32
    WSACleanup();
#endif // _WIN32
    return 0;
}
