#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <inttypes.h>
#include <cs101_master_unbalanced.h>
#include <napi.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <vector>

extern "C" {
#include "hal_serial.h"
#include "cs101_master.h"
#include "hal_thread.h"
#include "hal_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
}

using namespace Napi;
using namespace std;

FunctionReference IEC101MasterUnbalanced::constructor;

Object IEC101MasterUnbalanced::Init(Napi::Env env, Object exports) {
    Function func = DefineClass(env, "IEC101MasterUnbalanced", {
        InstanceMethod("connect", &IEC101MasterUnbalanced::Connect),
        InstanceMethod("disconnect", &IEC101MasterUnbalanced::Disconnect),
        InstanceMethod("sendStartDT", &IEC101MasterUnbalanced::SendStartDT),
        InstanceMethod("sendStopDT", &IEC101MasterUnbalanced::SendStopDT),
        InstanceMethod("sendCommands", &IEC101MasterUnbalanced::SendCommands),
        InstanceMethod("getStatus", &IEC101MasterUnbalanced::GetStatus),
        InstanceMethod("addSlave", &IEC101MasterUnbalanced::AddSlave),
        InstanceMethod("pollSlave", &IEC101MasterUnbalanced::PollSlave)
    });

    constructor = Persistent(func);
    constructor.SuppressDestruct();
    exports.Set("IEC101MasterUnbalanced", func);
    return exports;
}

IEC101MasterUnbalanced::IEC101MasterUnbalanced(const CallbackInfo &info) : ObjectWrap<IEC101MasterUnbalanced>(info) {
    if (info.Length() < 1 || !info[0].IsFunction()) {
        Napi::TypeError::New(info.Env(), "Expected a callback function").ThrowAsJavaScriptException();
        return;
    }

    Napi::Function emit = info[0].As<Napi::Function>();
    running = false;
    originatorAddress = 1; // Инициализация по умолчанию
    asduAddress = 0;       // Инициализация новой переменной по умолчанию

    try {
        tsfn = ThreadSafeFunction::New(
            info.Env(),
            emit,
            "IEC101MasterUnbalancedTSFN",
            0,
            1,
            [](Napi::Env) {}
        );
    } catch (const std::exception& e) {
        printf("Failed to create ThreadSafeFunction: %s\n", e.what());
        Napi::Error::New(info.Env(), string("TSFN creation failed: ") + e.what()).ThrowAsJavaScriptException();
    }
}

