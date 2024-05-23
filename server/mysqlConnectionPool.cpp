#include <stdlib.h>
#include <iostream>
#include <sstream>
#include <thread>
#include <stdexcept>
#include <chrono>  // For std::chrono
#include <ctime>   // For std::time_t
#include <iomanip> // For std::put_time
#include <memory>
#include <fstream>
#include <unordered_map>

#include "mysql.h"
#include "mqs_def.h"

// Function declarations and config loader

static bool executeSqlSimple(const std::string &query, std::shared_ptr<sql::Connection> conn);

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

MySqlConnectionPool::MySqlConnectionPool(int poolSize, const std::string& dbUri, const std::string& user, const std::string& password)
    : poolSize(poolSize), dbUri(dbUri), user(user), password(password) {
    for (int i = 0; i < poolSize; ++i) {
        createConnection();
    }
}

MySqlConnectionPool::~MySqlConnectionPool() {
    // Release resources held by the connection pool
        while (!connectionQueue.empty())
        {
            connectionQueue.pop(); // Clear the connection queue
        }
    // No need to explicitly release mutex, std::mutex destructor takes care of it
}

std::shared_ptr<sql::Connection> MySqlConnectionPool::getConnection() {
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

bool MySqlConnectionPool::isConnectionValid(std::shared_ptr<sql::Connection> conn) {
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

void MySqlConnectionPool::returnConnection(std::shared_ptr<sql::Connection> conn) {
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

void MySqlConnectionPool::createConnection() {
    try {
        auto conn = createNewConnection();
        if (conn) {
            connectionQueue.push(conn);
        }
    } catch (const sql::SQLException& e) {
        std::cerr << "Connection creation failed: " << e.what() << std::endl;
    }
}

std::shared_ptr<sql::Connection> MySqlConnectionPool::createNewConnection() {
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

MySqlConnectionManager::MySqlConnectionManager(bool verbose, const std::string& configFile) : sVerbose(verbose) {
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

MySqlConnectionManager::~MySqlConnectionManager() {
     pool.reset(); // Release the connection pool
}

std::shared_ptr<sql::Connection> MySqlConnectionManager::getConnection() {
    return pool->getConnection();
}

void MySqlConnectionManager::returnConnection(std::shared_ptr<sql::Connection> conn) {
    pool->returnConnection(conn);
}

bool MySqlConnectionManager::createTables() {
    return true;
}  

std::unique_ptr<MySqlConnectionManager> mysqlInit(bool verbose) {   
    try {
        std::string configFile = "db_config.txt";
        auto manager = std::make_unique<MySqlConnectionManager>(verbose, configFile);
        return manager;
    } catch (const std::exception& e) {
        std::cerr << "Failed to initialize MySQL connection manager: " << e.what() << std::endl;
        return nullptr;
    }
}

// Function to get Company ID from Device

int getCompanyIDFromDevice(uint64_t deviceID, std::shared_ptr<sql::Connection> conn) {
    int companyID = 0;
    int64_t signedDeviceID = static_cast<int64_t>(deviceID);
    try {
        auto delStmt = [](sql::PreparedStatement* pstmt) { delete pstmt; };
        std::unique_ptr<sql::PreparedStatement, decltype(delStmt)> pstmt(
            conn->prepareStatement("SELECT COLUMN_NAME FROM TABLE_NAME WHERE COLUMN_NAME = ?"),
            delStmt);

        pstmt->setInt64(1, signedDeviceID);
        std::cerr << "signedDeviceID: " << signedDeviceID << std::endl;

        auto delRes = [](sql::ResultSet* res) { delete res; };
        std::unique_ptr<sql::ResultSet, decltype(delRes)> res(
            pstmt->executeQuery(),
            delRes
        );

        if (res->next()) {
            companyID = res->getInt("COLUMN_NAME");
        } else {
            std::cerr << "No entry found for the provided Device ID." << std::endl;
        }
    } catch (const sql::SQLException& e) {
        std::cerr << "MySQL Error: " << e.what() << std::endl;
    }

    return companyID;
}

// Function to fetch unique meat types for a company

std::vector<int> getUniqueMeatTypesForCompany(int companyID, std::shared_ptr<sql::Connection> conn) {
    std::vector<int> uniqueMeatTypes;
    try {
        auto delStmt = [](sql::PreparedStatement* pstmt) { delete pstmt; };
        std::unique_ptr<sql::PreparedStatement, decltype(delStmt)> pstmt(
            conn->prepareStatement("SELECT DISTINCT COLUMN_NAME FROM TABLE_NAME WHERE COLUMN_NAME = ?"),
            delStmt);

        pstmt->setInt(1, companyID);

        auto delRes = [](sql::ResultSet* res) { delete res; };
        std::unique_ptr<sql::ResultSet, decltype(delRes)> res(
            pstmt->executeQuery(),
            delRes
        );

        while (res->next()) {
            uniqueMeatTypes.push_back(res->getInt("COLUMN_NAME"));
        }
    } catch (const sql::SQLException& e) {
        std::cerr << "MySQL Error: " << e.what() << std::endl;
    }

    return uniqueMeatTypes;
}

// Utility function to get current time as string

std::string getCurrentTimeAsString() {
    auto now = std::chrono::system_clock::now();
    std::time_t currentTime = std::chrono::system_clock::to_time_t(now);
    std::tm localTime = *std::localtime(&currentTime);
    std::ostringstream oss;
    oss << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

// Function to get batch codes by current date

BatchInfo getStructBatchCodesByCurrentDate(int companyId, std::shared_ptr<sql::Connection> conn) {
    BatchInfo batchInfo;
    try {
        std::string query = "SELECT COLUMN_NAME1, COLUMN_NAME2, COLUMN_NAME3 FROM TABLE_NAME WHERE COLUMN_NAME4 = ? AND COLUMN_NAME5 <= ? AND COLUMN_NAME6 >= ?";
        std::unique_ptr<sql::PreparedStatement> pstmt(conn->prepareStatement(query));
        pstmt->setInt(1, companyId);
        pstmt->setString(2, getCurrentTimeAsString());
        pstmt->setString(3, getCurrentTimeAsString());

        std::unique_ptr<sql::ResultSet> res(pstmt->executeQuery());

        if (res->rowsCount() > 0) {
            while (res->next()) {
                std::string batchCode = res->getString("COLUMN_NAME1");
                uint32_t meatType = res->getUInt("COLUMN_NAME2");
                uint32_t batch_uid = res->getUInt("COLUMN_NAME3");
                std::cerr << "batch_uid " << batch_uid << std::endl;

                BatchCodeInfo batchCodeInfo;
                std::strncpy(batchCodeInfo.batchID, batchCode.c_str(), sizeof(batchCodeInfo.batchID) - 1);
                batchCodeInfo.batchID[sizeof(batchCodeInfo.batchID) - 1] = '\0';
                batchCodeInfo.meat_type = meatType;
                batchCodeInfo.batch_uid = batch_uid;

                batchInfo.batchCodes.push_back(batchCodeInfo);
            }

            res->last();
            batchInfo.totalCount = res->getRow();
        } else {
            batchInfo.totalCount = 0;
        }
    } catch (const sql::SQLException &e) {
        std::cerr << "MySQL Error: " << e.what() << std::endl;
        batchInfo.totalCount = 0;
    }

    return batchInfo;
}

// Simple SQL execution function

static bool executeSqlSimple(const std::string &query, std::shared_ptr<sql::Connection> conn) {
    bool success = false;
    try {
        std::unique_ptr<sql::Statement> stmt(conn->createStatement());
        stmt->execute(query);
        success = true;
    } catch (const sql::SQLException &e) {
        std::cerr << "MySQL Error: " << e.what() << std::endl;
        success = false;
    }
    return success;
}

// Insert raw data points

static bool insertRawDatapoints(const MqsRawDataSet_t &rawDataSet, uint32_t dataCount, uint32_t raw_data_sweep_id, uint32_t dataOffset, std::shared_ptr<sql::Connection> conn) {
    bool ok = true;
    if (dataCount > 0) {
        try {
            std::unique_ptr<sql::PreparedStatement> pstmt(
                conn->prepareStatement("INSERT INTO TABLE_NAME (COLUMN_NAME1, COLUMN_NAME2, COLUMN_NAME3, COLUMN_NAME4) VALUES (?, ?, ?, ?)"));
            for (uint32_t i = 0; i < dataCount; i++) {
                const MqsRawDataPoint_t &rawDataPoint = rawDataSet.data[dataOffset + i];
                pstmt->setUInt(1, raw_data_sweep_id);
                pstmt->setUInt(2, i);
                pstmt->setDouble(3, rawDataPoint.phaseAngle);
                pstmt->setDouble(4, rawDataPoint.impedance);
                pstmt->executeUpdate();
            }
        } catch (const sql::SQLException &e) {
            std::cerr << "MySQL Error: " << e.what() << std::endl;
            ok = false;
        }
    }
    return ok;
}

// Insert raw data sweep

static uint32_t insertRawDataSweep(const MqsRawDataSweep_t &rawDataSweep, std::shared_ptr<sql::Connection> conn) {
    try {
        auto delStmt = [](sql::PreparedStatement *pstmt) { delete pstmt; };
        std::unique_ptr<sql::PreparedStatement, decltype(delStmt)> pstmt(
            conn->prepareStatement("INSERT INTO TABLE_NAME (COLUMN_NAME1, COLUMN_NAME2) VALUES (?, ?)"),
            delStmt);
        pstmt->setDouble(1, rawDataSweep.startFrequency);
        pstmt->setDouble(2, rawDataSweep.frequencyIncrement);
        pstmt->executeUpdate();

        auto delStmt2 = [](sql::Statement *stmt) { delete stmt; };
        std::unique_ptr<sql::Statement, decltype(delStmt2)> stmt(
            conn->createStatement(),
            delStmt2);

        auto delRes = [](sql::ResultSet *res) { delete res; };
        std::unique_ptr<sql::ResultSet, decltype(delRes)> result(
            stmt->executeQuery("SELECT LAST_INSERT_ID()"),
            delRes);

        uint32_t lastInsertID = 0;
        if (result->next()) {
            lastInsertID = result->getUInt(1);
        }
        return lastInsertID;
    } catch (const sql::SQLException &e) {
        std::cerr << "MySQL Error: " << e.what() << std::endl;
        return 0;
    }
}

// Insert raw data sweep and data points

static uint32_t insertRawDataSweepAndDatapoints(const MqsRawDataSet_t &rawDataSet, const MqsRawDataSweep_t &rawDataSweep, uint32_t dataOffset, std::shared_ptr<sql::Connection> conn) {
    uint32_t raw_data_sweep_id = insertRawDataSweep(rawDataSweep, conn);
    insertRawDatapoints(rawDataSet, rawDataSweep.dataCount, raw_data_sweep_id, dataOffset, conn);
    return raw_data_sweep_id;
}

// Insert dataset

bool mysqlInsertDataSet(uint64_t idDevice, MqsDataSet_t &dataSet, std::shared_ptr<sql::Connection> conn) {
    try {
        std::unique_ptr<sql::PreparedStatement> pstmt(
            conn->prepareStatement(
                "INSERT INTO TABLE_NAME "
                "(COLUMN_NAME1, COLUMN_NAME2, COLUMN_NAME3, COLUMN_NAME4, COLUMN_NAME5, COLUMN_NAME6, COLUMN_NAME7, COLUMN_NAME8, COLUMN_NAME9, COLUMN_NAME10) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, FROM_UNIXTIME(?), ?)"));
        pstmt->setUInt64(1, dataSet.idMeasurement);
        pstmt->setUInt64(2, idDevice);
        pstmt->setUInt(3, dataSet.idCartridge);
        pstmt->setUInt(4, dataSet.versionDevice);
        pstmt->setUInt(5, dataSet.versionAlgorithm);
        pstmt->setUInt(6, dataSet.meatType);
        pstmt->setDouble(7, dataSet.meatQualityIndex);
        pstmt->setDouble(8, dataSet.meatTemperature);
        pstmt->setUInt64(9, dataSet.timestamp);
        pstmt->setUInt(10, dataSet.batch_uid);
        pstmt->executeUpdate();

        std::unique_ptr<sql::PreparedStatement> pstmtRaw(
            conn->prepareStatement(
                "INSERT INTO TABLE_NAME "
                "(COLUMN_NAME1, COLUMN_NAME2, COLUMN_NAME3, COLUMN_NAME4, COLUMN_NAME5) "
                "VALUES (?, ?, ?, ?, ?)"));
        pstmtRaw->setUInt64(1, idDevice);
        pstmtRaw->setUInt(2, dataSet.idMeasurement);
        pstmtRaw->setDouble(3, dataSet.sweep1fs);
        pstmtRaw->setDouble(4, dataSet.sweep2fs);
        pstmtRaw->setUInt(5, static_cast<unsigned int>(dataSet.initFreq));
        pstmtRaw->executeUpdate();

        return true;
    } catch (const sql::SQLException &e) {
        std::cerr << "MySQL Error: " << e.what() << std::endl;
        return false;
    }
}

// Insert raw data set

static bool insertRawDataSet(uint64_t idDevice, uint32_t idMeasurement, uint32_t baseId, uint32_t afterExposureId, std::shared_ptr<sql::Connection> conn) {
    try {
        std::unique_ptr<sql::PreparedStatement> pstmt(
            conn->prepareStatement(
                "UPDATE TABLE_NAME "
                "SET COLUMN_NAME1 = ?, COLUMN_NAME2 = ? "
                "WHERE COLUMN_NAME3 = ? AND COLUMN_NAME4 = ?"));

        pstmt->setUInt(1, baseId);
        pstmt->setUInt(2, afterExposureId);
        pstmt->setUInt64(3, idDevice);
        pstmt->setUInt(4, idMeasurement);
        pstmt->executeUpdate();

        return true;
    } catch (const sql::SQLException &e) {
        std::cerr << "MySQL Error: " << e.what() << std::endl;
        return false;
    }
}

// Insert raw data set and data points

bool mysqlInsertRawDataSet(uint64_t idDevice, const MqsRawDataSet_t &rawDataSet, std::shared_ptr<sql::Connection> conn) {
    uint32_t baseId = insertRawDataSweepAndDatapoints(rawDataSet, rawDataSet.base, 0, conn);
    uint32_t afterExposureId = insertRawDataSweepAndDatapoints(rawDataSet, rawDataSet.afterExposure, rawDataSet.base.dataCount, conn);
    bool done = insertRawDataSet(idDevice, rawDataSet.idMeasurement, baseId, afterExposureId, conn);
    return done;
    std::string query;
    return executeSqlSimple(query, conn);
}

// Update raw data required

bool mysqlUpdateDataRequired(uint64_t idDevice, uint32_t idMeasurement, int32_t rawDatasetRequired, std::shared_ptr<sql::Connection> conn) {
    try {
        std::unique_ptr<sql::PreparedStatement> pstmt(
            conn->prepareStatement(
                "UPDATE TABLE_NAME SET COLUMN_NAME = ? WHERE COLUMN_NAME2 = ? AND COLUMN_NAME3 = ?"));

        pstmt->setInt(1, rawDatasetRequired);
        pstmt->setUInt64(2, idDevice);
        pstmt->setUInt(3, idMeasurement);
        pstmt->executeUpdate();

        return true;
    } catch (const sql::SQLException &e) {
        std::cerr << "MySQL Error: " << e.what() << std::endl;
        return false;
    }
}

// Get data required

uint32_t mysqlGetDataRequired(uint64_t idDevice, std::shared_ptr<sql::Connection> conn) {
    try {
        std::string query =
            "SELECT COLUMN_NAME FROM TABLE_NAME "
            "WHERE COLUMN_NAME2 = ? AND COLUMN_NAME3 > 0 "
            "AND COLUMN_NAME4 NOT IN "
            "(SELECT COLUMN_NAME4 FROM TABLE_NAME2 WHERE COLUMN_NAME5 = ?)";

        std::unique_ptr<sql::PreparedStatement> pstmt(
            conn->prepareStatement(query));

        pstmt->setUInt64(1, idDevice);
        pstmt->setUInt64(2, idDevice);

        std::unique_ptr<sql::ResultSet> result(pstmt->executeQuery());

        uint32_t value = 0;
        if (result->next()) {
            value = result->getUInt(1);
            if (true) {
                std::cout << "Executed " << query << " with result " << value << std::endl;
            }
        } else {
            if (true) {
                std::cout << "Executed " << query << " with empty result set" << std::endl;
            }
        }

        return value;
    } catch (const sql::SQLException &e) {
        std::cerr << "MySQL Error: " << e.what() << std::endl;
        return 0;
    }
}
