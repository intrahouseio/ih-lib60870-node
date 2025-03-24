#include <napi.h>
#include <string>
#include <vector>
#include "cs104_connection.h"
#include "cs101_information_objects.h"

using namespace Napi;

class IEC104Client : public ObjectWrap<IEC104Client> {
public:
    static Object Init(Napi::Env env, Object exports);
    IEC104Client(const CallbackInfo& info);
    ~IEC104Client();

    static FunctionReference constructor;
    std::string connId;

private:
    CS104_Connection connection;
    ThreadSafeFunction tsfn;
    bool connected = false;
    bool activated = false;

    static void RawMessageHandler(void* parameter, IMasterConnection con, CS101_ASDU asdu);
    static void ConnectionHandler(void* parameter, IMasterConnection con, CS104_ConnectionEvent event);
    
    Napi::Value Connect(const CallbackInfo& info);
    Napi::Value Disconnect(const CallbackInfo& info);
    Napi::Value SendStartDT(const CallbackInfo& info);
    Napi::Value SendStopDT(const CallbackInfo& info);
    Napi::Value SendSingleCommand(const CallbackInfo& info);
    Napi::Value GetStatus(const CallbackInfo& info);
};

FunctionReference IEC104Client::constructor;

Object IEC104Client::Init(Napi::Env env, Object exports) {
    Function func = DefineClass(env, "IEC104Client", {
        InstanceMethod("connect", &IEC104Client::Connect),
        InstanceMethod("disconnect", &IEC104Client::Disconnect),
        InstanceMethod("sendStartDT", &IEC104Client::SendStartDT),
        InstanceMethod("sendStopDT", &IEC104Client::SendStopDT),
        InstanceMethod("sendSingleCommand", &IEC104Client::SendSingleCommand),
        InstanceMethod("getStatus", &IEC104Client::GetStatus)
    });

    constructor = Persistent(func);
    constructor.SuppressDestruct();
    exports.Set("IEC104Client", func);
    return exports;
}

IEC104Client::IEC104Client(const CallbackInfo& info) : ObjectWrap<IEC104Client>(info) {
    if (info.Length() > 0) {
        connId = info[0].As<String>();
    }
    
    connection = CS104_Connection_create();
    CS104_Connection_setConnectionHandler(connection, ConnectionHandler, this);
    CS104_Connection_setASDUReceivedHandler(connection, RawMessageHandler, this);
    
    // Set default parameters
    CS104_Connection_setAPCIParameters(connection, 12, 8, 3000);
}

IEC104Client::~IEC104Client() {
    if (connected) {
        CS104_Connection_destroy(connection);
    }
    
}

Value IEC104Client::Connect(const CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    if (info.Length() < 2) {
        throw TypeError::New(env, "Missing IP and port arguments");
    }

    std::string ip = info[0].As<String>();
    int port = info[1].As<Number>().Int32Value();
    
    CS104_Connection_setRemoteAddress(connection, ip.c_str());
    CS104_Connection_setRemotePort(connection, port);
    
    connected = CS104_Connection_connect(connection);
    
    if (connected) {
        tsfn = ThreadSafeFunction::New(
            env,
            Function::New(env, [](const CallbackInfo& info) {}),
            "TSFN",
            0,
            1,
            [](Napi::Env) {}
        );
    }
    
    return Boolean::New(env, connected);
}

Value IEC104Client::Disconnect(const CallbackInfo& info) {
    if (connected) {
        CS104_Connection_disconnect(connection);
        connected = false;
        activated = false;
    }
    return info.Env().Undefined();
}

