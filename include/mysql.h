#ifndef MYSQL_H
#define MYSQL_H

#include <vector>
#include <string>

struct BatchCodeInfo {
    char batchID[8];
    uint32_t meat_type;
};

// Modify BatchInfo to use a vector of BatchCodeInfo structs
struct BatchInfo {
    std::vector<BatchCodeInfo> batchCodes;
    int totalCount;
};

BatchInfo getStructBatchCodesByCurrentDate(int companyId);
extern int getCompanyForIdDevice(int idDevice);

#endif /* MYSQL_H */
