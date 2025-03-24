#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <vector>       // Добавлено для std::vector
#include <tuple>        // Добавлено для std::tuple
#include "cs104_client.h"
#include <inttypes.h>
#include <stdexcept>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>



using namespace Napi;
using namespace std;

Napi::FunctionReference IEC104Client::constructor;

Napi::Object IEC104Client::Init(Napi::Env env, Napi::Object exports) {
    Napi::Function func = DefineClass(env, "IEC104Client", {
        InstanceMethod("connect", &IEC104Client::Connect),
        InstanceMethod("disconnect", &IEC104Client::Disconnect),
        InstanceMethod("sendStartDT", &IEC104Client::SendStartDT),
        InstanceMethod("sendStopDT", &IEC104Client::SendStopDT),
        InstanceMethod("sendCommands", &IEC104Client::SendCommands),
        InstanceMethod("getStatus", &IEC104Client::GetStatus)
    });

    constructor = Napi::Persistent(func);
    constructor.SuppressDestruct();
    exports.Set("IEC104Client", func);
    return exports;
}

IEC104Client::IEC104Client(const Napi::CallbackInfo& info) : Napi::ObjectWrap<IEC104Client>(info) {
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
            "IEC104ClientTSFN",
            0,
            1,
            [](Napi::Env) {}
        );
    } catch (const std::exception& e) {
        printf("Failed to create ThreadSafeFunction: %s\n", e.what());
        Napi::Error::New(info.Env(), string("TSFN creation failed: ") + e.what()).ThrowAsJavaScriptException();
    }
}

IEC104Client::~IEC104Client() {
    std::lock_guard<std::mutex> lock(this->connMutex);
    if (running) {
        running = false;
        if (connected) {
            printf("Destructor closing connection, clientID: %s\n", clientID.c_str());
            CS104_Connection_destroy(connection);
            connected = false;
            activated = false;
        }
        if (_thread.joinable()) {
            _thread.join();
        }
        tsfn.Release();
    }
}

