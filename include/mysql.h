#ifndef MYSQL_H
#define MYSQL_H

#include <vector>
#include "mqs_def.h"
#include <string>
#include <cstring>
#include <memory>
#include <mutex>
#include <queue>

#include <mysql_connection.h>
#include <driver.h>
#include <exception.h>
#include <resultset.h>
#include <statement.h>
#include <prepared_statement.h>

// Define the MySqlConnectionPool class directly in the header file
class MySqlConnectionPool {
public:
    MySqlConnectionPool(int poolSize, const std::string& dbUri, const std::string& user, const std::string& password);
    ~MySqlConnectionPool();
    std::shared_ptr<sql::Connection> getConnection();
    void returnConnection(std::shared_ptr<sql::Connection> conn);

private:
    bool isConnectionValid(std::shared_ptr<sql::Connection> conn);
    void createConnection();
    std::shared_ptr<sql::Connection> createNewConnection();

    std::queue<std::shared_ptr<sql::Connection>> connectionQueue;
    std::mutex mutex;
    int poolSize;
    std::string dbUri;
    std::string user;
    std::string password;
};

class MySqlConnectionManager {
public:
    MySqlConnectionManager(bool verbose, const std::string& configFile);
    ~MySqlConnectionManager();
    std::shared_ptr<sql::Connection> getConnection();
    void returnConnection(std::shared_ptr<sql::Connection> conn);

private:
    bool sVerbose;
    std::unique_ptr<MySqlConnectionPool> pool;
    bool createTables();
};

// Define a global function to get the connection pool instance

// Declare the getConnection and returnConnection functions

struct BatchCodeInfo {
    char batchID[8];
    uint32_t meat_type;
};

// Modify BatchInfo to use a vector of BatchCodeInfo structs
struct BatchInfo {
    std::vector<BatchCodeInfo> batchCodes;
    int totalCount;
};

extern std::unique_ptr<MySqlConnectionManager> mysqlInit(bool verbose);
extern BatchInfo getStructBatchCodesByCurrentDate(int companyId, std::shared_ptr<sql::Connection> conn);
extern int getCompanyIDFromDevice(uint64_t deviceID, std::shared_ptr<sql::Connection> conn);
