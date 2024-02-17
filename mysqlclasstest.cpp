#include <stdlib.h>
#include <iostream>
#include <mysql_connection.h>
#include <driver.h>
#include <exception.h>
#include <resultset.h>
#include <statement.h>
#include <prepared_statement.h>
#include <sstream>

#include <chrono>  // For std::chrono
#include <ctime>   // For std::time_t
#include <iomanip> // For std::put_time

#include <queue>
#include <mutex>
#include <memory>
#include <thread>

//#include "mysql.h"
#include "mqs_def.h"

#include <gtest/gtest.h>

#include <fstream>
#include <unordered_map>


std::unordered_map<std::string, std::string> loadConfig(const std::string& configFile) {
	std::unordered_map<std::string, std::string> config;
	std::ifstream file(configFile);
	std::string line;
	while (std::getline(file, line)) {
		auto delimiterPos = line.find('=');
		if (delimiterPos != std::string::npos) {
			auto key = line.substr(0, delimiterPos);
			auto value = line.substr(delimiterPos + 1);

			// Trim leading and trailing whitespace characters
			auto valueBegin = value.find_first_not_of(" \t'");
			auto valueEnd = value.find_last_not_of(" \t'");
			if (valueBegin != std::string::npos && valueEnd != std::string::npos) {
				value = value.substr(valueBegin, valueEnd - valueBegin + 1);
			}

			config[key] = value;
		}
	}
	return config;
}
// check if these are correctly designed.

/**
 * @class MySqlConnectionPool
 * @brief Manages a pool of connections to a MySQL database.
 *
 * MySqlConnectionPool implements a connection pool for MySQL connections. It maintains a specified number
 * of connections in a pool, allowing for efficient reuse across different parts of an application.
 * This class is designed to manage database connections in a way that avoids the overhead associated with
 * repeatedly opening and closing connections. It also handles invalid connections by replacing them with new ones.
 *
 * Usage:
 * - Create an instance of MySqlConnectionPool with the desired number of connections and database credentials.
 * - Use getConnection to fetch an available connection from the pool for database operations.
 * - After completing operations, return the connection to the pool using returnConnection.
 *
 * Thread Safety:
 * - The class uses std::mutex to ensure thread safety, making it suitable for use in multi-threaded environments.
 *
 * Exception Handling:
 * - Throws std::runtime_error if no connections are available when requested.
 *
 * @note It's important to ensure that connections are always returned to the pool after use to avoid resource leaks.
 */
class MySqlConnectionPool
{
public:
    /**
     * @brief Constructs a connection pool with the specified size and database credentials.
     * @param poolSize The number of connections to maintain in the pool.
     * @param dbUri The URI of the database to connect to.
     * @param user The username for database access.
     * @param password The password for database access.
     */
    MySqlConnectionPool(int poolSize, const std::string& dbUri, const std::string& user, const std::string& password)
        : poolSize(poolSize), dbUri(dbUri), user(user), password(password)
    {
        for (int i = 0; i < poolSize; ++i)
        {
            createConnection();
        }
    }

    ~MySqlConnectionPool()
    {
        // Release resources held by the connection pool
        while (!connectionQueue.empty())
        {
            connectionQueue.pop(); // Clear the connection queue
        }
        // No need to explicitly release mutex, std::mutex destructor takes care of it
    }

    /**
     * @brief Fetches an available connection from the pool.
     * @return A shared pointer to an available sql::Connection object.
     * @throw std::runtime_error if no connections are available in the pool.
     */
    std::shared_ptr<sql::Connection> getConnection()
    {
        const int maxAttempts = 3;                                 // Maximum number of attempts to get a connection
        const std::chrono::milliseconds delayBetweenAttempts(100); // Delay between attempts

        std::unique_lock<std::mutex> lock(mutex);
        for (int attempt = 0; attempt < maxAttempts; ++attempt)
        {
            while (!connectionQueue.empty())
            {
                auto conn = connectionQueue.front();
                connectionQueue.pop(); // Immediately remove the connection from the queue
                if (isConnectionValid(conn))
                {
                    return conn; // Return the valid connection
                }
                else
                {
                    // Connection is not valid; attempt to create a new one and add it to the pool
                    try
                    {
                        auto newConn = createNewConnection();
                        if (newConn)
                        {
                            // Add the new connection back to the pool for future use
                            connectionQueue.push(newConn);
                            // Use continue to immediately proceed to attempt to fetch another connection
                            continue;
                        }
                    }
                    catch (const std::exception& e)
                    {
                        std::cerr << "Error creating a new connection: " << e.what() << std::endl;
                    }
                }
            }

            // If no valid connection was found and the queue is empty, wait before retrying
            if (attempt < maxAttempts - 1)
            {
                lock.unlock(); // Unlock the mutex while waiting
                std::this_thread::sleep_for(delayBetweenAttempts);
                lock.lock(); // Re-lock the mutex before retrying
            }
        }

        // After all attempts, if no connection is available, throw an exception
        throw std::runtime_error("Failed to obtain a database connection after multiple attempts");
    }

