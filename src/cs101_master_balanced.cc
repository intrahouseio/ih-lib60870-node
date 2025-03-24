#include <inttypes.h>
#include <napi.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <vector>

extern "C"
{
#include "hal_serial.h"  // Используем только этот заголовок
#include "cs101_master.h"
#include "hal_thread.h"
#include "hal_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
}

using namespace Napi;
using namespace std;

class IEC101MasterBalanced : public ObjectWrap<IEC101MasterBalanced>
{
public:
    static Object Init(Napi::Env env, Object exports);
    IEC101MasterBalanced(const CallbackInfo &info);
    ~IEC101MasterBalanced();

    static FunctionReference constructor;

private:
    CS101_Master master;
    SerialPort serialPort;
    std::thread _thread;
    std::atomic<bool> running;
    std::mutex connMutex;
    bool connected = false;
    bool activated = false;
    int clientId = 0;
    std::string clientID;
    int cnt = 0;
    ThreadSafeFunction tsfn;
    int asduAddress = 1; // Поле класса для хранения адреса ASDU

    static bool RawMessageHandler(void *parameter, int address, CS101_ASDU asdu);
    static void LinkLayerStateChanged(void *parameter, int address, LinkLayerState state);

    Napi::Value Connect(const CallbackInfo &info);
    Napi::Value Disconnect(const CallbackInfo &info);
    Napi::Value SendStartDT(const CallbackInfo &info);
    Napi::Value SendStopDT(const CallbackInfo &info);
    Napi::Value SendCommands(const CallbackInfo &info);
    Napi::Value GetStatus(const CallbackInfo &info);
};

FunctionReference IEC101MasterBalanced::constructor;

Object IEC101MasterBalanced::Init(Napi::Env env, Object exports)
{
    Function func = DefineClass(env, "IEC101MasterBalanced", {
        InstanceMethod("connect", &IEC101MasterBalanced::Connect),
        InstanceMethod("disconnect", &IEC101MasterBalanced::Disconnect),
        InstanceMethod("sendStartDT", &IEC101MasterBalanced::SendStartDT),
        InstanceMethod("sendStopDT", &IEC101MasterBalanced::SendStopDT),
        InstanceMethod("sendCommands", &IEC101MasterBalanced::SendCommands),
        InstanceMethod("getStatus", &IEC101MasterBalanced::GetStatus)
    });

    constructor = Persistent(func);
    constructor.SuppressDestruct();
    exports.Set("IEC101MasterBalanced", func);
    return exports;
}

IEC101MasterBalanced::IEC101MasterBalanced(const CallbackInfo &info) : ObjectWrap<IEC101MasterBalanced>(info)
{
    if (info.Length() < 1 || !info[0].IsFunction()) {
        Napi::TypeError::New(info.Env(), "Expected a callback function").ThrowAsJavaScriptException();
        return;
    }

    Napi::Function emit = info[0].As<Napi::Function>();
    running = false;

    try {
        tsfn = ThreadSafeFunction::New(
            info.Env(),
            emit,
            "IEC101MasterBalancedTSFN",
            0,
            1,
            [](Napi::Env) {}
        );
    } catch (const std::exception& e) {
        printf("Failed to create ThreadSafeFunction: %s\n", e.what());
        Napi::Error::New(info.Env(), string("TSFN creation failed: ") + e.what()).ThrowAsJavaScriptException();
    }
}

IEC101MasterBalanced::~IEC101MasterBalanced()
{
    std::lock_guard<std::mutex> lock(connMutex);
    if (running) {
        running = false;
        if (connected) {
            printf("Destructor closing connection, clientID: %s, clientId: %i\n", clientID.c_str(), clientId);
            CS101_Master_stop(master);
            CS101_Master_destroy(master);
            SerialPort_destroy(serialPort);
            connected = false;
            activated = false;
        }
        if (_thread.joinable()) {
            _thread.join();
        }
        tsfn.Release();
    }
}

