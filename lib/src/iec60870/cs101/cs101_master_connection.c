/*
 *  cs101_master_connection.c
 *
 *  Copyright 2017 MZ Automation GmbH
 *
 *  This file is part of lib60870-C
 *
 *  lib60870-C is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  lib60870-C is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with lib60870-C.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  See COPYING file for the complete license text.
 */

#include "iec60870_slave.h"

bool
IMasterConnection_isReady(IMasterConnection self)
{
    return self->isReady(self);
}

bool
IMasterConnection_sendASDU(IMasterConnection self, CS101_ASDU asdu)
{
    return self->sendASDU(self, asdu);
}

bool
IMasterConnection_sendACT_CON(IMasterConnection self, CS101_ASDU asdu, bool negative)
{
    return self->sendACT_CON(self, asdu, negative);
}

bool
IMasterConnection_sendACT_TERM(IMasterConnection self, CS101_ASDU asdu)
{
    return self->sendACT_TERM(self, asdu);
}

CS101_AppLayerParameters
IMasterConnection_getApplicationLayerParameters(IMasterConnection self)
{
    return self->getApplicationLayerParameters(self);
}

void
IMasterConnection_close(IMasterConnection self)
{
    if (self->close)
        self->close(self);
}

int
IMasterConnection_getPeerAddress(IMasterConnection self, char* addrBuf, int addrBufSize)
{
    if (self->getPeerAddress)
        return self->getPeerAddress(self, addrBuf, addrBufSize);
    else
        return 0;
}

static void receiveMessage(CS101_MasterConnection con)
{
    if (Frame_isComplete(frame)) {
        printf("Received frame from slave %d, size=%d: ", con->linkLayerAddress, Frame_getMsgSize(frame));
        uint8_t* buffer = Frame_getBuffer(frame);
        for (int i = 0; i < Frame_getMsgSize(frame); i++) {
            printf("%02x ", buffer[i]);
        }
        printf("\n");
        if (con->asduReceivedHandler != NULL) {
            con->asduReceivedHandler(con->asduReceivedHandlerParameter, con->linkLayerAddress, asdu);
        }
    }
}