IEC101MasterUnbalanced::~IEC101MasterUnbalanced() {
    std::lock_guard<std::mutex> lock(connMutex);
    if (running) {
        running = false;
        if (connected) {
            printf("Destructor closing connection, clientID: %s\n", clientID.c_str());
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

Napi::Value IEC101MasterUnbalanced::Connect(const CallbackInfo &info) {
    Napi::Env env = info.Env();

    // Проверяем, что передан один аргумент — объект с параметрами
    if (info.Length() < 1 || !info[0].IsObject()) {
        Napi::TypeError::New(env, "Expected an object with { portName (string), baudRate (number), clientID (string), [params (object)] }").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    Napi::Object config = info[0].As<Napi::Object>();

    // Извлекаем обязательные параметры из объекта
    if (!config.Has("portName") || !config.Get("portName").IsString() ||
        !config.Has("baudRate") || !config.Get("baudRate").IsNumber() ||
        !config.Has("clientID") || !config.Get("clientID").IsString()) {
        Napi::TypeError::New(env, "Object must contain 'portName' (string), 'baudRate' (number), and 'clientID' (string)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::string portName = config.Get("portName").As<String>().Utf8Value();
    int baudRate = config.Get("baudRate").As<Number>().Int32Value();
    clientID = config.Get("clientID").As<String>().Utf8Value();

    if (portName.empty() || baudRate <= 0 || clientID.empty()) {
        Napi::Error::New(env, "Invalid 'portName', 'baudRate', or 'clientID'").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    // Проверяем, запущен ли клиент
    {
        std::lock_guard<std::mutex> lock(connMutex);
        if (running) {
            Napi::Error::New(env, "Client already running").ThrowAsJavaScriptException();
            return env.Undefined();
        }
    }

    // Параметры по умолчанию
    int linkAddress = 1;
    int t0 = 30;
    int t1 = 15;
    int t2 = 10;
    int reconnectDelay = 5;
    int queueSize = 100;

    // Извлекаем дополнительные параметры, если передан объект params
    if (config.Has("params") && config.Get("params").IsObject()) {
        Napi::Object params = config.Get("params").As<Napi::Object>();
        if (params.Has("linkAddress")) linkAddress = params.Get("linkAddress").As<Number>().Int32Value();
        if (params.Has("originatorAddress")) originatorAddress = params.Get("originatorAddress").As<Number>().Int32Value();
        if (params.Has("asduAddress")) asduAddress = params.Get("asduAddress").As<Number>().Int32Value();
        if (params.Has("t0")) t0 = params.Get("t0").As<Number>().Int32Value();
        if (params.Has("t1")) t1 = params.Get("t1").As<Number>().Int32Value();
        if (params.Has("t2")) t2 = params.Get("t2").As<Number>().Int32Value();
        if (params.Has("reconnectDelay")) reconnectDelay = params.Get("reconnectDelay").As<Number>().Int32Value();
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
        if (t0 <= 0 || t1 <= 0 || t2 <= 0) {
            Napi::Error::New(env, "t0, t1, t2 must be positive").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        if (reconnectDelay < 1) {
            Napi::Error::New(env, "reconnectDelay must be at least 1 second").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        if (queueSize <= 0) {
            Napi::Error::New(env, "queueSize must be positive").ThrowAsJavaScriptException();
            return env.Undefined();
        }
    }

    try {
        printf("Creating serial connection to %s, baudRate: %d, clientID: %s\n", portName.c_str(), baudRate, clientID.c_str());
        fflush(stdout);
        serialPort = SerialPort_create(portName.c_str(), baudRate, 8, 'E', 1);
        if (!serialPort) {
            throw runtime_error("Failed to create serial port object");
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

        printf("Creating master object with queueSize: %d, clientID: %s\n", queueSize, clientID.c_str());
        fflush(stdout);
        master = CS101_Master_createEx(serialPort, &llParams, &alParams, IEC60870_LINK_LAYER_UNBALANCED, queueSize);
        if (!master) {
            SerialPort_destroy(serialPort);
            throw runtime_error("Failed to create master object");
        }

        CS101_Master_setASDUReceivedHandler(master, RawMessageHandler, this);
        CS101_Master_setLinkLayerStateChanged(master, LinkLayerStateChanged, this);
        CS101_Master_setOwnAddress(master, linkAddress);
        CS101_Master_useSlaveAddress(master, linkAddress);

        printf("Connecting with params: linkAddress=%d, originatorAddress=%d, asduAddress=%d, t0=%d, t1=%d, t2=%d, reconnectDelay=%d, queueSize=%d, clientID: %s\n",
               linkAddress, originatorAddress, asduAddress, t0, t1, t2, reconnectDelay, queueSize, clientID.c_str());
        fflush(stdout);

        running = true;
        _thread = std::thread([this, portName, baudRate, linkAddress, t0, t1, t2, reconnectDelay, queueSize]() {
            try {
                int retryCount = 0;
                while (running) {
                    printf("Attempting to connect (attempt %d), clientID: %s\n", retryCount + 1, clientID.c_str());
                    fflush(stdout);
                    bool connectSuccess = SerialPort_open(serialPort);
                    if (connectSuccess) {
                        printf("Serial port opened successfully, starting master, clientID: %s\n", clientID.c_str());
                        fflush(stdout);
                        CS101_Master_start(master);
                        Thread_sleep(1000); // Даём время на установку соединения
                        {
                            std::lock_guard<std::mutex> lock(this->connMutex);
                            connected = true;
                            activated = true;

                            // Отправляем начальный запрос опроса
                            CS101_AppLayerParameters alParams = CS101_Master_getAppLayerParameters(master);
                            CS101_ASDU asdu = CS101_ASDU_create(alParams, false, CS101_COT_REQUEST, 0, this->asduAddress, false, false);
                            CS101_ASDU_setTypeID(asdu, C_IC_NA_1);
                            InformationObject io = (InformationObject)InterrogationCommand_create(NULL, 0, IEC60870_QOI_STATION);
                            CS101_ASDU_addInformationObject(asdu, io);
                            CS101_Master_sendASDU(master, asdu);
                            printf("Initial interrogation sent to slave %d, typeId=100, ioa=0, value=20, clientID: %s\n", linkAddress, clientID.c_str());
                            fflush(stdout);
                            InformationObject_destroy(io);
                            CS101_ASDU_destroy(asdu);

                            tsfn.NonBlockingCall([this, linkAddress](Napi::Env env, Function jsCallback) {
                                Object eventObj = Object::New(env);
                                eventObj.Set("clientID", String::New(env, clientID.c_str()));
                                eventObj.Set("type", String::New(env, "control"));
                                eventObj.Set("event", String::New(env, "opened"));
                                eventObj.Set("reason", String::New(env, "link layer manually activated"));
                                eventObj.Set("slaveAddress", Napi::Number::New(env, linkAddress));
                                std::vector<napi_value> args = {String::New(env, "data"), eventObj};
                                jsCallback.Call(args);
                            });
                        }

                        while (running) {
                            CS101_Master_run(master);
                            Thread_sleep(100);
                            {
                                std::lock_guard<std::mutex> lock(this->connMutex);
                                if (!connected) break;
                            }
                        }

                        std::lock_guard<std::mutex> lock(this->connMutex);
                        if (running && !connected) {
                            printf("Connection lost, preparing to reconnect, clientID: %s\n", clientID.c_str());
                            fflush(stdout);
                            CS101_Master_stop(master);
                            CS101_Master_destroy(master);
                            SerialPort_destroy(serialPort);

                            printf("Recreating serial port and master, clientID: %s\n", clientID.c_str());
                            fflush(stdout);
                            serialPort = SerialPort_create(portName.c_str(), baudRate, 8, 'E', 1);
                            if (!serialPort) {
                                printf("Failed to recreate serial port object, clientID: %s\n", clientID.c_str());
                                fflush(stdout);
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
                            alParams.originatorAddress = this->originatorAddress;
                            alParams.sizeOfCA = 2;
                            alParams.sizeOfIOA = 3;
                            alParams.maxSizeOfASDU = 249;

                            master = CS101_Master_createEx(serialPort, &llParams, &alParams, IEC60870_LINK_LAYER_UNBALANCED, queueSize);
                            if (!master) {
                                SerialPort_destroy(serialPort);
                                printf("Failed to recreate master object, clientID: %s\n", clientID.c_str());
                                fflush(stdout);
                                throw runtime_error("Failed to recreate master object for reconnect");
                            }

                            CS101_Master_setASDUReceivedHandler(master, RawMessageHandler, this);
                            CS101_Master_setLinkLayerStateChanged(master, LinkLayerStateChanged, this);
                            CS101_Master_setOwnAddress(master, linkAddress);
                            CS101_Master_useSlaveAddress(master, linkAddress);
                        } else if (!running && connected) {
                            printf("Thread stopped by client, closing connection, clientID: %s\n", clientID.c_str());
                            fflush(stdout);
                            CS101_Master_stop(master);
                            CS101_Master_destroy(master);
                            SerialPort_destroy(serialPort);
                            connected = false;
                            activated = false;
                            return;
                        }
                    } else {
                        printf("Serial port failed to open, clientID: %s\n", clientID.c_str());
                        fflush(stdout);
                        tsfn.NonBlockingCall([=](Napi::Env env, Function jsCallback) {
                            Object eventObj = Object::New(env);
                            eventObj.Set("clientID", String::New(env, clientID.c_str()));
                            eventObj.Set("type", String::New(env, "control"));
                            eventObj.Set("event", String::New(env, "reconnecting"));
                            eventObj.Set("reason", String::New(env, string("attempt ") + to_string(retryCount + 1)));
                            std::vector<napi_value> args = {String::New(env, "data"), eventObj};
                            jsCallback.Call(args);
                        });
                    }

                    if (running && !connected) {
                        retryCount++;
                        printf("Reconnection attempt %d failed, retrying in %d seconds, clientID: %s\n", retryCount, reconnectDelay, clientID.c_str());
                        fflush(stdout);
                        Thread_sleep(reconnectDelay * 1000);
                    }
                }
            } catch (const std::exception& e) {
                printf("Exception in connection thread: %s, clientID: %s\n", e.what(), clientID.c_str());
                fflush(stdout);
                std::lock_guard<std::mutex> lock(this->connMutex);
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
        printf("Exception in Connect: %s, clientID: %s\n", e.what(), clientID.c_str());
        fflush(stdout);
        Napi::Error::New(env, string("Connect failed: ") + e.what()).ThrowAsJavaScriptException();
        return env.Undefined();
    }
}

Napi::Value IEC101MasterUnbalanced::Disconnect(const CallbackInfo &info) {
    Napi::Env env = info.Env();
    {
        std::lock_guard<std::mutex> lock(connMutex);
        if (running) {
            running = false;
            if (connected) {
                printf("Disconnect called by client, clientID: %s\n", clientID.c_str());
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

Napi::Value IEC101MasterUnbalanced::SendStartDT(const CallbackInfo &info) {
    Napi::Env env = info.Env();
    std::lock_guard<std::mutex> lock(connMutex);
    if (!connected) {
        Napi::Error::New(env, "Not connected").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    printf("SendStartDT called, activating link layer, clientID: %s\n", clientID.c_str());
    CS101_Master_start(master);
    return Boolean::New(env, true);
}

Napi::Value IEC101MasterUnbalanced::SendStopDT(const CallbackInfo &info) {
    Napi::Env env = info.Env();
    std::lock_guard<std::mutex> lock(connMutex);
    if (!connected || !activated) {
        Napi::Error::New(env, "Not connected or not activated").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    printf("SendStopDT called, stopping link layer, clientID: %s\n", clientID.c_str());
    CS101_Master_stop(master);
    activated = false;
    return Boolean::New(env, true);
}

Napi::Value IEC101MasterUnbalanced::SendCommands(const CallbackInfo &info) {
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
            Napi::Value value = cmdObj.Get("value");

            printf("Sending command: typeId=%d, ioa=%d, value=%f, clientID: %s\n", typeId, ioa, value.ToNumber().DoubleValue(), clientID.c_str());
            CS101_ASDU asdu = CS101_ASDU_create(alParams, false, CS101_COT_ACTIVATION, 0, this->asduAddress, false, false); // Используем asduAddress

            switch (typeId) {
                case C_SC_NA_1: {
                    if (!value.IsBoolean()) {
                        Napi::TypeError::New(env, "C_SC_NA_1 requires 'value' as boolean").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    bool val = value.As<Napi::Boolean>();
                    SingleCommand sc = SingleCommand_create(NULL, ioa, val, false, IEC60870_QUALITY_GOOD);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)sc);
                    CS101_Master_sendASDU(master, asdu);
                    SingleCommand_destroy(sc);
                    break;
                }
                case C_DC_NA_1: {
                    if (!value.IsNumber()) {
                        Napi::TypeError::New(env, "C_DC_NA_1 requires 'value' as number (0-3)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    int val = value.As<Napi::Number>().Int32Value();
                    if (val < 0 || val > 3) {
                        Napi::RangeError::New(env, "C_DC_NA_1 'value' must be 0-3").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    DoubleCommand dc = DoubleCommand_create(NULL, ioa, val, false, IEC60870_QUALITY_GOOD);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)dc);
                    CS101_Master_sendASDU(master, asdu);
                    DoubleCommand_destroy(dc);
                    break;
                }
                case C_RC_NA_1: {
                    if (!value.IsNumber()) {
                        Napi::TypeError::New(env, "C_RC_NA_1 requires 'value' as number (0-3)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    int val = value.As<Napi::Number>().Int32Value();
                    if (val < 0 || val > 3) {
                        Napi::RangeError::New(env, "C_RC_NA_1 'value' must be 0-3").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    StepCommand rc = StepCommand_create(NULL, ioa, (StepCommandValue)val, false, IEC60870_QUALITY_GOOD);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)rc);
                    CS101_Master_sendASDU(master, asdu);
                    StepCommand_destroy(rc);
                    break;
                }
                case C_SE_NA_1: {
                    if (!value.IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_NA_1 requires 'value' as number (-1.0 to 1.0)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    float val = value.As<Napi::Number>().FloatValue();
                    if (val < -1.0f || val > 1.0f) {
                        Napi::RangeError::New(env, "C_SE_NA_1 'value' must be between -1.0 and 1.0").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    SetpointCommandNormalized scn = SetpointCommandNormalized_create(NULL, ioa, val, false, IEC60870_QUALITY_GOOD);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)scn);
                    CS101_Master_sendASDU(master, asdu);
                    SetpointCommandNormalized_destroy(scn);
                    break;
                }
                case C_SE_NB_1: {
                    if (!value.IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_NB_1 requires 'value' as number (-32768 to 32767)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    int val = value.As<Napi::Number>().Int32Value();
                    if (val < -32768 || val > 32767) {
                        Napi::RangeError::New(env, "C_SE_NB_1 'value' must be between -32768 and 32767").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    SetpointCommandScaled scs = SetpointCommandScaled_create(NULL, ioa, val, false, IEC60870_QUALITY_GOOD);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)scs);
                    CS101_Master_sendASDU(master, asdu);
                    SetpointCommandScaled_destroy(scs);
                    break;
                }
                case C_SE_NC_1: {
                    if (!value.IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_NC_1 requires 'value' as number").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    float val = value.As<Napi::Number>().FloatValue();
                    SetpointCommandShort scsf = SetpointCommandShort_create(NULL, ioa, val, false, IEC60870_QUALITY_GOOD);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)scsf);
                    CS101_Master_sendASDU(master, asdu);
                    SetpointCommandShort_destroy(scsf);
                    break;
                }
                case C_BO_NA_1: {
                    if (!value.IsNumber()) {
                        Napi::TypeError::New(env, "C_BO_NA_1 requires 'value' as number (32-bit unsigned integer)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    uint32_t val = value.As<Napi::Number>().Uint32Value();
                    Bitstring32Command bc = Bitstring32Command_create(NULL, ioa, val);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)bc);
                    CS101_Master_sendASDU(master, asdu);
                    Bitstring32Command_destroy(bc);
                    break;
                }
                case C_SC_TA_1: {
                    if (!value.IsBoolean() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_SC_TA_1 requires 'value' (boolean) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    bool val = value.As<Napi::Boolean>();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    SingleCommandWithCP56Time2a sc = SingleCommandWithCP56Time2a_create(NULL, ioa, val, false, IEC60870_QUALITY_GOOD, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)sc);
                    CS101_Master_sendASDU(master, asdu);
                    SingleCommandWithCP56Time2a_destroy(sc);
                    break;
                }
                case C_DC_TA_1: {
                    if (!value.IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_DC_TA_1 requires 'value' (number 0-3) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    int val = value.As<Napi::Number>().Int32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (val < 0 || val > 3) {
                        Napi::RangeError::New(env, "C_DC_TA_1 'value' must be 0-3").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    DoubleCommandWithCP56Time2a dc = DoubleCommandWithCP56Time2a_create(NULL, ioa, val, false, IEC60870_QUALITY_GOOD, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)dc);
                    CS101_Master_sendASDU(master, asdu);
                    DoubleCommandWithCP56Time2a_destroy(dc);
                    break;
                }
                case C_RC_TA_1: {
                    if (!value.IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_RC_TA_1 requires 'value' (number 0-3) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    int val = value.As<Napi::Number>().Int32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (val < 0 || val > 3) {
                        Napi::RangeError::New(env, "C_RC_TA_1 'value' must be 0-3").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    StepCommandWithCP56Time2a rc = StepCommandWithCP56Time2a_create(NULL, ioa, (StepCommandValue)val, false, IEC60870_QUALITY_GOOD, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)rc);
                    CS101_Master_sendASDU(master, asdu);
                    StepCommandWithCP56Time2a_destroy(rc);
                    break;
                }
                case C_SE_TA_1: {
                    if (!value.IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_TA_1 requires 'value' (number -1.0 to 1.0) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    float val = value.As<Napi::Number>().FloatValue();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (val < -1.0f || val > 1.0f) {
                        Napi::RangeError::New(env, "C_SE_TA_1 'value' must be between -1.0 and 1.0").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    SetpointCommandNormalizedWithCP56Time2a scn = SetpointCommandNormalizedWithCP56Time2a_create(NULL, ioa, val, false, IEC60870_QUALITY_GOOD, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)scn);
                    CS101_Master_sendASDU(master, asdu);
                    SetpointCommandNormalizedWithCP56Time2a_destroy(scn);
                    break;
                }
                case C_SE_TB_1: {
                    if (!value.IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_TB_1 requires 'value' (number -32768 to 32767) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    int val = value.As<Napi::Number>().Int32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (val < -32768 || val > 32767) {
                        Napi::RangeError::New(env, "C_SE_TB_1 'value' must be between -32768 and 32767").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    SetpointCommandScaledWithCP56Time2a scs = SetpointCommandScaledWithCP56Time2a_create(NULL, ioa, val, false, IEC60870_QUALITY_GOOD, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)scs);
                    CS101_Master_sendASDU(master, asdu);
                    SetpointCommandScaledWithCP56Time2a_destroy(scs);
                    break;
                }
                case C_SE_TC_1: {
                    if (!value.IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_TC_1 requires 'value' (number) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    float val = value.As<Napi::Number>().FloatValue();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    SetpointCommandShortWithCP56Time2a scsf = SetpointCommandShortWithCP56Time2a_create(NULL, ioa, val, false, IEC60870_QUALITY_GOOD, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)scsf);
                    CS101_Master_sendASDU(master, asdu);
                    SetpointCommandShortWithCP56Time2a_destroy(scsf);
                    break;
                }
                case C_BO_TA_1: {
                    if (!value.IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_BO_TA_1 requires 'value' (32-bit number) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    uint32_t val = value.As<Napi::Number>().Uint32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    Bitstring32CommandWithCP56Time2a bc = Bitstring32CommandWithCP56Time2a_create(NULL, ioa, val, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)bc);
                    CS101_Master_sendASDU(master, asdu);
                    Bitstring32CommandWithCP56Time2a_destroy(bc);
                    break;
                }
                case C_IC_NA_1: {
                    CS101_ASDU_setTypeID(asdu, C_IC_NA_1);
                    CS101_ASDU_setCOT(asdu, CS101_COT_REQUEST);
                    InformationObject io = (InformationObject)InterrogationCommand_create(NULL, ioa, value.As<Napi::Number>().Uint32Value());
                    CS101_ASDU_addInformationObject(asdu, io);
                    CS101_Master_sendASDU(master, asdu);
                    InformationObject_destroy(io);
                    break;
                }
                case C_CI_NA_1: {
                    CS101_ASDU_setTypeID(asdu, C_CI_NA_1);
                    CS101_ASDU_setCOT(asdu, CS101_COT_REQUEST);
                    InformationObject io = (InformationObject)CounterInterrogationCommand_create(NULL, ioa, value.As<Napi::Number>().Uint32Value());
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
                    if (!value.IsNumber()) {
                        Napi::TypeError::New(env, "C_CS_NA_1 requires 'value' as number (timestamp)").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    uint64_t val = value.As<Napi::Number>().Int64Value();
                    CS101_ASDU_setTypeID(asdu, C_CS_NA_1);
                    CS101_ASDU_setCOT(asdu, CS101_COT_ACTIVATION);
                    InformationObject io = (InformationObject)ClockSynchronizationCommand_create(NULL, ioa, CP56Time2a_createFromMsTimestamp(NULL, val));
                    CS101_ASDU_addInformationObject(asdu, io);
                    CS101_Master_sendASDU(master, asdu);
                    InformationObject_destroy(io);
                    break;
                }
                default:
                    printf("Unsupported command type: %d, clientID: %s\n", typeId, clientID.c_str());
                    CS101_ASDU_destroy(asdu);
                    allSuccess = false;
                    continue;
            }

            CS101_ASDU_destroy(asdu);
            printf("Sent command: typeId=%d, ioa=%d, clientID: %s\n", typeId, ioa, clientID.c_str());
        }
        return Boolean::New(env, allSuccess);
    } catch (const std::exception& e) {
        printf("Exception in SendCommands: %s, clientID: %s\n", e.what(), clientID.c_str());
        Napi::Error::New(env, string("SendCommands failed: ") + e.what()).ThrowAsJavaScriptException();
        return Boolean::New(env, false);
    }
}

Napi::Value IEC101MasterUnbalanced::GetStatus(const CallbackInfo &info) {
    Napi::Env env = info.Env();
    std::lock_guard<std::mutex> lock(connMutex);
    Object status = Object::New(env);
    status.Set("connected", Boolean::New(env, connected));
    status.Set("activated", Boolean::New(env, activated));
    status.Set("clientID", String::New(env, clientID.c_str()));
    return status;
}

Napi::Value IEC101MasterUnbalanced::AddSlave(const CallbackInfo &info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsNumber()) {
        Napi::TypeError::New(env, "Expected slaveAddress (number)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    int slaveAddress = info[0].As<Number>().Int32Value();

    if (slaveAddress < 0 || slaveAddress > 255) {
        Napi::RangeError::New(env, "slaveAddress must be 0-255").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::lock_guard<std::mutex> lock(connMutex);
    if (!connected) {
        Napi::Error::New(env, "Not connected").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    CS101_Master_addSlave(master, slaveAddress);
    printf("Added slave with address %d, clientID: %s\n", slaveAddress, clientID.c_str());

    return env.Undefined();
}

Napi::Value IEC101MasterUnbalanced::PollSlave(const CallbackInfo &info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsNumber()) {
        Napi::TypeError::New(env, "Expected slaveAddress (number)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    int slaveAddress = info[0].As<Number>().Int32Value();

    if (slaveAddress < 0 || slaveAddress > 255) {
        Napi::RangeError::New(env, "slaveAddress must be 0-255").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::lock_guard<std::mutex> lock(connMutex);
    if (!connected || !activated) {
        Napi::Error::New(env, "Not connected or not activated").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    LinkLayerState state = LL_STATE_IDLE; // Замените на актуальный getter, если доступен
    printf("Polling slave with address %d, clientID: %s, link state: %d (0=IDLE, 1=ERROR, 2=BUSY, 3=AVAILABLE)\n", 
           slaveAddress, clientID.c_str(), state);
    CS101_Master_pollSingleSlave(master, slaveAddress);
    printf("Poll completed for slave %d, clientID: %s\n", slaveAddress, clientID.c_str());

    return Boolean::New(env, true);
}

void IEC101MasterUnbalanced::LinkLayerStateChanged(void *parameter, int address, LinkLayerState state) {
    IEC101MasterUnbalanced *client = static_cast<IEC101MasterUnbalanced *>(parameter);
    std::string eventStr;
    std::string reason;

    printf("LinkLayerStateChanged called, address: %d, state: %d, clientID: %s\n", address, state, client->clientID.c_str());

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

    printf("Link layer event: %s, reason: %s, clientID: %s, slaveAddress: %d\n", eventStr.c_str(), reason.c_str(), client->clientID.c_str(), address);

    client->tsfn.NonBlockingCall([=](Napi::Env env, Function jsCallback) {
        Object eventObj = Object::New(env);
        eventObj.Set("clientID", String::New(env, client->clientID.c_str()));
        eventObj.Set("type", String::New(env, "control"));
        eventObj.Set("event", String::New(env, eventStr));
        eventObj.Set("reason", String::New(env, reason));
        eventObj.Set("slaveAddress", Number::New(env, address));
        std::vector<napi_value> args = {String::New(env, "data"), eventObj};
        jsCallback.Call(args);
    });
}

bool IEC101MasterUnbalanced::RawMessageHandler(void *parameter, int address, CS101_ASDU asdu) {
    IEC101MasterUnbalanced *client = static_cast<IEC101MasterUnbalanced *>(parameter);
    IEC60870_5_TypeID typeID = CS101_ASDU_getTypeID(asdu);
    int numberOfElements = CS101_ASDU_getNumberOfElements(asdu);
    int receivedAsduAddress = CS101_ASDU_getCA(asdu); // Получаем адрес ASDU из полученного сообщения

    printf("RawMessageHandler called, address: %d, typeID: %d, elements: %d, clientID: %s\n", address, typeID, numberOfElements, client->clientID.c_str());

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
            printf("ASDU type: %s, clientID: %s, asduAddress: %d, ioa: %i, value: %f, quality: %u, timestamp: %" PRIu64 ", cnt: %i, slaveAddress: %d\n",
                   TypeID_toString(typeID), client->clientID.c_str(), receivedAsduAddress, ioa, val, quality, timestamp, client->cnt, address);
        }

        client->tsfn.NonBlockingCall([=](Napi::Env env, Function jsCallback) {
            Napi::Array jsArray = Napi::Array::New(env, elements.size());
            for (size_t i = 0; i < elements.size(); i++) {
                const auto& [ioa, val, quality, timestamp] = elements[i];
                Napi::Object msg = Napi::Object::New(env);
                msg.Set("clientID", String::New(env, client->clientID.c_str()));
                msg.Set("typeId", Number::New(env, typeID));
                msg.Set("asdu", Number::New(env, receivedAsduAddress)); // Добавляем полученный asduAddress
                msg.Set("ioa", Number::New(env, ioa));
                msg.Set("val", Number::New(env, val));
                msg.Set("quality", Number::New(env, quality));
                msg.Set("slaveAddress", Number::New(env, address));
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
        printf("Exception in RawMessageHandler: %s, clientID: %s\n", e.what(), client->clientID.c_str());
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