#pragma once

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <algorithm>
#include <string>
#include <vector>
#include <sstream>
#include <thread>

#pragma comment(lib, "pdh.lib")

class ResourceUsage {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER gpuCounter = nullptr;
    std::wstring gpuError;
    ULONGLONG previousTime = 0, previousCpu = 0;
    unsigned processors = std::max(1u, std::thread::hardware_concurrency());
    static ULONGLONG ticks(FILETIME value) {
        return (static_cast<ULONGLONG>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    }
    static std::wstring failure(const wchar_t* action, PDH_STATUS code) {
        std::wstringstream out;
        out << action << L" (0x" << std::hex << static_cast<unsigned long>(code) << L")";
        return out.str();
    }
public:
    struct Sample {
        double cpu = 0, gpu = 0;
        bool cpuValid = false, gpuValid = false;
        std::wstring cpuStatus, gpuStatus;
    };
    ResourceUsage() {
        PDH_STATUS result = PdhOpenQueryW(nullptr, 0, &query);
        if (result != ERROR_SUCCESS) { gpuError = failure(L"GPU counters unavailable", result); return; }
        result = PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &gpuCounter);
        if (result != ERROR_SUCCESS) gpuError = failure(L"GPU engine counter unavailable", result);
        else {
            result = PdhCollectQueryData(query);
            if (result != ERROR_SUCCESS && result != PDH_NO_DATA)
                gpuError = failure(L"GPU counter sampling failed", result);
        }
    }
    ~ResourceUsage() { if (query) PdhCloseQuery(query); }
    ResourceUsage(const ResourceUsage&) = delete;
    ResourceUsage& operator=(const ResourceUsage&) = delete;
    Sample sample() {
        Sample out;
        FILETIME created{}, exited{}, kernel{}, user{}, now{};
        GetSystemTimeAsFileTime(&now);
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
            ULONGLONG time = ticks(now), cpu = ticks(kernel) + ticks(user);
            if (previousTime && time > previousTime && cpu >= previousCpu) {
                out.cpu = std::min(100.0, 100.0 * (cpu - previousCpu) / (time - previousTime) / processors);
                out.cpuValid = true;
            } else out.cpuStatus = L"sampling";
            previousTime = time;
            previousCpu = cpu;
        } else out.cpuStatus = L"unavailable (Windows error " + std::to_wstring(GetLastError()) + L")";

        if (!gpuError.empty()) { out.gpuStatus = gpuError; return out; }
        PDH_STATUS result = PdhCollectQueryData(query);
        if (result != ERROR_SUCCESS) { out.gpuStatus = failure(L"GPU sampling failed", result); return out; }
        DWORD bytes = 0, count = 0;
        result = PdhGetFormattedCounterArrayW(gpuCounter, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
        if (result == PDH_CSTATUS_NO_INSTANCE || (result == ERROR_SUCCESS && !count)) {
            out.gpuStatus = L"no engine sample";
            return out;
        }
        if (result != PDH_MORE_DATA) { out.gpuStatus = failure(L"GPU sample unavailable", result); return out; }
        std::vector<BYTE> storage(bytes);
        auto items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(storage.data());
        result = PdhGetFormattedCounterArrayW(gpuCounter, PDH_FMT_DOUBLE, &bytes, &count, items);
        if (result != ERROR_SUCCESS) { out.gpuStatus = failure(L"GPU sample unavailable", result); return out; }
        std::wstring prefix = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_";
        for (DWORD i = 0; i < count; ++i) {
            if (std::wstring(items[i].szName).compare(0, prefix.size(), prefix) != 0) continue;
            auto& value = items[i].FmtValue;
            if (value.CStatus == PDH_CSTATUS_VALID_DATA || value.CStatus == PDH_CSTATUS_NEW_DATA) {
                out.gpu = std::max(out.gpu, std::min(100.0, value.doubleValue));
                out.gpuValid = true;
            }
        }
        if (!out.gpuValid) out.gpuStatus = L"no active app engine";
        return out;
    }
};
