#include <iostream>
#include <stdio.h>
#include <string>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sstream>
#include <filesystem>

#include "server.h"
#include "mysql.h"

#include <sys/epoll.h>


//--------------------Local defines  -------------------------------------------------------

//--------------------Local data -------------------------------------------------------

static int sLink = -1;
static bool sVerbose = false;

struct ThreadData {
    int connection; // Connection identifier or file descriptor
    std::shared_ptr<MySqlConnectionManager> mysqlManager; // Shared pointer for thread safety and RAII
};

//Implementations of HandleCase1-8... and various helper functions. 


static bool handleRequest(int connection, uint64_t& idDevice, MqsCldMsg_t& request, void*& rawDataSetAssemblyBuffer, uint32_t& maxAssemblyLength, std::shared_ptr<sql::Connection> conn)
{
    bool more = true;
    MqsCldTopic_t topic = static_cast<MqsCldTopic_t>(request.topic);
    if (sVerbose)
    {   
        printf("Processing request %d\n", topic);
    }
    switch (topic)
    {
    case CASE1:
        // Corresponding operation for CASE1
        handleCase1(connection, idDevice, request, conn);
        break;
    case CASE2:
        // Corresponding operation for CASE2
        handleCase2(connection, idDevice, request);
        break;
    case CASE3:
        // Corresponding operation for CASE3
        handleCase3(connection, request);
        break;
    case CASE4:
        // Corresponding operation for CASE4
        more = false;
        break;
    case CASE5:
        // Corresponding operation for CASE5
        handleCase5(connection, idDevice, request, rawDataSetAssemblyBuffer, maxAssemblyLength, conn);
        break;
    case CASE6:
        // Corresponding operation for CASE6
        handleCase6(connection, idDevice, request, conn);
        break;
    case CASE7:
        // Corresponding operation for CASE7
        handleCase7(connection, request);
        break;
    case CASE8:
        // Corresponding operation for CASE8
        handleCase8(connection, idDevice, request, conn);
        break;
    default:
        fprintf(stderr, "Error: Unhandled request type %d\n", topic);
        break;
    }
    return more;
}

struct ThreadData
{
    int connection;                                       // Connection identifier or file descriptor
    std::shared_ptr<MySqlConnectionManager> mysqlManager; // Shared pointer for thread safety and RAII
};

/**
 * @brief Handles an individual client connection in a dedicated thread.
 *
 * This function is designed to be executed in a separate thread for each client connection accepted by the server.
 * It wraps the raw pointer to `ThreadData` in a `std::unique_ptr` for automatic memory management, ensuring
 * the dynamically allocated `ThreadData` is properly cleaned up when the function exits. The function processes
 * incoming messages from the connected client, handles them according to their type, and maintains a loop that
 * continues until a termination condition is met (e.g., a disconnect message is received or an error occurs).
 *
 * @param pData A `void*` pointer to `ThreadData` struct that contains the connection identifier and a pointer
 *              to the MySQL connection pool. This pointer is cast back to `ThreadData*` and managed via `std::unique_ptr`.
 *
 * @return Always returns `NULL`.
 *
 * @note The function uses a static variable `connectionId` to assign a unique ID to each connection. This is
 *       not thread-safe and should be replaced with a thread-safe mechanism if concurrent access is possible.
 *       The function retrieves a database connection from the pool at the beginning and ensures it is returned
 *       to the pool before exiting, demonstrating the use of RAII for resource management. It also carefully
 *       handles potential disconnects and inactivity using a loop that checks for message receipt and inactivity
 *       timeouts.
 */
