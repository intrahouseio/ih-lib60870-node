/*const { IEC104Client } = require('../build/Release/addon_iec60870');
const util = require('util');
const fs = require('fs');

const connectionParams = {
    ip: "192.168.0.102",
    port: 2404,
    clientID: "client1",
    ipReserve: "192.168.0.102",
    reconnectDelay: 2,
    originatorAddress: 1,
    asduAddress: 1,
    k: 12,
    w: 8,
    t0: 30,
    t1: 60,
    t2: 20,
    t3: 40
};

let isSelectingFile = false;
let retryCount = 0;
const maxRetries = 3;
let isReceivingDirectory = false;
let allFiles = [];
let lastFileListTime = null;
let fileStream = null;
let currentFileIndex = 0;

const client = new IEC104Client((event, data) => {
    console.log(`[${new Date().toISOString()}] Server Event: ${event}, Data: ${util.inspect(data, { depth: null })}`);

    const sendSelectCommand = (fileName, attempt) => {
        console.log(`Preparing to send F_SC_NA_1 for ${fileName} (attempt ${attempt}/${maxRetries})`);
        try {
            const status = client.getStatus();
            console.log('Current connection status:', status);
            if (!status.connected || !status.activated) {
                console.error('Connection is not active or not activated, skipping command...');
                return false;
            }

            const result = client.sendCommands([{ 
                typeId: 122, 
                ioa: 0, 
                value: fileName,
                asdu: 1, 
                cot: 13,
                scq: 1 
            }]);
            if (!result) {
                console.error(`Failed to send F_SC_NA_1 for ${fileName} (result: ${result})`);
                return false;
            }
            console.log(`Successfully sent F_SC_NA_1: IOA=0, SCQ=1, File=${fileName}, COT=13, clientID: ${connectionParams.clientID}`);
            return true;
        } catch (err) {
            console.error(`Exception while sending F_SC_NA_1 for ${fileName}: ${err.message}`);
            return false;
        }
    };

    const sendAckFileCommand = (ioa, fileName, nof, attempt = 1) => {
        console.log(`Preparing to send F_AF_NA_1 for ${fileName} (attempt ${attempt}/2)`);
        try {
            const status = client.getStatus();
            console.log('Current connection status:', status);
            if (!status.connected || !status.activated) {
                console.error('Connection is not active, cannot send F_AF_NA_1');
                client.disconnect();
                return;
            }

            const value = nof !== 0 ? String(nof) : fileName;
            const result = client.sendCommands([{ 
                typeId: 124, 
                ioa, 
                value, 
                asdu: 1, 
                cot: 13 
            }]);
            if (!result) {
                console.error(`Failed to send F_AF_NA_1 for ${fileName} (result: ${result})`);
                if (attempt < 2) {
                    console.log(`Retrying F_AF_NA_1 after delay (attempt ${attempt + 1}/2)`);
                    setTimeout(() => sendAckFileCommand(ioa, fileName, nof, attempt + 1), 1000);
                } else {
                    console.error('Max retries reached for F_AF_NA_1, moving to next file...');
                    downloadNextFile();
                }
                return;
            }
            console.log(`Successfully sent F_AF_NA_1: IOA=${ioa}, NOF=${value}, COT=13, clientID: ${connectionParams.clientID}`);
            downloadNextFile();
        } catch (err) {
            console.error(`Exception while sending F_AF_NA_1 for ${fileName}: ${err.message}`);
            if (attempt < 2) {
                console.log(`Retrying F_AF_NA_1 after delay (attempt ${attempt + 1}/2)`);
                setTimeout(() => sendAckFileCommand(ioa, fileName, nof, attempt + 1), 1000);
            } else {
                console.error('Max retries reached for F_AF_NA_1, moving to next file...');
                downloadNextFile();
            }
        }
    };

    const downloadNextFile = () => {
        currentFileIndex++;
        if (currentFileIndex < allFiles.length) {
            const fileToSelect = allFiles[currentFileIndex];
            const fileName = fileToSelect.fileName;
            console.log(`Selecting next file: ${fileName} (${currentFileIndex + 1}/${allFiles.length})`);
            isSelectingFile = true;
            retryCount = 0;

            if (sendSelectCommand(fileName, retryCount + 1)) {
                setTimeout(() => {
                    if (isSelectingFile) {
                        console.error(`Timeout waiting for fileReady for ${fileName}`);
                        isSelectingFile = false;
                        retryCount++;
                        if (retryCount < maxRetries) {
                            console.log(`Retrying file selection for ${fileName}...`);
                            sendSelectCommand(fileName, retryCount + 1);
                        } else {
                            console.error(`Max retries reached for ${fileName}, moving to next file...`);
                            downloadNextFile();
                        }
                    }
                }, 30000);
            } else {
                console.error(`Initial sendSelectCommand failed for ${fileName}`);
                retryCount++;
                if (retryCount < maxRetries) {
                    setTimeout(() => sendSelectCommand(fileName, retryCount + 1), 2000);
                } else {
                    console.error(`Max retries reached for ${fileName}, moving to next file...`);
                    downloadNextFile();
                }
            }
        } else {
            console.log('All files processed, disconnecting...');
            client.disconnect();
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
        client.requestFileList();
    }

    if (data.type === 'fileList' && isReceivingDirectory) {
        console.log('Received file list entry:', data);
        allFiles.push(data);
        lastFileListTime = Date.now();
        console.log('Current allFiles:', allFiles.map(f => f.fileName));
    }

    if (data.type === 'error') {
        console.error(`Server error: ${data.message}`);
        isSelectingFile = false;
        if (data.message.includes('Failed to send command')) {
            retryCount++;
            if (retryCount < maxRetries) {
                console.log(`Retrying file selection for ${allFiles[currentFileIndex]?.fileName || 'unknown'} (attempt ${retryCount + 1}/${maxRetries})`);
                setTimeout(() => {
                    if (currentFileIndex < allFiles.length) {
                        sendSelectCommand(allFiles[currentFileIndex].fileName, retryCount + 1);
                    }
                }, 2000);
            } else {
                console.error(`Max retries reached for file ${allFiles[currentFileIndex]?.fileName || 'unknown'}, moving to next file...`);
                downloadNextFile();
            }
        } else if (data.message.includes('Unexpected FRQ value')) {
            console.warn(`Ignoring unexpected FRQ value (${data.frq}) for file ${data.fileName}, proceeding with file transfer...`);
            isSelectingFile = false;
            retryCount = 0;
            fileStream = fs.createWriteStream(`./downloaded_oscill_${data.fileName}.zip`);
            client.sendCommands([{ 
                typeId: 122, 
                ioa: data.ioa, 
                value: data.fileName, 
                asdu: 1, 
                cot: 13, 
                scq: 2 
            }]);
        } else {
            console.error('Non-retryable error, disconnecting...');
            client.disconnect();
        }
    }

    if (isReceivingDirectory) {
        const checkDirectoryTimeout = setInterval(() => {
            const timeSinceLastPacket = Date.now() - lastFileListTime;
            if (timeSinceLastPacket >= 20000 && isReceivingDirectory) {
                console.log('Finished receiving file directory:', allFiles.map(f => ({ fileName: f.fileName, fileSize: f.fileSize, timestamp: f.timestamp })));
                isReceivingDirectory = false;
                clearInterval(checkDirectoryTimeout);

                if (allFiles.length > 0) {
                    console.log('Available files:', allFiles.map(f => f.fileName));
                    currentFileIndex = -1; // Начнем с первого файла
                    downloadNextFile(); // Начинаем скачивание первого файла
                } else {
                    console.error('No files received in directory list');
                    client.disconnect();
                }
            }
        }, 1000);
    }

    if (data.type === 'fileReady') {
        console.log(`File ${data.fileName} is ready`);
        isSelectingFile = false;
        retryCount = 0;
        fileStream = fs.createWriteStream(`./downloaded_oscill_${data.fileName}.zip`);
        client.sendCommands([{ 
            typeId: 122, 
            ioa: data.ioa, 
            value: data.fileName, 
            asdu: 1, 
            cot: 13, 
            scq: 2 
        }]);
    }

    if (data.type === 'sectionReady') {
        console.log(`Section ready for file ${data.fileName}, NOS=${data.nos}`);
        client.sendCommands([{ 
            typeId: 122, 
            ioa: data.ioa, 
            value: data.fileName, 
            asdu: 1, 
            cot: 13, 
            scq: 6 
        }]);
    }

    if (data.type === 'fileData') {
        console.log(`Received file segment: IOA=${data.ioa}, Name=${data.fileName}, Size=${data.data.length} bytes`);
        if (fileStream) {
            fileStream.write(Buffer.from(data.data));
        } else {
            console.error(`File stream not initialized for ${data.fileName}`);
        }
    }

    if (data.type === 'fileEnd') {
        console.log(`File transfer completed: ${data.fileName}`);
        if (fileStream) {
            fileStream.end();
            fileStream = null;
            console.log(`File saved as ./downloaded_oscill_${data.fileName}.zip`);
        }
        setTimeout(() => {
            sendAckFileCommand(data.ioa, data.fileName, data.nof);
        }, 100);
    }

    if (data.type === 'warning') {
        console.warn(`Server warning: ${data.message}`, data);
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
    
    await sleep(300000); // Увеличиваем время ожидания для обработки всех файлов
    console.log('Disconnecting client...');
    client.disconnect();
}

main().catch(err => {
    console.error('Error in main:', err);
    client.disconnect();
});*/

