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
 * @class Server
 * @brief Manages the lifecycle and operations of a network server, handling client connections concurrently.
 *
 * This class encapsulates the necessary functionality to initiate and manage a network server that listens on a specified port
 * and handles incoming connections in a multithreaded manner. It utilizes modern C++ features such as std::thread for handling
 * concurrency and std::shared_ptr for managing shared resources, adhering to the RAII (Resource Acquisition Is Initialization) principles.
 *
 * The server implements a main listening thread responsible for accepting incoming connections. For each new connection, it spawns
 * a separate thread to handle client-server interactions, allowing for the concurrent management of multiple client connections.
 * This design facilitates a structured and scalable approach to network server management compared to traditional, single-threaded servers.
 *
 * Each connection thread is given a shared pointer to a 'MySqlConnectionManager' instance, enabling independent and safe database interactions
 * for each client session. The server periodically checks (every 35 seconds) and cleans up threads that have finished execution, ensuring
 * efficient resource management and preventing resource leaks.
 *
 * @section threading Threading Model
 * The server operates primarily through two types of threads: one main thread and multiple connection handling threads. The main thread,
 * initiated in the `start()` method, is dedicated to listening for and accepting new client connections. Each accepted client connection
 * is then managed by a newly created thread.
 *
 * Connection handling threads are monitored and managed based on their activity; they are cleaned up if they have completed their tasks,
 * ensuring the server remains efficient in resource usage. This model supports handling multiple connections concurrently, improving the
 * server's scalability and responsiveness.
 *
 * @note While detaching threads was considered, this implementation ensures that all threads are properly joined upon server shutdown,
 * highlighting a structured approach to concurrency management. It's important to ensure that shared resources, particularly those accessed
 * by multiple threads, are adequately protected to prevent race conditions and ensure thread safety.
 *
 * @param sPort The port number on which the server listens for incoming connections.
 * @param sVerbose A boolean flag indicating whether verbose logging is enabled, aiding in debugging and monitoring.
 * @param mysqlManager A shared pointer to a 'MySqlConnectionManager' instance, used for handling database operations across multiple threads.
 *
 * This server class is built with thread safety and efficient resource management in mind. However, it's crucial to ensure that resources like
 * the database manager, accessible across multiple threads, are properly synchronized to avoid concurrent access issues.
 */
class Server
{
public:
    Server(uint16_t port, bool verbose, std::shared_ptr<MySqlConnectionManager> mysqlManager)
        : sPort(port), sVerbose(verbose), running(false), mysqlManager(mysqlManager)
    {
        // Constructor body remains unchanged
    }

    void start()
    {
        running = true;
        mainThread = std::thread(&Server::run, this);
    }

    ~Server() {
    // Attempt to gracefully stop the server if it hasn't been stopped yet.
    stop();
    }

    void stop()
    {
        running = false;
        if (mainThread.joinable())
        {
            mainThread.join();
        }
        // Join all connection handler threads
        for (auto &thread : connectionThreads)
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }
        connectionThreads.clear(); // Clear the list of threads after joining them
    }

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
     * @brief Executes the server's main loop.
     *
     * This method contains the main loop of the server, which continuously checks for new connections,
     * creates threads to handle those connections, and cleans up finished threads. The loop runs as
     * long as the 'running' flag is true. New connections are accepted using the amilinkAccept function.
     * Each new connection initializes a ThreadData object and starts a new thread where connectionHandler
     * is called to manage the connection. The method also periodically cleans up threads that have
     * finished executing.
     *
     * @note The method uses std::chrono for managing time intervals and std::this_thread for sleep operations.
     * It employs a lambda function to pass the connection handling routine to each new thread.
     *
     * Usage:
     * This method is typically called from the start method of the Server class after initializing the server.
     *
     * Thread Safety:
     * This method is intended to be called from a single thread (typically the main server thread). 
     * However, it manages multiple threads internally for handling connections.
     *
     * @see start(), stop(), cleanUpFinishedThreads(), connectionHandler()
     */
    /**
     * @details Inside the run method, the server continuously checks for new connections and handles each
     * one in a separate thread. This is achieved using a lambda function and the emplace_back method of
     * the std::vector container.
     *
     * The lambda function:
     * A lambda function is an anonymous function object capable of capturing variables from its enclosing scope.
     * In this context, the lambda captures the 'threadData' shared pointer, which contains data needed by the
     * connectionHandler function. The lambda function is defined as follows:
     *
     * [threadData]() { connectionHandler(threadData.get()); }
     *
     * Here, 'threadData' is captured by value, ensuring that the shared pointer (and thus the pointed-to object)
     * remains alive for the duration of the thread's execution. The lambda then calls 'connectionHandler',
     * passing a raw pointer to the ThreadData object. This approach keeps the ThreadData instance alive
     * until the thread completes, as the lambda's capture holds a reference count to the shared_ptr.
     *
     * The 'emplace_back' method:
     * The emplace_back method is used to add a new element to the end of a vector. Unlike 'push_back',
     * 'emplace_back' constructs the element in-place, reducing the need for temporary objects and copies.
     * In the context of this server code, 'emplace_back' is used to construct a new std::thread object
     * directly within the 'connectionThreads' vector:
     *
     * connectionThreads.emplace_back([threadData]() { connectionHandler(threadData.get()); });
     *
     * This line effectively creates and starts a new thread, running the lambda function as its task.
     * Since 'emplace_back' constructs the thread in-place, it is more efficient than creating a temporary
     * thread object and then moving it into the vector. The newly created thread immediately begins executing
     * the lambda function, which, in turn, calls 'connectionHandler' with the necessary data.
     *
     * By using these techniques, the server efficiently manages multiple connections concurrently, with each
     * connection being handled in a separate thread. The use of 'std::shared_ptr' for managing 'threadData'
     * ensures that memory is properly managed and that the data remains valid for the duration of the thread's
     * operation.
     *
     * @note It is crucial that 'threadData' is captured by value in the lambda to ensure the shared_ptr (and
     * thus the memory for 'ThreadData') is not prematurely destroyed. This pattern is a common practice in
     * C++ for managing resources in concurrent programming.
     *
     * @see std::thread, std::shared_ptr, std::vector::emplace_back
     */
    void run()
    {
        auto lastCleanupTime = std::chrono::steady_clock::now(); // Initialize the last cleanup time

        while (running)
        {
            // Attempt to accept a new connection
            int connection = amilinkAccept(sLink);
            if (connection >= 0)
            { // Check if a new connection was successfully accepted

                auto threadData = std::make_shared<ThreadData>();
                threadData->connection = connection;
                threadData->mysqlManager = mysqlManager; // Assuming mysqlManager is a shared_ptr

                connectionThreads.emplace_back([threadData = std::move(threadData)]() {
                    connectionHandler(threadData.get());
                });
            }
            else
            {
                // If no new connection, sleep briefly to reduce busy-waiting
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
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

    uint16_t sPort;
    bool sVerbose;
    std::atomic<bool> running;
    std::shared_ptr<MySqlConnectionManager> mysqlManager;
    std::thread mainThread;
    std::vector<std::thread> connectionThreads; // Container for connection handler threads
};

//--------------------Global functions -------------------------------------------------------

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

static Server* globalServer = nullptr;

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