static void *connectionHandler(void *pData)
{
    static int32_t connectionId = 0;
    // Change: No need for unique_ptr here if passed as shared_ptr, but assuming this is simplified:
    auto data = static_cast<ThreadData *>(pData); // Direct use if passing raw but better to pass smart pointers directly
    int connection = data->connection;

    uint64_t idDevice = 0;
    connectionId++;
    int32_t _connectionId = connectionId;
    bool more = true;
    if (sVerbose)
    {
        std::cout << "New connection " << _connectionId << std::endl;
    }
    int32_t msgCount = 0;
    void *rawDataSetAssemblyBuffer = nullptr;
    uint32_t maxAssemblyLength = 0;
    time_t lastMessage = time(nullptr);
    auto conn = data->mysqlManager->getConnection();
    while (more)
    {
        msgCount++;
        MqsCldMsg_t request;
        if (sVerbose)
        {
            std::cout << "Waiting for message " << msgCount << " on connection " << _connectionId << std::endl;
        }
        int32_t messageLength = amilinkReadMessage(connection, request);
        if (messageLength > 0)
        {
            more = handleRequest(connection, idDevice, request, rawDataSetAssemblyBuffer, maxAssemblyLength, conn);
            time(&lastMessage);
        }
        else
        {
            time_t now = time(nullptr);
            bool activity = (lastMessage + MAX_INACTIVITY) > now;
            more = activity && amilinkHeartbeat(connection);
        }
    }
    data->mysqlManager->returnConnection(conn);
    amilinkClose(connection);
    if (sVerbose)
    {
        std::cout << "Terminating connection " << _connectionId << std::endl;
    }
    return NULL;
}

/**
 * @brief Initializes and starts the server, including setting up the MySQL connection manager.
 *
 * This function configures the server based on the specified port and verbosity settings. It initializes
 * the MySQL connection manager with provided configuration details and then creates a server instance
 * to listen for incoming connections. The server's lifecycle is managed via a unique_ptr, ensuring
 * automatic cleanup upon termination or exception. The MySQL connection manager is shared among threads
 * created by the server to handle client connections, ensuring efficient database operations.
 *
 * @param port The port number on which the server will listen for incoming connections.
 * @param verbose A boolean flag indicating whether verbose logging should be enabled.
 *
 * @note The function prepares the listening socket, initializes the MySQL connection manager, and starts
 *       the server thread. It demonstrates the use of modern C++ practices such as smart pointers for
 *       resource management and std::thread for multithreading. The MySQL connection manager is initialized
 *       with a configuration file path, and its lifetime is managed by shared_ptr, guaranteeing that it remains
 *       available as long as the server is running. The server itself is encapsulated in a unique_ptr, ensuring
 *       that it is properly destroyed when the application exits or if serverInit is called again after stopping
 *       the server. This function embodies the RAII (Resource Acquisition Is Initialization) principle, making
 *       resource management automatic and exception-safe.
 *
 * Usage example:
 * @code
 *   uint16_t port = 8080;
 *   bool verbose = true;
 *   serverInit(port, verbose);
 * @endcode
 *
 * After calling this function, the server is actively listening on the specified port and ready to handle
 * incoming connections. The server will use the MySQL connection manager for database interactions, sharing
 * this resource among all connection handler threads.
 */
class Server
{
public:
    /**
     * @brief Constructor for the Server class.
     * @param port Port number on which the server will listen for incoming connections.
     * @param verbose Flag indicating whether to log detailed operational messages.
     * @param mysqlManager Shared pointer to an initialized MySqlConnectionManager for handling database operations.
     */
    Server(uint16_t port, bool verbose, std::shared_ptr<MySqlConnectionManager> mysqlManager)
        : sPort(port), sVerbose(verbose), mysqlManager(mysqlManager), running(false), efd(-1) {}

    /**
     * @brief Destructor for the Server class.
     *
     * Cleans up resources used by the server, including closing the epoll file descriptor and the listening socket.
     */
    ~Server()
    {
        if (efd >= 0)
            close(efd);
        // if (sLink >= 0) close(sLink);
    }

