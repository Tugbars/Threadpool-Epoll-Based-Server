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

#include "plf/plf.h"
#include "mqs/mqs.h"
#include "aminicserver.h"
#include "mysql.h"


//--------------------Local defines  -------------------------------------------------------

//--------------------Local data -------------------------------------------------------

static int sLink = -1;
static bool sVerbose = false;

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

struct ThreadData {
    int connection; // Connection identifier or file descriptor
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
static void* connectionHandler(void* pData)
{
    static int32_t connectionId = 0;

    std::unique_ptr<ThreadData> data(static_cast<ThreadData*>(pData));

    //code for preparing the variables.
   
    auto conn = data->mysqlManager->getConnection();
    while (more)
    {
        msgCount++;
        MqsCldMsg_t request;
        if (sVerbose)
        {
            std::cout << "Waiting for message " << msgCount << " on connection " << _connectionId << std::endl;
        }
        int32_t messageLength = ReadMessage(connection, request);
        if (messageLength > 0)
        {
            more = handleRequest(connection, idDevice, request, rawDataSetAssemblyBuffer, maxAssemblyLength, conn);        
            time(&lastMessage);
        }
        else
        {
            time_t now = time(nullptr);
            bool activity = (lastMessage + MAX_INACTIVITY) > now;
            more = activity && sendHeartbeat(connection);
        }
    }
    pool->returnConnection(conn); //?!
    CloseSocket(connection);
    if (sVerbose)
    {
        std::cout << "Terminating connection " << _connectionId << std::endl;
    }
    return NULL;
}

/**
 * @class Server
 * @brief Manages the lifecycle and operations of a network server, including handling client connections.
 *
 * This class encapsulates the functionality required to start a server that listens on a specified port
 * and handles incoming connections in a multithreaded manner. It leverages modern C++ features such as
 * std::thread for concurrency and std::shared_ptr for memory management, adhering to RAII principles.
 *
 * The transition to this class-based version aims to maintain the same operational goals as previous 
 * implementations but with a more organized and object-oriented approach. The server maintains a main listening 
 * thread responsible for accepting incoming connections and spawns a new detached thread for each connection 
 * to handle client-server interaction independently. This design allows the server to manage multiple client 
 * connections concurrently by delegating each connection to a separate thread.
 *
 * Upon receiving a new connection, the server spawns a new thread to handle the connection, passing along
 * a shared pointer to a `MySqlConnectionManager` instance. This allows each connection handler to independently
 * and safely interact with the database.
 *
 * @section usage Usage
 * To use this class, create an instance with the desired port, verbosity flag, and a shared pointer to an
 * initialized `MySqlConnectionManager`. Call `start()` to begin listening for connections, and `stop()` to
 * terminate the server and clean up resources.
 *
 * @code
 *   auto mysqlManager = std::make_shared<MySqlConnectionManager>(verbose, configFilePath);
 *   Server server(8080, true, mysqlManager);
 *   server.start();
 *   // Server is now running
 *   server.stop();
 * @endcode
 *
 * @section threading Threading Model
 * The server runs its listening loop in a separate thread, spawned in the `start()` method. Each accepted
 * connection is handled in its own detached thread, allowing for concurrent handling of multiple clients.
 * This threading model is designed to align with the operational goals of handling multiple connections 
 * concurrently in a more structured and manageable way compared to traditional implementations.
 *
 * @note Detaching threads simplifies management but requires careful consideration of resource lifetimes
 * and synchronization. Ensure that any shared resources are thread-safe and properly synchronized.
 *
 * @param sPort The port number on which the server will listen for incoming connections.
 * @param sVerbose Boolean flag indicating whether verbose logging is enabled.
 * @param mysqlManager A shared pointer to a `MySqlConnectionManager` instance for database operations.
 *
 * The class is designed with thread safety in mind; however, proper synchronization of shared resources
 * accessed by connection handlers is the responsibility of those resources (e.g., the `MySqlConnectionManager`).
 */
class Server {
public:
    // Update the constructor to accept a shared pointer to MySqlConnectionManager
    Server(uint16_t port, bool verbose, std::shared_ptr<MySqlConnectionManager> mysqlManager)
    : sPort(port), sVerbose(verbose), mysqlManager(mysqlManager) {
    sLink = Listen(sPort); // Initialize listening on the specified port. // just like the old serverInit(uint16_t port, bool verbose)
    }

    void start() {
        running = true;
        mainThread = std::thread(&Server::run, this);
    }

    void stop() {
        running = false;
        if (mainThread.joinable()) {
            mainThread.join();
        }
        // Clean up resources, close connections, etc.
    }

private:
    void run() {
        while (running) {
            int connection = amilinkAccept(sLink); //just like the static void* serverThread(void* dummy)
            if (connection < 0) {
                continue; // Handle error or break if server is stopping
            }

            // Make sure to capture mysqlManager in the lambda to use it inside connectionHandler
            auto threadData = std::make_shared<ThreadData>(ThreadData{connection, mysqlManager});
            /**
             * @brief Spawns and detaches a new thread for handling a client connection.
             * 
             * This code snippet creates a new thread dedicated to handling a specific client connection. The thread is 
             * provided with a shared pointer to `ThreadData`, which contains the connection details and a shared pointer 
             * to the `MySqlConnectionManager`. This allows the connection handler function to access the necessary 
             * resources for processing the client request. After the thread is created, it is immediately detached,
             * meaning it will run independently from the thread that spawned it. The detached thread is responsible for
             * completing its task (handling the client connection) and will be automatically cleaned up by the system 
             * once it finishes its execution.
             * 
             * Detaching a thread is useful in scenarios where the parent thread does not need to wait for the child 
             * thread to finish before continuing its execution. This is particularly relevant for server applications 
             * that handle multiple client connections simultaneously, allowing the server to remain responsive to new 
             * connections while previous connections are being processed in the background.
             * 
             * @param threadData A `std::shared_ptr<ThreadData>` object containing the necessary data for the connection 
             * handler, including the client connection identifier and a pointer to the MySQL connection manager. The 
             * shared pointer ensures that the resources are managed safely across threads, adhering to RAII principles.
             * 
             * @note It is crucial to ensure that any shared resources accessed by the detached thread, such as the MySQL 
             * connection manager, are thread-safe and properly synchronized to prevent data races or other concurrency 
             * issues. Additionally, the lifetime of shared resources must be managed carefully to ensure they remain 
             * valid for the duration of the thread's execution.
             * 
             * Usage of detached threads simplifies resource management but requires careful consideration of the detached 
             * thread's lifetime and the thread-safety of shared resources.
             */
            std::thread([threadData]() {
                connectionHandler(threadData.get());
            }).detach(); // Detach the thread //bunu tam anlamıyorum işte. tam anlamadığım bu.
        }
    }

    uint16_t sPort;
    bool sVerbose;
    bool running = false;
    std::shared_ptr<MySqlConnectionManager> mysqlManager; // Correctly store the mysqlManager
    std::thread mainThread;
};

//--------------------Global functions -------------------------------------------------------

std::unique_ptr<Server> serverInstance;
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
void serverInit(uint16_t port, bool verbose) {
    sVerbose = verbose;
    std::cout << "Preparing to listen on port " << port << std::endl;

    // Initialize the MySQL connection manager, if not already initialized elsewhere.
    auto mysqlManager = std::make_shared<MySqlConnectionManager>(verbose, "path/to/db_config.txt");
    
    // Create and start the server instance.
    serverInstance = std::make_unique<Server>(port, verbose, mysqlManager);
    serverInstance->start();

    std::cout << "Server initialized and listening on port " << port << std::endl;
}

