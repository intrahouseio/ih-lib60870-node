#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "cs101_slave1.h"
#include <inttypes.h>
#include <stdexcept>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace Napi;
using namespace std;

Napi::FunctionReference IEC101Slave::constructor;

Napi::Object IEC101Slave::Init(Napi::Env env, Napi::Object exports) {
    Napi::Function func = DefineClass(env, "IEC101Slave", {
        InstanceMethod("connect", &IEC101Slave::Connect),
        InstanceMethod("disconnect", &IEC101Slave::Disconnect),
        InstanceMethod("sendCommands", &IEC101Slave::SendCommands),
        InstanceMethod("getStatus", &IEC101Slave::GetStatus)
    });
    constructor = Napi::Persistent(func);
    constructor.SuppressDestruct();
    exports.Set("IEC101Slave", func);
    return exports;
}

IEC101Slave::IEC101Slave(const Napi::CallbackInfo& info) : Napi::ObjectWrap<IEC101Slave>(info) {
    if (info.Length() < 1 || !info[0].IsFunction()) {
        Napi::TypeError::New(info.Env(), "Expected a callback function").ThrowAsJavaScriptException();
        return;
    }

    Napi::Function emit = info[0].As<Napi::Function>();
    running = false;

    try {
        tsfn = Napi::ThreadSafeFunction::New(
            info.Env(),
            emit,
            "IEC101SlaveTSFN",
            0,
            1,
            [](Napi::Env) {}
        );
    } catch (const std::exception& e) {
        printf("Failed to create ThreadSafeFunction: %s\n", e.what());
        Napi::Error::New(info.Env(), string("TSFN creation failed: ") + e.what()).ThrowAsJavaScriptException();
    }
}

IEC101Slave::~IEC101Slave() {
    std::lock_guard<std::mutex> lock(connMutex);
    if (running) {
        running = false;
        if (connected) {
            printf("Destructor closing connection, clientID: %s, clientId: %i\n", clientID.c_str(), clientId);
            CS101_Slave_stop(slave);
            CS101_Slave_destroy(slave);
            SerialPort_destroy(serialPort);
            connected = false;
        }
        if (_thread.joinable()) {
            _thread.join();
        }
        tsfn.Release();
    }
}

