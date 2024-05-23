#ifndef IP_UTIL_H
#define IP_UTIL_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <sys/socket.h>
#include <netinet/in.h>



/***************************************** Global functions *****************************************/
/**
 * @brief Overview of the interaction between file descriptors, system calls, and epoll.
 *
 * When your code is compiled with GCC (or any other compiler), it is transformed into an executable binary
 * program that interacts with the operating system through system calls. System calls are how a program requests
 * services from the operating system's kernel, including creating and manipulating file descriptors.
 *
 * Here's the process:
 *
 * - Socket Creation: When your program calls the socket() function, this is translated into a system call to the
 *   operating system's kernel. The kernel handles this call by allocating resources for a new socket and returning
 *   a file descriptor—an integer that represents this socket within your program. This file descriptor does not
 *   inherently contain information about being a socket, but it is a reference that the kernel associates with
 *   the socket's resources.
 *
 * - File Descriptors: In Unix-like operating systems, file descriptors are used as abstract references to various
 *   types of resources, not just files. They can refer to regular files, directories, sockets, pipes, and more.
 *   The kernel maintains a table for each process, mapping file descriptors to the actual resources. This table is
 *   what allows the kernel to know what resource a file descriptor refers to.
 *
 * - Epoll and File Descriptors: When you add a file descriptor to an epoll instance (using epoll_ctl()), you're
 *   simply telling the kernel to monitor events on that descriptor. Since the kernel maintains the mapping between
 *   file descriptors and their resources, it knows whether a particular file descriptor refers to a socket, a file,
 *   or another type of resource. Therefore, when epoll reports that a file descriptor is ready for reading or writing,
 *   it is relying on the kernel's understanding of what that file descriptor represents.
 *
 * - Compilation and Execution: The compilation process (using GCC or another compiler) does not change the nature of
 *   how file descriptors work. Compilation translates your C or C++ code into machine code that the operating system
 *   can execute, but the behavior of system calls like socket(), accept(), and functions interacting with epoll is
 *   defined by the operating system's runtime environment, not the compiler.
 *
 * - Runtime: At runtime, when your compiled program is executed, the operating system (Linux, in this case) manages
 *   the mapping of file descriptors to their respective resources. It is the operating system's responsibility to
 *   know that a file descriptor returned by socket() or accept() refers to a socket.
 *
 * In summary, the association between file descriptors and sockets is managed by the operating system's kernel,
 * not by the GCC compiler. The compiler's role is to translate your source code into machine code; the management
 * of file descriptors and their association with specific resources like sockets is handled by the operating system
 * at runtime.
 */

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
