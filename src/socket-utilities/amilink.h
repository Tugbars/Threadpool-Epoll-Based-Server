#ifndef AMILINK_H
#define AMILINK_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <string>
#include <stdint.h>

#include "mqs/mqs.h"

#define DEFAULT_LOSE_PACKET_INTERVAL (0)
#define DEFAULT_HOST ("localhost")
#define DEFAULT_PORT (51111)

    extern int amilinkAccept(int link);
    extern void amilinkClose(int connection);
    extern int amilinkConnect(std::string host, uint16_t port);
    int32_t amilinkGetResponse(int connection, const MqsCldMsg_t &request, MqsCldMsg_t &response);
    extern bool amilinkHeartbeat(int connection);
    extern void amilinkInit(bool verbose, int32_t losePacketInterval = DEFAULT_LOSE_PACKET_INTERVAL);
    extern int amilinkListen(uint16_t port);
    extern int32_t amilinkReadMessage(int connection, MqsCldMsg_t &message);
    extern int32_t amilinkSendMessage(int connection, const MqsCldMsg_t &message);

#define READSLEEP (20000)             // us
#define MAX_INACTIVITY (60)           // seconds

#ifdef __cplusplus
}
#endif

#endif /* AMILINK_H */
