const { IEC104Client } = require('./builds/linux_arm/addon_iec60870');
const util = require('util')
const client = new IEC104Client((event, data) => {
    if (data.event === 'opened') client.sendStartDT();
    console.log(`Server 1 Event: ${event}, Data: ${util.inspect(data)}`);
    if (data.event === 'activated') client.sendCommands([
        { typeId: 45, ioa: 145, value: true },    // C_SC_NA_1: Включить  
        { typeId: 46, ioa: 146, value: 1 },      // C_DC_NA_1: Включить
        { typeId: 47, ioa: 147, value: 1 },      // C_RC_NA_1: Увеличить
        { typeId: 48, ioa: 148, value: 0.001 },  // C_SE_NA_1: Уставка нормализованная
        { typeId: 49, ioa: 149, value: 5000 },   // C_SE_NB_1: Уставка масштабированная
        { typeId: 50, ioa: 150, value: 123.45 }, // C_SE_NC_1: Уставка с плавающей точкой      
    ]);
});

const client2 = new IEC104Client((event, data) => {
    if (data.event === 'opened') client2.sendStartDT();
    console.log(`Server 2 Event: ${event}, Data: ${util.inspect(data)}`);
});

async function main() {
    const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

    client.connect('192.168.0.102', 2404, 1, 'client number 1', {
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

    client2.connect('192.168.0.10', 2404, 2, 'client number 2', {
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