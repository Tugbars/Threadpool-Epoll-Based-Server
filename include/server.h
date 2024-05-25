#ifndef AMINICSERVER_H
#define AMINICSERVER_H

#include <stdint.h>
#include <vector>
#include <atomic>
#include <thread>
#include <memory>
#include <sys/epoll.h>
#include "threadPool.h"

// Forward declaration of MySqlConnectionManager class
class MySqlConnectionManager;

class Server {
public:
    Server(uint16_t port, bool verbose, std::shared_ptr<MySqlConnectionManager> mysqlManager);
    ~Server();
    void start();
    void stop();
    bool initialize();
    void run();
    bool modifyEpollEvent(int efd, int fd, int events);
    void setNonBlocking(int sock);
    void handleNewConnection();
    void adjustThreadPoolSize();

    int efd;

private:
    uint16_t sPort;
    bool sVerbose;
    std::atomic<bool> running;
    std::thread mainThread;
    std::shared_ptr<MySqlConnectionManager> mysqlManager;
    ThreadPool threadPool; // Thread pool member
    std::atomic<int> activeConnections; // To keep track of active connections
    std::mutex resizeMutex; // Mutex to protect resizing logic
};

extern Server* globalServer;
extern void serverInit(uint16_t port, bool verbose);

#define FILE_LIBRARY ("./library/")

#endif /* SERVER_H */
