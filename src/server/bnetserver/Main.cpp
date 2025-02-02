/*
 * Copyright (C) 2008-2016 TrinityCore <http://www.trinitycore.org/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/**
* @file main.cpp
* @brief Authentication Server main program
*
* This file contains the main program for the
* authentication server
*/

#include <boost/asio/signal_set.hpp>
#include <boost/program_options.hpp>
#include <cds/gc/hp.h>
#include <cds/init.h>
#include <google/protobuf/stubs/common.h>
#include <iostream>
#include <csignal>

#include "AppenderDB.h"
#include "Common.h"
#include "DatabaseEnv.h"
#include "DeadlineTimer.h"
#include "GitRevision.h"
#include "IoContext.h"
#include "LoginRESTService.h"
#include "ProcessPriority.h"
#include "RealmList.h"
#include "SessionManager.h"
#include "SslContext.h"
#include "Util.h"
#include "Banner.h"

using boost::asio::ip::tcp;
using namespace boost::program_options;

#ifndef _TRINITY_BNET_CONFIG
# define _TRINITY_BNET_CONFIG  "bnetserver.conf"
#endif

#if PLATFORM == PLATFORM_WINDOWS
#include "ServiceWin32.h"
char serviceName[] = "bnetserver";
char serviceLongName[] = "SPP Legion V2 bnet service";
char serviceDescription[] = "SPP Legion V2 Battle.net emulator authentication service";
/*
* -1 - not in service mode
*  0 - stopped
*  1 - running
*  2 - paused
*/
int m_ServiceStatus = -1;

void ServiceStatusWatcher(std::weak_ptr<Trinity::Asio::DeadlineTimer> serviceStatusWatchTimerRef, std::weak_ptr<Trinity::Asio::IoContext> ioContextRef, boost::system::error_code const& error);
#endif

bool StartDB();
void StopDB();
void SignalHandler(std::weak_ptr<Trinity::Asio::IoContext> ioContextRef, boost::system::error_code const& error, int signalNumber);
void KeepDatabaseAliveHandler(std::weak_ptr<Trinity::Asio::DeadlineTimer> dbPingTimerRef, int32 dbPingInterval, boost::system::error_code const& error);
void BanExpiryHandler(std::weak_ptr<Trinity::Asio::DeadlineTimer> banExpiryCheckTimerRef, int32 banExpiryCheckInterval, boost::system::error_code const& error);
variables_map GetConsoleArguments(int argc, char** argv, std::string& configFile, std::string& configService);

