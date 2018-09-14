/** \file
    \brief WLAN Optimizer
    \copyright Copyright (c) 2018 Christopher A. Taylor.  All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
    * Neither the name of WLAN Optimizer nor the names of its contributors may be
      used to endorse or promote products derived from this software without
      specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
    ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.
*/

#include "WLANOptimizer.h"

#include <atomic>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include <thread>

static std::mutex APILock;

#ifndef WLANOPT_DISABLE_LOGGING
#include "Logger.h"
static logger::Channel Logger("WLANOptimizer", logger::Level::Debug);
#endif // WLANOPT_DISABLE_LOGGING


//------------------------------------------------------------------------------
// Windows Version

#ifdef _WIN32

#undef _WIN32_WINNT
#define _WIN32_WINNT _WIN32_WINNT_WIN7 /* must be defined for wlanapi.h */
#include <windows.h>
#include <wlanapi.h>
#pragma comment(lib, "wlanapi")
#pragma comment(lib, "ole32")

// Client handle shared between all calls.
// This is done because settings only persist until handle or app is closed.
static HANDLE m_clientHandle = nullptr;

// Set a Wlan setting
static OptimizeWLAN_Result SetWlanSetting(
    const GUID* guidPtr,
    WLAN_INTF_OPCODE opcode,
    bool enable)
{
    DWORD dataSize = 0;
    void* dataPtr = nullptr;
    WLAN_OPCODE_VALUE_TYPE opcodeType = wlan_opcode_value_type_invalid;
    DWORD queryResult = ::WlanQueryInterface(
        m_clientHandle,
        guidPtr,
        opcode,
        nullptr,
        &dataSize,
        &dataPtr,
        &opcodeType);

    // If the query call failed:
    if (queryResult != ERROR_SUCCESS ||
        dataSize < 1 ||
        !dataPtr ||
        opcodeType == wlan_opcode_value_type_invalid)
    {
        return OptimizeWLAN_ReadFailure;
    }

    // If the current value of the setting is already the one we wanted:
    const bool currentValue = *(BOOL*)dataPtr != 0;
    if (currentValue == enable) {
        // Avoid extra work (common case in steady state)
        return OptimizeWLAN_Preconfigured;
    }

    BOOL targetValue = enable ? TRUE : FALSE;

    // Note this function takes about 1 second to complete.
    const DWORD setResult = ::WlanSetInterface(
        m_clientHandle,
        guidPtr,
        opcode,
        (DWORD)sizeof(targetValue),
        &targetValue,
        nullptr);

    // If set call failed:
    if (setResult != ERROR_SUCCESS)
    {
        // If the error was related to permissions, bubble that up:
        if (setResult == ERROR_ACCESS_DENIED) {
            return OptimizeWLAN_AccessDenied;
        }

        return OptimizeWLAN_SetFailure;
    }

    dataSize = 0;
    dataPtr = nullptr;
    opcodeType = wlan_opcode_value_type_invalid;
    queryResult = ::WlanQueryInterface(
        m_clientHandle,
        guidPtr,
        opcode,
        nullptr,
        &dataSize,
        &dataPtr,
        &opcodeType);

    // If the readback call failed:
    if (queryResult != ERROR_SUCCESS ||
        dataSize < 1 ||
        !dataPtr ||
        *(BOOL*)dataPtr != targetValue ||
        opcodeType == wlan_opcode_value_type_invalid)
    {
        return OptimizeWLAN_ReadFailure;
    }

    return OptimizeWLAN_Applied;
}

#endif // _WIN32


//------------------------------------------------------------------------------
// OptimizeWLAN()

