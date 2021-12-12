#pragma once

using namespace std;

enum class LogLevel
{
    Error,
    Message,
    Info,
    Trace
};

struct helper
{
    template<typename T>
    static ostream& op(ostream& o, const T& v)
    {
        return o << ' ' << v;
    }

    template<typename T>
    static ostream& op(ostream& o, const vector<T>& v)
    {
        op(o, '[');
        for (const auto& iv : v) {
            op(o, iv);
        }
        op(o, ']');
        return o;
    }
};

LogLevel g_log_level = LogLevel::Info;

template<typename... Args>
void Log(LogLevel lvl, Args&&... args)
{
    if (lvl > g_log_level)
        return;

    // only 1 log line at a time please
    static mutex lock;
    lock_guard<mutex> scope_guard(lock);

    static const char* lvl_tags[] = {
        "[ERR]",
        "[MSG]",
        "[INF]",
        "[TRC]",
    };

    cout << lvl_tags[(int)lvl];
    (helper::op(cout, args), ...);
    cout << endl;
}

#define LogError(...) Log(LogLevel::Error, __VA_ARGS__)
#define LogMessage(...) Log(LogLevel::Message, __VA_ARGS__)
#define LogInfo(...) Log(LogLevel::Info, __VA_ARGS__)
#define LogTrace(...) Log(LogLevel::Trace, __VA_ARGS__)
