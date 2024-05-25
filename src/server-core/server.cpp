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
#include <stdexcept>
#include <fstream>

#include "amilink/amilink.h"
#include "plf/plf.h"
#include "mqs/mqs.h"
#include "server.h"
#include "mysql.h"

// include the file library in your makefile.
#include <nlohmann/json.hpp>
#include <fcntl.h> // For the O_NONBLOCK flag

//--------------------Local defines  -------------------------------------------------------

//--------------------Local data -------------------------------------------------------

static bool sVerbose = false;
static std::atomic<int32_t> connectionId(0); // Global atomic counter for connection IDs
static bool isAlgorithmNeeded = false;

Server *globalServer = nullptr;

//--------------------Forward declarations of functions -------------------------------------

//--------------------Local functions -------------------------------------------------------

static bool checkAssemblyBuffer(const MqsCldFile_t &block, void *&rawDataSetAssemblyBuffer, uint32_t &maxAssemblyLength)
{
    uint32_t offset = block.offset;
    uint32_t length = block.length;
    uint32_t totalLength = block.totalLength;
    bool ok = false;
    if ((offset + length <= totalLength)) // 0 gelirse de buraya giriyoruz code.
    {
        if (offset == 0) // gelen very 0 da olabilir
        {
            if (length >= sizeof(MqsRawDataSet_t))
            {
                const MqsRawDataSet_t *pRawDataSet = (MqsRawDataSet_t *)&(block.data);
                uint32_t dataCount = pRawDataSet->base.dataCount + pRawDataSet->afterExposure.dataCount;
                uint32_t maxLength = sizeof(MqsRawDataSet_t) + dataCount * sizeof(MqsRawDataPoint_t);
                if (totalLength == maxLength)
                {
                    ok = true;
                    rawDataSetAssemblyBuffer = realloc(rawDataSetAssemblyBuffer, maxLength);
                    maxAssemblyLength = maxLength;
                    if (sVerbose)
                    {
                        printf("Allocated buffer for rawDataSet_t of %d bytes\n", maxAssemblyLength);
                    }
                }
                else
                {
                    fprintf(stderr, "Error, Raw data length mismatch, totalLength=%d, maxLength=%d\n", totalLength, maxLength);
                }
            }
            else
            {
                fprintf(stderr, "Error, Raw data to short, length=%d\n", length);
            }
        }
        else
        {
            if (totalLength == maxAssemblyLength)
            {
                ok = true;
            }
            else
            {
                fprintf(stderr, "Error, Total length must not change during transfer, totalLength=%d, maxAssemblyLength=%d\n", totalLength, maxAssemblyLength);
            }
        }
    }
    else
    {
        fprintf(stderr, "Error, Raw data to long, offset=%d, length=%d, totalLength=%d", offset, length, totalLength);
    }
    return ok;
}

static bool hasPostfix(std::string const &fullString, std::string const &postfix)
{
    int32_t postfixSize = postfix.size();
    int32_t fullStringSize = fullString.size();
    if (fullStringSize >= postfixSize)
    {
        return (0 == fullString.compare(fullStringSize - postfixSize, postfixSize, postfix));
    }
    else
    {
        return false;
    }
}

static bool hasPrefix(std::string const &fullString, std::string const &prefix)
{
    return fullString.compare(0, prefix.size(), prefix) == 0;
}

static uint32_t getFileVersion(std::string prefix, std::string postfix)
{
    DIR *dir;
    struct dirent *ent;
    uint32_t versionNumber = 0;
    if (sVerbose)
    {
        std::cout << "Searching version with prefix '" << prefix << "' and postfix '" << postfix << "'" << std::endl;
    }
    if ((dir = opendir(FILE_LIBRARY)) != NULL)
    {
        while ((ent = readdir(dir)) != NULL)
        {
            std::string filename(ent->d_name);
            bool match = hasPrefix(filename, prefix) && hasPostfix(filename, postfix);
            if (match)
            {
                std::string version = filename.substr(0, filename.size() - postfix.size()).substr(prefix.size());
                try
                {
                    // converts String into integer.
                    versionNumber = std::stoi(version, nullptr, 10);
                }
                catch (...)
                {
                    // nothing
                }
                if (sVerbose)
                {
                    std::cout << "Match <" << versionNumber << "> in " << filename << std::endl;
                }
            }
        }
        closedir(dir);
    }
    else
    {
        perror("Error with listing files");
    }
    return versionNumber;
}

