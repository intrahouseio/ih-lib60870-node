// cs101_slave.h
#ifndef CS101_SLAVE1_H
#define CS101_SLAVE1_H

#include <napi.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

extern "C" {
#include "hal_serial.h"
#include "cs101_slave.h"
#include "cs101_information_objects.h"
#include "iec60870_common.h"
#include "hal_thread.h"
#include "hal_time.h"
#include "link_layer_parameters.h"
}

class IEC101Slave : public Napi::ObjectWrap<IEC101Slave> {
public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports);
    IEC101Slave(const Napi::CallbackInfo& info);
    virtual ~IEC101Slave();

private:

    static Napi::FunctionReference constructor;
   
    CS101_Slave slave;
    SerialPort serialPort;
    std::thread _thread;
    std::atomic<bool> running;
    std::mutex connMutex;
    bool connected = false;
    int clientId = 0;
    std::string clientID;
    int cnt = 0;
    Napi::ThreadSafeFunction tsfn;
    IMasterConnection masterConnection = nullptr;

    static bool RawMessageHandler(void* parameter, IMasterConnection connection, CS101_ASDU asdu);
    static void LinkLayerStateChanged(void* parameter, int address, LinkLayerState state);

    Napi::Value Connect(const Napi::CallbackInfo& info);
    Napi::Value Disconnect(const Napi::CallbackInfo& info);
    Napi::Value SendCommands(const Napi::CallbackInfo& info);
    Napi::Value GetStatus(const Napi::CallbackInfo& info);
};

#endif // CS101_SLAVE1_H