#ifndef IP_UTIL_H
#define IP_UTIL_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <sys/socket.h>
#include <netinet/in.h>

    int IPUtilAccept(int socket);
    void IPUtilClose(int socket);
    int IPUtilConnect(const char *host, int32_t port);
    int IPUtilListen(int32_t port);
    int32_t IPUtilReceiveData(int socket, uint8_t *buffer, int maxlen);
    int32_t IPUtilSendData(int socket, const uint8_t *buffer, int32_t datalen);

#ifdef __cplusplus
}
#endif

#endif