    /**
     * @brief Initializes the server's listening socket and epoll instance.
     *
     * Sets up the listening socket for non-blocking I/O and registers it with the epoll instance to monitor
     * for incoming connection events.
     *
     * @return True if initialization was successful, false otherwise.
     */
    bool initialize()
    {
        /**
         * @brief Configures the server's listening socket for non-blocking I/O and registers it with an epoll instance.
         *
         * This function performs the following steps:
         * 1. Retrieves the current file status flags of the listening socket (sLink) using the F_GETFL command.
         * 2. Checks if the retrieval was successful. If not, returns false indicating failure.
         * 3. Sets the file descriptor to non-blocking mode by combining the current flags with O_NONBLOCK
         *    using bitwise OR and applying them with the F_SETFL command.
         * 4. Checks if setting the non-blocking mode was successful. If not, returns false indicating failure.
         * 5. Creates an epoll instance for monitoring I/O events and stores the file descriptor of the new
         *    instance in 'efd'.
         * 6. Initializes an epoll_event structure to specify the interest in read events (EPOLLIN) for the
         *    listening socket.
         * 7. Registers the listening socket with the epoll instance using epoll_ctl with the EPOLL_CTL_ADD command,
         *    to start monitoring for incoming connections.
         *
         * @return True if the listening socket was successfully set to non-blocking mode and registered with the epoll instance,
         *         false otherwise.
         *
         * @note This setup allows the server to handle multiple client connections efficiently in a non-blocking manner,
         *       using the epoll mechanism to get notified about I/O events without blocking the main thread.
         */
        // Example for setting non-blocking mode
        int flags = fcntl(sLink, F_GETFL, 0);
        if (flags == -1)
            return false;
        if (fcntl(sLink, F_SETFL, flags | O_NONBLOCK) == -1)
            return false;

        efd = epoll_create1(0);
        if (efd == -1)
            return false;

        struct epoll_event event;
        event.data.fd = sLink;
        event.events = EPOLLIN; // Interested in read events
        if (epoll_ctl(efd, EPOLL_CTL_ADD, sLink, &event) == -1)
            return false;

        return true;
    }

    /**
     * @brief Starts the server's main loop in a separate thread.
     *
     * Initiates the asynchronous server operation, including listening for new connections
     * and dispatching client requests to handler threads.
     */
    void start()
    {
        if (!initialize())
        {
            std::cerr << "Server initialization failed\n";
            return;
        }
        running = true;
        mainThread = std::thread(&Server::run, this);
    }

    /**
     * @brief Stops the server's main loop and cleans up resources.
     *
     * Signals the server loop to stop and waits for the main thread and any active connection handler threads to join.
     */
    void stop()
    {
        running = false;
        if (mainThread.joinable())
        {
            mainThread.join();
        }
    }

    /**
     * @brief Cleans up finished connection handler threads.
     *
     * Iterates through the list of active connection handler threads and removes any that have completed their execution.
     */
    void cleanUpFinishedThreads()
    {
        for (auto it = connectionThreads.begin(); it != connectionThreads.end();)
        {
            if (!it->joinable())
            {
                // If the thread is not joinable, it has finished execution.
                it = connectionThreads.erase(it); // Erase returns the next iterator.
            }
            else
            {
                ++it; // Move to the next thread.
            }
        }
    }

