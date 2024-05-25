#include <string>
#include <string.h>
#include <unistd.h>

#include "iputil.h"
#include "amilink.h"
#include "mqs/mqs.h"
#include "plf/slip/slip.h"
#include "plf/crc/crc16.h"

#include "aminicserver.h"

/***************************************** Local defines *****************************************/

#define XMIT_BUFFER_MAX_SIZE (2 * sizeof(MqsCldFrame_t))

/***************************************** Local data *****************************************/

static bool sVerbose = false;
static int32_t sLosePacketInterval = false;

/***************************************** Forward declarations  *****************************************/

static void logBuffer(const char *str, const uint8_t *buffer, int32_t length);
static MqsCldCmd_t readAck(int connection);
static int32_t readFrame(int connection, MqsCldFrame_t *pFrame);
static int32_t readPhysicalFrame(int connection, MqsCldFrame_t *pFrame);
static void sendAck(int connection);
static void sendNack(int connection);

/***************************************** Private functions *****************************************/

static int32_t linkSendHeader(int connection, MqsCldHeader_t &cldHeader)
{
    MqsCldFrame_t frame = {
        .header = cldHeader};
    uint8_t xmitBuffer[XMIT_BUFFER_MAX_SIZE];
    uint16_t xmitBufferLength = 0;
    Slip_RawToSlip((uint8_t *)&frame, sizeof(MqsCldHeader_t), xmitBuffer, &xmitBufferLength);
    logBuffer(">> Ack", xmitBuffer, xmitBufferLength);
    return IPUtilSendData(connection, xmitBuffer, xmitBufferLength);
}

static uint16_t linkSendData(int connection, const MqsCldMsg_t &message, uint16_t length)
{
    MqsCldCmd_t acknowledged = CMD_Last;
    MqsCldHeader_t sendheader = {
        .crc16 = CalcCrc16(length, (const uint8_t *)&message, CRC16_INITIAL_SEED),
        .cmd = CMD_DATA};
    MqsCldFrame_t frame = {.header = sendheader};
    uint8_t xmitBuffer[XMIT_BUFFER_MAX_SIZE];
    uint16_t frameLength = sizeof(MqsCldHeader_t) + length;
    uint16_t xmitBufferLength = 0;
    if (sVerbose)
    {
        printf("Sending frame with crc16=0x%04X, topic=%d, %d app bytes, total %d bytes before slip\n", sendheader.crc16, message.topic, length, frameLength);
        logBuffer(">> Frame data before SLIP", (const uint8_t *)&frame, frameLength);
    }
    memcpy(frame.data, &message, length);
    Slip_RawToSlip((const uint8_t *)&frame, frameLength, xmitBuffer, &xmitBufferLength);
    if (sVerbose)
    {
        logBuffer(">> Frame data SLIP encoded", xmitBuffer, xmitBufferLength);
    }

    // Set connection to EPOLLOUT before sending data
    globalServer->modifyEpollEvent(globalServer->efd, connection, EPOLLOUT | EPOLLET);
    IPUtilSendData(connection, xmitBuffer, xmitBufferLength);

    // Set connection to EPOLLIN before waiting for ACK
    globalServer->modifyEpollEvent(globalServer->efd, connection, EPOLLIN | EPOLLET);
    acknowledged = readAck(connection);
    if (acknowledged != CMD_ACK)
    {
        fprintf(stderr, "ERROR: Failed to get acknowledge on message\n");
    }
    return acknowledged == CMD_ACK ? length : 0;
}

static uint16_t linkSendRetry(int connection, const MqsCldMsg_t &message, uint16_t length)
{
    int32_t retry = 0;
    int32_t dataSent = 0;
    while ((retry < MQS_CLD_RETRY_MAX) && (length != dataSent))
    {
        retry++;
        dataSent = linkSendData(connection, message, length);
    }
    return dataSent;
}

static void logBuffer(const char *str, const uint8_t *buffer, int32_t length)
{
    if (sVerbose)
    {
        printf("%s (%d bytes): ", str, length);
        if (length > 32)
        {
            length = 32;
        }
        for (int32_t i = 0; i < length; i++)
        {
            printf("%02x ", buffer[i]);
        }
        printf("\n");
    }
}

