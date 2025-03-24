#ifndef CS104_SERVER_H
#define CS104_SERVER_H

#include <napi.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <map>

extern "C" {
#include "cs104_slave.h"
#include "hal_thread.h"
#include "hal_time.h"
}

class IEC104Server : public Napi::ObjectWrap<IEC104Server> {
public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports);
    IEC104Server(const Napi::CallbackInfo& info);
    virtual ~IEC104Server();

private:
    static Napi::FunctionReference constructor;

    CS104_Slave server;
    std::thread _thread;
    std::atomic<bool> running;
    std::mutex connMutex; // Synchronize connection state changes
    bool started = false;
    int serverId = 0;
    std::string serverID;
    int cnt = 0;
    Napi::ThreadSafeFunction tsfn;
    std::map<IMasterConnection, int> clientConnections; // Map of client connections to client IDs

    static bool ConnectionRequestHandler(void *parameter, const char *ipAddress);
    static void ConnectionEventHandler(void *parameter, IMasterConnection connection, CS104_PeerConnectionEvent event);
    static bool RawMessageHandler(void *parameter, IMasterConnection connection, CS101_ASDU asdu);

    Napi::Value Start(const Napi::CallbackInfo& info);
    Napi::Value Stop(const Napi::CallbackInfo& info);
    Napi::Value SendCommands(const Napi::CallbackInfo& info);
    Napi::Value GetStatus(const Napi::CallbackInfo& info);
};

#endif // CS104_SERVER_H