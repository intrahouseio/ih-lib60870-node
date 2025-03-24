const { IEC104Server } = require('./build/Release/addon_iec60870');

// Создание экземпляра сервера с функцией обратного вызова для обработки событий
const server = new IEC104Server((event, data) => {
    console.log(`Server Event: ${event}, Data: ${JSON.stringify(data, null, 2)}`);

    if (event === 'data') {
        if (Array.isArray(data)) {
            data.forEach(cmd => {
                const { serverID, clientId, typeId, ioa, val, quality, timestamp } = cmd;

                // Обработка входящих команд
                switch (typeId) {
                    case 45: // C_SC_NA_1 (Single Command)
                        console.log(`Received Single Command: clientId=${clientId}, ioa=${ioa}, value=${val}`);
                        // Ответ: отправка текущего значения как M_SP_NA_1
                        server.sendCommands(clientId, [
                            { typeId: 1, ioa: ioa, value: val === 1 }
                        ]);
                        break;

                    case 46: // C_DC_NA_1 (Double Command)
                        console.log(`Received Double Command: clientId=${clientId}, ioa=${ioa}, value=${val}`);
                        // Ответ: отправка текущего значения как M_DP_NA_1
                        server.sendCommands(clientId, [
                            { typeId: 3, ioa: ioa, value: Math.round(val) }
                        ]);
                        break;

                    case 47: // C_RC_NA_1 (Regulating Step Command)
                        console.log(`Received Step Command: clientId=${clientId}, ioa=${ioa}, value=${val}`);
                        // Ответ: отправка текущего значения как M_ST_NA_1
                        server.sendCommands(clientId, [
                            { typeId: 5, ioa: ioa, value: Math.round(val) }
                        ]);
                        break;

                    case 100: // C_IC_NA_1 (Interrogation Command)
                        console.log(`Received Interrogation Command: clientId=${clientId}, ioa=${ioa}, qoi=${val}`);
                        // Ответ: отправка текущих значений в качестве спонтанных данных
                        server.sendCommands(clientId, [
                            { typeId: 1, ioa: 1, value: true }, // M_SP_NA_1
                            { typeId: 3, ioa: 2, value: 1 },    // M_DP_NA_1
                            { typeId: 5, ioa: 3, value: 42 },   // M_ST_NA_1
                            { typeId: 13, ioa: 4, value: 23.5, timestamp: Date.now() } // M_ME_TF_1 с меткой времени
                        ]);
                        break;

                    case 103: // C_CS_NA_1 (Clock Synchronization Command)
                        console.log(`Received Clock Sync Command: clientId=${clientId}, ioa=${ioa}, timestamp=${timestamp}`);
                        break;

                    default:
                        console.log(`Unhandled command typeId=${typeId}, clientId=${clientId}, ioa=${ioa}, value=${val}`);
                }
            });
        } else {
            // Обработка событий контроля (connected, disconnected и т.д.)
            console.log(`Control Event: ${data.event}, Client ID: ${data.clientId}, Reason: ${data.reason}`);
        }
    }
});

// Параметры сервера
const port = 2404;
const serverId = 1;
const serverID = "Server1";
const params = {
    originatorAddress: 1,
    k: 12,
    w: 8,
    t0: 30,
    t1: 15,
    t2: 10,
    t3: 20,
    maxClients: 5
};

// Запуск сервера
console.log(`Starting IEC 104 Server on port ${port} with serverID: ${serverID}, serverId: ${serverId}`);
server.start(port, serverId, serverID, params);

// Проверка статуса сервера через 2 секунды
setTimeout(() => {
    const status = server.getStatus();
    console.log('Server Status:', JSON.stringify(status, null, 2));
}, 2000);

// Пример отправки спонтанных данных через 5 секунд
setTimeout(() => {
    const status = server.getStatus();
    if (status.connectedClients.length > 0) {
        const clientId = status.connectedClients[0];
        console.log(`Sending spontaneous data to client ${clientId}`);
        server.sendCommands(clientId, [
            { typeId: 1, ioa: 10, value: true }, // M_SP_NA_1
            { typeId: 13, ioa: 11, value: 42.7, timestamp: Date.now() } // M_ME_TF_1
        ]);
    } else {
        console.log('No clients connected to send spontaneous data');
    }
}, 5000);

// Обработка завершения процесса
process.on('SIGINT', () => {
    console.log('Stopping server...');
    server.stop();
    process.exit(0);
});