static MqsCldCmd_t readAck(int connection)
{
    MqsCldCmd_t acknowledged = CMD_NACK;
    MqsCldFrame_t rawBuffer = {0};
    int32_t rawLength = readFrame(connection, &rawBuffer);
    if (rawLength == sizeof(MqsCldHeader_t))
    {
        if (rawBuffer.header.crc16 == CRC16_INITIAL_SEED && rawBuffer.header.cmd == CMD_ACK)
        {
            acknowledged = CMD_ACK;
            if (sVerbose)
            {
                printf("ACK read\n");
            }
        }
        else if (rawBuffer.header.crc16 == CRC16_INITIAL_SEED && rawBuffer.header.cmd == CMD_NACK)
        {
            acknowledged = CMD_NACK;
            if (sVerbose)
            {
                printf("NACK read\n");
            }
        }
        else
        {
            fprintf(stderr, "ERROR: bad acknowledge\n");
        }
    }
    else if (rawLength == 0)
    {
        fprintf(stderr, "ERROR: No acknowledge received\n");
    }
    else
    {
        fprintf(stderr, "ERROR: Bad acknowledge, size error\n");
    }
    return acknowledged;
}

static int32_t readFrame(int connection, MqsCldFrame_t *pFrame)
{
    static int32_t lossFreeRun = 0;
    int32_t length = 0;
    bool gotHeartbeat = false;
    do
    {
        length = readPhysicalFrame(connection, pFrame);
        gotHeartbeat = (length == sizeof(MqsCldHeader_t)) && (pFrame->header.cmd == CMD_HEARTBEAT);
        if (gotHeartbeat && sVerbose)
        {
            printf("Got heartbeat\n");
        }
    } while (gotHeartbeat);
    if (sLosePacketInterval > 0)
    {
        lossFreeRun++;
        if (lossFreeRun >= sLosePacketInterval)
        {
            printf("Simulating packet loss/corruption!!!\n");
            length = 0;
            lossFreeRun = 0;
        }
    }
    return length;
}

int32_t readMessage(int connection, MqsCldMsg_t &message)
{
    MqsCldFrame_t rawBuffer = {0};
    int32_t rawLength = readFrame(connection, &rawBuffer);
    int32_t messageLength = 0;

    if (rawLength >= sizeof(MqsCldHeader_t))
    {
        if (rawBuffer.header.cmd == CMD_DATA)
        {
            messageLength = rawLength - sizeof(MqsCldHeader_t);
            Crc16_t crc = CalcCrc16(messageLength, rawBuffer.data, CRC16_INITIAL_SEED);
            if (rawBuffer.header.crc16 == crc)
            {
                memcpy(&message, rawBuffer.data, messageLength);
                // Set connection to EPOLLOUT before sending ACK
                globalServer->modifyEpollEvent(globalServer->efd, connection, EPOLLOUT | EPOLLET);
                sendAck(connection);
                // Set connection back to EPOLLIN after sending ACK
                globalServer->modifyEpollEvent(globalServer->efd, connection, EPOLLIN | EPOLLET);
            }
            else
            {
                fprintf(stderr, "Expected crc 0x%04X but got 0x%04Xd, messageLength=%d\n", rawBuffer.header.crc16, crc, messageLength);
                rawLength = 0;
                // Set connection to EPOLLOUT before sending NACK
                globalServer->modifyEpollEvent(globalServer->efd, connection, EPOLLOUT | EPOLLET);
                sendNack(connection);
                // Set connection back to EPOLLIN after sending NACK
                globalServer->modifyEpollEvent(globalServer->efd, connection, EPOLLIN | EPOLLET);
            }
        }
        else
        {
            fprintf(stderr, "Expected C_DATA but got %d\n", rawBuffer.header.cmd);
        }
    }
    return messageLength;
}

