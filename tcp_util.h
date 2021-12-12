#pragma once

struct socket_close_exception : public runtime_error
{
    socket_close_exception()
      : runtime_error("Socket closed")
    {}
    ~socket_close_exception() {}
};

struct INetStream
{
    virtual void SendN(size_t n, const void* data)         = 0;
    virtual void RecvN(size_t n, void* dst)                = 0;
    virtual int WaitForDataToRecv(chrono::seconds timeout) = 0;
    virtual void Close()                                   = 0;
};

class TCPStream : public INetStream
{
    SOCKET handle;

    TCPStream() {}

  public:
    TCPStream(const char* host, int port)
    {
        auto portstr = to_string(port);
        struct addrinfo hints
        {};
        struct addrinfo *r, *res = nullptr;

        if (getaddrinfo(host, portstr.c_str(), &hints, &res) != 0) {
            throw std::runtime_error("Cannot resolve hostname");
        }
        for (r = res; r; r = r->ai_next) {
            handle = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
            if (handle == INVALID_SOCKET) {
                continue;
            }
            if (connect(handle, r->ai_addr, (int)r->ai_addrlen) == 0) {
                break;
            }
            closesocket(handle);
        }
        freeaddrinfo(res);
        if (!r) {
            throw std::runtime_error("Cannot connect to host");
        }
    }

    TCPStream(int port)
    {
        auto portstr = to_string(port);
        struct addrinfo hints
        {};
        struct addrinfo* res = nullptr;

        hints.ai_flags = AI_PASSIVE;
        if (getaddrinfo(nullptr, portstr.c_str(), &hints, &res) != 0) {
            throw std::runtime_error("Cannot resolve hostname");
        }

        handle = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

        int opt = 1;
        if (setsockopt(handle, SOL_SOCKET, SO_REUSEADDR,
                       reinterpret_cast<char*>(&opt), sizeof(opt))) {
            throw std::runtime_error("setsockopt");
        }

        if (::bind(handle, res->ai_addr, (int)res->ai_addrlen) < 0) {
            throw std::runtime_error("bind failed");
        }
        freeaddrinfo(res);

        if (listen(handle, 10) == SOCKET_ERROR) {
            closesocket(handle);
            throw std::runtime_error("Could not listen to socket");
        }
    }

    virtual void SendN(size_t n, const void* data) override
    {
        LogTrace("Send", n);
        auto c = reinterpret_cast<const char*>(data);
        do {
            auto r = send(handle, c, n, 0);
            if (r <= 0) {
                LogError("Send result", r);
                throw socket_close_exception();
            } else {
                c += r;
                n -= r;
            }
        } while (n);
    }

    virtual void RecvN(size_t n, void* dst) override
    {
        LogTrace("Recv", n);
        auto c = reinterpret_cast<char*>(dst);
        do {
            auto r = recv(handle, c, n, 0);
            if (r <= 0) {
                LogError("Recv result", r);
                throw socket_close_exception();
            } else {
                c += r;
                n -= r;
            }
        } while (n);
    }

    virtual int WaitForDataToRecv(chrono::seconds timeout) override
    {
        fd_set fds;
        struct timeval tv;
        int result;

        FD_ZERO(&fds);
        FD_SET(handle, &fds);

        tv.tv_sec  = (int)timeout.count();
        tv.tv_usec = 0;
        result     = select(handle + 1, &fds, NULL, NULL, &tv);
        if (result > 0) {
            if (FD_ISSET(handle, &fds)) {
                return 1;
            }
        } else if (result == SOCKET_ERROR) {
            throw std::runtime_error("Error on select()");
        }
        return 0;
    }

    virtual void Close() override { closesocket(handle); }

    TCPStream Accept()
    {
        struct addrinfo address;
        socklen_t addrlen = sizeof address;
        TCPStream client;
        client.handle = accept(
          handle, reinterpret_cast<struct sockaddr*>(&address), &addrlen);
        if (client.handle == INVALID_SOCKET) {
            // cout << "Accept Last error " << WSAGetLastError() << endl;
            throw std::runtime_error("Accept failed");
        }

        return client;
    }
};

struct TSerialToStream
{
    INetStream& stream;

    template<typename T>
    void SendN(const T& t)
    {
        stream.SendN(sizeof(T), &t);
    }

    int WaitForDataToRecv(chrono::seconds timeout)
    {
        return stream.WaitForDataToRecv(timeout);
    }

    template<typename T>
    T RecvN()
    {
        T tmp{};
        stream.RecvN(sizeof(T), &tmp);
        return tmp;
    }

    void Close() { stream.Close(); }
};
