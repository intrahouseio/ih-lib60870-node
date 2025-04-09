const { IEC104Client } = require('../build/Release/addon_iec60870'); // Ваш модуль
const util = require('util');
const fs = require('fs'); // Для записи файлов на диск

// Создаем клиента с обработчиком событий
const client = new IEC104Client((event, data) => {
    // Выводим все события для отладки
    console.log(`Server Event: ${event}, Data: ${util.inspect(data, { depth: null })}`);

    // После открытия соединения отправляем STARTDT
    if (data.event === 'opened') {
        console.log('Connection opened, sending STARTDT...');
        client.sendStartDT();
    }

    // После активации соединения отправляем команды, включая запрос списка файлов
    if (data.event === 'activated') {
        console.log('Connection activated, sending commands...');
        client.sendCommands([
            { typeId: 100, ioa: 0, value: 20, asdu: 1 }, // C_IC_NA_1: Общий опрос
            { typeId: 102, ioa: 0, value: 0, asdu: 1 },   // C_CI_NT_1: Запрос списка файлов
            { typeId: 45, ioa: 145, value: true, asdu: 1, bselCmd: true, ql: 1 }, // C_SC_NA_1: Включить
            { typeId: 46, ioa: 146, value: 1, asdu: 1, bselCmd: 1, ql: 0 }, // C_DC_NA_1: Включить
            { typeId: 47, ioa: 147, value: 1, asdu: 1, bselCmd: 1, ql: 0 }, // C_RC_NA_1: Увеличить
            { typeId: 48, ioa: 148, value: 0.001, asdu: 1, bselCmd: 1, ql: 0 }, // C_SE_NA_1: Уставка нормализованная (selCmd исправлено на bselCmd)
            { typeId: 49, ioa: 149, value: 5000, asdu: 1, bselCmd: 1, ql: 0 }, // C_SE_NB_1: Уставка масштабированная
            { typeId: 50, ioa: 150, value: 123.45, asdu: 1 } // C_SE_NC_1: Уставка с плавающей точкой
        ]);
    }

    // Обработка списка файлов
    if (data.type === 'fileList') {
        console.log('Received file list:', data.files);
        if (data.files && data.files.length > 0) {
            console.log('Requesting all files...');
            client.readFiles(); // Запрашиваем все файлы
            // Альтернативно, можно запросить конкретный файл по IOA:
            // client.readFiles(data.files[0].ioa);
        } else {
            console.log('No files available in the list.');
        }
    }

    // Обработка данных файла
    if (data.type === 'fileData') {
        console.log(`Received file: IOA=${data.ioa}, Name=${data.fileName}, Size=${data.data.length} bytes`);
        try {
            fs.writeFileSync(`./downloaded_${data.fileName}`, data.data);
            console.log(`File saved as ./downloaded_${data.fileName}`);
        } catch (err) {
            console.error(`Failed to save file ${data.fileName}:`, err);
        }
    }
});

// Функция для подключения и ожидания
async function main() {
    const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

    // Настройки подключения
    const connectionParams = {
        ip: "192.168.0.102",
        port: 2404,
        clientID: "client1",
        ipReserve: "192.168.0.102",
        reconnectDelay: 2,
        originatorAddress: 1,
        k: 12,
        w: 8,
        t0: 30,
        t1: 15,
        t2: 10,
        t3: 20
    };

    console.log('Connecting to server with params:', connectionParams);
    client.connect(connectionParams);

    // Даем время на выполнение операций (можно увеличить при необходимости)
    await sleep(5000);

    // Пример явного запроса списка файлов через 5 секунд (опционально)
    console.log('Manually requesting file list...');
    client.sendCommands([{ typeId: 102, ioa: 0, value: 0, asdu: 1 }]);
}

// Запуск программы
main().catch(err => {
    console.error('Error in main:', err);
});