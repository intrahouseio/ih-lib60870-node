const { IEC101MasterUnbalanced } = require('../build/Release/addon_iec60870');
const util = require('util');

let isInitialized = false;

const master = new IEC101MasterUnbalanced((event, data) => {
    try {
        if (event === 'data') {
            if (Array.isArray(data)) {
                console.log('Master: Data received from device:', data.length, 'elements');
                data.forEach(item => {
                    console.log(`  Slave: ${item.slaveAddress}, TypeID: ${item.typeId}, IOA: ${item.ioa}, Value: ${item.val}, Quality: ${item.quality}${item.timestamp ? `, Timestamp: ${item.timestamp}` : ''}`);
                });
            } else if (data.event === 'opened' && !isInitialized) {
                console.log('Master: Serial connection opened.');
                master.sendStartDT();
                console.log('Master: Adding slave with address 1...');
                master.addSlave(1);
                console.log('Master: Adding slave with address 2...');
                master.addSlave(2);
                console.log('Master: Sending interrogation command to slave 1...');
                master.sendCommands([{ typeId: 100, ioa: 0, value: 20 }], 1);
                console.log('Master: Sending interrogation command to slave 2...');
                master.sendCommands([{ typeId: 100, ioa: 0, value: 20 }], 2);
                console.log('Master: Polling slave 1...');
                master.pollSlave(1);
                console.log('Master: Polling slave 2...');
                master.pollSlave(2);
                isInitialized = true;
            } else if (data.event === 'failed') {
                console.error(`Master: Connection failed for slave ${data.slaveAddress} - ${data.reason}`);
                // Не сбрасываем isInitialized, чтобы продолжать пытаться
            } else if (data.event === 'reconnecting') {
                console.log(`Master: Reconnecting - ${data.reason}`);
            } else if (data.event === 'busy' || data.event === 'opened') {
                console.log(`Master: Link layer event - ${data.event}: ${data.reason}`);
            } else if (data.event === 'error') {
                console.error(`Master: Error for slave ${data.slaveAddress} - ${data.reason}`);
            } else {
                console.log('Master: Unhandled data event:', util.inspect(data));
            }
        }
        console.log(`CS101 Event: ${event}, Data: ${util.inspect(data, { depth: null })}`);
    } catch (error) {
        console.error(`CS101 Master Error: ${error.message}`);
    }
});

async function main() {
    const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

    try {
        console.log('Starting IEC 60870-5-101 master in unbalanced mode...');
        master.connect({
            portName: "/dev/tty.usbserial-A505KXKT",
            baudRate: 9600,
            clientID: "cs101_master_1",
            params: {
                linkAddress: 3,
                originatorAddress: 3,
                t0: 120,
                t1: 60,
                t2: 40,
                reconnectDelay: 10,
                queueSize: 1000,
                slaveAddresses: [1, 2]
            }
        });

        await sleep(1000);
        const status = master.getStatus();
        console.log(`Initial Status: ${util.inspect(status)}`);
        console.log('Master initialized. Monitoring events...');

        // Периодический опрос
        setInterval(async () => {
            const currentStatus = master.getStatus();
            if (currentStatus.connected) {
                try {
                    console.log('Master: Polling slave 1...');
                    master.pollSlave(1);
                    master.sendCommands([{ typeId: 100, ioa: 0, value: 20 }], 1);
                } catch (error) {
                    console.error(`Master: Failed to poll slave 1 - ${error.message}`);
                }
                await sleep(2000); // Задержка 2 секунды между опросами
                try {
                    console.log('Master: Polling slave 2...');
                    master.pollSlave(2);
                    master.sendCommands([{ typeId: 100, ioa: 0, value: 20 }], 2);
                } catch (error) {
                    console.error(`Master: Failed to poll slave 2 - ${error.message}`);
                }
            } else {
                console.log('Master: Skipping poll - not connected');
            }
        }, 15000); // Опрос каждые 15 секунд
    } catch (error) {
        console.error(`Main Error: ${error.message}`);
        process.exit(1);
    }
}

main().catch(err => console.error(`Startup Error: ${err.message}`));

process.on('SIGINT', () => {
    console.log('Shutting down CS101 master...');
    master.disconnect();
    process.exit(0);
});