void IEC104Client::RawMessageHandler(void* parameter, CS104_Connection con, CS101_ASDU asdu) {
    IEC104Client* client = static_cast<IEC104Client*>(parameter);
    
    int typeId = CS101_ASDU_getTypeID(asdu);
    int ca = CS101_ASDU_getCA(asdu);
    bool test = CS101_ASDU_isTest(asdu);
    
    client->tsfn.BlockingCall([=](Napi::Env env, Function jsCallback) {
        Object message = Object::New(env);
        message.Set("connId", String::New(env, client->connId));
        message.Set("typeId", Number::New(env, typeId));
        message.Set("ca", Number::New(env, ca));
        message.Set("test", Boolean::New(env, test));

        switch (typeId) {
            case M_SP_NA_1: { // Type 1: Single Point Information
                InformationObject io = CS101_ASDU_getElement(asdu, 0);
                if (io) {
                    message.Set("ioa", Number::New(env, InformationObject_getObjectAddress(io)));
                    message.Set("value", Boolean::New(env, SinglePointInformation_getValue((SinglePointInformation)io)));
                    InformationObject_destroy(io);
                }
                break;
            }
            case M_ME_NA_1: { // Type 3: Measured Value, Normalized
                InformationObject io = CS101_ASDU_getElement(asdu, 0);
                if (io) {
                    message.Set("ioa", Number::New(env, InformationObject_getObjectAddress(io)));
                    message.Set("value", Number::New(env, MeasuredValueNormalized_getValue((MeasuredValueNormalized)io)));
                    InformationObject_destroy(io);
                }
                break;
            }
            case M_ST_NA_1: { // Type 5: Step Position Information
                InformationObject io = CS101_ASDU_getElement(asdu, 0);
                if (io) {
                    message.Set("ioa", Number::New(env, InformationObject_getObjectAddress(io)));
                    message.Set("value", Number::New(env, StepPositionInformation_getValue((StepPositionInformation)io)));
                    InformationObject_destroy(io);
                }
                break;
            }
            case M_BO_NA_1: { // Type 7: Bitstring of 32 bits
                InformationObject io = CS101_ASDU_getElement(asdu, 0);
                if (io) {
                    message.Set("ioa", Number::New(env, InformationObject_getObjectAddress(io)));
                    message.Set("value", Number::New(env, BitString32_getValue((BitString32)io)));
                    InformationObject_destroy(io);
                }
                break;
            }
            case M_ME_NB_1: { // Type 11: Measured Value, Scaled
                InformationObject io = CS101_ASDU_getElement(asdu, 0);
                if (io) {
                    message.Set("ioa", Number::New(env, InformationObject_getObjectAddress(io)));
                    message.Set("value", Number::New(env, MeasuredValueScaled_getValue((MeasuredValueScaled)io)));
                    InformationObject_destroy(io);
                }
                break;
            }
            case M_ME_NC_1: { // Type 13: Measured Value, Short Floating Point
                InformationObject io = CS101_ASDU_getElement(asdu, 0);
                if (io) {
                    message.Set("ioa", Number::New(env, InformationObject_getObjectAddress(io)));
                    message.Set("value", Number::New(env, MeasuredValueShort_getValue((MeasuredValueShort)io)));
                    InformationObject_destroy(io);
                }
                break;
            }
            case M_IT_NA_1: { // Type 15: Integrated Totals (Counter)
                InformationObject io = CS101_ASDU_getElement(asdu, 0);
                if (io) {
                    message.Set("ioa", Number::New(env, InformationObject_getObjectAddress(io)));
                    message.Set("value", Number::New(env, BinaryCounterReading_getValue((BinaryCounterReading)io)));
                    InformationObject_destroy(io);
                }
                break;
            }
            case M_SP_TB_1: { // Type 30: Single Point Information with Time Tag CP56Time2a
                InformationObject io = CS101_ASDU_getElement(asdu, 0);
                if (io) {
                    message.Set("ioa", Number::New(env, InformationObject_getObjectAddress(io)));
                    message.Set("value", Boolean::New(env, SinglePointWithCP56Time2a_getValue((SinglePointWithCP56Time2a)io)));
                    InformationObject_destroy(io);
                }
                break;
            }
            case M_ST_TB_1: { // Type 31: Step Position Information with Time Tag CP56Time2a
                InformationObject io = CS101_ASDU_getElement(asdu, 0);
                if (io) {
                    message.Set("ioa", Number::New(env, InformationObject_getObjectAddress(io)));
                    message.Set("value", Number::New(env, StepPositionWithCP56Time2a_getValue((StepPositionWithCP56Time2a)io)));
                    InformationObject_destroy(io);
                }
                break;
            }
            case M_BO_TB_1: { // Type 32: Bitstring of 32 bits with Time Tag CP56Time2a
                InformationObject io = CS101_ASDU_getElement(asdu, 0);
                if (io) {
                    message.Set("ioa", Number::New(env, InformationObject_getObjectAddress(io)));
                    message.Set("value", Number::New(env, Bitstring32WithCP56Time2a_getValue((Bitstring32WithCP56Time2a)io)));
                    InformationObject_destroy(io);
                }
                break;
            }
            case M_ME_TD_1: { // Type 33: Measured Value, Normalized with Time Tag CP56Time2a
                InformationObject io = CS101_ASDU_getElement(asdu, 0);
                if (io) {
                    message.Set("ioa", Number::New(env, InformationObject_getObjectAddress(io)));
                    message.Set("value", Number::New(env, MeasuredValueNormalizedWithCP56Time2a_getValue((MeasuredValueNormalizedWithCP56Time2a)io)));
                    InformationObject_destroy(io);
                }
                break;
            }
            case M_ME_TE_1: { // Type 34: Measured Value, Scaled with Time Tag CP56Time2a
                InformationObject io = CS101_ASDU_getElement(asdu, 0);
                if (io) {
                    message.Set("ioa", Number::New(env, InformationObject_getObjectAddress(io)));
                    message.Set("value", Number::New(env, MeasuredValueScaledWithCP56Time2a_getValue((MeasuredValueScaledWithCP56Time2a)io)));
                    InformationObject_destroy(io);
                }
                break;
            }
            case M_ME_TF_1: { // Type 35: Measured Value, Short Floating Point with Time Tag CP56Time2a
                InformationObject io = CS101_ASDU_getElement(asdu, 0);
                if (io) {
                    message.Set("ioa", Number::New(env, InformationObject_getObjectAddress(io)));
                    message.Set("value", Number::New(env, MeasuredValueShortWithCP56Time2a_getValue((MeasuredValueShortWithCP56Time2a)io)));
                    InformationObject_destroy(io);
                }
                break;
            }
            case M_IT_TB_1: { // Type 36: Integrated Totals with Time Tag CP56Time2a
                InformationObject io = CS101_ASDU_getElement(asdu, 0);
                if (io) {
                    message.Set("ioa", Number::New(env, InformationObject_getObjectAddress(io)));
                    message.Set("value", Number::New(env, BinaryCounterReading_getValue((BinaryCounterReading)io)));
                    InformationObject_destroy(io);
                }
                break;
            }
            case M_EP_TD_1: { // Type 37: Event of Protection Equipment with Time Tag CP56Time2a
                InformationObject io = CS101_ASDU_getElement(asdu, 0);
                if (io) {
                    message.Set("ioa", Number::New(env, InformationObject_getObjectAddress(io)));
                    message.Set("value", Number::New(env, EventOfProtectionEquipmentWithCP56Time2a_getEventState((EventOfProtectionEquipmentWithCP56Time2a)io)));
                    InformationObject_destroy(io);
                }
                break;
            }
        }

        jsCallback.Call({ message });
    });
}

