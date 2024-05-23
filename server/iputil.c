// UNIX header files mostly.
#include <stdio.h>
#include <string.h>
#include <sys/types.h> //Used for clock ID type in the clock and timer functions.
#include <sys/socket.h>
#include <sys/ioctl.h>  // system IO definitions and structures.
#include <netinet/in.h> // Internet address family https://www.ibm.com/docs/en/zos/2.4.0?topic=files-netinetinh-internet-protocol-family
#include <net/if.h>     //sockets local interfaces // https://www.ibm.com/docs/en/zos/2.1.0?topic=files-netifh
#include <unistd.h>     //  provides access to the POSIX operating system API. https://en.wikipedia.org/wiki/Unistd.h
#include <arpa/inet.h>  //https://www.ibm.com/docs/en/zos/2.1.0?topic=files-arpaineth https://sites.uclouvain.be/SystInfo/usr/include/arpa/inet.h.html
#include <netdb.h>      // https://www.ibm.com/docs/en/zvse/6.2?topic=files-netdbh
#include <errno.h>      //https://en.wikipedia.org/wiki/Errno.h
#include <fcntl.h>

#include "iputil.h"

/**
 * @file network_util.c
 * @brief Network Utilities for Dual-Stack IPv4/IPv6 Support
 *
 * This module provides utilities to support dual-stack mode, enabling
 * applications to handle both IPv4 and IPv6 traffic. Using dual-stack mode
 * is essential for modern network applications to ensure they operate
 * efficiently across diverse network environments that may use either
 * IPv4 or IPv6, or both.
 *
 * ## Why Dual-Stack Mode?
 *
 * @li \b Broader Compatibility: Dual-stack applications can communicate
 * across both IPv4 and IPv6 networks. This is critical as global IPv6
 * adoption grows while many networks still operate using IPv4. Dual-stack
 * support ensures maximum compatibility with existing and future network
 * infrastructures.
 *
 * @li \b Simplified Network Management: By supporting both IP versions,
 * network administrators can deploy and manage networks without concerning
 * themselves with the limitations and additional complexity introduced
 * by having to support a single IP version network. Dual-stack mode
 * enables seamless operation and reduces the need for transition mechanisms.
 *
 * @li \b Improved Performance: IPv6 offers several technical improvements
 * over IPv4, including more efficient routing and packet handling. Networks
 * utilizing IPv6 can potentially deliver better performance and lower latency.
 * Dual-stack mode allows applications to leverage these benefits in IPv6-enabled
 * environments while maintaining support for IPv4.
 *
 * @li \b Future-proofing: As the availability of IPv4 addresses continues to
 * decline, the shift towards IPv6 will accelerate. Applications that are
 * dual-stack capable are future-proofed to work with both types of networks,
 * avoiding obsolescence and ensuring compliance with future network standards.
 *
 * @li \b Regulatory Compliance: Certain industries and services may require
 * IPv6 capabilities due to regulatory demands or to meet service-level agreements.
 * Dual-stack capability ensures compliance with such requirements.
 *
 * Using dual-stack mode in applications not only enhances their network compatibility
 * and performance but also aligns with modern networking practices that anticipate
 * future shifts in IP technology.
 *
 * @see setsockopt()
 * @see socket()
 * @see bind()
 * @see connect()
 *
 * @note This module requires the POSIX sockets API and has been tested for compatibility
 * with various operating systems including Linux, macOS, and Windows.
 */

/***************************************** Private functions *****************************************/

/*
int32_t IPUtilHostToIp(const char *host, struct sockaddr_storage *addr) {
    struct addrinfo hints = {}, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;

    int result = getaddrinfo(host, NULL, &hints, &res);
    if (result != 0) {
        fprintf(stderr, "Error getting address info for %s: %s\n", host, gai_strerror(result));
        return -1;
    }

    // Copy the first resolved address to the caller's storage
    if (res->ai_family == AF_INET) { // IPv4
        memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in));
    } else { // IPv6
        memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in6));
    }

    freeaddrinfo(res);
    return 0;
}
*/

