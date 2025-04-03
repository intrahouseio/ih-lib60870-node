#ifndef CS104_CLIENT_H
#define CS104_CLIENT_H


#include <mutex>
#include <thread>
#include <napi.h>
#include <atomic>
#include <vector>

extern "C" {
#include "cs104_connection.h"
#include "hal_thread.h"
#include "hal_time.h"
}

class IEC104Client : public Napi::ObjectWrap<IEC104Client> {
public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports);
    IEC104Client(const Napi::CallbackInfo& info);
    virtual ~IEC104Client();

private:
    static Napi::FunctionReference constructor;
    
    int originatorAddress;
    CS104_Connection connection;
    std::thread _thread;
    std::atomic<bool> running;
    std::mutex connMutex;
    bool connected = false;
    bool activated = false;
    //int clientId = 0;
    std::string clientID;
    int cnt = 0;
    int asduAddress; 
    Napi::ThreadSafeFunction tsfn;

    static bool RawMessageHandler(void* parameter, int address, CS101_ASDU asdu);
    static void ConnectionHandler(void* parameter, CS104_Connection con, CS104_ConnectionEvent event);

    Napi::Value Connect(const Napi::CallbackInfo& info);
    Napi::Value Disconnect(const Napi::CallbackInfo& info);
    Napi::Value SendStartDT(const Napi::CallbackInfo& info);
    Napi::Value SendStopDT(const Napi::CallbackInfo& info);
    Napi::Value SendCommands(const Napi::CallbackInfo& info);
    Napi::Value GetStatus(const Napi::CallbackInfo& info);
};

#endif // CS104_CLIENT_H