void IEC104Client::ConnectionHandler(void* parameter, CS104_Connection con, CS104_ConnectionEvent event) {
    IEC104Client* client = static_cast<IEC104Client*>(parameter);
    
    std::string eventStr;
    switch (event) {
        case CS104_CONNECTION_OPENED: eventStr = "opened"; break;
        case CS104_CONNECTION_CLOSED: 
            eventStr = "closed";
            client->connected = false;
            client->activated = false;
            break;
        case CS104_CONNECTION_STARTDT_CON_RECEIVED:
            eventStr = "activated";
            client->activated = true;
            break;
        case CS104_CONNECTION_STOPDT_CON_RECEIVED:
            eventStr = "deactivated";
            client->activated = false;
            break;
    }

    client->tsfn.BlockingCall([=](Napi::Env env, Function jsCallback) {
        Object eventObj = Object::New(env);
        eventObj.Set("connId", String::New(env, client->connId));
        eventObj.Set("type", String::New(env, "control"));
        eventObj.Set("event", String::New(env, eventStr));
        jsCallback.Call({ eventObj });
    });
}

Value IEC104Client::SendStartDT(const CallbackInfo& info) {
    if (!connected) return Boolean::New(info.Env(), false);
    
    IMasterConnection_sendStartDT(CS104_Connection_getMasterConnection(connection));
    return Boolean::New(info.Env(), true);
}

Value IEC104Client::SendStopDT(const CallbackInfo& info) {
    if (!connected) return Boolean::New(info.Env(), false);
    
    IMasterConnection_sendStopDT(CS104_Connection_getMasterConnection(connection));
    return Boolean::New(info.Env(), true);
}

Value IEC104Client::SendSingleCommand(const CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    if (!connected || !activated) {
        return Boolean::New(env, false);
    }
    
    if (info.Length() < 3) {
        throw TypeError::New(env, "Missing command parameters");
    }
    
    int ioa = info[0].As<Number>().Int32Value();
    bool value = info[1].As<Boolean>();
    bool select = info[2].As<Boolean>();
    
    CS101_AppLayerParameters alParams = CS104_Connection_getAppLayerParameters(connection);
    CS101_ASDU asdu = CS101_ASDU_create(alParams, false, CS101_COT_ACTIVATION, 0, 1, false, false);
    
    SingleCommand sc = SingleCommand_create(nullptr, ioa, value, select);
    CS101_ASDU_addInformationObject(asdu, (InformationObject)sc);
    
    IMasterConnection_sendASDU(CS104_Connection_getMasterConnection(connection), asdu);
    
    CS101_ASDU_destroy(asdu);
    InformationObject_destroy((InformationObject)sc);
    
    return Boolean::New(env, true);
}

Value IEC104Client::GetStatus(const CallbackInfo& info) {
    Object status = Object::New(info.Env());
    status.Set("connected", Boolean::New(info.Env(), connected));
    status.Set("activated", Boolean::New(info.Env(), activated));
    return status;
}

NODE_API_MODULE(lib60870_client, IEC104Client::Init)