Napi::Value IEC101MasterBalanced::Connect(const CallbackInfo &info)
{
    Napi::Env env = info.Env();

    if (info.Length() < 4 || !info[0].IsString() || !info[1].IsNumber() || !info[2].IsNumber() || !info[3].IsString()) {
        Napi::TypeError::New(env, "Expected port (string), baudRate (number), clientId (number), clientID (string), [params (object)]").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::string portName = info[0].As<String>();
    int baudRate = info[1].As<Number>().Int32Value();
    clientId = info[2].As<Number>().Int32Value();
    clientID = info[3].As<String>();

    if (portName.empty() || baudRate <= 0 || clientID.empty()) {
        Napi::Error::New(env, "Invalid port, baud rate, or clientID").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    {
        std::lock_guard<std::mutex> lock(connMutex);
        if (running) {
            Napi::Error::New(env, "Client already running").ThrowAsJavaScriptException();
            return env.Undefined();
        }
    }

    int linkAddress = 1;
    int originatorAddress = 1;
    asduAddress = 1; // Инициализируем поле класса
    int k = 12;
    int w = 8;
    int t0 = 30;
    int t1 = 15;
    int t2 = 10;
    int t3 = 20;
    int reconnectDelay = 5;
    int maxRetries = 10;
    int queueSize = 100;

    if (info.Length() > 4 && info[4].IsObject()) {
        Napi::Object params = info[4].As<Napi::Object>();
        if (params.Has("linkAddress")) linkAddress = params.Get("linkAddress").As<Number>().Int32Value();
        if (params.Has("originatorAddress")) originatorAddress = params.Get("originatorAddress").As<Number>().Int32Value();
        if (params.Has("asduAddress")) asduAddress = params.Get("asduAddress").As<Number>().Int32Value();
        if (params.Has("k")) k = params.Get("k").As<Number>().Int32Value();
        if (params.Has("w")) w = params.Get("w").As<Number>().Int32Value();
        if (params.Has("t0")) t0 = params.Get("t0").As<Number>().Int32Value();
        if (params.Has("t1")) t1 = params.Get("t1").As<Number>().Int32Value();
        if (params.Has("t2")) t2 = params.Get("t2").As<Number>().Int32Value();
        if (params.Has("t3")) t3 = params.Get("t3").As<Number>().Int32Value();
        if (params.Has("reconnectDelay")) reconnectDelay = params.Get("reconnectDelay").As<Number>().Int32Value();
        if (params.Has("maxRetries")) maxRetries = params.Get("maxRetries").As<Number>().Int32Value();
        if (params.Has("queueSize")) queueSize = params.Get("queueSize").As<Number>().Int32Value();

        if (linkAddress < 0 || linkAddress > 255) {
            Napi::Error::New(env, "linkAddress must be 0-255").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        if (originatorAddress < 0 || originatorAddress > 255) {
            Napi::Error::New(env, "originatorAddress must be 0-255").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        if (asduAddress < 0 || asduAddress > 65535) {
            Napi::Error::New(env, "asduAddress must be 0-65535").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        if (k <= 0 || w <= 0 || t0 <= 0 || t1 <= 0 || t2 <= 0 || t3 <= 0) {
            Napi::Error::New(env, "k, w, t0, t1, t2, t3 must be positive").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        if (reconnectDelay < 1) {
            Napi::Error::New(env, "reconnectDelay must be at least 1 second").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        if (maxRetries < 0) {
            Napi::Error::New(env, "maxRetries must be non-negative").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        if (queueSize <= 0) {
            Napi::Error::New(env, "queueSize must be positive").ThrowAsJavaScriptException();
            return env.Undefined();
        }
    }

    try {
        printf("Creating serial connection to %s, baudRate: %d, clientID: %s, clientId: %i\n", portName.c_str(), baudRate, clientID.c_str(), clientId);
        serialPort = SerialPort_create(portName.c_str(), baudRate, 8, 'E', 1);
        if (!serialPort) {
            throw runtime_error("Failed to create serial port object");
        }

        // Создаём структуру параметров канального уровня
        struct sLinkLayerParameters llParams;
        llParams.addressLength = 1; // Значение по умолчанию из cs101_master.c
        llParams.timeoutForAck = t1 * 1000; // Переводим в миллисекунды
        llParams.timeoutRepeat = t2 * 1000;
        llParams.timeoutLinkState = t0 * 1000;
        llParams.useSingleCharACK = true;

        // Создаём структуру параметров прикладного уровня
        struct sCS101_AppLayerParameters alParams;
        alParams.sizeOfTypeId = 1;
        alParams.sizeOfVSQ = 1;
        alParams.sizeOfCOT = 2;
        alParams.originatorAddress = originatorAddress;
        alParams.sizeOfCA = 2;
        alParams.sizeOfIOA = 3;
        alParams.maxSizeOfASDU = 249;

        // Создаём мастер
        master = CS101_Master_createEx(serialPort, &llParams, &alParams, IEC60870_LINK_LAYER_BALANCED, queueSize);
        if (!master) {
            SerialPort_destroy(serialPort);
            throw runtime_error("Failed to create master object");
        }

        // Устанавливаем обработчики
        CS101_Master_setASDUReceivedHandler(master, RawMessageHandler, this);
        CS101_Master_setLinkLayerStateChanged(master, LinkLayerStateChanged, this);
        CS101_Master_setDIR(master, true);
        CS101_Master_setOwnAddress(master, linkAddress);
        CS101_Master_useSlaveAddress(master, linkAddress);

        printf("Connecting with params: linkAddress=%d, originatorAddress=%d, asduAddress=%d, k=%d, w=%d, t0=%d, t1=%d, t2=%d, t3=%d, reconnectDelay=%d, maxRetries=%d, queueSize=%d, clientID: %s, clientId: %i\n",
               linkAddress, originatorAddress, asduAddress, k, w, t0, t1, t2, t3, reconnectDelay, maxRetries, queueSize, clientID.c_str(), clientId);

        running = true;
        _thread = std::thread([this, portName, baudRate, linkAddress, originatorAddress, t0, t1, t2, reconnectDelay, maxRetries, queueSize] {
            try {
                int retryCount = 0;
                while (running && retryCount <= maxRetries) {
                    SerialPort_open(serialPort);
                    CS101_Master_start(master);
                    {
                        std::lock_guard<std::mutex> lock(connMutex);
                        connected = true;
                        activated = true; // В Balanced Mode активация автоматическая
                    }
                    printf("Attempting to connect (attempt %d/%d), clientID: %s, clientId: %i\n", retryCount + 1, maxRetries + 1, clientID.c_str(), clientId);

                    while (running) {
                        CS101_Master_run(master);
                        Thread_sleep(100);
                        std::lock_guard<std::mutex> lock(connMutex);
                        if (!connected) break;
                    }

                    std::lock_guard<std::mutex> lock(connMutex);
                    if (running && !connected) {
                        printf("Connection lost, preparing to reconnect, clientID: %s, clientId: %i\n", clientID.c_str(), clientId);
                        CS101_Master_stop(master);
                        CS101_Master_destroy(master);
                        SerialPort_destroy(serialPort);

                        serialPort = SerialPort_create(portName.c_str(), baudRate, 8, 'E', 1);
                        if (!serialPort) {
                            throw runtime_error("Failed to recreate serial port object for reconnect");
                        }

                        struct sLinkLayerParameters llParams;
                        llParams.addressLength = 1;
                        llParams.timeoutForAck = t1 * 1000;
                        llParams.timeoutRepeat = t2 * 1000;
                        llParams.timeoutLinkState = t0 * 1000;
                        llParams.useSingleCharACK = true;

                        struct sCS101_AppLayerParameters alParams;
                        alParams.sizeOfTypeId = 1;
                        alParams.sizeOfVSQ = 1;
                        alParams.sizeOfCOT = 2;
                        alParams.originatorAddress = originatorAddress;
                        alParams.sizeOfCA = 2;
                        alParams.sizeOfIOA = 3;
                        alParams.maxSizeOfASDU = 249;

                        master = CS101_Master_createEx(serialPort, &llParams, &alParams, IEC60870_LINK_LAYER_BALANCED, queueSize);
                        if (!master) {
                            SerialPort_destroy(serialPort);
                            throw runtime_error("Failed to recreate master object for reconnect");
                        }

                        CS101_Master_setASDUReceivedHandler(master, RawMessageHandler, this);
                        CS101_Master_setLinkLayerStateChanged(master, LinkLayerStateChanged, this);
                        CS101_Master_setDIR(master, true);
                        CS101_Master_setOwnAddress(master, linkAddress);
                        CS101_Master_useSlaveAddress(master, linkAddress);
                    } else if (!running && connected) {
                        printf("Thread stopped by client, closing connection, clientID: %s, clientId: %i\n", clientID.c_str(), clientId);
                        CS101_Master_stop(master);
                        CS101_Master_destroy(master);
                        SerialPort_destroy(serialPort);
                        connected = false;
                        activated = false;
                        return;
                    }

                    if (running && !connected) {
                        retryCount++;
                        printf("Reconnection attempt %d/%d failed, retrying in %d seconds, clientID: %s, clientId: %i\n", retryCount, maxRetries, reconnectDelay, clientID.c_str(), clientId);
                        tsfn.NonBlockingCall([=](Napi::Env env, Function jsCallback) {
                            Object eventObj = Object::New(env);
                            eventObj.Set("clientID", String::New(env, clientID.c_str()));
                            eventObj.Set("type", String::New(env, "control"));
                            eventObj.Set("event", String::New(env, "reconnecting"));
                            eventObj.Set("reason", String::New(env, string("attempt ") + to_string(retryCount) + " of " + to_string(maxRetries)));
                            std::vector<napi_value> args = {String::New(env, "data"), eventObj};
                            jsCallback.Call(args);
                        });
                        Thread_sleep(reconnectDelay * 1000);
                    }

                    if (retryCount >= maxRetries) {
                        running = false;
                        printf("Max reconnection attempts reached, giving up, clientID: %s, clientId: %i\n", clientID.c_str(), clientId);
                        tsfn.NonBlockingCall([=](Napi::Env env, Function jsCallback) {
                            Object eventObj = Object::New(env);
                            eventObj.Set("clientID", String::New(env, clientID.c_str()));
                            eventObj.Set("type", String::New(env, "control"));
                            eventObj.Set("event", String::New(env, "failed"));
                            eventObj.Set("reason", String::New(env, "max reconnection attempts reached"));
                            std::vector<napi_value> args = {String::New(env, "data"), eventObj};
                            jsCallback.Call(args);
                        });
                        break;
                    }
                }
            } catch (const std::exception& e) {
                printf("Exception in connection thread: %s, clientID: %s, clientId: %i\n", e.what(), clientID.c_str(), clientId);
                std::lock_guard<std::mutex> lock(connMutex);
                running = false;
                if (connected) {
                    CS101_Master_stop(master);
                    CS101_Master_destroy(master);
                    SerialPort_destroy(serialPort);
                    connected = false;
                    activated = false;
                }
                tsfn.NonBlockingCall([=](Napi::Env env, Function jsCallback) {
                    Object eventObj = Object::New(env);
                    eventObj.Set("clientID", String::New(env, clientID.c_str()));
                    eventObj.Set("type", String::New(env, "control"));
                    eventObj.Set("event", String::New(env, "error"));
                    eventObj.Set("reason", String::New(env, string("Thread exception: ") + e.what()));
                    std::vector<napi_value> args = {String::New(env, "data"), eventObj};
                    jsCallback.Call(args);
                });
            }
        });

        return env.Undefined();
    } catch (const std::exception& e) {
        printf("Exception in Connect: %s, clientID: %s, clientId: %i\n", e.what(), clientID.c_str(), clientId);
        Napi::Error::New(env, string("Connect failed: ") + e.what()).ThrowAsJavaScriptException();
        return env.Undefined();
    }
}

Napi::Value IEC101MasterBalanced::Disconnect(const CallbackInfo &info)
{
    Napi::Env env = info.Env();
    {
        std::lock_guard<std::mutex> lock(connMutex);
        if (running) {
            running = false;
            if (connected) {
                printf("Disconnect called by client, clientID: %s, clientId: %i\n", clientID.c_str(), clientId);
                CS101_Master_stop(master);
                CS101_Master_destroy(master);
                SerialPort_destroy(serialPort);
                connected = false;
                activated = false;
            }
        }
    }

    if (_thread.joinable()) {
        _thread.join();
    }

    std::lock_guard<std::mutex> lock(connMutex);
    tsfn.Release();

    return env.Undefined();
}

Napi::Value IEC101MasterBalanced::SendStartDT(const CallbackInfo &info)
{
    Napi::Env env = info.Env();
    std::lock_guard<std::mutex> lock(connMutex);
    if (!connected) {
        Napi::Error::New(env, "Not connected").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    printf("SendStartDT called, already activated in Balanced Mode, clientID: %s, clientId: %i\n", clientID.c_str(), clientId);
    return Boolean::New(env, true);
}

Napi::Value IEC101MasterBalanced::SendStopDT(const CallbackInfo &info)
{
    Napi::Env env = info.Env();
    std::lock_guard<std::mutex> lock(connMutex);
    if (!connected || !activated) {
        Napi::Error::New(env, "Not connected or not activated").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    printf("SendStopDT called, emulating deactivation in Balanced Mode, clientID: %s, clientId: %i\n", clientID.c_str(), clientId);
    activated = false;
    return Boolean::New(env, true);
}

Napi::Value IEC101MasterBalanced::SendCommands(const CallbackInfo &info)
{
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsArray()) {
        Napi::TypeError::New(env, "Expected commands (array of objects with 'typeId', 'ioa', 'value', and optional fields)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    Napi::Array commands = info[0].As<Napi::Array>();

    std::lock_guard<std::mutex> lock(connMutex);
    if (!connected || !activated) {
        Napi::Error::New(env, "Not connected or not activated").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    CS101_AppLayerParameters alParams = CS101_Master_getAppLayerParameters(master);

    try {
        bool allSuccess = true;
        for (uint32_t i = 0; i < commands.Length(); i++) {
            Napi::Value item = commands[i];
            if (!item.IsObject()) {
                Napi::TypeError::New(env, "Each command must be an object").ThrowAsJavaScriptException();
                return env.Undefined();
            }

            Napi::Object cmdObj = item.As<Napi::Object>();
            if (!cmdObj.Has("typeId") || !cmdObj.Has("ioa") || !cmdObj.Has("value")) {
                Napi::TypeError::New(env, "Each command must have 'typeId' (number), 'ioa' (number), and 'value'").ThrowAsJavaScriptException();
                return env.Undefined();
            }

            int typeId = cmdObj.Get("typeId").As<Napi::Number>().Int32Value();
            int ioa = cmdObj.Get("ioa").As<Napi::Number>().Int32Value();
            CS101_ASDU asdu = CS101_ASDU_create(alParams, false, CS101_COT_ACTIVATION, 0, this->asduAddress, false, false);

            // В сбалансированном режиме мы не получаем явного подтверждения отправки, поэтому success всегда true
            CS101_Master_sendASDU(master, asdu); // Добавляем ASDU в очередь

            switch (typeId) {
                case C_SC_NA_1: {
                    if (!cmdObj.Get("value").IsBoolean()) {
                        Napi::TypeError::New(env, "C_SC_NA_1 requires 'value' as boolean").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    bool value = cmdObj.Get("value").As<Napi::Boolean>();
                    SingleCommand sc = SingleCommand_create(NULL, ioa, value, false, IEC60870_QUALITY_GOOD);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)sc);
                    CS101_Master_sendASDU(master, asdu);
                    SingleCommand_destroy(sc);
                    break;
                }

                case C_DC_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_DC_NA_1 requires 'value' as number (0-3)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    if (value < 0 || value > 3) {
                        Napi::RangeError::New(env, "C_DC_NA_1 'value' must be 0-3").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    DoubleCommand dc = DoubleCommand_create(NULL, ioa, value, false, IEC60870_QUALITY_GOOD);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)dc);
                    CS101_Master_sendASDU(master, asdu);
                    DoubleCommand_destroy(dc);
                    break;
                }

                case C_RC_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_RC_NA_1 requires 'value' as number (0-3)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    if (value < 0 || value > 3) {
                        Napi::RangeError::New(env, "C_RC_NA_1 'value' must be 0-3").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    StepCommand rc = StepCommand_create(NULL, ioa, (StepCommandValue)value, false, IEC60870_QUALITY_GOOD);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)rc);
                    CS101_Master_sendASDU(master, asdu);
                    StepCommand_destroy(rc);
                    break;
                }

                case C_SE_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_NA_1 requires 'value' as number (-1.0 to 1.0)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    float value = cmdObj.Get("value").As<Napi::Number>().FloatValue();
                    if (value < -1.0f || value > 1.0f) {
                        Napi::RangeError::New(env, "C_SE_NA_1 'value' must be between -1.0 and 1.0").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    SetpointCommandNormalized scn = SetpointCommandNormalized_create(NULL, ioa, value, false, IEC60870_QUALITY_GOOD);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)scn);
                    CS101_Master_sendASDU(master, asdu);
                    SetpointCommandNormalized_destroy(scn);
                    break;
                }

                case C_SE_NB_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_NB_1 requires 'value' as number (-32768 to 32767)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    if (value < -32768 || value > 32767) {
                        Napi::RangeError::New(env, "C_SE_NB_1 'value' must be between -32768 and 32767").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    SetpointCommandScaled scs = SetpointCommandScaled_create(NULL, ioa, value, false, IEC60870_QUALITY_GOOD);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)scs);
                    CS101_Master_sendASDU(master, asdu);
                    SetpointCommandScaled_destroy(scs);
                    break;
                }

                case C_SE_NC_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_NC_1 requires 'value' as number").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    float value = cmdObj.Get("value").As<Napi::Number>().FloatValue();
                    SetpointCommandShort scsf = SetpointCommandShort_create(NULL, ioa, value, false, IEC60870_QUALITY_GOOD);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)scsf);
                    CS101_Master_sendASDU(master, asdu);
                    SetpointCommandShort_destroy(scsf);
                    break;
                }

                case C_BO_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_BO_NA_1 requires 'value' as number (32-bit unsigned integer)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    uint32_t value = cmdObj.Get("value").As<Napi::Number>().Uint32Value();
                    Bitstring32Command bc = Bitstring32Command_create(NULL, ioa, value);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)bc);
                    CS101_Master_sendASDU(master, asdu);
                    Bitstring32Command_destroy(bc);
                    break;
                }

                case C_SC_TA_1: {
                    if (!cmdObj.Get("value").IsBoolean() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_SC_TA_1 requires 'value' (boolean) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    bool value = cmdObj.Get("value").As<Napi::Boolean>();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    SingleCommandWithCP56Time2a sc = SingleCommandWithCP56Time2a_create(NULL, ioa, value, false, IEC60870_QUALITY_GOOD, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)sc);
                    CS101_Master_sendASDU(master, asdu);
                    SingleCommandWithCP56Time2a_destroy(sc);
                    break;
                }

                case C_DC_TA_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_DC_TA_1 requires 'value' (number 0-3) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (value < 0 || value > 3) {
                        Napi::RangeError::New(env, "C_DC_TA_1 'value' must be 0-3").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    DoubleCommandWithCP56Time2a dc = DoubleCommandWithCP56Time2a_create(NULL, ioa, value, false, IEC60870_QUALITY_GOOD, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)dc);
                    CS101_Master_sendASDU(master, asdu);
                    DoubleCommandWithCP56Time2a_destroy(dc);
                    break;
                }

                case C_RC_TA_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_RC_TA_1 requires 'value' (number 0-3) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (value < 0 || value > 3) {
                        Napi::RangeError::New(env, "C_RC_TA_1 'value' must be 0-3").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    StepCommandWithCP56Time2a rc = StepCommandWithCP56Time2a_create(NULL, ioa, (StepCommandValue)value, false, IEC60870_QUALITY_GOOD, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)rc);
                    CS101_Master_sendASDU(master, asdu);
                    StepCommandWithCP56Time2a_destroy(rc);
                    break;
                }

                case C_SE_TA_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_TA_1 requires 'value' (number -1.0 to 1.0) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    float value = cmdObj.Get("value").As<Napi::Number>().FloatValue();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (value < -1.0f || value > 1.0f) {
                        Napi::RangeError::New(env, "C_SE_TA_1 'value' must be between -1.0 and 1.0").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    SetpointCommandNormalizedWithCP56Time2a scn = SetpointCommandNormalizedWithCP56Time2a_create(NULL, ioa, value, false, IEC60870_QUALITY_GOOD, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)scn);
                    CS101_Master_sendASDU(master, asdu);
                    SetpointCommandNormalizedWithCP56Time2a_destroy(scn);
                    break;
                }

                case C_SE_TB_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_TB_1 requires 'value' (number -32768 to 32767) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (value < -32768 || value > 32767) {
                        Napi::RangeError::New(env, "C_SE_TB_1 'value' must be between -32768 and 32767").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    SetpointCommandScaledWithCP56Time2a scs = SetpointCommandScaledWithCP56Time2a_create(NULL, ioa, value, false, IEC60870_QUALITY_GOOD, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)scs);
                    CS101_Master_sendASDU(master, asdu);
                    SetpointCommandScaledWithCP56Time2a_destroy(scs);
                    break;
                }

                case C_SE_TC_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_TC_1 requires 'value' (number) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    float value = cmdObj.Get("value").As<Napi::Number>().FloatValue();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    SetpointCommandShortWithCP56Time2a scsf = SetpointCommandShortWithCP56Time2a_create(NULL, ioa, value, false, IEC60870_QUALITY_GOOD, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)scsf);
                    CS101_Master_sendASDU(master, asdu);
                    SetpointCommandShortWithCP56Time2a_destroy(scsf);
                    break;
                }

                case C_BO_TA_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_BO_TA_1 requires 'value' (32-bit number) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    uint32_t value = cmdObj.Get("value").As<Napi::Number>().Uint32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    Bitstring32CommandWithCP56Time2a bc = Bitstring32CommandWithCP56Time2a_create(NULL, ioa, value, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)bc);
                    CS101_Master_sendASDU(master, asdu);
                    Bitstring32CommandWithCP56Time2a_destroy(bc);
                    break;
                }

                case C_IC_NA_1: {
                    CS101_ASDU_setTypeID(asdu, C_IC_NA_1);
                    CS101_ASDU_setCOT(asdu, CS101_COT_REQUEST);
                    InformationObject io = (InformationObject)InterrogationCommand_create(NULL, ioa, cmdObj.Get("value").As<Napi::Number>().Uint32Value());
                    CS101_ASDU_addInformationObject(asdu, io);
                    CS101_Master_sendASDU(master, asdu);
                    InformationObject_destroy(io);
                    break;
                }

                case C_CI_NA_1: {
                    CS101_ASDU_setTypeID(asdu, C_CI_NA_1);
                    CS101_ASDU_setCOT(asdu, CS101_COT_REQUEST);
                    InformationObject io = (InformationObject)CounterInterrogationCommand_create(NULL, ioa, cmdObj.Get("value").As<Napi::Number>().Uint32Value());
                    CS101_ASDU_addInformationObject(asdu, io);
                    CS101_Master_sendASDU(master, asdu);
                    InformationObject_destroy(io);
                    break;
                }

                case C_RD_NA_1: {
                    CS101_ASDU_setTypeID(asdu, C_RD_NA_1);
                    CS101_ASDU_setCOT(asdu, CS101_COT_REQUEST);
                    InformationObject io = (InformationObject)ReadCommand_create(NULL, ioa);
                    CS101_ASDU_addInformationObject(asdu, io);
                    CS101_Master_sendASDU(master, asdu);
                    InformationObject_destroy(io);
                    break;
                }

                case C_CS_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_CS_NA_1 requires 'value' as number (timestamp)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    uint64_t value = cmdObj.Get("value").As<Napi::Number>().Int64Value();
                    CS101_ASDU_setTypeID(asdu, C_CS_NA_1);
                    CS101_ASDU_setCOT(asdu, CS101_COT_ACTIVATION);
                    InformationObject io = (InformationObject)ClockSynchronizationCommand_create(NULL, ioa, CP56Time2a_createFromMsTimestamp(NULL, value));
                    CS101_ASDU_addInformationObject(asdu, io);
                    CS101_Master_sendASDU(master, asdu);
                    InformationObject_destroy(io);
                    break;
                }

                default:
                    printf("Unsupported command type: %d, clientID: %s, clientId: %i\n", typeId, clientID.c_str(), clientId);
                    CS101_ASDU_destroy(asdu);
                    allSuccess = false;
                    continue;
            }

            CS101_ASDU_destroy(asdu);

            printf("Sent command: typeId=%d, ioa=%d, clientID: %s, clientId: %i\n", typeId, ioa, clientID.c_str(), clientId);
        }
        return Boolean::New(env, allSuccess);
    } catch (const std::exception& e) {
        printf("Exception in SendCommands: %s, clientID: %s, clientId: %i\n", e.what(), clientID.c_str(), clientId);
        Napi::Error::New(env, string("SendCommands failed: ") + e.what()).ThrowAsJavaScriptException();
        return Boolean::New(env, false);
    }
}