Napi::Value IEC104Client::Connect(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 3 || !info[0].IsString() || !info[1].IsNumber() || !info[2].IsString()) {
        Napi::TypeError::New(env, "Expected IP (string), port (number), clientID (string), [params (object)]").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::string ip = info[0].As<Napi::String>();
    int port = info[1].As<Napi::Number>().Int32Value();
    clientID = info[2].As<Napi::String>();

    if (ip.empty() || port <= 0 || clientID.empty()) {
        Napi::Error::New(env, "Invalid IP, port, or clientID").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    {
        std::lock_guard<std::mutex> lock(this->connMutex);
        if (running) {
            Napi::Error::New(env, "Client already running").ThrowAsJavaScriptException();
            return env.Undefined();
        }
    }

    int originatorAddress = 1;
    int k = 12;
    int w = 8;
    int t0 = 30;
    int t1 = 15;
    int t2 = 10;
    int t3 = 20;
    int reconnectDelay = 5;

    if (info.Length() > 3 && info[3].IsObject()) {
        Napi::Object params = info[3].As<Napi::Object>();
        if (params.Has("originatorAddress")) originatorAddress = params.Get("originatorAddress").As<Napi::Number>().Int32Value();
        if (params.Has("k")) k = params.Get("k").As<Napi::Number>().Int32Value();
        if (params.Has("w")) w = params.Get("w").As<Napi::Number>().Int32Value();
        if (params.Has("t0")) t0 = params.Get("t0").As<Napi::Number>().Int32Value();
        if (params.Has("t1")) t1 = params.Get("t1").As<Napi::Number>().Int32Value();
        if (params.Has("t2")) t2 = params.Get("t2").As<Napi::Number>().Int32Value();
        if (params.Has("t3")) t3 = params.Get("t3").As<Napi::Number>().Int32Value();
        if (params.Has("reconnectDelay")) reconnectDelay = params.Get("reconnectDelay").As<Napi::Number>().Int32Value();

        if (originatorAddress < 0 || originatorAddress > 255) {
            Napi::Error::New(env, "originatorAddress must be 0-255").ThrowAsJavaScriptException();
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
    }

    try {
        printf("Creating connection to %s:%d, clientID: %s\n", ip.c_str(), port, clientID.c_str());
        connection = CS104_Connection_create(ip.c_str(), port);
        if (!connection) {
            throw runtime_error("Failed to create connection object");
        }

        CS101_AppLayerParameters alParams = CS104_Connection_getAppLayerParameters(connection);
        alParams->originatorAddress = originatorAddress;

        CS104_APCIParameters apciParams = CS104_Connection_getAPCIParameters(connection);
        apciParams->k = k;
        apciParams->w = w;
        apciParams->t0 = t0;
        apciParams->t1 = t1;
        apciParams->t2 = t2;
        apciParams->t3 = t3;

        printf("Connecting with params: originatorAddress=%d, k=%d, w=%d, t0=%d, t1=%d, t2=%d, t3=%d, reconnectDelay=%d, clientID: %s\n",
               originatorAddress, k, w, t0, t1, t2, t3, reconnectDelay, clientID.c_str());

        running = true;
        _thread = std::thread([this, ip, port, originatorAddress, k, w, t0, t1, t2, t3, reconnectDelay] {
            try {
                int retryCount = 0;
                while (running) {  // Постоянные попытки переподключения
                    CS104_Connection_setConnectionHandler(connection, ConnectionHandler, this);
                    CS104_Connection_setASDUReceivedHandler(connection, RawMessageHandler, this);
                    printf("Attempting to connect (attempt %d), clientID: %s\n", retryCount + 1, clientID.c_str());
                    bool connectSuccess = CS104_Connection_connect(connection);
                    {
                        std::lock_guard<std::mutex> lock(this->connMutex);
                        connected = connectSuccess;
                        activated = false;
                    }
                    if (connectSuccess) {
                        printf("Connected successfully, clientID: %s\n", clientID.c_str());
                        retryCount = 0;

                        while (running) {
                            {
                                std::lock_guard<std::mutex> lock(this->connMutex);
                                if (!connected) break;
                            }
                            Thread_sleep(100);
                        }

                        std::lock_guard<std::mutex> lock(this->connMutex);
                        if (running && !connected) {
                            printf("Connection lost, preparing to reconnect, clientID: %s\n", clientID.c_str());
                            CS104_Connection_destroy(connection);
                            connection = CS104_Connection_create(ip.c_str(), port);
                            if (!connection) {
                                throw runtime_error("Failed to recreate connection object for reconnect");
                            }
                            CS101_AppLayerParameters alParams = CS104_Connection_getAppLayerParameters(connection);
                            alParams->originatorAddress = originatorAddress;
                            CS104_APCIParameters apciParams = CS104_Connection_getAPCIParameters(connection);
                            apciParams->k = k;
                            apciParams->w = w;
                            apciParams->t0 = t0;
                            apciParams->t1 = t1;
                            apciParams->t2 = t2;
                            apciParams->t3 = t3;
                        } else if (!running && connected) {
                            printf("Thread stopped by client, closing connection, clientID: %s\n", clientID.c_str());
                            CS104_Connection_destroy(connection);
                            connected = false;
                            activated = false;
                            return;
                        }
                    } else {
                        printf("Connection failed, clientID: %s\n", clientID.c_str());
                        tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
                            Napi::Object eventObj = Napi::Object::New(env);
                            eventObj.Set("clientID", Napi::String::New(env, clientID.c_str()));
                            eventObj.Set("type", Napi::String::New(env, "control"));
                            eventObj.Set("event", Napi::String::New(env, "reconnecting"));
                            eventObj.Set("reason", Napi::String::New(env, string("attempt ") + to_string(retryCount + 1)));
                            std::vector<napi_value> args = {Napi::String::New(env, "data"), eventObj};
                            jsCallback.Call(args);
                        });
                    }

                    if (running && !connected) {
                        retryCount++;
                        printf("Reconnection attempt %d failed, retrying in %d seconds, clientID: %s\n", retryCount, reconnectDelay, clientID.c_str());
                        Thread_sleep(reconnectDelay * 1000);
                    }
                }
            } catch (const std::exception& e) {
                printf("Exception in connection thread: %s, clientID: %s\n", e.what(), clientID.c_str());
                std::lock_guard<std::mutex> lock(this->connMutex);
                running = false;
                if (connected) {
                    CS104_Connection_destroy(connection);
                    connected = false;
                    activated = false;
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
        printf("Exception in Connect: %s, clientID: %s\n", e.what(), clientID.c_str());
        Napi::Error::New(env, string("Connect failed: ") + e.what()).ThrowAsJavaScriptException();
        return env.Undefined();
    }
}

Napi::Value IEC104Client::Disconnect(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    {
        std::lock_guard<std::mutex> lock(this->connMutex);
        if (running) {
            running = false;
            if (connected) {
                printf("Disconnect called by client, clientID: %s\n", clientID.c_str());
                CS104_Connection_destroy(connection);
                connected = false;
                activated = false;
            }
        }
    }

    if (_thread.joinable()) {
        _thread.join();
    }

    std::lock_guard<std::mutex> lock(this->connMutex);
    tsfn.Release();

    return env.Undefined();
}

Napi::Value IEC104Client::SendStartDT(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    std::lock_guard<std::mutex> lock(this->connMutex);
    if (!connected) {
        Napi::Error::New(env, "Not connected").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    try {
        CS104_Connection_sendStartDT(connection);
        return Napi::Boolean::New(env, true);
    } catch (const std::exception& e) {
        printf("Exception in SendStartDT: %s, clientID: %s\n", e.what(), clientID.c_str());
        Napi::Error::New(env, string("SendStartDT failed: ") + e.what()).ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }
}

Napi::Value IEC104Client::SendStopDT(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    std::lock_guard<std::mutex> lock(this->connMutex);
    if (!connected || !activated) {
        Napi::Error::New(env, "Not connected or not activated").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    try {
        CS104_Connection_sendStopDT(connection);
        return Napi::Boolean::New(env, true);
    } catch (const std::exception& e) {
        printf("Exception in SendStopDT: %s, clientID: %s\n", e.what(), clientID.c_str());
        Napi::Error::New(env, string("SendStopDT failed: ") + e.what()).ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }
}

Napi::Value IEC104Client::SendCommands(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsArray()) {
        Napi::TypeError::New(env, "Expected commands (array of objects with 'typeId', 'ioa', 'value', and optional fields)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    Napi::Array commands = info[0].As<Napi::Array>();

    std::lock_guard<std::mutex> lock(this->connMutex);
    if (!connected || !activated) {
        Napi::Error::New(env, "Not connected or not activated").ThrowAsJavaScriptException();
        return env.Undefined();
    }

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

            // Извлечение bselCmd и ql с значениями по умолчанию
            bool bselCmd = cmdObj.Has("bselCmd") && cmdObj.Get("bselCmd").IsBoolean() ? cmdObj.Get("bselCmd").As<Napi::Boolean>() : false;
            int ql = cmdObj.Has("ql") && cmdObj.Get("ql").IsNumber() ? cmdObj.Get("ql").As<Napi::Number>().Int32Value() : 0;
            if (ql < 0 || ql > 31) {  // Ограничение квалификатора согласно IEC 60870-5-101/104
                Napi::RangeError::New(env, "ql must be between 0 and 31").ThrowAsJavaScriptException();
                return env.Undefined();
            }

            CS101_ASDU asdu = CS101_ASDU_create(CS104_Connection_getAppLayerParameters(connection), false, CS101_COT_ACTIVATION, 0, 1, false, false);

            bool success = false;
            switch (typeId) {
                case C_SC_NA_1: {
                    if (!cmdObj.Get("value").IsBoolean()) {
                        Napi::TypeError::New(env, "C_SC_NA_1 requires 'value' as boolean").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    bool value = cmdObj.Get("value").As<Napi::Boolean>();
                    SingleCommand sc = SingleCommand_create(NULL, ioa, value, bselCmd, ql);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)sc);
                    success = CS104_Connection_sendASDU(connection, asdu);
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
                    DoubleCommand dc = DoubleCommand_create(NULL, ioa, value, bselCmd, ql);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)dc);
                    success = CS104_Connection_sendASDU(connection, asdu);
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
                    StepCommand rc = StepCommand_create(NULL, ioa, (StepCommandValue)value, bselCmd, ql);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)rc);
                    success = CS104_Connection_sendASDU(connection, asdu);
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
                    SetpointCommandNormalized scn = SetpointCommandNormalized_create(NULL, ioa, value, bselCmd, ql);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)scn);
                    success = CS104_Connection_sendASDU(connection, asdu);
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
                    SetpointCommandScaled scs = SetpointCommandScaled_create(NULL, ioa, value, bselCmd, ql);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)scs);
                    success = CS104_Connection_sendASDU(connection, asdu);
                    SetpointCommandScaled_destroy(scs);
                    break;
                }

                case C_SE_NC_1: {
                    if (!cmdObj.Get("value").IsNumber()) {
                        Napi::TypeError::New(env, "C_SE_NC_1 requires 'value' as number").ThrowAsJavaScriptException();
                        return env.Undefined();
                    }
                    float value = cmdObj.Get("value").As<Napi::Number>().FloatValue();
                    SetpointCommandShort scsf = SetpointCommandShort_create(NULL, ioa, value, bselCmd, ql);
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)scsf);
                    success = CS104_Connection_sendASDU(connection, asdu);
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
                    success = CS104_Connection_sendASDU(connection, asdu);
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
                    SingleCommandWithCP56Time2a sc = SingleCommandWithCP56Time2a_create(NULL, ioa, value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)sc);
                    success = CS104_Connection_sendASDU(connection, asdu);
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
                    DoubleCommandWithCP56Time2a dc = DoubleCommandWithCP56Time2a_create(NULL, ioa, value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)dc);
                    success = CS104_Connection_sendASDU(connection, asdu);
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
                    StepCommandWithCP56Time2a rc = StepCommandWithCP56Time2a_create(NULL, ioa, (StepCommandValue)value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)rc);
                    success = CS104_Connection_sendASDU(connection, asdu);
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
                    SetpointCommandNormalizedWithCP56Time2a scn = SetpointCommandNormalizedWithCP56Time2a_create(NULL, ioa, value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)scn);
                    success = CS104_Connection_sendASDU(connection, asdu);
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
                    SetpointCommandScaledWithCP56Time2a scs = SetpointCommandScaledWithCP56Time2a_create(NULL, ioa, value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)scs);
                    success = CS104_Connection_sendASDU(connection, asdu);
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
                    SetpointCommandShortWithCP56Time2a scsf = SetpointCommandShortWithCP56Time2a_create(NULL, ioa, value, bselCmd, ql, CP56Time2a_createFromMsTimestamp(NULL, timestamp));
                    CS101_ASDU_addInformationObject(asdu, (InformationObject)scsf);
                    success = CS104_Connection_sendASDU(connection, asdu);
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
                    success = CS104_Connection_sendASDU(connection, asdu);
                    Bitstring32CommandWithCP56Time2a_destroy(bc);
                    break;
                }

                case C_IC_NA_1: {
                    CS101_ASDU_setTypeID(asdu, C_IC_NA_1);
                    CS101_ASDU_setCOT(asdu, CS101_COT_REQUEST);
                    InformationObject io = (InformationObject)InterrogationCommand_create(NULL, ioa, cmdObj.Get("value").As<Napi::Number>().Uint32Value());
                    CS101_ASDU_addInformationObject(asdu, io);
                    success = CS104_Connection_sendASDU(connection, asdu);
                    InformationObject_destroy(io);
                    break;
                }

                case C_CI_NA_1: {
                    CS101_ASDU_setTypeID(asdu, C_CI_NA_1);
                    CS101_ASDU_setCOT(asdu, CS101_COT_REQUEST);
                    InformationObject io = (InformationObject)CounterInterrogationCommand_create(NULL, ioa, cmdObj.Get("value").As<Napi::Number>().Uint32Value());
                    CS101_ASDU_addInformationObject(asdu, io);
                    success = CS104_Connection_sendASDU(connection, asdu);
                    InformationObject_destroy(io);
                    break;
                }

                case C_RD_NA_1: {
                    CS101_ASDU_setTypeID(asdu, C_RD_NA_1);
                    CS101_ASDU_setCOT(asdu, CS101_COT_REQUEST);
                    InformationObject io = (InformationObject)ReadCommand_create(NULL, ioa);
                    CS101_ASDU_addInformationObject(asdu, io);
                    success = CS104_Connection_sendASDU(connection, asdu);
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
                    success = CS104_Connection_sendASDU(connection, asdu);
                    InformationObject_destroy(io);
                    break;
                }

                default:
                    printf("Unsupported command typeId: %d, clientID: %s\n", typeId, clientID.c_str());
                    CS101_ASDU_destroy(asdu);
                    continue;
            }

            CS101_ASDU_destroy(asdu);

            if (!success) {
                allSuccess = false;
                printf("Failed to send command: typeId=%d, ioa=%d, clientID: %s\n", typeId, ioa, clientID.c_str());
            } else {
                printf("Sent command: typeId=%d, ioa=%d, bselCmd=%d, ql=%d, clientID: %s\n", typeId, ioa, bselCmd, ql, clientID.c_str());
            }
        }
        return Napi::Boolean::New(env, allSuccess);
    } catch (const std::exception& e) {
        printf("Exception in SendCommands: %s, clientID: %s\n", e.what(), clientID.c_str());
        Napi::Error::New(env, string("SendCommands failed: ") + e.what()).ThrowAsJavaScriptException();
        return Napi::Boolean::New(env, false);
    }
}