static void handleDataLogPushRequest(int efd, int connection, uint64_t &idDevice, MqsCldMsg_t &request, std::shared_ptr<sql::Connection> conn)
{
    MqsDataSet_t dataSet = request.dataLogPushReq;
    mysqlInsertDataSet(idDevice, dataSet, conn);
    MqsCldMsg_t response = {.topic = MQS_DATA_LOG_PUSH_RESPONSE};

    // Set connection to EPOLLOUT before sending
    globalServer->modifyEpollEvent(efd, connection, EPOLLOUT | EPOLLET);
    amilinkSendMessage(connection, response);

    // Set connection back to EPOLLIN after sending
    globalServer->modifyEpollEvent(efd, connection, EPOLLIN | EPOLLET);
}


static void handleRawDataPushRequest(int efd, int connection, uint64_t &idDevice, MqsCldMsg_t &request, void *&rawDataSetAssemblyBuffer, uint32_t &maxAssemblyLength, std::shared_ptr<sql::Connection> conn)
{
    MqsCldFile_t block = request.rawDataPushReq;

    if (checkAssemblyBuffer(block, rawDataSetAssemblyBuffer, maxAssemblyLength))
    {
        uint32_t offset = block.offset;
        uint32_t length = block.length;
        uint32_t totalLength = block.totalLength;
        memcpy(((char *)rawDataSetAssemblyBuffer) + offset, block.data, block.length);
        if (offset + length == totalLength)
        {
            MqsRawDataSet_t *pRawDataSet = (MqsRawDataSet_t *)rawDataSetAssemblyBuffer;
            mysqlInsertRawDataSet(idDevice, *pRawDataSet, conn);
            mysqlUpdateDataRequired(idDevice, pRawDataSet->idMeasurement, 0, conn);
            MqsCldMsg_t response = {.topic = MQS_RAW_DATA_PUSH_RESPONSE};

            // Set connection to EPOLLOUT before sending
            globalServer->modifyEpollEvent(efd, connection, EPOLLOUT | EPOLLET);
            amilinkSendMessage(connection, response);

            // Set connection back to EPOLLIN after sending
            globalServer->modifyEpollEvent(efd, connection, EPOLLIN | EPOLLET);

            free(rawDataSetAssemblyBuffer);
            rawDataSetAssemblyBuffer = nullptr;
            maxAssemblyLength = 0;
        }
    }
}

static void handleRawDataRequiredRequest(int efd, int connection, uint64_t &idDevice, MqsCldMsg_t &request, std::shared_ptr<sql::Connection> conn)
{
    MqsCldMsg_t response = {.topic = MQS_RAW_DATA_REQUIRED_RESPONSE};
    response.rawDataRequiredResp.idMeasurement = mysqlGetDataRequired(idDevice, conn);

    // Set connection to EPOLLOUT before sending
    globalServer->modifyEpollEvent(efd, connection, EPOLLOUT | EPOLLET);
    amilinkSendMessage(connection, response);

    // Set connection back to EPOLLIN after sending
    globalServer->modifyEpollEvent(efd, connection, EPOLLIN | EPOLLET);
}