Napi::Value IEC101Slave::Connect(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 4 || !info[0].IsString() || !info[1].IsNumber() || !info[2].IsNumber() || !info[3].IsString()) {
        Napi::TypeError::New(env, "Expected port (string), baudRate (number), clientId (number), clientID (string), [params (object)]").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::string portName = info[0].As<Napi::String>();
    int baudRate = info[1].As<Napi::Number>().Int32Value();
    clientId = info[2].As<Napi::Number>().Int32Value();
    clientID = info[3].As<Napi::String>();

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
    int originatorAddress = 0;
    int t0 = 30;
    int t1 = 15;
    int t2 = 10;
    int reconnectDelay = 5;
    int maxRetries = 10;
    int queueSize = 100;

    if (info.Length() > 4 && info[4].IsObject()) {
        Napi::Object params = info[4].As<Napi::Object>();
        if (params.Has("linkAddress")) linkAddress = params.Get("linkAddress").As<Napi::Number>().Int32Value();
        if (params.Has("originatorAddress")) originatorAddress = params.Get("originatorAddress").As<Napi::Number>().Int32Value();
        if (params.Has("t0")) t0 = params.Get("t0").As<Napi::Number>().Int32Value();
        if (params.Has("t1")) t1 = params.Get("t1").As<Napi::Number>().Int32Value();
        if (params.Has("t2")) t2 = params.Get("t2").As<Napi::Number>().Int32Value();
        if (params.Has("reconnectDelay")) reconnectDelay = params.Get("reconnectDelay").As<Napi::Number>().Int32Value();
        if (params.Has("maxRetries")) maxRetries = params.Get("maxRetries").As<Napi::Number>().Int32Value();
        if (params.Has("queueSize")) queueSize = params.Get("queueSize").As<Napi::Number>().Int32Value();

        if (linkAddress < 0 || linkAddress > 255) {
            Napi::Error::New(env, "linkAddress must be 0-255").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        if (originatorAddress < 0 || originatorAddress > 255) {
            Napi::Error::New(env, "originatorAddress must be 0-255").ThrowAsJavaScriptException();
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

        LinkLayerParameters llParams = LinkLayerParameters();
        llParams->addressLength = 1;
        llParams->timeoutForAck = t1 * 1000;
        llParams->timeoutRepeat = t2 * 1000;
        llParams->timeoutLinkState = t0 * 1000;
        llParams->useSingleCharACK = true;

        CS101_AppLayerParameters alParams = CS101_AppLayerParameters();
        alParams->sizeOfTypeId = 1;
        alParams->sizeOfVSQ = 1;
        alParams->sizeOfCOT = 2;
        alParams->originatorAddress = originatorAddress;
        alParams->sizeOfCA = 2;
        alParams->sizeOfIOA = 3;
        alParams->maxSizeOfASDU = 249;

        slave = CS101_Slave_createEx(serialPort, llParams, alParams, IEC60870_LINK_LAYER_UNBALANCED, queueSize, queueSize);
        if (!slave) {
            SerialPort_destroy(serialPort);
            throw runtime_error("Failed to create slave object");
        }

        CS101_Slave_setASDUHandler(slave, RawMessageHandler, this);
        CS101_Slave_setLinkLayerStateChanged(slave, LinkLayerStateChanged, this);
        CS101_Slave_setLinkLayerAddress(slave, linkAddress);

        printf("Connecting with params: linkAddress=%d, originatorAddress=%d, t0=%d, t1=%d, t2=%d, reconnectDelay=%d, maxRetries=%d, queueSize=%d, clientID: %s, clientId: %i\n",
               linkAddress, originatorAddress, t0, t1, t2, reconnectDelay, maxRetries, queueSize, clientID.c_str(), clientId);

        running = true;
        _thread = std::thread([this, portName, baudRate, linkAddress, originatorAddress, t0, t1, t2, reconnectDelay, maxRetries, queueSize] {
            try {
                int retryCount = 0;
                while (running && retryCount <= maxRetries) {
                    SerialPort_open(serialPort);
                    CS101_Slave_start(slave);
                    {
                        std::lock_guard<std::mutex> lock(connMutex);
                        connected = true;
                    }
                    printf("Attempting to connect (attempt %d/%d), clientID: %s, clientId: %i\n", retryCount + 1, maxRetries + 1, clientID.c_str(), clientId);

                    while (running) {
                        CS101_Slave_run(slave);
                        Thread_sleep(100);
                        std::lock_guard<std::mutex> lock(connMutex);
                        if (!connected) break;
                    }

                    std::lock_guard<std::mutex> lock(connMutex);
                    if (running && !connected) {
                        printf("Connection lost, preparing to reconnect, clientID: %s, clientId: %i\n", clientID.c_str(), clientId);
                        CS101_Slave_stop(slave);
                        CS101_Slave_destroy(slave);
                        SerialPort_destroy(serialPort);

                        serialPort = SerialPort_create(portName.c_str(), baudRate, 8, 'E', 1);
                        if (!serialPort) {
                            throw runtime_error("Failed to recreate serial port object for reconnect");
                        }

                        LinkLayerParameters llParams = LinkLayerParameters();
                        llParams->addressLength = 1;
                        llParams->timeoutForAck = t1 * 1000;
                        llParams->timeoutRepeat = t2 * 1000;
                        llParams->timeoutLinkState = t0 * 1000;
                        llParams->useSingleCharACK = true;

                        CS101_AppLayerParameters alParams = CS101_AppLayerParameters();
                        alParams->sizeOfTypeId = 1;
                        alParams->sizeOfVSQ = 1;
                        alParams->sizeOfCOT = 2;
                        alParams->originatorAddress = originatorAddress;
                        alParams->sizeOfCA = 2;
                        alParams->sizeOfIOA = 3;
                        alParams->maxSizeOfASDU = 249;

                        slave = CS101_Slave_createEx(serialPort, llParams, alParams, IEC60870_LINK_LAYER_UNBALANCED, queueSize, queueSize);
                        if (!slave) {
                            SerialPort_destroy(serialPort);
                            throw runtime_error("Failed to recreate slave object for reconnect");
                        }

                        CS101_Slave_setASDUHandler(slave, RawMessageHandler, this);
                        CS101_Slave_setLinkLayerStateChanged(slave, LinkLayerStateChanged, this);
                        CS101_Slave_setLinkLayerAddress(slave, linkAddress);
                    } else if (!running && connected) {
                        printf("Thread stopped by client, closing connection, clientID: %s, clientId: %i\n", clientID.c_str(), clientId);
                        CS101_Slave_stop(slave);
                        CS101_Slave_destroy(slave);
                        SerialPort_destroy(serialPort);
                        connected = false;
                        return;
                    }

                    if (running && !connected) {
                        retryCount++;
                        printf("Reconnection attempt %d/%d failed, retrying in %d seconds, clientID: %s, clientId: %i\n", retryCount, maxRetries, clientID.c_str(), clientId);
                        tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
                            Napi::Object eventObj = Napi::Object::New(env);
                            eventObj.Set("clientID", Napi::String::New(env, clientID.c_str()));
                            eventObj.Set("type", Napi::String::New(env, "control"));
                            eventObj.Set("event", Napi::String::New(env, "reconnecting"));
                            eventObj.Set("reason", Napi::String::New(env, string("attempt ") + to_string(retryCount) + " of " + to_string(maxRetries)));
                            std::vector<napi_value> args = {Napi::String::New(env, "data"), eventObj};
                            jsCallback.Call(args);
                        });
                        Thread_sleep(reconnectDelay * 1000);
                    }

                    if (retryCount >= maxRetries) {
                        running = false;
                        printf("Max reconnection attempts reached, giving up, clientID: %s, clientId: %i\n", clientID.c_str(), clientId);
                        tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
                            Napi::Object eventObj = Napi::Object::New(env);
                            eventObj.Set("clientID", Napi::String::New(env, clientID.c_str()));
                            eventObj.Set("type", Napi::String::New(env, "control"));
                            eventObj.Set("event", Napi::String::New(env, "failed"));
                            eventObj.Set("reason", Napi::String::New(env, "max reconnection attempts reached"));
                            std::vector<napi_value> args = {Napi::String::New(env, "data"), eventObj};
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
                    CS101_Slave_stop(slave);
                    CS101_Slave_destroy(slave);
                    SerialPort_destroy(serialPort);
                    connected = false;
                }
                tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
                    Napi::Object eventObj = Napi::Object::New(env);
                    eventObj.Set("clientID", Napi::String::New(env, clientID.c_str()));
                    eventObj.Set("type", Napi::String::New(env, "control"));
                    eventObj.Set("event", Napi::String::New(env, "error"));
                    eventObj.Set("reason", Napi::String::New(env, string("Thread exception: ") + e.what()));
                    std::vector<napi_value> args = {Napi::String::New(env, "data"), eventObj};
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

Napi::Value IEC101Slave::Disconnect(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    {
        std::lock_guard<std::mutex> lock(connMutex);
        if (running) {
            running = false;
            if (connected) {
                printf("Disconnect called by client, clientID: %s, clientId: %i\n", clientID.c_str(), clientId);
                CS101_Slave_stop(slave);
                CS101_Slave_destroy(slave);
                SerialPort_destroy(serialPort);
                connected = false;
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

Napi::Value IEC101Slave::SendCommands(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsArray()) {
        Napi::TypeError::New(env, "Expected commands (array of objects with 'typeId', 'ioa', 'value', and optional fields)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    Napi::Array commands = info[0].As<Napi::Array>();

    std::lock_guard<std::mutex> lock(connMutex);
    if (!connected || !masterConnection) {
        Napi::Error::New(env, "Not connected or no master connection available").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    CS101_AppLayerParameters alParams = CS101_Slave_getAppLayerParameters(slave);

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

            bool bselCmd = false;
            int ql = 0;
            int cot = CS101_COT_SPONTANEOUS;
            uint8_t quality = IEC60870_QUALITY_GOOD;

            int typeId = cmdObj.Get("typeId").As<Napi::Number>().Int32Value();
            int ioa = cmdObj.Get("ioa").As<Napi::Number>().Int32Value();
            if (cmdObj.Has("bselCmd") && cmdObj.Get("bselCmd").IsBoolean()) {
                bselCmd = cmdObj.Get("bselCmd").As<Napi::Boolean>();
            }
            if (cmdObj.Has("ql") && cmdObj.Get("ql").IsNumber()) {
                ql = cmdObj.Get("ql").As<Napi::Number>().Int32Value();
                if (ql < 0 || ql > 31) {
                    Napi::RangeError::New(env, "ql must be between 0 and 31").ThrowAsJavaScriptException();
                    return env.Undefined();
                }
            }
            if (cmdObj.Has("cot") && cmdObj.Get("cot").IsNumber()) {
                cot = cmdObj.Get("cot").As<Napi::Number>().Int32Value();
                if (cot < 0 || cot > 63) {
                    Napi::RangeError::New(env, "cot must be between 0 and 63").ThrowAsJavaScriptException();
                    return env.Undefined();
                }
            }
            if (cmdObj.Has("quality") && cmdObj.Get("quality").IsNumber()) {
                quality = cmdObj.Get("quality").As<Napi::Number>().Uint32Value();
            }
            uint64_t timestamp = cmdObj.Has("timestamp") ? cmdObj.Get("timestamp").As<Napi::Number>().Int64Value() : 0;

          

            CS101_ASDU asdu = CS101_ASDU_create(alParams, false, (CS101_CauseOfTransmission)cot, 0, asduAddress, false, false);

            bool success = false;
             switch (typeId) {
               case M_SP_NA_1: {
                    if (!cmdObj.Get("value").IsBoolean()) {
                        Napi::TypeError::New(env, "M_SP_NA_1 requires 'value' as boolean").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    bool value = cmdObj.Get("value").As<Napi::Boolean>();
                    SinglePointInformation sp = SinglePointInformation_create(NULL, ioa, value, quality);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)sp);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    SinglePointInformation_destroy(sp);
                    break;
                }
                case M_DP_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "M_DP_NA_1 requires 'value' as number (0-3)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    if (value < 0 || value > 3) {
                        Napi::RangeError::New(env, "M_DP_NA_1 'value' must be 0-3").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    DoublePointInformation dp = DoublePointInformation_create(NULL, ioa, (DoublePointValue)value, quality);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)dp);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    DoublePointInformation_destroy(dp);
                    break;
                }
                case M_ST_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "M_ST_NA_1 requires 'value' as number (-64 to 63)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    if (value < -64 || value > 63) {
                        Napi::RangeError::New(env, "M_ST_NA_1 'value' must be between -64 and 63").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    StepPositionInformation st = StepPositionInformation_create(NULL, ioa, value, false, quality);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)st);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    StepPositionInformation_destroy(st);
                    break;
                }
                case M_BO_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "M_BO_NA_1 requires 'value' as number (32-bit unsigned integer)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    uint32_t value = cmdObj.Get("value").As<Napi::Number>().Uint32Value();
                    BitString32 bo = BitString32_create(NULL, ioa, value);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)bo);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    BitString32_destroy(bo);
                    break;
                }
               case M_ME_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "M_ME_NA_1 requires 'value' as number (-1.0 to 1.0)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    float value = cmdObj.Get("value").As<Napi::Number>().FloatValue();
                    if (value < -1.0f || value > 1.0f) {
                        Napi::RangeError::New(env, "M_ME_NA_1 'value' must be between -1.0 and 1.0").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    MeasuredValueNormalized mn = MeasuredValueNormalized_create(NULL, ioa, value, quality);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)mn);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    MeasuredValueNormalized_destroy(mn);
                    break;
                }
                case M_ME_NB_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "M_ME_NB_1 requires 'value' as number (-32768 to 32767)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    double doubleValue = cmdObj.Get("value").As<Napi::Number>().DoubleValue();
                    int value = static_cast<int>(doubleValue);
                    if (value < -32768 || value > 32767) {
                        Napi::RangeError::New(env, "M_ME_NB_1 'value' must be between -32768 and 32767").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    MeasuredValueScaled ms = MeasuredValueScaled_create(NULL, ioa, value, quality);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)ms);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    MeasuredValueScaled_destroy(ms);
                    break;
                }
                case M_ME_NC_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "M_ME_NC_1 requires 'value' as number").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    double doubleValue = cmdObj.Get("value").As<Napi::Number>().DoubleValue();                  
                    float value = static_cast<float>(doubleValue);
                    MeasuredValueShort mc = MeasuredValueShort_create(NULL, ioa, value, quality);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)mc);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    MeasuredValueShort_destroy(mc);
                    break;
                }
                case M_IT_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "M_IT_NA_1 requires 'value' as number").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    BinaryCounterReading bcr = BinaryCounterReading_create(NULL, value, 0, false, false, false);
                    IntegratedTotals it = IntegratedTotals_create(NULL, ioa, bcr);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)it);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    IntegratedTotals_destroy(it);
                    BinaryCounterReading_destroy(bcr);
                    break;
                }
                case M_SP_TB_1: {
                    if (!cmdObj.Get("value").IsBoolean() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "M_SP_TB_1 requires 'value' (boolean) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    bool value = cmdObj.Get("value").As<Napi::Boolean>();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    SinglePointWithCP56Time2a sp = SinglePointWithCP56Time2a_create(NULL, ioa, value, quality, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)sp);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    SinglePointWithCP56Time2a_destroy(sp);
                    break;
                }
                case M_DP_TB_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "M_DP_TB_1 requires 'value' (number 0-3) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (value < 0 || value > 3) {
                        Napi::RangeError::New(env, "M_DP_TB_1 'value' must be 0-3").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    DoublePointWithCP56Time2a dp = DoublePointWithCP56Time2a_create(NULL, ioa, (DoublePointValue)value, quality, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)dp);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    DoublePointWithCP56Time2a_destroy(dp);
                    break;
                }
                case M_ST_TB_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "M_ST_TB_1 requires 'value' (number -64 to 63) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (value < -64 || value > 63) {
                        Napi::RangeError::New(env, "M_ST_TB_1 'value' must be between -64 and 63").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    StepPositionWithCP56Time2a st = StepPositionWithCP56Time2a_create(NULL, ioa, value, false, quality, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)st);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    StepPositionWithCP56Time2a_destroy(st);
                    break;
                }
                case M_BO_TB_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "M_BO_TB_1 requires 'value' (32-bit number) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    uint32_t value = cmdObj.Get("value").As<Napi::Number>().Uint32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    Bitstring32WithCP56Time2a bo = Bitstring32WithCP56Time2a_create(NULL, ioa, value, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)bo);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    Bitstring32WithCP56Time2a_destroy(bo);
                    break;
                }
                case M_ME_TD_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "M_ME_TD_1 requires 'value' (number -1.0 to 1.0) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    double doubleValue = cmdObj.Get("value").As<Napi::Number>().DoubleValue();                  
                    float value = static_cast<float>(doubleValue);
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (value < -1.0f || value > 1.0f) {
                        Napi::RangeError::New(env, "M_ME_TD_1 'value' must be between -1.0 and 1.0").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    MeasuredValueNormalizedWithCP56Time2a mn = MeasuredValueNormalizedWithCP56Time2a_create(NULL, ioa, value, quality, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)mn);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    MeasuredValueNormalizedWithCP56Time2a_destroy(mn);
                    break;
                }
                case M_ME_TE_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "M_ME_TE_1 requires 'value' (number -32768 to 32767) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (value < -32768 || value > 32767) {
                        Napi::RangeError::New(env, "M_ME_TE_1 'value' must be between -32768 and 32767").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    MeasuredValueScaledWithCP56Time2a ms = MeasuredValueScaledWithCP56Time2a_create(NULL, ioa, value, quality, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)ms);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    MeasuredValueScaledWithCP56Time2a_destroy(ms);
                    break;
                }
                case M_ME_TF_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "M_ME_TF_1 requires 'value' (number) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                   double doubleValue = cmdObj.Get("value").As<Napi::Number>().DoubleValue();                  
                    float value = static_cast<float>(doubleValue);
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    MeasuredValueShortWithCP56Time2a mc = MeasuredValueShortWithCP56Time2a_create(NULL, ioa, value, quality, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)mc);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    MeasuredValueShortWithCP56Time2a_destroy(mc);
                    break;
                }
                case M_IT_TB_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "M_IT_TB_1 requires 'value' (number) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    BinaryCounterReading bcr = BinaryCounterReading_create(NULL, value, 0, false, false, false);
                    IntegratedTotalsWithCP56Time2a it = IntegratedTotalsWithCP56Time2a_create(NULL, ioa, bcr, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)it);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    IntegratedTotalsWithCP56Time2a_destroy(it);
                    BinaryCounterReading_destroy(bcr);
                    break;
                }
                case C_SC_NA_1: {
                    if (!cmdObj.Get("value").IsBoolean()) {
                        Napi::TypeError::New(env, "C_SC_NA_1 requires 'value' as boolean").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    bool value = cmdObj.Get("value").As<Napi::Boolean>();
                    SingleCommand sc = SingleCommand_create(NULL, ioa, value, bselCmd, ql);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)sc);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    SingleCommand_destroy(sc);
                    break;
                }
                case C_DC_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_DC_NA_1 requires 'value' as number (0-3)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    if (value < 0 || value > 3) {
                        Napi::RangeError::New(env, "C_DC_NA_1 'value' must be 0-3").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    DoubleCommand dc = DoubleCommand_create(NULL, ioa, value, bselCmd, ql);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)dc);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    DoubleCommand_destroy(dc);
                    break;
                }
                case C_RC_TA_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_RC_TA_1 requires 'value' (number 0-3) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (value < 0 || value > 3) {
                        Napi::RangeError::New(env, "C_RC_TA_1 'value' must be 0-3").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    StepCommandWithCP56Time2a rc = StepCommandWithCP56Time2a_create(NULL, ioa, (StepCommandValue)value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)rc);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    StepCommandWithCP56Time2a_destroy(rc);
                    break;
                }
                case C_SE_TA_1: {
                    if (!cmdObj.Get("value").IsString() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_TA_1 requires 'value' (string representing float -1.0 to 1.0) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    std::string valueStr = cmdObj.Get("value").As<Napi::String>().Utf8Value();
                    float value;
                    try {
                        value = std::stof(valueStr);
                    } catch (...) {
                        Napi::TypeError::New(env, "C_SE_TA_1 'value' must be a valid float string").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (value < -1.0f || value > 1.0f) {
                        Napi::RangeError::New(env, "C_SE_TA_1 'value' must be between -1.0 and 1.0").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    SetpointCommandNormalizedWithCP56Time2a se = SetpointCommandNormalizedWithCP56Time2a_create(NULL, ioa, value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)se);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    SetpointCommandNormalizedWithCP56Time2a_destroy(se);
                    break;
                }
                case C_SE_NB_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_NB_1 requires 'value' as number (-32768 to 32767)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    if (value < -32768 || value > 32767) {
                        Napi::RangeError::New(env, "C_SE_NB_1 'value' must be between -32768 and 32767").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    SetpointCommandScaled se = SetpointCommandScaled_create(NULL, ioa, value, bselCmd, ql);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)se);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    SetpointCommandScaled_destroy(se);
                    break;
                }
                case C_SE_NC_1: {
                    if (!cmdObj.Get("value").IsString()) {
                        Napi::TypeError::New(env, "C_SE_NC_1 requires 'value' as string representing a float").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    std::string valueStr = cmdObj.Get("value").As<Napi::String>().Utf8Value();
                    float value;
                    try {
                        value = std::stof(valueStr);
                    } catch (...) {
                        Napi::TypeError::New(env, "C_SE_NC_1 'value' must be a valid float string").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    SetpointCommandShort se = SetpointCommandShort_create(NULL, ioa, value, bselCmd, ql);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)se);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    SetpointCommandShort_destroy(se);
                    break;
                }
                case C_BO_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_BO_NA_1 requires 'value' as number (32-bit unsigned integer)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    uint32_t value = cmdObj.Get("value").As<Napi::Number>().Uint32Value();
                    Bitstring32Command bo = Bitstring32Command_create(NULL, ioa, value);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)bo);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    Bitstring32Command_destroy(bo);
                    break;
                }
                case C_SC_TA_1: {
                    if (!cmdObj.Get("value").IsBoolean() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_SC_TA_1 requires 'value' (boolean) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    bool value = cmdObj.Get("value").As<Napi::Boolean>();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    SingleCommandWithCP56Time2a sc = SingleCommandWithCP56Time2a_create(NULL, ioa, value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)sc);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    SingleCommandWithCP56Time2a_destroy(sc);
                    break;
                }
                case C_DC_TA_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_DC_TA_1 requires 'value' (number 0-3) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (value < 0 || value > 3) {
                        Napi::RangeError::New(env, "C_DC_TA_1 'value' must be 0-3").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    DoubleCommandWithCP56Time2a dc = DoubleCommandWithCP56Time2a_create(NULL, ioa, value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)dc);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    DoubleCommandWithCP56Time2a_destroy(dc);
                    break;
                }
                case C_SE_TB_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_TB_1 requires 'value' (number -32768 to 32767) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    if (value < -32768 || value > 32767) {
                        Napi::RangeError::New(env, "C_SE_TB_1 'value' must be between -32768 and 32767").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    SetpointCommandScaledWithCP56Time2a se = SetpointCommandScaledWithCP56Time2a_create(NULL, ioa, value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)se);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    SetpointCommandScaledWithCP56Time2a_destroy(se);
                    break;
                }
                case C_SE_TC_1: {
                    if (!cmdObj.Get("value").IsString() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_TC_1 requires 'value' (string representing float) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    std::string valueStr = cmdObj.Get("value").As<Napi::String>().Utf8Value();
                    float value;
                    try {
                        value = std::stof(valueStr);
                    } catch (...) {
                        Napi::TypeError::New(env, "C_SE_TC_1 'value' must be a valid float string").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    SetpointCommandShortWithCP56Time2a se = SetpointCommandShortWithCP56Time2a_create(NULL, ioa, value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)se);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    SetpointCommandShortWithCP56Time2a_destroy(se);
                    break;
                }
                case C_BO_TA_1: {
                    if (!cmdObj.Get("value").IsNumber() || !cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_BO_TA_1 requires 'value' (32-bit number) and 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    uint32_t value = cmdObj.Get("value").As<Napi::Number>().Uint32Value();
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    Bitstring32CommandWithCP56Time2a bo = Bitstring32CommandWithCP56Time2a_create(NULL, ioa, value, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)bo);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    Bitstring32CommandWithCP56Time2a_destroy(bo);
                    break;
                }
                case C_IC_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_IC_NA_1 requires 'value' as number (QOI, 0-255)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    if (value < 0 || value > 255) {
                        Napi::RangeError::New(env, "C_IC_NA_1 'value' (QOI) must be 0-255").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    InterrogationCommand ic = InterrogationCommand_create(NULL, ioa, value);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)ic);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    InterrogationCommand_destroy(ic);
                    break;
                }
                case C_CI_NA_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_CI_NA_1 requires 'value' as number (QCC, 0-255)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    int value = cmdObj.Get("value").As<Napi::Number>().Int32Value();
                    if (value < 0 || value > 255) {
                        Napi::RangeError::New(env, "C_CI_NA_1 'value' (QCC) must be 0-255").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    CounterInterrogationCommand ci = CounterInterrogationCommand_create(NULL, ioa, value);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)ci);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    CounterInterrogationCommand_destroy(ci);
                    break;
                }
                case C_RD_NA_1: {
                    ReadCommand rd = ReadCommand_create(NULL, ioa);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)rd);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    ReadCommand_destroy(rd);
                    break;
                }
                case C_CS_NA_1: {
                    if (!cmdObj.Has("timestamp") || !cmdObj.Get("timestamp").IsNumber()) {
                        Napi::TypeError::New(env, "C_CS_NA_1 requires 'timestamp' (number)").ThrowAsJavaScriptException();
                        CS101_ASDU_destroy(asdu);
                        return env.Undefined();
                    }
                    uint64_t timestamp = cmdObj.Get("timestamp").As<Napi::Number>().Int64Value();
                    ClockSynchronizationCommand cs = ClockSynchronizationCommand_create(NULL, ioa, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)cs);
                    success = IMasterConnection_sendASDU(masterConnection, asdu);
                    ClockSynchronizationCommand_destroy(cs);
                    break;
                }

                default:
                    printf("Unsupported command type ID: %d, clientID: %s, clientId: %i\n", typeId, clientID.c_str(), clientId);
                    CS101_ASDU_destroy(asdu);
                    allSuccess = false;
                    continue;
            }

            CS101_ASDU_destroy(asdu);
            if (!success) {
                allSuccess = false;
                printf("Failed to send command: typeId=%d, ioa=%d, clientID: %s, clientId: %i\n", typeId, ioa, clientID.c_str(), clientId);
            } else {
                printf("Sent command: typeId=%d, ioa=%d, clientID: %s, clientId: %i\n", typeId, ioa, clientID.c_str(), clientId);
            }
        }
        return Napi::Boolean::New(env, allSuccess);
    } catch (const std::exception& e) {
        printf("Exception in SendCommands: %s, clientID: %s, clientId: %i\n", e.what(), clientID.c_str(), clientId);
        Napi::Error::New(env, string("SendCommands failed: ") + e.what()).ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }
}