static int32_t readPhysicalFrame(int connection, MqsCldFrame_t *pFrame)
{
    uint8_t *deslip = (uint8_t *)pFrame;
    uint16_t rawLength = 0;
    bool slipNormal = true;
    const uint16_t maxLength = sizeof(*pFrame);
    int32_t timeoutCounter = 0;
    int32_t byteCount = 0;
    if (sVerbose)
    {
        printf("Reading frame...\n");
    }
    while ((rawLength <= maxLength) && (timeoutCounter < MQS_CLD_MESSAGE_TIMEOUT))
    {
        uint8_t ch;
        int32_t bytesRead = IPUtilReceiveData(connection, &ch, 1);
        // TBD optimize reading by reading larger chunks into cache
        if (bytesRead < 1)
        {
            timeoutCounter += READSLEEP;
            usleep(READSLEEP);
        }
        else
        {
            byteCount++;
            bool done = Slip_SlipCharToRawChars(ch, deslip + rawLength, &rawLength, &slipNormal, maxLength);
            if (done)
            {
                break;
            }
        }
    }
    if (sVerbose)
    {
        printf("Read %d bytes SLIP encoded\n", byteCount);
    }
    if (timeoutCounter < MQS_CLD_MESSAGE_TIMEOUT)
    {
        logBuffer("<< Frame data (SLIP decoded)", deslip, rawLength);
    }
    else
    {
        fprintf(stderr, "Error, timeout in readFrame\n");
        rawLength = 0;
    }
    return rawLength;
}

// static int32_t readPhysicalFrame(int connection, MqsCldFrame_t *pFrame)
// {
//     uint8_t *deslip = (uint8_t *)pFrame;
//     uint16_t rawLength = 0;
//     bool slipNormal = true;
//     const uint16_t maxLength = sizeof(*pFrame);
//     int32_t timeoutCounter = 0;
//     int32_t byteCount = 0;
//     const int bufferSize = 128;
//     uint8_t buffer[bufferSize];

//     if (sVerbose) {
//         printf("Reading frame...\n");
//     }

//     while ((rawLength <= maxLength) && (timeoutCounter < MQS_CLD_MESSAGE_TIMEOUT)) {
//         int32_t bytesRead = IPUtilReceiveData(connection, buffer, bufferSize);
//         if (bytesRead < 0) {
//             timeoutCounter += READSLEEP;
//             usleep(READSLEEP);
//         } else if (bytesRead == 0) {
//             // No data read, consider it as EAGAIN or EWOULDBLOCK
//             break;
//         } else {
//             for (int i = 0; i < bytesRead; i++) {
//                 byteCount++;
//                 bool done = Slip_SlipCharToRawChars(buffer[i], deslip + rawLength, &rawLength, &slipNormal, maxLength);
//                 if (done) {
//                     break;
//                 }
//             }
//         }
//     }

//     if (sVerbose) {
//         printf("Read %d bytes SLIP encoded\n", byteCount);
//     }
//     if (timeoutCounter < MQS_CLD_MESSAGE_TIMEOUT) {
//         logBuffer("<< Frame data (SLIP decoded)", deslip, rawLength);
//     } else {
//         fprintf(stderr, "Error, timeout in readFrame\n");
//         rawLength = 0;
//     }
//     return rawLength;
// }

static void sendAck(int connection)
{
    if (sVerbose)
    {
        printf("Sending ack:\n");
    }
    MqsCldHeader_t sendHeader = {
        .crc16 = CRC16_INITIAL_SEED,
        .cmd = CMD_ACK};
    linkSendHeader(connection, sendHeader);
}

static void sendNack(int connection)
{
    if (sVerbose)
    {
        printf("Sending nack:\n");
    }
    MqsCldHeader_t sendHeader = {
        .crc16 = CRC16_INITIAL_SEED,
        .cmd = CMD_NACK};
    linkSendHeader(connection, sendHeader);
}

/***************************************** Global functions *****************************************/

int amilinkAccept(int link)
{
    return IPUtilAccept(link);
}

void amilinkClose(int connection)
{
    IPUtilClose(connection);
}

int amilinkConnect(std::string host, uint16_t port)
{
    return IPUtilConnect(host.c_str(), port);
}