const { IEC104Client } = require('../build/Release/addon_iec60870');
const util = require('util');

const client = new IEC104Client((event, data) => {
    if (data.event === 'opened') {
        client.sendStartDT();
    }
    console.log(`Server 1 Event: ${event}, Data: ${util.inspect(data)}`);
    /*if (data.event === 'activated') { // 
        client.sendCommands([
            { typeId: 100, ioa: 0, value: 20, asdu: 1 }, // C_IC_NA_1: Команда опроса
            { typeId: 45, ioa: 145, value: true, asdu: 1, bselCmd: true, ql: 1 }, // C_SC_NA_1: Включить
            { typeId: 46, ioa: 146, value: 1, asdu: 1, bselCmd: 1, ql: 0 }, // C_DC_NA_1: Включить
            { typeId: 47, ioa: 147, value: 1, asdu: 1, bselCmd: 1, ql: 0 }, // C_RC_NA_1: Увеличить
            { typeId: 48, ioa: 148, value: "0.001", asdu: 1, bselCmd: 1, ql: 0 }, // C_SE_NA_1: Уставка нормализованная (selCmd исправлено на bselCmd)
            { typeId: 49, ioa: 149, value: 5000, asdu: 1, bselCmd: 1, ql: 0 }, // C_SE_NB_1: Уставка масштабированная
            { typeId: 50, ioa: 150, value: "123.45", asdu: 1 } // C_SE_NC_1: Уставка с плавающей точкой
        ]); 
    }*/
});

/*const client2 = new IEC104Client((event, data) => {
    if (data.event === 'opened') {
        client2.sendStartDT();
    }
    console.log(`Server 2 Event: ${event}, Data: ${util.inspect(data)}`);
});*/

async function main() {
    const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

    client.connect({
        ip: "127.0.0.1",
        port: 2404,
        clientID: "client1",
        ipReserve: "192.168.0.11",
        reconnectDelay: 2, // Задержка переподключения в секундах
        originatorAddress: 1,
        k: 12,
        w: 8,
        t0: 30,
        t1: 15,
        t2: 10,
        t3: 20,
         maxRetries: 5 
    });

    /*client2.connect({
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
        // maxRetries: 5 
    });*/

    
    await sleep(1000);
}

main();