Napi::Value IEC101Slave::GetStatus(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    std::lock_guard<std::mutex> lock(connMutex);
    Napi::Object status = Napi::Object::New(env);
    status.Set("connected", Napi::Boolean::New(env, connected));
    status.Set("clientId", Napi::Number::New(env, clientId));
    status.Set("clientID", Napi::String::New(env, clientID));
    return status;
}

void IEC101Slave::LinkLayerStateChanged(void* parameter, int address, LinkLayerState state) {
    IEC101Slave* client = static_cast<IEC101Slave*>(parameter);
    std::string eventStr;
    std::string reason;

    {
        std::lock_guard<std::mutex> lock(client->connMutex);
        switch (state) {
            case LL_STATE_ERROR:
                eventStr = "failed";
                reason = "link layer error";
                client->connected = false;
                break;
            case LL_STATE_AVAILABLE:
                eventStr = "opened";
                reason = "link layer available";
                client->connected = true;
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
                break;
        }
    }

    printf("Link layer event: %s, reason: %s, clientID: %s, clientId: %i, slaveAddress: %d\n", eventStr.c_str(), reason.c_str(), client->clientID.c_str(), client->clientId, address);

    client->tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
        Napi::Object eventObj = Napi::Object::New(env);
        eventObj.Set("clientID", Napi::String::New(env, client->clientID.c_str()));
        eventObj.Set("type", Napi::String::New(env, "control"));
        eventObj.Set("event", Napi::String::New(env, eventStr));
        eventObj.Set("reason", Napi::String::New(env, reason));
        eventObj.Set("slaveAddress", Napi::Number::New(env, address));
        std::vector<napi_value> args = {Napi::String::New(env, "data"), eventObj};
        jsCallback.Call(args);
    });
}