int IPUtilAccept(int socket)
{
    struct sockaddr_in client;
    socklen_t client_len = sizeof(client);
    int newSocket = accept(socket, (struct sockaddr *)&client, &client_len);
    if (newSocket < 0)
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            fprintf(stderr, "Error accepting socket on %d: %s\n", socket, strerror(errno));
        }
        return -1;
    }
    return newSocket;
}

void IPUtilClose(int socket)
{
    close(socket);
}

int IPUtilConnect(const char *host, int32_t port)
{
    struct addrinfo hints = {}, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; // IPv4 and IPv6
    hints.ai_socktype = SOCK_STREAM;

    char portStr[6];
    snprintf(portStr, sizeof(portStr), "%d", port);

    if (getaddrinfo(host, portStr, &hints, &res) != 0)
    {
        fprintf(stderr, "Error, could not resolve host: %s\n", host);
        return -1;
    }

    int sd;
    for (rp = res; rp != NULL; rp = rp->ai_next)
    {
        sd = socket(rp->ai_family, rp->ai_socktype | SOCK_NONBLOCK, rp->ai_protocol);
        if (sd == -1)
            continue;

        if (connect(sd, rp->ai_addr, rp->ai_addrlen) != -1 || errno == EINPROGRESS)
        {
            break; // Successfully connected or in progress
        }

        close(sd);
    }

    if (rp == NULL)
    {
        fprintf(stderr, "Could not connect\n");
        return -1;
    }

    freeaddrinfo(res);
    return sd;
}

int IPUtilListen(int32_t port)
{
    int sd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sd < 0)
    {
        fprintf(stderr, "Socket creation failed: %s\n", strerror(errno));
        return -1;
    }

    // Enable dual-stack mode if supported
    int no = 0;
    if (setsockopt(sd, IPPROTO_IPV6, IPV6_V6ONLY, (void *)&no, sizeof(no)) < 0)
    {
        fprintf(stderr, "Failed to set dual-stack: %s\n", strerror(errno));
        // Continue to try binding as IPv6 only if dual-stack is not supported
    }

    struct sockaddr_in6 addr = {
        .sin6_family = AF_INET6,
        .sin6_port = htons(port),
        .sin6_addr = IN6ADDR_ANY_INIT};

    if (bind(sd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        fprintf(stderr, "Bind failed on port %d: %s\n", port, strerror(errno));
        close(sd);
        return -1;
    }

    if (listen(sd, 4) < 0)
    { // 4 is the backlog, number of pending connections in the queue
        fprintf(stderr, "Listen failed on port %d: %s\n", port, strerror(errno));
        close(sd);
        return -1;
    }

    return sd; // Successfully created and bound a listening socket
}

int32_t IPUtilReceiveData(int socket, uint8_t *buffer, int maxlen)
{
    int32_t totalBytesRead = 0;
    while (totalBytesRead < maxlen) {
        int32_t bytesRead = recv(socket, buffer + totalBytesRead, maxlen - totalBytesRead, MSG_DONTWAIT);
        if (bytesRead < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // No more data to read
            } else {
                perror("recv");
                return -1;
            }
        } else if (bytesRead == 0) {
            break; // Connection closed
        } else {
            totalBytesRead += bytesRead;
        }
    }
    return totalBytesRead;
}


int32_t IPUtilSendData(int socket, const uint8_t *buffer, int32_t datalen)
{
    int32_t totalSent = 0;
    while (totalSent < datalen)
    {
        int32_t sendLen = send(socket, buffer + totalSent, datalen - totalSent, MSG_DONTWAIT);
        if (sendLen < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break; // Non-blocking send can't proceed now
            }
            else
            {
                fprintf(stderr, "Error in send, %d, %s\n", errno, strerror(errno));
                return -1; // Return an error
            }
        }
        totalSent += sendLen;
    }
    return totalSent;
}