int OptimizeWLAN(int enable)
{
    std::lock_guard<std::mutex> locker(APILock);

#ifdef _WIN32

    // If the handle has not been opened yet:
    if (!m_clientHandle)
    {
        const DWORD clientVersion = 2;
        DWORD negotiatedVersion = 0;
        const DWORD openResult = ::WlanOpenHandle(
            clientVersion,
            nullptr,
            &negotiatedVersion,
            &m_clientHandle);

        // If the handle could not be opened:
        if (openResult != ERROR_SUCCESS ||
            !m_clientHandle)
        {
            return OptimizeWLAN_Unavailable;
        }
    }

    OptimizeWLAN_Result result = OptimizeWLAN_NoConnections;

    WLAN_INTERFACE_INFO_LIST* infoListPtr = nullptr;
    const DWORD enumResult = ::WlanEnumInterfaces(
        m_clientHandle,
        nullptr,
        &infoListPtr);

    // If the enumeration call succeeded:
    if (enumResult == ERROR_SUCCESS && infoListPtr)
    {
        const int count = (int)infoListPtr->dwNumberOfItems;

        // For each item:
        for (int i = 0; i < count; ++i)
        {
            const WLAN_INTERFACE_INFO* info = &infoListPtr->InterfaceInfo[i];

            // In my testing I found that only connected WiFi adapters are able to change this setting,
            // as otherwise WlanSetInterface() returns ERROR_INVALID_STATE.
            if (info->isState != wlan_interface_state_connected) {
                continue;
            }

            OptimizeWLAN_Result setResult;

            setResult = SetWlanSetting(
                &info->InterfaceGuid,
                wlan_intf_opcode_media_streaming_mode,
                enable != 0);

            // For multiple WiFi adapters, we should overwrite any status
            // result that is less important.  For example, OptimizeWLAN_Preconfigured
            // should be replaced by OptimizeWLAN_Applied or an error code.
            if (result < setResult) {
                result = setResult;
            }

            setResult = SetWlanSetting(
                &info->InterfaceGuid,
                wlan_intf_opcode_background_scan_enabled,
                enable == 0);

            // Ditto
            if (result < setResult) {
                result = setResult;
            }
        } // For each enumerated WLAN interface
    } // End if enumeration succeeded

    // Free memory for the enumerated info list
    if (infoListPtr != nullptr) {
        ::WlanFreeMemory(infoListPtr);
    }

    // Leak handle intentionally here - When the app closes it will be released.
    //::WlanCloseHandle(m_clientHandle, nullptr);

    return result;

#else //_WIN32

    (void)enable; // Unused

    // TBD: Are there also correctable issues on Mac/Linux?
    return OptimizeWLAN_Unavailable;

#endif //_WIN32
}


//------------------------------------------------------------------------------
// WLANOptimizerThread

class WLANOptimizerThread
{
public:
    /// Start the optimizer thread
    void Start();

    /// Stop the optimizer thread
    void Stop();

protected:
    /// Lock preventing thread safety issues around Start() and Stop()
    mutable std::mutex StartStopLock;

    /// Lock protecting WakeCondition
    mutable std::mutex WakeLock;

    /// Condition that indicates the thread should wake up
    std::condition_variable WakeCondition;

    /// Background thread
    std::shared_ptr<std::thread> Thread;

    /// Should thread terminate?
    std::atomic<bool> Terminated = ATOMIC_VAR_INIT(true);


    /// Thread loop
    void Loop();
};

void WLANOptimizerThread::Start()
{
    std::lock_guard<std::mutex> startStopLocker(StartStopLock);

    if (!Thread)
    {
        Terminated = false;
        Thread = std::make_shared<std::thread>(&WLANOptimizerThread::Loop, this);
    }
}

void WLANOptimizerThread::Stop()
{
    std::lock_guard<std::mutex> startStopLocker(StartStopLock);

    if (Thread)
    {
        Terminated = true;

        // Make sure that queue notification happens after termination flag is set
        {
            std::unique_lock<std::mutex> wakeLocker(WakeLock);
            WakeCondition.notify_all();
        }

        // Wait for thread to stop
        try
        {
            if (Thread->joinable()) {
                Thread->join();
            }
        }
        catch (std::system_error& /*err*/)
        {
        }

        Thread = nullptr;
    }
}

void WLANOptimizerThread::Loop()
{
    // Time between optimization attempts.  Retries because WiFi might reconnect.
    static const auto kOptimizeInterval = std::chrono::seconds(11);

    while (!Terminated)
    {
        int optimizeResult = OptimizeWLAN(1);

        // If result was a failure code,
        // and the code was not that no connections were available:
        if (OWLAN_IS_ERROR(optimizeResult))
        {
#ifndef WLANOPT_DISABLE_LOGGING
            Logger.Error("Quitting: OptimizeWLAN() failed with error ", optimizeResult);
#endif // WLANOPT_DISABLE_LOGGING
            // Stop trying to optimize WLAN if we hit an unexpected failure
            break;
        }

#ifndef WLANOPT_DISABLE_LOGGING
        if (optimizeResult == OptimizeWLAN_Applied) {
            Logger.Info("Optimized WiFi adapter settings for low latency");
        }
        else if (optimizeResult == OptimizeWLAN_Preconfigured) {
            //Logger.Debug("Already successfully applied settings");
        }
        else if (optimizeResult == OptimizeWLAN_NoConnections) {
            //Logger.Debug("No WiFi connections available");
        }
#endif // WLANOPT_DISABLE_LOGGING

        // unique_lock used since QueueCondition.wait requires it
        std::unique_lock<std::mutex> locker(WakeLock);

        if (!Terminated) {
            WakeCondition.wait_for(locker, kOptimizeInterval);
        }
    }
}


static WLANOptimizerThread m_WlanOptimizer;

void StartWLANOptimizerThread()
{
#ifdef _WIN32
    // Only run the thread on Windows because currently it has no optimizations
    // for other platforms.
    m_WlanOptimizer.Start();
#endif // _WIN32
}

void StopWLANOptimizerThread()
{
#ifdef _WIN32
    m_WlanOptimizer.Stop();
#endif // _WIN32
}