int32_t amilinkGetResponse(int connection, const MqsCldMsg_t &request, MqsCldMsg_t &response)
{
    int32_t messageLength = 0;
    int32_t written = amilinkSendMessage(connection, request); //request is message and that's where the topic information is
    if (written > 0)
    {
        messageLength = amilinkReadMessage(connection, response);
    }
    return messageLength;
}

void amilinkInit(bool verbose, int32_t losePacketInterval)
{
    sVerbose = verbose;
    sLosePacketInterval = losePacketInterval;
}

int amilinkListen(uint16_t port)
{
    return IPUtilListen(port);
}

int32_t amilinkReadMessage(int connection, MqsCldMsg_t &message)
{
    int32_t retry = 0;
    int32_t messageLength = 0;
    while ((retry < MQS_CLD_RETRY_MAX) && (messageLength == 0))
    {
        retry++;
        messageLength = readMessage(connection, message);
    }
    return messageLength;
}

bool amilinkHeartbeat(int connection)
{
    if (sVerbose)
    {
        printf("Sending heartbeat:\n");
    }
    MqsCldHeader_t sendheader = {
        .crc16 = CRC16_INITIAL_SEED,
        .cmd = CMD_HEARTBEAT};
    MqsCldFrame_t frame = {.header = sendheader};
    uint8_t xmitBuffer[XMIT_BUFFER_MAX_SIZE];
    uint16_t xmitBufferLength = 0;
    Slip_RawToSlip((uint8_t *)&frame, sizeof(MqsCldHeader_t), xmitBuffer, &xmitBufferLength);
    logBuffer(">> Heartbeat", xmitBuffer, xmitBufferLength);

    // Set connection to EPOLLOUT before sending heartbeat
    globalServer->modifyEpollEvent(globalServer->efd, connection, EPOLLOUT | EPOLLET);
    int32_t sendLen = IPUtilSendData(connection, xmitBuffer, xmitBufferLength);
    // Set connection back to EPOLLIN after sending heartbeat
    globalServer->modifyEpollEvent(globalServer->efd, connection, EPOLLIN | EPOLLET);

    return sendLen == xmitBufferLength;
}

int32_t amilinkSendMessage(int connection, const MqsCldMsg_t &message)
{
    int32_t length = sizeof(message.topic);
    MqsCldTopic_t topic = static_cast<MqsCldTopic_t>(message.topic);
    switch (topic)
    {
    case MQS_DATA_LOG_PUSH_REQUEST:
        length += sizeof(message.dataLogPushReq);
        break;
    case MQS_DATA_LOG_PUSH_RESPONSE:
        length += sizeof(message.dataLogPushResp);
        break;
    case  MQS_FETCH_TEST_REQUEST:
        length += sizeof(message.testFetchReq);
        break;
    case MQS_FETCH_TEST_RESPONSE:
        length += sizeof(message.testFetchResp);
        break;
    case MQS_FILE_REQUEST:
        length += sizeof(message.filePullReq);
        break;
    case MQS_FILE_RESPONSE:
        length += sizeof(message.filePullResp) - MQS_CLD_DATA_PAYLOAD_MAX + message.filePullResp.length;
        break;
    case MQS_RAW_DATA_PUSH_REQUEST:
        length += sizeof(message.rawDataPushReq) - MQS_CLD_DATA_PAYLOAD_MAX + message.rawDataPushReq.length;
        break;
    case MQS_RAW_DATA_PUSH_RESPONSE:
        length += sizeof(message.rawDataPushResponse);
        break;
    case MQS_RAW_DATA_REQUIRED_REQUEST:
        length += sizeof(message.rawDataRequiredReq);
        break;
    case MQS_RAW_DATA_REQUIRED_RESPONSE:
        length += sizeof(message.rawDataRequiredResp);
        break;
    case MQS_VERSION_REQUEST:
        length += sizeof(message.versionReq);
        break;
    case MQS_VERSION_RESPONSE:
        length += sizeof(message.versionResp);
        break;
    case MQS_DEVICE_REGISTER_REQUEST:
        length += sizeof(message.deviceRegisterReq);
        break;
    case MQS_DEVICE_REGISTER_RESPONSE:
        length += sizeof(message.deviceRegisterResp);
        break;
    default:
        break;
    }
    return linkSendRetry(connection, message, length);
}