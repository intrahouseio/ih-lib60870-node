const { IEC104Client } = require('../build/Release/addon_iec60870');
const util = require('util')
const client = new IEC104Client((event, data) => {
    if (data.event === 'opened') client.sendStartDT();
    console.log(`Server 1 Event: ${event}, Data: ${util.inspect(data)}`);
    if (data.event === 'activated') client.sendCommands([
        { typeId: 100, ioa: 0, asdu: 2, value: 20 },    // команда общего опроса
        { typeId: 45, ioa: 145, value: true, asdu: 1, bselCmd: true, ql: 1 },    // C_SC_NA_1: Включить  
        { typeId: 46, ioa: 146, value: 1, asdu: 1, bselCmd: 1, ql: 0 },      // C_DC_NA_1: Включить
        { typeId: 47, ioa: 147, value: 1, asdu: 1, bselCmd: 1, ql: 0 },      // C_RC_NA_1: Увеличить
        { typeId: 48, ioa: 148, value: 0.001, asdu: 1, selCmd: 1, ql: 0 },  // C_SE_NA_1: Уставка нормализованная
        { typeId: 49, ioa: 149, value: 5000, asdu: 1, bselCmd: 1, ql: 0 },   // C_SE_NB_1: Уставка масштабированная
        { typeId: 50, ioa: 150, value: 123.45, asdu: 1 }, // C_SE_NC_1: Уставка с плавающей точкой      
    ]);
});

const client2 = new IEC104Client((event, data) => {
    if (data.event === 'opened') client2.sendStartDT();
    console.log(`Server 2 Event: ${event}, Data: ${util.inspect(data)}`);
});

async function main() {
    const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

    client.connect({
        ip: "192.168.0.10",
        port: 2404,
        clientID: "client1",
        ipReserve: "192.168.0.11",
        reconnectDelay: 2,           // Задержка переподключения в секундах
        originatorAddress: 1, 
        asduAddress: 1,       
        k: 12,
        w: 8,
        t0: 30,
        t1: 15,
        t2: 10,
        t3: 20,
        reconnectDelay: 2,
        maxRetries: 5
    });

    client2.connect({
        ip: "192.168.0.102",
        port: 2404,
        clientID: "client2",
        originatorAddress: 1,
        k: 12,
        w: 8,
        t0: 30,
        t1: 15,
        t2: 10,
        t3: 20,
        reconnectDelay: 2,
        maxRetries: 5
    });

    // Ждём некоторое время (опционально, если нужно синхронизировать действия)
    await sleep(1000);
}

main();