int main(int argc, char** argv)
{
    signal(SIGABRT, &Trinity::AbortHandler);

    std::string configFile = _TRINITY_BNET_CONFIG;
    std::string configService;
    auto vm = GetConsoleArguments(argc, argv, configFile, configService);
    // exit if help or version is enabled
    if (vm.count("help") || vm.count("version"))
        return 0;

    GOOGLE_PROTOBUF_VERIFY_VERSION;

    std::shared_ptr<void> protobufHandle(nullptr, [](void*) { google::protobuf::ShutdownProtobufLibrary(); });

#if PLATFORM == TC_PLATFORM_WINDOWS
    if (configService.compare("install") == 0)
        return WinServiceInstall() ? 0 : 1;
    if (configService.compare("uninstall") == 0)
        return WinServiceUninstall() ? 0 : 1;
    if (configService.compare("run") == 0)
        return WinServiceRun() ? 0 : 1;
#endif

    std::string configError;
    if (!sConfigMgr->LoadInitial(configFile, configError))
    {
        printf("Error in config file: %s\n", configError.c_str());
        return 1;
    }

    sLog->Initialize(nullptr);

    Trinity::Banner::Show("bnetserver", [](char const* text)
    {
        TC_LOG_INFO(LOG_FILTER_BATTLENET, "%s", text);
    }, []()
    {
        TC_LOG_INFO(LOG_FILTER_BATTLENET, "Using configuration file %s.", sConfigMgr->GetFilename().c_str());
        TC_LOG_INFO(LOG_FILTER_BATTLENET, "Using SSL version: %s (library: %s)", OPENSSL_VERSION_TEXT, SSLeay_version(SSLEAY_VERSION));
        TC_LOG_INFO(LOG_FILTER_BATTLENET, "Using Boost version: %i.%i.%i", BOOST_VERSION / 100000, BOOST_VERSION / 100 % 1000, BOOST_VERSION % 100);
    }
    );

    cds::Initialize();
    cds::gc::HP hpGC;
    cds::threading::Manager::attachThread();

    std::shared_ptr<void> cdsHandle(nullptr, [](void*) { cds::threading::Manager::detachThread(); cds::Terminate(); });

    // Seed the OpenSSL's PRNG here.
    // That way it won't auto-seed when calling BigNumber::SetRand and slow down the first world login
    BigNumber seed;
    seed.SetRand(16 * 8);

    // bnetserver PID file creation
    std::string pidFile = sConfigMgr->GetStringDefault("PidFile", "");
    if (!pidFile.empty())
    {
        if (uint32 pid = CreatePIDFile(pidFile))
            TC_LOG_INFO(LOG_FILTER_BATTLENET, "Daemon PID: %u\n", pid);
        else
        {
            TC_LOG_ERROR(LOG_FILTER_BATTLENET, "Cannot create PID file %s.\n", pidFile.c_str());
            return 1;
        }
    }

    if (!Battlenet::SslContext::Initialize())
    {
        TC_LOG_ERROR(LOG_FILTER_BATTLENET, "Failed to initialize SSL context");
        return 1;
    }

    // Initialize the database connection
    if (!StartDB())
        return 1;

    std::shared_ptr<void> dbHandle(nullptr, [](void*) { StopDB(); });

    std::shared_ptr<Trinity::Asio::IoContext> ioContext = std::make_shared<Trinity::Asio::IoContext>();

    // Start the listening port (acceptor) for auth connections
    int32 bnport = sConfigMgr->GetIntDefault("BattlenetPort", 1119);
    if (bnport < 0 || bnport > 0xFFFF)
    {
        TC_LOG_ERROR(LOG_FILTER_BATTLENET, "Specified battle.net port (%d) out of allowed range (1-65535)", bnport);
        return 1;
    }

    if (!sLoginService.Start(ioContext.get()))
    {
        TC_LOG_ERROR(LOG_FILTER_BATTLENET, "Failed to initialize login service");
        return 1;
    }

    std::shared_ptr<void> sLoginServiceHandle(nullptr, [](void*) { sLoginService.Stop(); });

    // Get the list of realms for the server
    sRealmList->Initialize(*ioContext, sConfigMgr->GetIntDefault("RealmsStateUpdateDelay", 10));

    std::shared_ptr<void> sRealmListHandle(nullptr, [](void*) { sRealmList->Close(); });

    std::string bindIp = sConfigMgr->GetStringDefault("BindIP", "0.0.0.0");

    int networkThreads = sConfigMgr->GetIntDefault("Network.Threads", 1);
    if (networkThreads <= 0)
    {
        TC_LOG_ERROR(LOG_FILTER_BATTLENET, "Network.Threads must be greater than 0");
        return 0;
    }

    if (!sSessionMgr.StartNetwork(*ioContext, bindIp, bnport, networkThreads))
    {
        TC_LOG_ERROR(LOG_FILTER_BATTLENET, "Failed to initialize network");
        return 1;
    }

    std::shared_ptr<void> sSessionMgrHandle(nullptr, [](void*) { sSessionMgr.StopNetwork(); });

    boost::asio::signal_set signals(*ioContext, SIGINT, SIGTERM);
#if PLATFORM == TC_PLATFORM_WINDOWS
    signals.add(SIGBREAK);
#endif
    signals.async_wait(std::bind(&SignalHandler, std::weak_ptr<Trinity::Asio::IoContext>(ioContext), std::placeholders::_1, std::placeholders::_2));

    // Set process priority according to configuration settings
    SetProcessPriority("server.bnetserver");

    // Enabled a timed callback for handling the database keep alive ping
    int32 dbPingInterval = sConfigMgr->GetIntDefault("MaxPingTime", 30);
    std::shared_ptr<Trinity::Asio::DeadlineTimer> dbPingTimer = std::make_shared<Trinity::Asio::DeadlineTimer>(*ioContext);
    dbPingTimer->expires_from_now(boost::posix_time::minutes(dbPingInterval));
    dbPingTimer->async_wait(std::bind(&KeepDatabaseAliveHandler, std::weak_ptr<Trinity::Asio::DeadlineTimer>(dbPingTimer), dbPingInterval, std::placeholders::_1));

    int32 banExpiryCheckInterval = sConfigMgr->GetIntDefault("BanExpiryCheckInterval", 60);
    std::shared_ptr<Trinity::Asio::DeadlineTimer> banExpiryCheckTimer = std::make_shared<Trinity::Asio::DeadlineTimer>(*ioContext);
    banExpiryCheckTimer->expires_from_now(boost::posix_time::seconds(banExpiryCheckInterval));
    banExpiryCheckTimer->async_wait(std::bind(&BanExpiryHandler, std::weak_ptr<Trinity::Asio::DeadlineTimer>(banExpiryCheckTimer), banExpiryCheckInterval, std::placeholders::_1));

    TC_LOG_INFO(LOG_FILTER_BATTLENET, "Battlenet::Initialized");

#if PLATFORM == TC_PLATFORM_WINDOWS
    std::shared_ptr<Trinity::Asio::DeadlineTimer> serviceStatusWatchTimer;
    if (m_ServiceStatus != -1)
    {
        serviceStatusWatchTimer = std::make_shared<Trinity::Asio::DeadlineTimer>(*ioContext);
        serviceStatusWatchTimer->expires_from_now(boost::posix_time::seconds(1));
        serviceStatusWatchTimer->async_wait(std::bind(&ServiceStatusWatcher,
            std::weak_ptr<Trinity::Asio::DeadlineTimer>(serviceStatusWatchTimer),
            std::weak_ptr<Trinity::Asio::IoContext>(ioContext),
            std::placeholders::_1));
    }
#endif

    // Start the io service worker loop
    ioContext->run();

    banExpiryCheckTimer->cancel();
    dbPingTimer->cancel();

    TC_LOG_INFO(LOG_FILTER_BATTLENET, "Halting process...");

    signals.cancel();

    return 0;
}