Napi::Value IEC101MasterBalanced::GetStatus(const CallbackInfo &info)
{
    Napi::Env env = info.Env();
    std::lock_guard<std::mutex> lock(connMutex);
    Object status = Object::New(env);
    status.Set("connected", Boolean::New(env, connected));
    status.Set("activated", Boolean::New(env, activated));
    status.Set("clientId", Number::New(env, clientId));
    status.Set("clientID", String::New(env, clientID.c_str()));
    return status;
}

void IEC101MasterBalanced::LinkLayerStateChanged(void *parameter, int address, LinkLayerState state)
{
    IEC101MasterBalanced *client = static_cast<IEC101MasterBalanced *>(parameter);
    std::string eventStr;
    std::string reason;

    {
        std::lock_guard<std::mutex> lock(client->connMutex);
        switch (state) {
            case LL_STATE_ERROR:
                eventStr = "failed";
                reason = "link layer error";
                client->connected = false;
                client->activated = false;
                break;
            case LL_STATE_AVAILABLE:
                eventStr = "opened";
                reason = "link layer available";
                client->connected = true;
                client->activated = true;
                break;
            case LL_STATE_BUSY:
                eventStr = "busy";
                reason = "link layer busy";
                client->connected = true;
                break;
            case LL_STATE_IDLE:
                eventStr = "closed";
                reason = client->running ? "link layer closed unexpectedly" : "link layer closed by client";
                client->connected = false;
                client->activated = false;
                break;
        }
    }

    printf("Link layer event: %s, reason: %s, clientID: %s, clientId: %i\n", eventStr.c_str(), reason.c_str(), client->clientID.c_str(), client->clientId);

    client->tsfn.NonBlockingCall([=](Napi::Env env, Function jsCallback) {
        Object eventObj = Object::New(env);
        eventObj.Set("clientID", String::New(env, client->clientID.c_str()));
        eventObj.Set("type", String::New(env, "control"));
        eventObj.Set("event", String::New(env, eventStr));
        eventObj.Set("reason", String::New(env, reason));
        std::vector<napi_value> args = {String::New(env, "data"), eventObj};
        jsCallback.Call(args);
    });
}