/*
let isSelectingFile = false;
Syndrome = false;
let retryCount = 0;
const maxRetries = 3;
let isReceivingDirectory = false;
let allFiles = [];
let lastFileListTime = null;

const client = new IEC104Client((event, data) => {
    console.log(`[${new Date().toISOString()}] Server Event: ${event}, Data: ${util.inspect(data, { depth: null })}`);

    

    const sendSelectCommand = (fileName, ioa, attempt) => {
        console.log(`Preparing to send F_SC_NA_1 for ${fileName} with IOA=${ioa} (attempt ${attempt}/${maxRetries})`);
        try {
            const status = client.getStatus();
            console.log('Current connection status:', status);
            if (!status.connected || !status.activated) {
                console.error('Connection is not active or not activated, skipping command...');
                return false;
            }

            // Используем имя файла в формате, полученном от сервера (например, "2b26")
            const result = client.sendCommands([{ 
                typeId: 122, 
                ioa: ioa, 
                value: fileName, // Отправляем точное имя файла, например, "2b26"
                asdu: 1, 
                cot: 13, 
                scq: 1 
            }]);
            if (!result) {
                console.error(`Failed to send F_SC_NA_1 for ${fileName} (result: ${result})`);
                return false;
            }
            console.log(`Successfully sent F_SC_NA_1: IOA=${ioa}, SCQ=1, File=${fileName}, COT=13, clientID: ${connectionParams.clientID}`);
            return true;
        } catch (err) {
            console.error(`Exception while sending F_SC_NA_1 for ${fileName}: ${err.message}`);
            return false;
        }
    };

    if (data.event === 'opened') {
        console.log('Connection opened, sending STARTDT...');
        client.sendStartDT();
    }

    if (data.event === 'activated') {
        console.log('Connection activated, requesting file directory...');
        allFiles = [];
        isReceivingDirectory = true;
        lastFileListTime = Date.now();
        client.sendCommands([{ typeId: 122, ioa: 0, value: 4, asdu: 1, cot: 5, scq: 0 }]);
    }

    if (data.type === 'fileList' && isReceivingDirectory) {
        console.log('Received partial file list:', data.files);
        allFiles = allFiles.concat(data.files);
        lastFileListTime = Date.now();
    }

    if (isReceivingDirectory) {
        const checkDirectoryTimeout = setInterval(() => {
            const timeSinceLastPacket = Date.now() - lastFileListTime;
            if (timeSinceLastPacket >= 10000 && isReceivingDirectory) {
                console.log('Finished receiving file directory:', allFiles);
                isReceivingDirectory = false;
                clearInterval(checkDirectoryTimeout);

                if (allFiles.length > 0) {
                    // Выбираем первый файл или конкретный, если нужно (например, "2b26")
                    const fileToSelect = allFiles.find(f => f.fileName === '2b26') || allFiles[0];
                    const fileName = fileToSelect.fileName;
                    const ioa = fileToSelect.ioa;
                    console.log(`Selecting file: ${fileName} with IOA=${ioa}`);
                    isSelectingFile = true;
                    retryCount = 0;

                    if (sendSelectCommand(fileName, ioa, retryCount + 1)) {
                        setTimeout(() => {
                            if (isSelectingFile) {
                                console.error(`Timeout waiting for fileReady for ${fileName}`);
                                isSelectingFile = false;
                                retryCount++;
                                if (retryCount < maxRetries) {
                                    console.log(`Retrying file selection for ${fileName}...`);
                                    sendSelectCommand(fileName, ioa, retryCount + 1);
                                } else {
                                    console.error("Max retries reached, giving up on file selection...");
                                }
                            }
                        }, 30000);
                    }
                }
            }
        }, 1000); // Проверяем каждую секунду
    }

    if (data.type === 'fileReady') {
        console.log(`File ${data.fileName} is ready`);
        isSelectingFile = false;
        retryCount = 0;
        client.sendCommands([{ typeId: 122, ioa: data.ioa, value: data.fileName, asdu: 1, cot: 13, scq: 2 }]);
    }

    if (data.type === 'sectionReady') {
        console.log(`Section ready for file ${data.fileName}`);
        client.sendCommands([{ typeId: 122, ioa: data.ioa, value: data.fileName, asdu: 1, cot: 13, scq: 6 }]);
    }

    if (data.type === 'fileData') {
        console.log(`Received file segment: IOA=${data.ioa}, Name=${data.fileName}, Size=${data.data.length} bytes`);
        fs.appendFileSync(`./downloaded_${data.fileName}`, Buffer.from(data.data));
        client.sendCommands([{ typeId: 122, ioa: data.ioa, value: data.fileName, asdu: 1, cot: 13, scq: 6 }]);
    }

    if (data.type === 'fileEnd') {
        console.log(`File transfer completed: ${data.fileName}`);
        client.sendCommands([{ typeId: 124, ioa: data.ioa, asdu: 1 }]);
        console.log(`File saved as ./downloaded_${data.fileName}`);
    }
});

async function main() {
    const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
    console.log('Connecting to server with params:', connectionParams);
    try {
        client.connect(connectionParams);
    } catch (err) {
        console.error('Initial connection failed:', err);
    }
    await sleep(120000);
    console.log('Disconnecting client...');
    client.disconnect();
}

main().catch(err => {
    console.error('Error in main:', err);
    client.disconnect();
});*/