/// Initialize connection to the database
bool StartDB()
{
    MySQL::Library_Init();

    std::string dbstring = sConfigMgr->GetStringDefault("LoginDatabaseInfo", "");
    if (dbstring.empty())
    {
        TC_LOG_ERROR(LOG_FILTER_BATTLENET, "Database not specified");
        return false;
    }

    int32 worker_threads = sConfigMgr->GetIntDefault("LoginDatabase.WorkerThreads", 1);
    if (worker_threads < 1 || worker_threads > 32)
    {
        TC_LOG_ERROR(LOG_FILTER_BATTLENET, "Improper value specified for LoginDatabase.WorkerThreads, defaulting to 1.");
        worker_threads = 1;
    }

    int32 synch_threads = sConfigMgr->GetIntDefault("LoginDatabase.SynchThreads", 1);
    if (synch_threads < 1 || synch_threads > 32)
    {
        TC_LOG_ERROR(LOG_FILTER_BATTLENET, "Improper value specified for LoginDatabase.SynchThreads, defaulting to 1.");
        synch_threads = 1;
    }

    if (!LoginDatabase.Open(dbstring, uint8(worker_threads), uint8(synch_threads)))
        return false;

    TC_LOG_INFO(LOG_FILTER_BATTLENET, "Started auth database connection pool.");
    sLog->SetRealmID(0); // Enables DB appenders when realm is set.
    return true;
}

/// Close the connection to the database
void StopDB()
{
    LoginDatabase.Close();
    MySQL::Library_End();
}

void SignalHandler(std::weak_ptr<Trinity::Asio::IoContext> ioContextRef, boost::system::error_code const& error, int /*signalNumber*/)
{
    if (!error)
        if (std::shared_ptr<Trinity::Asio::IoContext> ioContext = ioContextRef.lock())
            ioContext->stop();
}