bool IEC101Slave::RawMessageHandler(void* parameter, IMasterConnection connection, CS101_ASDU asdu) {
    IEC101Slave* client = static_cast<IEC101Slave*>(parameter);
    IEC60870_5_TypeID typeID = CS101_ASDU_getTypeID(asdu);
    int numberOfElements = CS101_ASDU_getNumberOfElements(asdu);

    client->masterConnection = connection;

    try {
        vector<tuple<int, double, uint8_t, uint64_t, bool, int>> elements;

        switch (typeID) {
            case C_SC_NA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    SingleCommand io = (SingleCommand)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = SingleCommand_getState(io) ? 1.0 : 0.0;
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = 0;
                        bool bselCmd = SingleCommand_isSelect(io);
                        int ql = SingleCommand_getQU(io);
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        SingleCommand_destroy(io);
                    }
                }
                break;
            }
            case C_DC_NA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    DoubleCommand io = (DoubleCommand)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = static_cast<double>(DoubleCommand_getState(io));
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = 0;
                        bool bselCmd = DoubleCommand_isSelect(io);
                        int ql = DoubleCommand_getQU(io);
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        DoubleCommand_destroy(io);
                    }
                }
                break;
            }
            case C_RC_NA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    StepCommand io = (StepCommand)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = static_cast<double>(StepCommand_getState(io));
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = 0;
                        bool bselCmd = StepCommand_isSelect(io);
                        int ql = StepCommand_getQU(io);
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        StepCommand_destroy(io);
                    }
                }
                break;
            }
            case C_SE_NA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    SetpointCommandNormalized io = (SetpointCommandNormalized)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = SetpointCommandNormalized_getValue(io);
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = 0;
                        bool bselCmd = SetpointCommandNormalized_isSelect(io);
                        int ql = SetpointCommandNormalized_getQL(io);
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        SetpointCommandNormalized_destroy(io);
                    }
                }
                break;
            }
            case C_SE_NB_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    SetpointCommandScaled io = (SetpointCommandScaled)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = SetpointCommandScaled_getValue(io);
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = 0;
                        bool bselCmd = SetpointCommandScaled_isSelect(io);
                        int ql = SetpointCommandScaled_getQL(io);
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        SetpointCommandScaled_destroy(io);
                    }
                }
                break;
            }
            case C_SE_NC_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    SetpointCommandShort io = (SetpointCommandShort)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = SetpointCommandShort_getValue(io);
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = 0;
                        bool bselCmd = SetpointCommandShort_isSelect(io);
                        int ql = SetpointCommandShort_getQL(io);
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        SetpointCommandShort_destroy(io);
                    }
                }
                break;
            }
            case C_BO_NA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    Bitstring32Command io = (Bitstring32Command)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = static_cast<double>(Bitstring32Command_getValue(io));
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = 0;
                        bool bselCmd = false;
                        int ql = 0;
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        Bitstring32Command_destroy(io);
                    }
                }
                break;
            }
            case C_SC_TA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    SingleCommandWithCP56Time2a io = (SingleCommandWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = SingleCommand_getState((SingleCommand)io) ? 1.0 : 0.0;
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = CP56Time2a_toMsTimestamp(SingleCommandWithCP56Time2a_getTimestamp(io));
                        bool bselCmd = SingleCommand_isSelect((SingleCommand)io);
                        int ql = SingleCommand_getQU((SingleCommand)io);
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        SingleCommandWithCP56Time2a_destroy(io);
                    }
                }
                break;
            }
            case C_DC_TA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    DoubleCommandWithCP56Time2a io = (DoubleCommandWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = static_cast<double>(DoubleCommand_getState((DoubleCommand)io));
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = CP56Time2a_toMsTimestamp(DoubleCommandWithCP56Time2a_getTimestamp(io));
                        bool bselCmd = DoubleCommand_isSelect((DoubleCommand)io);
                        int ql = DoubleCommand_getQU((DoubleCommand)io);
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        DoubleCommandWithCP56Time2a_destroy(io);
                    }
                }
                break;
            }
            case C_RC_TA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    StepCommandWithCP56Time2a io = (StepCommandWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = static_cast<double>(StepCommand_getState((StepCommand)io));
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = CP56Time2a_toMsTimestamp(StepCommandWithCP56Time2a_getTimestamp(io));
                        bool bselCmd = StepCommand_isSelect((StepCommand)io);
                        int ql = StepCommand_getQU((StepCommand)io);
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        StepCommandWithCP56Time2a_destroy(io);
                    }
                }
                break;
            }
            case C_SE_TA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    SetpointCommandNormalizedWithCP56Time2a io = (SetpointCommandNormalizedWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = SetpointCommandNormalized_getValue((SetpointCommandNormalized)io);
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = CP56Time2a_toMsTimestamp(SetpointCommandNormalizedWithCP56Time2a_getTimestamp(io));
                        bool bselCmd = SetpointCommandNormalized_isSelect((SetpointCommandNormalized)io);
                        int ql = SetpointCommandNormalized_getQL((SetpointCommandNormalized)io);
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        SetpointCommandNormalizedWithCP56Time2a_destroy(io);
                    }
                }
                break;
            }
            case C_SE_TB_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    SetpointCommandScaledWithCP56Time2a io = (SetpointCommandScaledWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = SetpointCommandScaled_getValue((SetpointCommandScaled)io);
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = CP56Time2a_toMsTimestamp(SetpointCommandScaledWithCP56Time2a_getTimestamp(io));
                        bool bselCmd = SetpointCommandScaled_isSelect((SetpointCommandScaled)io);
                        int ql = SetpointCommandScaled_getQL((SetpointCommandScaled)io);
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        SetpointCommandScaledWithCP56Time2a_destroy(io);
                    }
                }
                break;
            }
            case C_SE_TC_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    SetpointCommandShortWithCP56Time2a io = (SetpointCommandShortWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = SetpointCommandShort_getValue((SetpointCommandShort)io);
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = CP56Time2a_toMsTimestamp(SetpointCommandShortWithCP56Time2a_getTimestamp(io));
                        bool bselCmd = SetpointCommandShort_isSelect((SetpointCommandShort)io);
                        int ql = SetpointCommandShort_getQL((SetpointCommandShort)io);
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        SetpointCommandShortWithCP56Time2a_destroy(io);
                    }
                }
                break;
            }
            case C_BO_TA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    Bitstring32CommandWithCP56Time2a io = (Bitstring32CommandWithCP56Time2a)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = static_cast<double>(Bitstring32Command_getValue((Bitstring32Command)io));
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = CP56Time2a_toMsTimestamp(Bitstring32CommandWithCP56Time2a_getTimestamp(io));
                        bool bselCmd = false;
                        int ql = 0;
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        Bitstring32CommandWithCP56Time2a_destroy(io);
                    }
                }
                break;
            }
            case C_IC_NA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    InterrogationCommand io = (InterrogationCommand)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = InterrogationCommand_getQOI(io);
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = 0;
                        bool bselCmd = false;
                        int ql = 0;
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        InterrogationCommand_destroy(io);
                    }
                }
                break;
            }
            case C_CI_NA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    CounterInterrogationCommand io = (CounterInterrogationCommand)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = CounterInterrogationCommand_getQCC(io);
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = 0;
                        bool bselCmd = false;
                        int ql = 0;
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        CounterInterrogationCommand_destroy(io);
                    }
                }
                break;
            }
            case C_RD_NA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    ReadCommand io = (ReadCommand)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = 0;
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = 0;
                        bool bselCmd = false;
                        int ql = 0;
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        ReadCommand_destroy(io);
                    }
                }
                break;
            }
            case C_CS_NA_1: {
                for (int i = 0; i < numberOfElements; i++) {
                    ClockSynchronizationCommand io = (ClockSynchronizationCommand)CS101_ASDU_getElement(asdu, i);
                    if (io) {
                        int ioa = InformationObject_getObjectAddress((InformationObject)io);
                        double val = 0;
                        uint8_t quality = IEC60870_QUALITY_GOOD;
                        uint64_t timestamp = CP56Time2a_toMsTimestamp(ClockSynchronizationCommand_getTime(io));
                        bool bselCmd = false;
                        int ql = 0;
                        elements.emplace_back(ioa, val, quality, timestamp, bselCmd, ql);
                        ClockSynchronizationCommand_destroy(io);
                    }
                }
                break;
            }

            default:
                printf("Received unsupported ASDU type: %s (%i), clientID: %s\n", TypeID_toString(typeID), typeID, client->clientID.c_str());
                return false;
        }

       for (const auto& [ioa, val, quality, timestamp, bselCmd, ql] : elements) {
            printf("ASDU type: %s, clientID: %s, clientId: %i, ioa: %i, value: %f, quality: %u, timestamp: %" PRIu64 ", bselCmd: %d, ql: %d, cnt: %i\n",
                   TypeID_toString(typeID), client->clientID.c_str(), client->clientId, ioa, val, quality, timestamp, bselCmd, ql, client->cnt);
        }

        client->tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
            Napi::Array jsArray = Napi::Array::New(env, elements.size());
            for (size_t i = 0; i < elements.size(); i++) {
                 const auto& [ioa, val, quality, timestamp, bselCmd, ql] = elements[i];
                Napi::Object msg = Napi::Object::New(env);
                msg.Set("clientID", Napi::String::New(env, client->clientID.c_str()));
                msg.Set("typeId", Napi::Number::New(env, typeID));
                msg.Set("ioa", Napi::Number::New(env, ioa));
                msg.Set("val", Napi::Number::New(env, val));
                msg.Set("quality", Napi::Number::New(env, quality));
                msg.Set("bselCmd", Napi::Boolean::New(env, bselCmd));
                msg.Set("ql", Napi::Number::New(env, ql));
                if (timestamp > 0) {
                    msg.Set("timestamp", Napi::Number::New(env, static_cast<double>(timestamp)));
                }
                jsArray[i] = msg;
            }
            std::vector<napi_value> args = {Napi::String::New(env, "data"), jsArray};
            jsCallback.Call(args);
            client->cnt++;
        });

        return true;
    } catch (const std::exception& e) {
        printf("Exception in RawMessageHandler: %s, clientID: %s, clientId: %i\n", e.what(), client->clientID.c_str(), client->clientId);
        client->tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
            Napi::Object eventObj = Napi::Object::New(env);
            eventObj.Set("clientID", Napi::String::New(env, client->clientID.c_str()));
            eventObj.Set("type", Napi::String::New(env, "error"));
            eventObj.Set("reason", Napi::String::New(env, string("ASDU handling failed: ") + e.what()));
            std::vector<napi_value> args = {Napi::String::New(env, "data"), eventObj};
            jsCallback.Call(args);
        });
        return false;
    }
}