    /**
     * @brief Returns a used connection back to the pool for future reuse.
     * @param conn The connection to be returned to the pool.
     */
    void returnConnection(std::shared_ptr<sql::Connection> conn) {
    std::unique_lock<std::mutex> lock(mutex);
    if (isConnectionValid(conn)) {
        connectionQueue.push(conn);
    } else {
        // Connection is no longer valid. Discard and create a new one for the pool.
        auto newConn = createNewConnection();
        if (newConn) {
            connectionQueue.push(newConn);
           }
        }
    }

private:
    /**
     * @brief Checks if a connection is still valid.
     * @param conn The database connection to check.
     * @return True if the connection is valid, False otherwise.
     */
    bool isConnectionValid(std::shared_ptr<sql::Connection> conn)
    {
        try
        {
            std::unique_ptr<sql::Statement> stmt(conn->createStatement());
            std::unique_ptr<sql::ResultSet> res(stmt->executeQuery("SELECT 1"));
            return res->next(); // Check if query executed successfully
        }
        catch (const sql::SQLException&)
        {
            return false; // Connection is not valid
        }
    }
    /**
     * @brief Creates and adds a new connection to the pool.pushes it inside the pool.
     * This function is called during pool initialization and when replacing invalid connections.
     */
    void createConnection()
    {
        try
        {
            auto conn = createNewConnection();
            if (conn)
            {
                connectionQueue.push(conn);
            }
        }
        catch (const sql::SQLException& e)
        {
            std::cerr << "Connection creation failed: " << e.what() << std::endl;
        }
    }

    /**
     * @brief Creates a new database connection.
     * @return A shared pointer to a new sql::Connection object.
     */
    std::shared_ptr<sql::Connection> createNewConnection()
    {
        const int maxCreateAttempts = 3; // Maximum attempts to create a new connection
        for (int attempt = 0; attempt < maxCreateAttempts; ++attempt)
        {
            try
            {
                sql::Driver* driver = get_driver_instance();
                auto conn = std::shared_ptr<sql::Connection>(driver->connect(dbUri, user, password));
                if (conn)
                {
                    conn->setSchema("aminic"); // Select the aminic database
                    return conn;
                }
            }
            catch (const sql::SQLException& e)
            {
                std::cerr << "Attempt " << (attempt + 1) << " failed to create new connection: " << e.what() << std::endl;
                if (attempt < maxCreateAttempts - 1)
                {
                    std::this_thread::sleep_for(std::chrono::seconds(1)); // Wait before retrying
                }
            }
        }
        std::cerr << "Failed to create new connection after " << maxCreateAttempts << " attempts." << std::endl;
        return nullptr;
    }

    std::queue<std::shared_ptr<sql::Connection>> connectionQueue; ///< Queue to manage the pool of connections.
    std::mutex mutex;                                             ///< Mutex for thread safety.
    int poolSize;                                                 ///< The size of the connection pool.
    std::string dbUri;                                            ///< Database URI.
    std::string user;                                             ///< Database username.
    std::string password;                                         ///< Database password.
};

class MySqlConnectionManager
{
public:
    MySqlConnectionManager(bool verbose, const std::string& configFile) : sVerbose(verbose)
    {
        try
        {
            auto config = loadConfig(configFile);

            if (sVerbose)
            {
                std::cout << "Initializing connection pool with size: " << config["poolSize"] << std::endl;
            }

            int poolSize = std::stoi(config["poolSize"]);
            pool = std::make_unique<MySqlConnectionPool>(poolSize, config["dbUri"], config["user"], config["password"]);

            if (!createTables())
            {
                std::cerr << "Failed to create tables" << std::endl;
                throw std::runtime_error("Failed to create tables");
            }

            std::cout << "Database initialized successfully" << std::endl;
        }
        catch (const std::exception &e)
        {
            std::cerr << "Initialization Error: " << e.what() << std::endl;
            throw; // Rethrow the exception to the caller
        }
    }   

