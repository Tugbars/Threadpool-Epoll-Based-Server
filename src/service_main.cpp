/*******************************************************************************
 * TekPartner A/S
 * $Header: https://svn.tekpartner.dk/svn/ANI_P01/sw/tags/mqs_v2_r561/tools/aminic/aminic.cpp 392 2020-11-20 10:46:31Z kgo $
 *******************************************************************************/

#include <iostream>
#include <string>
#include <string.h>
#include <unistd.h>

#include "plf/plf.h"
#include "plf/crc/crc16.h"
#include "amilink/amilink.h"
#include "aminicserver.h"
#include "mysql.h"

 //--------------------Local defines  -------------------------------------------------------

 //--------------------Local data -------------------------------------------------------

static std::string sAppName;
static bool sVerbose = true;
static uint16_t sPort = DEFAULT_PORT; //in amilink.h

//--------------------Forward declarations of functions -------------------------------------

static void usage();

//--------------------Local functions -------------------------------------------------------

static void cleanExit(int exitCode)
{
    exit(exitCode);
}

static void parseParameters(int argc, char* argv[])
{
    int opt = -1;
    while ((opt = getopt(argc, argv, "p:v")) != -1)
    {
        switch (opt)
        {
        case 'p': // communication baud Rate
            sPort = (atoi(optarg));
            if (sPort < 0)
            {
                usage();
                cleanExit(EXIT_FAILURE);
            }
            break;
        case 'v': // verbose
            sVerbose = true;
            break;
        default:
            usage();
            break;
        }
    }
    if (optind < argc)
    {
        fprintf(stderr, "Unexpected parameter after options\n");
        usage();
    }
}

static void usage()
{
    std::cout << std::endl;
    std::cout << "Usage: " << sAppName << " [-p port] [-v]" << std::endl;
    std::cout << "Required parameters" << std::endl;
    std::cout << "-p port, default " << DEFAULT_PORT << std::endl;
    std::cout << "-v, verbose output, default no" << std::endl;
    std::cout << "Example: " << sAppName << " -v -p 3002" << std::endl;
}

//--------------------Global functions -------------------------------------------------------
//should change to mysqlInit.
int main(int argc, char* argv[])
{
    std::cout << (PLF_VENDOR_NAME " " PLF_PRODUCT_NAME " | Cloud Service (" __DATE__ ", " __TIME__ ")") << std::endl;
    sAppName = argv[0];
    parseParameters(argc, argv);
    amilinkInit(sVerbose);

    serverInit(sPort, sVerbose);
    
    std::cout << "All initialized" << std::endl;
    while (1)
    {
        sleep(1);
    }
    return 0;
}

//parseParameter nerede callaniyor. 
