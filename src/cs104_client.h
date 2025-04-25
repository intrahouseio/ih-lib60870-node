#ifndef CS104_CLIENT_H
#define CS104_CLIENT_H

#include <mutex>
#include <thread>
#include <napi.h>
#include <atomic>
#include <vector>
#include <map> // Добавляем для std::map

extern "C" {
#include "cs104_connection.h"
#include "hal_thread.h"
#include "hal_time.h"
}

struct FileInfo {
    std::string name;
    uint32_t size;
    uint64_t timestamp;
};

class IEC104Client : public Napi::ObjectWrap<IEC104Client> {
public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports);
    IEC104Client(const Napi::CallbackInfo& info);
    virtual ~IEC104Client();

private:
    //std::vector<FileInfo> fileList; // Изменено с std::vector<std::pair<int, std::string>>
    std::map<uint16_t, FileInfo> fileList; // Ключ — NOF
    static Napi::FunctionReference constructor;
    uint16_t currentNOF; // Добавляем для хранения текущего NOF
    
    int originatorAddress;
    CS104_Connection connection;
    std::thread _thread;
    std::atomic<bool> running;
    std::mutex connMutex;
    bool connected = false;
    bool activated = false;
    std::string clientID;
    int cnt = 0;
    int asduAddress; 
    bool usingPrimaryIp;

    //std::vector<std::pair<int, std::string>> fileList; // IOA и имя файла
    std::map<int, std::vector<uint8_t>> fileData; // Хранение фрагментов файла по IOA

    Napi::ThreadSafeFunction tsfn;

    static bool RawMessageHandler(void* parameter, int address, CS101_ASDU asdu);
    static void ConnectionHandler(void* parameter, CS104_Connection con, CS104_ConnectionEvent event);

    Napi::Value Connect(const Napi::CallbackInfo& info);
    Napi::Value Disconnect(const Napi::CallbackInfo& info);
    Napi::Value SendStartDT(const Napi::CallbackInfo& info);
    Napi::Value SendStopDT(const Napi::CallbackInfo& info);
    Napi::Value SendCommands(const Napi::CallbackInfo& info);
    Napi::Value GetStatus(const Napi::CallbackInfo& info);
    Napi::Value RequestFileList(const Napi::CallbackInfo& info);
    Napi::Value SelectFile(const Napi::CallbackInfo& info); // Новый метод для выбора файла
    Napi::Value OpenFile(const Napi::CallbackInfo& info);     // Новый метод для открытия файла
    Napi::Value RequestFileSegment(const Napi::CallbackInfo& info); // Новый метод для запроса сегмента
    Napi::Value ConfirmFileTransfer(const Napi::CallbackInfo& info); 
    std::string getFileNameByNOF(uint16_t nof); // Заменяем getFileNameByIOA
    
};

#endif // CS104_CLIENT_H