    ~MySqlConnectionManager()
    {
        pool.reset(); // Release the connection pool
    }

    std::shared_ptr<sql::Connection> getConnection()
    {
        return pool->getConnection();
    }

    void returnConnection(std::shared_ptr<sql::Connection> conn)
    {
        pool->returnConnection(conn);
    }

private:
    bool sVerbose;
    std::unique_ptr<MySqlConnectionPool> pool; // Manage the connection pool

    bool createTables()
    {
        // Your table creation logic here
        return true;
    }
};

std::unique_ptr<MySqlConnectionManager> mysqlInit(bool verbose) {   
    try {
         
        std::string configFile = "db_config.txt";
        auto manager = std::make_unique<MySqlConnectionManager>(verbose, configFile);
        // Any other initialization logic specific to your application
        return manager;
    }
    catch (const std::exception& e) {
        std::cerr << "Failed to initialize MySQL connection manager: " << e.what() << std::endl;
        return nullptr; // Return nullptr to indicate failure
    }
}

int getCompanyIDFromDevice(uint64_t deviceID, std::shared_ptr<sql::Connection> conn)
{
    int companyID = 0; // Default value if no results are found
    //int64_t signedDeviceID = static_cast<int64_t>(deviceID);
    int64_t signedDeviceID = static_cast<int64_t>(deviceID);

    try
    {
        // Create a prepared statement to fetch the Company ID based on the Device ID
        auto delStmt = [](sql::PreparedStatement* pstmt)
        { delete pstmt; };
        std::unique_ptr<sql::PreparedStatement, decltype(delStmt)> pstmt(
            conn->prepareStatement("SELECT Company FROM Device WHERE ID = ?"),
            delStmt);

        pstmt->setInt64(1, signedDeviceID);
        std::cerr << "signedDeviceID: " << signedDeviceID << std::endl;

        // Custom deleter lambda function
        auto delRes = [](sql::ResultSet* res)
        { delete res; };
        // Creating a unique_ptr to manage the sql::ResultSet
        std::unique_ptr<sql::ResultSet, decltype(delRes)> res(
            pstmt->executeQuery(), //  execute the SQL query and return
            delRes                 // The custom deleter, will be called automatically when the ptr goes out of the scope.
        );

        if (res->next())
        {
            // Fetch and set the Company ID
            companyID = res->getInt("Company");
        }
        else
        {
            std::cerr << "No entry found for the provided Device ID." << std::endl;
        }
    }
    catch (const sql::SQLException& e)
    {
        std::cerr << "MySQL Error: " << e.what() << std::endl;
    }

    return companyID; // Return 0 if no entry is found or the retrieved Company ID
}


std::string getCurrentTimeAsString()
{
    auto now = std::chrono::system_clock::now();
    std::time_t currentTime = std::chrono::system_clock::to_time_t(now);

    std::tm localTime = *std::localtime(&currentTime);

    std::ostringstream oss;
    oss << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

BatchInfo getStructBatchCodesByCurrentDate(int companyId, std::shared_ptr<sql::Connection> conn)
{
    BatchInfo batchInfo;
    try
    {
        // Prepare the SQL query
        std::string query = "SELECT BatchCode, MeatType FROM Batch WHERE CompanyID = ? AND StartTime <= ? AND EndTime >= ?";
        std::unique_ptr<sql::PreparedStatement> pstmt(conn->prepareStatement(query));
        pstmt->setInt(1, companyId);
        pstmt->setString(2, getCurrentTimeAsString());
        pstmt->setString(3, getCurrentTimeAsString());

        // Execute the query and obtain the result set
        std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());

        if (res->rowsCount() > 0)
        {
            while (res->next())
            {
                std::string batchCode = res->getString("BatchCode");
                uint32_t meatType = res->getUInt("MeatType");

                BatchCodeInfo batchCodeInfo;
                std::strncpy(batchCodeInfo.batchID, batchCode.c_str(), sizeof(batchCodeInfo.batchID) - 1);
                batchCodeInfo.batchID[sizeof(batchCodeInfo.batchID) - 1] = '\0'; // Ensuring null termination
                batchCodeInfo.meat_type = meatType;

                batchInfo.batchCodes.push_back(batchCodeInfo);
            }

            res->last();
            batchInfo.totalCount = res->getRow();
        }
        else
        {
            batchInfo.totalCount = 0;
        }
    }
    catch (const sql::SQLException &e)
    {
        std::cerr << "MySQL Error: " << e.what() << std::endl;
        batchInfo.totalCount = 0;
    }

    return batchInfo;
}