bool IEC101MasterBalanced::RawMessageHandler(void *parameter, int address, CS101_ASDU asdu)
{
    IEC101MasterBalanced *client = static_cast<IEC101MasterBalanced *>(parameter);
    IEC60870_5_TypeID typeID = CS101_ASDU_getTypeID(asdu);
    int numberOfElements = CS101_ASDU_getNumberOfElements(asdu);

    try {
        vector<tuple<int, double, uint8_t, uint64_t>> elements;

        switch (typeID) {
            case M_SP_NA_1:
                for (int i = 0; i < numberOfElements; i++) {
                    SinglePointInformation io = (SinglePointInformation)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = SinglePointInformation_getValue(io) ? 1.0 : 0.0;
                        uint8_t quality = SinglePointInformation_getQuality(io);
                        uint64_t timestamp = 0;
                        elements.emplace_back(ioa, val, quality, timestamp);
                        SinglePointInformation_destroy(io);
                    }
                }
                break;

            case M_DP_NA_1:
                for (int i = 0; i < numberOfElements; i++) {
                    DoublePointInformation io = (DoublePointInformation)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = static_cast<double>(DoublePointInformation_getValue(io));
                        uint8_t quality = DoublePointInformation_getQuality(io);
                        uint64_t timestamp = 0;
                        elements.emplace_back(ioa, val, quality, timestamp);
                        DoublePointInformation_destroy(io);
                    }
                }
                break;

            case M_ST_NA_1:
                for (int i = 0; i < numberOfElements; i++) {
                    StepPositionInformation io = (StepPositionInformation)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = static_cast<double>(StepPositionInformation_getValue(io));
                        uint8_t quality = StepPositionInformation_getQuality(io);
                        uint64_t timestamp = 0;
                        elements.emplace_back(ioa, val, quality, timestamp);
                        StepPositionInformation_destroy(io);
                    }
                }
                break;

            case M_BO_NA_1:
                for (int i = 0; i < numberOfElements; i++) {
                    BitString32 io = (BitString32)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = static_cast<double>(BitString32_getValue(io));
                        uint8_t quality = BitString32_getQuality(io);
                        uint64_t timestamp = 0;
                        elements.emplace_back(ioa, val, quality, timestamp);
                        BitString32_destroy(io);
                    }
                }
                break;

            case M_ME_NA_1:
                for (int i = 0; i < numberOfElements; i++) {
                    MeasuredValueNormalized io = (MeasuredValueNormalized)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = MeasuredValueNormalized_getValue(io);
                        uint8_t quality = MeasuredValueNormalized_getQuality(io);
                        uint64_t timestamp = 0;
                        elements.emplace_back(ioa, val, quality, timestamp);
                        MeasuredValueNormalized_destroy(io);
                    }
                }
                break;

            case M_ME_NB_1:
                for (int i = 0; i < numberOfElements; i++) {
                    MeasuredValueScaled io = (MeasuredValueScaled)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = MeasuredValueScaled_getValue(io);
                        uint8_t quality = MeasuredValueScaled_getQuality(io);
                        uint64_t timestamp = 0;
                        elements.emplace_back(ioa, val, quality, timestamp);
                        MeasuredValueScaled_destroy(io);
                    }
                }
                break;

            case M_ME_NC_1:
                for (int i = 0; i < numberOfElements; i++) {
                    MeasuredValueShort io = (MeasuredValueShort)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = MeasuredValueShort_getValue(io);
                        uint8_t quality = MeasuredValueShort_getQuality(io);
                        uint64_t timestamp = 0;
                        elements.emplace_back(ioa, val, quality, timestamp);
                        MeasuredValueShort_destroy(io);
                    }
                }
                break;

            case M_IT_NA_1:
                for (int i = 0; i < numberOfElements; i++) {
                    IntegratedTotals io = (IntegratedTotals)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = BinaryCounterReading_getValue(IntegratedTotals_getBCR(io));
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = 0;
                        elements.emplace_back(ioa, val, quality, timestamp);
                        IntegratedTotals_destroy(io);
                    }
                }
                break;

            case M_SP_TB_1:
                for (int i = 0; i < numberOfElements; i++) {
                    SinglePointWithCP56Time2a io = (SinglePointWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = SinglePointInformation_getValue((SinglePointInformation)io) ? 1.0 : 0.0;
                        uint8_t quality = SinglePointInformation_getQuality((SinglePointInformation)io);
                        uint64_t timestamp = CP56Time2a_toMsTimestamp(SinglePointWithCP56Time2a_getTimestamp(io));
                        elements.emplace_back(ioa, val, quality, timestamp);
                        SinglePointWithCP56Time2a_destroy(io);
                    }
                }
                break;

            case M_DP_TB_1:
                for (int i = 0; i < numberOfElements; i++) {
                    DoublePointWithCP56Time2a io = (DoublePointWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = static_cast<double>(DoublePointInformation_getValue((DoublePointInformation)io));
                        uint8_t quality = DoublePointInformation_getQuality((DoublePointInformation)io);
                        uint64_t timestamp = CP56Time2a_toMsTimestamp(DoublePointWithCP56Time2a_getTimestamp(io));
                        elements.emplace_back(ioa, val, quality, timestamp);
                        DoublePointWithCP56Time2a_destroy(io);
                    }
                }
                break;

            case M_ST_TB_1:
                for (int i = 0; i < numberOfElements; i++) {
                    StepPositionWithCP56Time2a io = (StepPositionWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = static_cast<double>(StepPositionInformation_getValue((StepPositionInformation)io));
                        uint8_t quality = StepPositionInformation_getQuality((StepPositionInformation)io);
                        uint64_t timestamp = CP56Time2a_toMsTimestamp(StepPositionWithCP56Time2a_getTimestamp(io));
                        elements.emplace_back(ioa, val, quality, timestamp);
                        StepPositionWithCP56Time2a_destroy(io);
                    }
                }
                break;

            case M_BO_TB_1:
                for (int i = 0; i < numberOfElements; i++) {
                    Bitstring32WithCP56Time2a io = (Bitstring32WithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = static_cast<double>(BitString32_getValue((BitString32)io));
                        uint8_t quality = BitString32_getQuality((BitString32)io);
                        uint64_t timestamp = CP56Time2a_toMsTimestamp(Bitstring32WithCP56Time2a_getTimestamp(io));
                        elements.emplace_back(ioa, val, quality, timestamp);
                        Bitstring32WithCP56Time2a_destroy(io);
                    }
                }
                break;

            case M_ME_TD_1:
                for (int i = 0; i < numberOfElements; i++) {
                    MeasuredValueNormalizedWithCP56Time2a io = (MeasuredValueNormalizedWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = MeasuredValueNormalized_getValue((MeasuredValueNormalized)io);
                        uint8_t quality = MeasuredValueNormalized_getQuality((MeasuredValueNormalized)io);
                        uint64_t timestamp = CP56Time2a_toMsTimestamp(MeasuredValueNormalizedWithCP56Time2a_getTimestamp(io));
                        elements.emplace_back(ioa, val, quality, timestamp);
                        MeasuredValueNormalizedWithCP56Time2a_destroy(io);
                    }
                }
                break;

            case M_ME_TE_1:
                for (int i = 0; i < numberOfElements; i++) {
                    MeasuredValueScaledWithCP56Time2a io = (MeasuredValueScaledWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = MeasuredValueScaled_getValue((MeasuredValueScaled)io);
                        uint8_t quality = MeasuredValueScaled_getQuality((MeasuredValueScaled)io);
                        uint64_t timestamp = CP56Time2a_toMsTimestamp(MeasuredValueScaledWithCP56Time2a_getTimestamp(io));
                        elements.emplace_back(ioa, val, quality, timestamp);
                        MeasuredValueScaledWithCP56Time2a_destroy(io);
                    }
                }
                break;

            case M_ME_TF_1:
                for (int i = 0; i < numberOfElements; i++) {
                    MeasuredValueShortWithCP56Time2a io = (MeasuredValueShortWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = MeasuredValueShort_getValue((MeasuredValueShort)io);
                        uint8_t quality = MeasuredValueShort_getQuality((MeasuredValueShort)io);
                        uint64_t timestamp = CP56Time2a_toMsTimestamp(MeasuredValueShortWithCP56Time2a_getTimestamp(io));
                        elements.emplace_back(ioa, val, quality, timestamp);
                        MeasuredValueShortWithCP56Time2a_destroy(io);
                    }
                }
                break;

            case M_IT_TB_1:
                for (int i = 0; i < numberOfElements; i++) {
                    IntegratedTotalsWithCP56Time2a io = (IntegratedTotalsWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = BinaryCounterReading_getValue(IntegratedTotals_getBCR((IntegratedTotals)io));
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = CP56Time2a_toMsTimestamp(IntegratedTotalsWithCP56Time2a_getTimestamp(io));
                        elements.emplace_back(ioa, val, quality, timestamp);
                        IntegratedTotalsWithCP56Time2a_destroy(io);
                    }
                }
                break;

            default:
                printf("Received unsupported ASDU type: %s (%i), clientID: %s\n", TypeID_toString(typeID), typeID, client->clientID.c_str());
                return true;
        }

        for (const auto& [ioa, val, quality, timestamp] : elements) {
            printf("ASDU type: %s, clientID: %s, clientId: %i, ioa: %i, value: %f, quality: %u, timestamp: %" PRIu64 ", cnt: %i\n",
                   TypeID_toString(typeID), client->clientID.c_str(), client->clientId, ioa, val, quality, timestamp, client->cnt);
        }

        client->tsfn.NonBlockingCall([=](Napi::Env env, Function jsCallback) {
            Napi::Array jsArray = Napi::Array::New(env, elements.size());
            for (size_t i = 0; i < elements.size(); i++) {
                const auto& [ioa, val, quality, timestamp] = elements[i];
                Napi::Object msg = Napi::Object::New(env);
                msg.Set("clientID", String::New(env, client->clientID.c_str()));
                msg.Set("typeId", Number::New(env, typeID));
                msg.Set("ioa", Number::New(env, ioa));
                msg.Set("val", Number::New(env, val));
                msg.Set("quality", Number::New(env, quality));
                if (timestamp > 0) {
                    msg.Set("timestamp", Number::New(env, static_cast<double>(timestamp)));
                }
                jsArray[i] = msg;
            }
            std::vector<napi_value> args = {String::New(env, "data"), jsArray};
            jsCallback.Call(args);
            client->cnt++;
        });

        return true;
    } catch (const std::exception& e) {
        printf("Exception in RawMessageHandler: %s, clientID: %s, clientId: %i\n", e.what(), client->clientID.c_str(), client->clientId);
        client->tsfn.NonBlockingCall([=](Napi::Env env, Function jsCallback) {
            Object eventObj = Object::New(env);
            eventObj.Set("clientID", String::New(env, client->clientID.c_str()));
            eventObj.Set("type", String::New(env, "error"));
            eventObj.Set("reason", String::New(env, string("ASDU handling failed: ") + e.what()));
            std::vector<napi_value> args = {String::New(env, "data"), eventObj};
            jsCallback.Call(args);
        });
        return false;
    }
}
