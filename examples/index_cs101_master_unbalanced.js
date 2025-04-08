const { IEC101MasterUnbalanced } = require('../build/Release/addon_iec60870');
const util = require('util');

let isInitialized = false;

const master = new IEC101MasterUnbalanced((event, data) => {
    try {
        if (event === 'data') {
            if (Array.isArray(data)) {
                console.log('Master: Data received from device:', data.length, 'elements');
                if (data.length === 0) {
                    console.log('  Warning: No data elements received despite RawMessageHandler call');
                }
                data.forEach(item => {
                    console.log(`  Slave: ${item.slaveAddress}, IOA: ${item.ioa}, Value: ${item.val}, Quality: ${item.quality}${item.timestamp ? `, Timestamp: ${item.timestamp}` : ''}`);
                });
            } else if (data.event === 'opened' && !isInitialized) {
                console.log('Master: Serial connection opened.');
                master.sendStartDT();
                console.log('Master: Adding slave with address 1...');
                master.addSlave(1);
                console.log('Master: Adding slave with address 2...');
                master.addSlave(2);
                console.log('Master: Sending interrogation command to slave 1...');
                master.sendCommands([{ typeId: 100, ioa: 0, value: 20, bselCmd: true, ql: 1  }]);
                console.log('Master: Sending interrogation command to slave 2...');
                master.sendCommands([{ typeId: 100, ioa: 0, value: 20, bselCmd: true, ql: 1  }]);
                console.log('Master: Polling slave 1...');
                master.pollSlave(1);
                console.log('Master: Polling slave 2...');
                master.pollSlave(2);
                isInitialized = true;
            } else if (data.event === 'failed') {
                console.error(`Master: Connection failed - ${data.reason}`);
                isInitialized = false; // Сбрасываем флаг для повторной инициализации при восстановлении
            } else if (data.event === 'reconnecting') {
                console.log(`Master: Reconnecting - ${data.reason}`);
            } else {
                console.log('Master: Unhandled data event:', data.event); // Логируем необрабатываемые события
            }
        }
        console.log(`CS101 Event: ${event}, Data: ${util.inspect(data)}`);
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
            baudRate: 19200,
            clientID: "cs101_master_1",
            params: {
                linkAddress: 1,
                originatorAddress: 1,
                asduAddress: 1,
                t0: 30,
                t1: 15,
                t2: 10,
                reconnectDelay: 5,
                queueSize: 100
            }
        });
       
        await sleep(1000);
        const status = master.getStatus();
        console.log(`Initial Status: ${util.inspect(status)}`);
        console.log('Master initialized. Monitoring events...');

        setInterval(() => {
            const currentStatus = master.getStatus();
            if (currentStatus.connected && currentStatus.activated) {
                console.log('Master: Polling slave 1...');
                master.pollSlave(1);
                master.sendCommands([{ typeId: 100, ioa: 0, value: 20, bselCmd: true, ql: 1  }]);
                console.log('Master: Polling slave 2...');
                master.pollSlave(2);
                master.sendCommands([{ typeId: 100, ioa: 0, value: 20, bselCmd: true, ql: 1  }]);
            } else {
                console.log('Master: Skipping poll - not connected or not activated');
            }
        }, 2000);
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