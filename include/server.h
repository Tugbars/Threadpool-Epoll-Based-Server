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

/**
 * @brief Struct to encapsulate epoll context, including the epoll file descriptor and events vector.
 */
struct EpollContext {
    int efd;
    std::vector<struct epoll_event> events;
};

class Server {
public:
    /**
     * @brief Construct a new Server object.
     * 
     * @param port The port number to bind the server.
     * @param verbose Enable verbose logging.
     * @param mysqlManager Shared pointer to the MySqlConnectionManager.
     */
    Server(uint16_t port, bool verbose, std::shared_ptr<MySqlConnectionManager> mysqlManager);

    /**
     * @brief Destroy the Server object.
     * 
     * Stops the server and closes the listening socket and epoll file descriptor.
     */
    ~Server();

    /**
     * @brief Start the server.
     * 
     * Initializes the server and starts the main thread to handle incoming connections.
     */
    void start();

    /**
     * @brief Stop the server.
     * 
     * Stops the server, joins the main thread, and shuts down the thread pool.
     */
    void stop();

    EpollContext epollContext; ///< Epoll context for managing events.

    /**
     * @brief Modify an epoll event for a given file descriptor.
     * 
     * @param efd Epoll file descriptor.
     * @param fd File descriptor to modify.
     * @param events New events to monitor.
     * @return True if modification is successful, false otherwise.
     */
    bool modifyEpollEvent(int efd, int fd, int events);

private:
    uint16_t sPort; ///< The port number the server listens on.
    bool sVerbose; ///< Enable verbose logging.
    bool running; ///< Indicates if the server is running.
    int sLink; ///< Server listening socket.
    std::thread mainThread; ///< Main thread to handle server operations.
    ThreadPool threadPool; ///< Thread pool for handling client connections.
    std::shared_ptr<MySqlConnectionManager> mysqlManager; ///< Shared pointer to MySqlConnectionManager.
    std::atomic<size_t> activeConnections; ///< Number of active connections.

    /**
     * @brief Initialize the server.
     * 
     * Sets up the server listening socket and epoll instance.
     * @return True if initialization is successful, false otherwise.
     */
    bool initialize();

    /**
     * @brief Main server loop to handle incoming connections.
     */
    void run();

    /**
     * @brief Handle new incoming connections.
     * 
     * Accepts new connections and adds them to the epoll instance for monitoring.
     */
    void handleNewConnection();

    /**
     * @brief Set a socket to non-blocking mode.
     * 
     * @param sock The socket file descriptor.
     */
    void setNonBlocking(int sock);

    /**
     * @brief Adjust the size of the thread pool based on the current load.
     */
    void adjustThreadPoolSize();
};

extern Server* globalServer;
extern void serverInit(uint16_t port, bool verbose);

#define FILE_LIBRARY ("./library/")

#endif /* SERVER_H */