    /**
     * @brief Main loop for accepting and handling client connections.
     *
     * Continuously monitors the epoll instance for I/O events on the listening socket and connected client sockets.
     * Accepts new connections, sets them to non-blocking mode, and dispatches them to handler threads.
     */
    void run()
    {
        auto lastCleanupTime = std::chrono::steady_clock::now(); // Initialize the last cleanup time

        const int MAX_EVENTS = 10;
        struct epoll_event events[MAX_EVENTS]; // Buffer where events are returned

        while (running)
        {
            int n = epoll_wait(efd, events, MAX_EVENTS, -1); // Wait indefinitely for events

            for (int i = 0; i < n; ++i)
            {
                if (events[i].data.fd == sLink)
                {
                    // New connection on the listening socket
                    while (true)
                    {
                        int connection = amilinkAccept(sLink); // Use your existing accept function
                        if (connection < 0)
                        {
                            if (errno != EAGAIN && errno != EWOULDBLOCK)
                            {
                                perror("accept");
                            }
                            break; // No more incoming connections
                        }

                        setNonBlocking(connection); // Set new connection to non-blocking mode

                        struct epoll_event client_event;
                        client_event.data.fd = connection;
                        client_event.events = EPOLLIN | EPOLLET; // Read events, Edge Triggered mode
                        if (epoll_ctl(efd, EPOLL_CTL_ADD, connection, &client_event) == -1)
                        {
                            perror("epoll_ctl: add");
                            amilinkClose(connection); // Close the socket on error
                        }
                        else
                        {
                            auto threadData = std::make_shared<ThreadData>();
                            threadData->connection = connection;
                            threadData->mysqlManager = mysqlManager; // Assuming mysqlManager is a shared_ptr

                            connectionThreads.emplace_back([threadData = std::move(threadData)]()
                                                           { connectionHandler(threadData.get()); });
                        }
                    }
                }
                else
                {
                    // Existing connection has data or can be written
                    // You might want to handle this directly or offload to another thread,
                    // depending on your application's design
                }
            }

            // Check if it's time to perform cleanup
            auto currentTime = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - lastCleanupTime).count() >= 35000)
            {
                cleanUpFinishedThreads();      // Perform the cleanup of finished threads
                lastCleanupTime = currentTime; // Reset the cleanup timer
            }
        }
    }

private:
    /*
     * @brief Discusses the benefits of non-blocking mode for I/O operations.
     *
     * Setting a file descriptor to non-blocking mode has several advantages,
     * particularly in network programming and high-concurrency systems:
     *
     * - **Improved Resource Utilization**: In non-blocking mode, a thread does not
     *   wait for an I/O operation to complete and can continue performing other
     *   tasks. This leads to more efficient use of CPU resources compared to blocking mode,
     *   where the thread is put to sleep.
     *
     * - **Increased Concurrency**: Allows a single thread to manage multiple I/O operations
     *   on different file descriptors simultaneously, enhancing the concurrency of the application
     *   without the need for multiple threads.
     *
     * - **Better Responsiveness and Throughput**: The server remains responsive under high load
     *   and can continue to service new and ready connections, thus improving overall throughput
     *   and user experience.
     *
     * - **Scalability**: Scales well with the number of client connections by minimizing the overhead
     *   of managing multiple threads, especially beneficial under high network load.
     *
     * - **Control and Flexibility**: Offers more control over I/O operations, allowing the application
     *   to implement sophisticated handling strategies such as batching, prioritizing, and custom
     *   timeouts.
     *
     * Note: While non-blocking I/O increases performance and scalability, it also adds complexity
     * to error handling and state management. Frameworks or libraries can help manage this complexity.
     */
    void setNonBlocking(int sock)
    {
        int flags = fcntl(sock, F_GETFL, 0);
        if (flags != -1)
        {
            fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        }
    }

    int sPort;                                            ///< Port number on which the server listens for incoming connections.
    bool sVerbose;                                        ///< Flag indicating whether verbose logging is enabled.
    std::atomic<bool> running;                            ///< Atomic flag controlling the main loop execution.
    std::shared_ptr<MySqlConnectionManager> mysqlManager; ///< Shared pointer to the MySQL connection manager.
    std::thread mainThread;                               ///< Thread object for the server's main loop.
    int efd;                                              ///< File descriptor for the epoll instance.
    std::vector<std::thread> connectionThreads;           ///< Container for active connection handler threads.
};

static Server *globalServer = nullptr;

void serverInit(uint16_t port, bool verbose)
{
    try
    {
        // Initialize MySqlConnectionManager
        sVerbose = verbose;
        sLink = amilinkListen(port);

        std::cout << "Preparing to listen on port " << port << std::endl;
        std::string configFile = "db_config.txt"; // Provide the path to the config file
        auto mysqlManager = std::make_shared<MySqlConnectionManager>(verbose, configFile);

        globalServer = new Server(port, verbose, mysqlManager);

        // Start the server
        globalServer->start();

        std::cout << "Server initialized and listening on port " << port << std::endl;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error initializing server: " << e.what() << std::endl;
    }
}