static void handleDataFetchRequest(int efd, int connection, uint64_t &idDevice, MqsCldMsg_t &request, std::shared_ptr<sql::Connection> conn)
{
    MqsCldMsg_t response = {.topic = MQS_FETCH_TEST_RESPONSE};
    BatchInfo batchInfobuffer = {std::vector<BatchCodeInfo>(), 0};

    int companyID = getCompanyIDFromDevice(idDevice, conn);

    // Use getStructBatchCodesByConditions to populate the response
    if (companyID == 0)
    {
        // No need to do anything as batchInfobuffer is already initialized to zeros
    }
    else
    {
        batchInfobuffer = getStructBatchCodesByCurrentDate(companyID, conn);
    }

    if (batchInfobuffer.totalCount == 0)
    {
        for (int i = 0; i < 5; i++)
        {
            strcpy(response.testFetchResp.batchStruct[i].batchNumber, "");
            response.testFetchResp.batchStruct[i].batch_uid = 0;
            response.testFetchResp.batchStruct[i].meat_type = 0;
        }
    }
    else
    {
        // Copy batchID directly without conversion
        for (int i = 0; i < batchInfobuffer.totalCount; i++)
        {
            strncpy(response.testFetchResp.batchStruct[i].batchNumber, batchInfobuffer.batchCodes[i].batchID, sizeof(response.testFetchResp.batchStruct[i].batchNumber));
            response.testFetchResp.batchStruct[i].meat_type = batchInfobuffer.batchCodes[i].meat_type;
            response.testFetchResp.batchStruct[i].batch_uid = batchInfobuffer.batchCodes[i].batch_uid;
        }
    }

    response.testFetchResp.count = batchInfobuffer.totalCount;

    // Set connection to EPOLLOUT before sending
    globalServer->modifyEpollEvent(efd, connection, EPOLLOUT | EPOLLET);
    amilinkSendMessage(connection, response);

    // Set connection back to EPOLLIN after sending
    globalServer->modifyEpollEvent(efd, connection, EPOLLIN | EPOLLET);
}

static void handleDeviceRegistrationRequest(int efd, int connection, uint64_t &idDevice, MqsCldMsg_t &request)
{
    idDevice = request.deviceRegisterReq.idDevice;
    MqsCldMsg_t response = {.topic = MQS_DEVICE_REGISTER_RESPONSE};

    // Set connection to EPOLLOUT before sending
    globalServer->modifyEpollEvent(efd, connection, EPOLLOUT | EPOLLET);
    amilinkSendMessage(connection, response);

    // Set connection back to EPOLLIN after sending
    globalServer->modifyEpollEvent(efd, connection, EPOLLIN | EPOLLET);
}

static void fileErrorResponse(int efd, int connection)
{
    if (sVerbose)
    {
        printf("Sending 'zero' file response\n");
    }
    MqsCldMsg_t response = {.topic = MQS_FILE_RESPONSE};
    response.filePullResp.length = 0;
    response.filePullResp.offset = 0;
    response.filePullResp.totalLength = 0;

    // Set connection to EPOLLOUT before sending
    globalServer->modifyEpollEvent(efd, connection, EPOLLOUT | EPOLLET);
    amilinkSendMessage(connection, response);

    // Set connection back to EPOLLIN after sending
    globalServer->modifyEpollEvent(efd, connection, EPOLLIN | EPOLLET);
}

static uint32_t getAlgoVersion(const std::string &prefix, const std::string &postfix)
{ // pointer passed in. this should be checked carefully.
    DIR *dir;
    struct dirent *ent;
    uint32_t versionNumber = 0;

    if (true)
    { //(sVerbose)
        std::cout << "Searching version with prefix '" << prefix << "' and postfix '" << postfix << "'" << std::endl;
    }

    if ((dir = opendir(FILE_LIBRARY)) != NULL)
    { // dogru
        while ((ent = readdir(dir)) != NULL)
        {                                      // dogru
            std::string filename(ent->d_name); // dogru
            if (hasPrefix(filename, prefix) && hasPostfix(filename, postfix))
            { // boolean true. dogru bu da kullanilyor.
                // Construct full file path
                std::string fullPath = std::string(FILE_LIBRARY) + filename;

                std::ifstream fileStream(fullPath);
                if (!fileStream.is_open())
                {
                    std::cerr << "Cannot open file " << fullPath << std::endl;
                    continue;
                }

                try
                {
                    nlohmann::json jsonFile;
                    fileStream >> jsonFile; // Read the JSON file
                    fileStream.close();

                    // Extract versionNumber from the algorithm section if it exists
                    if (jsonFile.contains("algorithm") && jsonFile["algorithm"].contains("versionNumber"))
                    {
                        versionNumber = jsonFile["algorithm"]["versionNumber"].get<uint32_t>();
                        std::cout << "Algo Version Number: " << versionNumber << std::endl;
                    }

                    if (sVerbose)
                    {
                        std::cout << "Match <" << versionNumber << "> in " << filename << std::endl;
                    }
                }
                catch (const std::exception &e)
                {
                    std::cerr << "Error processing file " << filename << ": " << e.what() << std::endl;
                }
            }
        }
        closedir(dir);
    }
    else
    {
        perror("Error with listing files");
    }

    return versionNumber;
}