void KeepDatabaseAliveHandler(std::weak_ptr<Trinity::Asio::DeadlineTimer> dbPingTimerRef, int32 dbPingInterval, boost::system::error_code const& error)
{
    if (!error)
    {
        if (std::shared_ptr<Trinity::Asio::DeadlineTimer> dbPingTimer = dbPingTimerRef.lock())
        {
            TC_LOG_INFO(LOG_FILTER_BATTLENET, "Ping MySQL to keep connection alive");
            LoginDatabase.KeepAlive();

            dbPingTimer->expires_from_now(boost::posix_time::minutes(dbPingInterval));
            dbPingTimer->async_wait(std::bind(&KeepDatabaseAliveHandler, dbPingTimerRef, dbPingInterval, std::placeholders::_1));
        }
    }
}

void BanExpiryHandler(std::weak_ptr<Trinity::Asio::DeadlineTimer> banExpiryCheckTimerRef, int32 banExpiryCheckInterval, boost::system::error_code const& error)
{
    if (!error)
    {
        if (std::shared_ptr<Trinity::Asio::DeadlineTimer> banExpiryCheckTimer = banExpiryCheckTimerRef.lock())
        {
            LoginDatabase.Execute(LoginDatabase.GetPreparedStatement(LOGIN_DEL_EXPIRED_IP_BANS));
            LoginDatabase.Execute(LoginDatabase.GetPreparedStatement(LOGIN_UPD_EXPIRED_ACCOUNT_BANS));

            banExpiryCheckTimer->expires_from_now(boost::posix_time::seconds(banExpiryCheckInterval));
            banExpiryCheckTimer->async_wait(std::bind(&BanExpiryHandler, banExpiryCheckTimerRef, banExpiryCheckInterval, std::placeholders::_1));
        }
    }
}

#if PLATFORM == TC_PLATFORM_WINDOWS
void ServiceStatusWatcher(std::weak_ptr<Trinity::Asio::DeadlineTimer> serviceStatusWatchTimerRef, std::weak_ptr<Trinity::Asio::IoContext> ioContextRef, boost::system::error_code const& error)
{
    if (!error)
    {
        if (std::shared_ptr<Trinity::Asio::IoContext> ioContext = ioContextRef.lock())
        {
            if (m_ServiceStatus == 0)
            {
                ioContext->stop();
            }
            else if (std::shared_ptr<Trinity::Asio::DeadlineTimer> serviceStatusWatchTimer = serviceStatusWatchTimerRef.lock())
            {
                serviceStatusWatchTimer->expires_from_now(boost::posix_time::seconds(1));
                serviceStatusWatchTimer->async_wait(std::bind(&ServiceStatusWatcher, serviceStatusWatchTimerRef, ioContext, std::placeholders::_1));
            }
        }
    }
}
#endif

variables_map GetConsoleArguments(int argc, char** argv, std::string& configFile, std::string& configService)
{
    (void)configService;

    options_description all("Allowed options");
    all.add_options()
        ("help,h", "print usage message")
        ("version,v", "print version build info")
        ("config,c", value<std::string>(&configFile)->default_value(_TRINITY_BNET_CONFIG), "use <arg> as configuration file")
        ;
#if PLATFORM == PLATFORM_WINDOWS
    options_description win("Windows platform specific options");
    win.add_options()
        ("service,s", value<std::string>(&configService)->default_value(""), "Windows service options: [install | uninstall]")
        ;

    all.add(win);
#endif
    variables_map variablesMap;
    try
    {
        store(command_line_parser(argc, argv).options(all).allow_unregistered().run(), variablesMap);
        notify(variablesMap);
    }
    catch (std::exception& e)
    {
        std::cerr << e.what() << "\n";
    }

    if (variablesMap.count("help"))
    {
        std::cout << all << "\n";
    }
    else if (variablesMap.count("version"))
    {
        std::cout << GitRevision::GetFullVersion() << "\n";
    }

    return variablesMap;
}