Napi::Value IEC104Client::GetStatus(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    std::lock_guard<std::mutex> lock(this->connMutex);
    Napi::Object status = Napi::Object::New(env);
    status.Set("connected", Napi::Boolean::New(env, connected));
    status.Set("activated", Napi::Boolean::New(env, activated));
    status.Set("clientID", Napi::String::New(env, clientID.c_str()));
    return status;
}

void IEC104Client::ConnectionHandler(void* parameter, CS104_Connection con, CS104_ConnectionEvent event) {
    IEC104Client* client = static_cast<IEC104Client*>(parameter);
    std::string eventStr;
    std::string reason;

    {
        std::lock_guard<std::mutex> lock(client->connMutex);
        switch (event) {
            case CS104_CONNECTION_FAILED:
                eventStr = "failed";
                reason = "connection attempt failed";
                client->connected = false;
                client->activated = false;
                break;
            case CS104_CONNECTION_OPENED:
                eventStr = "opened";
                reason = "connection established";
                client->connected = true;
                break;
            case CS104_CONNECTION_CLOSED:
                eventStr = "closed";
                if (client->running) {
                    reason = "server closed connection or timeout";
                } else {
                    reason = "client closed connection";
                }
                client->connected = false;
                client->activated = false;
                break;
            case CS104_CONNECTION_STARTDT_CON_RECEIVED:
                eventStr = "activated";
                reason = "STARTDT confirmed";
                client->activated = true;
                break;
            case CS104_CONNECTION_STOPDT_CON_RECEIVED:
                eventStr = "deactivated";
                reason = "STOPDT confirmed";
                client->activated = false;
                break;
        }
    }

    printf("Connection event: %s, reason: %s, clientID: %s\n", eventStr.c_str(), reason.c_str(), client->clientID.c_str());

    client->tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
        Napi::Object eventObj = Napi::Object::New(env);
        eventObj.Set("clientID", Napi::String::New(env, client->clientID.c_str()));
        eventObj.Set("type", Napi::String::New(env, "control"));
        eventObj.Set("event", Napi::String::New(env, eventStr));
        eventObj.Set("reason", Napi::String::New(env, reason));
        std::vector<napi_value> args = {Napi::String::New(env, "conn"), eventObj};
        jsCallback.Call(args);
    });
}

bool IEC104Client::RawMessageHandler(void* parameter, int address, CS101_ASDU asdu) {
    IEC104Client* client = static_cast<IEC104Client*>(parameter);
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
            printf("ASDU type: %s, clientID: %s, ioa: %i, value: %f, quality: %u, timestamp: %" PRIu64 ", cnt: %i\n",
                   TypeID_toString(typeID), client->clientID.c_str(), ioa, val, quality, timestamp, client->cnt);
        }

        client->tsfn.NonBlockingCall([=](Napi::Env env, Napi::Function jsCallback) {
            Napi::Array jsArray = Napi::Array::New(env, elements.size());
            for (size_t i = 0; i < elements.size(); i++) {
                const auto& [ioa, val, quality, timestamp] = elements[i];
                Napi::Object msg = Napi::Object::New(env);
                msg.Set("clientID", Napi::String::New(env, client->clientID.c_str()));
                msg.Set("typeId", Napi::Number::New(env, typeID));
                msg.Set("ioa", Napi::Number::New(env, ioa));
                msg.Set("val", Napi::Number::New(env, val));
                msg.Set("quality", Napi::Number::New(env, quality));
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
        printf("Exception in RawMessageHandler: %s, clientID: %s\n", e.what(), client->clientID.c_str());
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