static void handleVersionRequest(int efd, int connection, MqsCldMsg_t &request)
{
    MqsCldMsg_t response = {.topic = MQS_VERSION_RESPONSE};
    std::string prefix(request.versionReq.versionPrefix);
    std::string postfix(request.versionReq.versionPostfix);

    if (postfix == ".algo")
    {
        response.versionResp.version = getAlgoVersion(prefix, postfix);
        if (response.versionResp.version != request.versionReq.algoVersion)
        {
            std::cout << "incoming algo Version from the other side " << request.versionReq.algoVersion << std::endl;
            isAlgorithmNeeded = true;
        }
    }
    else
    {
        std::cout << "dfu versionCheck activated. " << std::endl;
        response.versionResp.version = getFileVersion(prefix, postfix);
    }
    std::cout << "sent Version " << response.versionResp.version << std::endl;

    // Set connection to EPOLLOUT before sending
    globalServer->modifyEpollEvent(efd, connection, EPOLLOUT | EPOLLET);
    amilinkSendMessage(connection, response);

    // Set connection back to EPOLLIN after sending
    globalServer->modifyEpollEvent(efd, connection, EPOLLIN | EPOLLET);
}

static void handleAlgoRequest(int efd, int connection, MqsCldMsg_t &request, uint64_t &idDevice, std::shared_ptr<sql::Connection> conn)
{
    int companyID = getCompanyIDFromDevice(idDevice, conn);
    std::vector<int> requestedIds = getUniqueMeatTypesForCompany(companyID, conn);
    std::cout << "Requested Meat Type IDs for Company ID " << companyID << ": ";
    for (const int id : requestedIds)
    {
        std::cout << id << " ";
    }
    std::cout << std::endl;

    std::string filename(request.filePullReq.filename);
    std::string path = std::string(FILE_LIBRARY) + filename;

    if (true) 
    {
        std::cout << "Request for file " << filename << " with company ID " << companyID << std::endl;
    }
    try
    {
        std::ifstream fileStream(path);
        if (!fileStream.is_open())
        {
            throw std::runtime_error("Cannot open file " + path);
        }

        nlohmann::json originalJson;
        fileStream >> originalJson;
        fileStream.close();

        nlohmann::json jsonToSend;

        if (isAlgorithmNeeded)
        {
            jsonToSend["algorithm"] = originalJson["algorithm"];
            isAlgorithmNeeded = false;
        }

        nlohmann::json filteredMeatTypes;
        uint16_t count = 0;
        for (const auto &meatType : originalJson["meat_types"])
        {
            if (std::find(requestedIds.begin(), requestedIds.end(), meatType["id"].get<int>()) != requestedIds.end())
            {
                nlohmann::json newMeatType = {
                    {"id", meatType["id"]},
                    {"cubic", meatType["cubic"]},
                    {"quadratic", meatType["quadratic"]},
                    {"linear", meatType["linear"]},
                    {"adjConstant", meatType["adjConstant"]},
                    {"max_days", meatType["max_days"]}};
                filteredMeatTypes.push_back(newMeatType);
                count++;
                if (count >= 5)
                {
                    break;
                }
            }
        }

        jsonToSend["meat_types"] = filteredMeatTypes;
        std::string updatedJson = jsonToSend.dump();
        size_t remain = updatedJson.size();
        size_t offset = 0;
        MqsCldMsg_t response = {};
        response.topic = MQS_FILE_RESPONSE;

        while (remain > 0)
        {
            size_t chunkSize = std::min(remain, static_cast<size_t>(MQS_CLD_DATA_PAYLOAD_MAX));
            std::memcpy(response.filePullResp.data, updatedJson.data() + offset, chunkSize);

            if (sVerbose)
            {
                printf("File push remain=%zu, offset=%zu, readSize=%zu\n", remain, offset, chunkSize);
            }

            response.filePullResp.length = chunkSize;
            response.filePullResp.offset = offset;
            response.filePullResp.totalLength = updatedJson.size();

            // Set connection to EPOLLOUT before sending
            globalServer->modifyEpollEvent(efd, connection, EPOLLOUT | EPOLLET);
            int32_t dataSent = amilinkSendMessage(connection, response);

            if (dataSent <= 0)
            {
                fprintf(stderr, "Error sending file %s\n", filename.c_str());
                fileErrorResponse(efd, connection);
                return;
            }

            remain -= chunkSize;
            offset += chunkSize;

            // Set connection back to EPOLLIN after sending each chunk
            globalServer->modifyEpollEvent(efd, connection, EPOLLIN | EPOLLET);
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error processing file request: " << e.what() << std::endl;
        fileErrorResponse(efd, connection); // Error handling
    }
}

static void handleFirmwareRequest(int efd, int connection, MqsCldMsg_t &request)
{
    std::string filename(request.filePullReq.filename);
    std::string path = std::string(FILE_LIBRARY) + filename;
    if (sVerbose)
    {
        std::cout << "Request for file " << filename << std::endl;
    }
    bool ok = true;
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
    {
        fprintf(stderr, "Error opening file %s, %d %s\n", filename.c_str(), errno, strerror(errno));
        ok = false;
    }
    else
    {
        int32_t totalLength = lseek(fd, 0, SEEK_END);
        if (totalLength < 0)
        {
            fprintf(stderr, "Error seeking in file %s, %d %s\n", filename.c_str(), errno, strerror(errno));
            ok = false;
        }
        else
        {
            int32_t status = lseek(fd, 0, SEEK_SET);
            if (status < 0)
            {
                fprintf(stderr, "Error seeking in file %s, %d %s\n", filename.c_str(), errno, strerror(errno));
                ok = false;
            }
            else
            {
                uint32_t remain = totalLength;
                uint32_t offset = 0;
                MqsCldMsg_t response = {.topic = MQS_FILE_RESPONSE};

                while (remain > 0 && ok)
                {
                    uint32_t readSize = read(fd, response.filePullResp.data, MQS_CLD_DATA_PAYLOAD_MAX);
                    if (sVerbose)
                    {
                        printf("file push remain=%d, offset=%d, readsize=%d\n", remain, offset, readSize);
                    }
                    if (readSize > 0)
                    {
                        response.filePullResp.length = readSize;
                        response.filePullResp.offset = offset;
                        response.filePullResp.totalLength = totalLength;
                        remain -= readSize;
                        offset += readSize;

                        // Set connection to EPOLLOUT before sending
                        globalServer->modifyEpollEvent(efd, connection, EPOLLOUT | EPOLLET);
                        int32_t dataSent = amilinkSendMessage(connection, response);
                        ok = dataSent > 0;

                        // Set connection back to EPOLLIN after sending each chunk
                        globalServer->modifyEpollEvent(efd, connection, EPOLLIN | EPOLLET);
                    }
                    else
                    {
                        ok = false;
                        fprintf(stderr, "Error reading file %s, %d %s\n", filename.c_str(), errno, strerror(errno));
                    }
                }
            }
        }
        close(fd);
    }
    if (!ok)
    {
        fileErrorResponse(efd, connection);
    }
}

static void handleFileRequest(int efd, int connection, MqsCldMsg_t &request, uint64_t &idDevice, std::shared_ptr<sql::Connection> conn)
{
    std::string filename(request.filePullReq.filename);

    if (filename.length() > 5)
    {
        if (filename.substr(filename.length() - 5) == ".algo")
        {
            std::cout << "handle Algo Request. " << std::endl;
            handleAlgoRequest(efd, connection, request, idDevice, conn);
        }
        else if (filename.substr(filename.length() - 4) == ".dfu")
        {
            std::cout << "handle File Request. " << std::endl;
            handleFirmwareRequest(efd, connection, request);
        }
        else
        {
            std::cout << "Unsupported file type: " << filename << std::endl;
        }
    }
    else
    {
        std::cout << "Filename too short to determine request type: " << filename << std::endl;
    }
}

static bool handleRequest(int efd, int connection, uint64_t &idDevice, MqsCldMsg_t &request, void *&rawDataSetAssemblyBuffer, uint32_t &maxAssemblyLength, std::shared_ptr<sql::Connection> conn)
{
    bool more = true;
    MqsCldTopic_t topic = static_cast<MqsCldTopic_t>(request.topic);
    if (sVerbose)
    {
        printf("Got request %d\n", topic);
    }
    switch (topic)
    {
    case MQS_DATA_LOG_PUSH_REQUEST:
        handleDataLogPushRequest(efd, connection, idDevice, request, conn);
        break;
    case MQS_DEVICE_REGISTER_REQUEST:
        handleDeviceRegistrationRequest(efd, connection, idDevice, request);
        break;
    case MQS_FILE_REQUEST:
        handleFileRequest(efd, connection, request, idDevice, conn);
        break;
    case MQS_GOODBYE_REQUEST:
        more = false;
        break;
    case MQS_RAW_DATA_PUSH_REQUEST:
        handleRawDataPushRequest(efd, connection, idDevice, request, rawDataSetAssemblyBuffer, maxAssemblyLength, conn);
        break;
    case MQS_RAW_DATA_REQUIRED_REQUEST:
        handleRawDataRequiredRequest(efd, connection, idDevice, request, conn);
        break;
    case MQS_VERSION_REQUEST:
        handleVersionRequest(efd, connection, request);
        break;
    case MQS_FETCH_TEST_REQUEST:
        handleDataFetchRequest(efd, connection, idDevice, request, conn);
        break;
    default:
        fprintf(stderr, "Error: No handling of topic %d\n", topic);
        break;
    }
    return more;
}

struct ThreadData {
    int connection;                                       // Connection identifier or file descriptor
    std::shared_ptr<MySqlConnectionManager> mysqlManager; // Shared pointer for thread safety and RAII
    EpollContext *epollContext;                           // Pointer to the epoll context
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

static void *connectionHandler(void *pData) {
    auto data = static_cast<ThreadData *>(pData); // Direct use if passing raw but better to pass smart pointers directly
    int connection = data->connection;
    EpollContext &epollContext = *data->epollContext;

    uint64_t idDevice = 0;
    int32_t localConnectionId = connectionId.fetch_add(1, std::memory_order_relaxed);

    bool more = true;
    if (sVerbose) {
        std::cout << "New connection " << localConnectionId << std::endl;
    }
    int32_t msgCount = 0;
    void *rawDataSetAssemblyBuffer = nullptr;
    uint32_t maxAssemblyLength = 0;
    time_t lastMessage = time(nullptr);
    auto conn = data->mysqlManager->getConnection();

    while (more) {
        msgCount++;
        MqsCldMsg_t request;
        if (sVerbose) {
            std::cout << "Waiting for message " << msgCount << " on connection " << localConnectionId << std::endl;
        }

        // Wait for events on this connection
        struct epoll_event event;
        int n = epoll_wait(epollContext.efd, &event, 1, -1);

        if (n > 0 && event.data.fd == connection) {
            int32_t messageLength = amilinkReadMessage(connection, request);
            if (messageLength > 0) {
                more = handleRequest(epollContext.efd, connection, idDevice, request, rawDataSetAssemblyBuffer, maxAssemblyLength, conn);
                time(&lastMessage);
            } else {
                time_t now = time(nullptr);
                bool activity = (lastMessage + MAX_INACTIVITY) > now;
                more = activity && amilinkHeartbeat(connection);
            }
        }
    }
    // Cleanup epoll instance for this connection
    epoll_ctl(epollContext.efd, EPOLL_CTL_DEL, connection, nullptr);

    data->mysqlManager->returnConnection(conn);
    amilinkClose(connection);
    if (sVerbose) {
        std::cout << "Terminating connection " << localConnectionId << std::endl;
    }
    return NULL;
}

Server::Server(uint16_t port, bool verbose, std::shared_ptr<MySqlConnectionManager> mysqlManager)
    : sPort(port), sVerbose(verbose), running(false), mysqlManager(mysqlManager), sLink(-1), threadPool(10), activeConnections(0)
{
    // Initialize epoll context
    epollContext.efd = -1; // Initial invalid value
}

Server::~Server() {
    stop();
    if (epollContext.efd >= 0)
        close(epollContext.efd);
    if (sLink >= 0)
        close(sLink);
}

bool Server::initialize() {
    sLink = amilinkListen(sPort);
    if (sLink < 0) {
        perror("amilinkListen");
        return false;
    }

    int flags = fcntl(sLink, F_GETFL, 0);
    if (flags == -1)
        return false;
    if (fcntl(sLink, F_SETFL, flags | O_NONBLOCK) == -1)
        return false;

    epollContext.efd = epoll_create1(0);
    if (epollContext.efd == -1)
        return false;

    struct epoll_event event;
    event.data.fd = sLink;
    event.events = EPOLLIN; // Interested in read events
    if (epoll_ctl(epollContext.efd, EPOLL_CTL_ADD, sLink, &event) == -1)
        return false;

    return true;
}

void Server::start() {
    if (!initialize()) {
        std::cerr << "Server initialization failed\n";
        return;
    }
    running = true;
    mainThread = std::thread(&Server::run, this);
}

void Server::stop() {
    running = false;
    if (mainThread.joinable()) {
        mainThread.join();
    }
    threadPool.shutdown(); // Shutdown the thread pool
}

void Server::run() {
    const int INITIAL_EVENTS = 10;
    epollContext.events.resize(INITIAL_EVENTS); // Initial buffer size

    while (running) {
        int n = epoll_wait(epollContext.efd, epollContext.events.data(), epollContext.events.size(), -1); // Wait indefinitely for events

        // Resize the events buffer based on the number of active connections before handling new connections
        epollContext.events.resize(INITIAL_EVENTS + activeConnections.load());

        for (int i = 0; i < n; ++i) {
            if (epollContext.events[i].data.fd == sLink) {
                // New connection on the listening socket
                handleNewConnection();
            } else {
                // Handle other events or errors
            }
        }
    }
}

void Server::handleNewConnection() {
    while (true) {
        int connection = amilinkAccept(sLink); // Use your existing accept function
        if (connection < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("accept");
            }
            break; // No more incoming connections
        }

        setNonBlocking(connection); // Set new connection to non-blocking mode

        struct epoll_event client_event;
        client_event.data.fd = connection;
        client_event.events = EPOLLIN | EPOLLET; // Read events, Edge Triggered mode
        if (epoll_ctl(epollContext.efd, EPOLL_CTL_ADD, connection, &client_event) == -1) {
            perror("epoll_ctl: add");
            amilinkClose(connection); // Close the socket on error
        } else {
            activeConnections++;
            adjustThreadPoolSize(); // Adjust thread pool size based on active connections

            auto threadData = std::make_shared<ThreadData>();
            threadData->connection = connection;
            threadData->mysqlManager = mysqlManager; // Assuming mysqlManager is a shared_ptr
            threadData->epollContext = &epollContext; // Assign epollContext

            threadPool.enqueue([threadData, this] {
                connectionHandler(threadData.get());
                activeConnections--;
                adjustThreadPoolSize(); // Adjust thread pool size after handling the connection
            });
        }
    }
}

bool Server::modifyEpollEvent(int efd, int fd, int events) {
    struct epoll_event event;
    event.data.fd = fd;
    event.events = events;
    if (epoll_ctl(efd, EPOLL_CTL_MOD, fd, &event) == -1) {
        perror("epoll_ctl: mod");
        return false;
    }
    return true;
}

void Server::setNonBlocking(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags != -1) {
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }
}

void Server::adjustThreadPoolSize() {
    size_t queueSize = threadPool.getQueueSize();
    size_t currentSize = threadPool.getSize(); // Replace getActiveThreads() with getSize()
    size_t newSize = currentSize;

    if (queueSize > currentSize) {
        newSize = std::min(currentSize * 2, queueSize + currentSize);
    } else if (queueSize < currentSize / 2) {
        newSize = std::max(currentSize / 2, size_t(10));
    }

    threadPool.resize(newSize);
}

void serverInit(uint16_t port, bool verbose)
{
    try
    {
        // Initialize MySqlConnectionManager
        sVerbose = verbose;

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
