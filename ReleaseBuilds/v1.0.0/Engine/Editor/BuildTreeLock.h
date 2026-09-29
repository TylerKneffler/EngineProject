#pragma once

#include "pch.h"

namespace Engine::Editor
{
// CMake/MSBuild invocations that share a build directory also share all of
// its intermediate object files.  Holding this file without sharing prevents
// the editor's independent background builders from modifying those files at
// the same time.
class BuildTreeLock
{
public:
    BuildTreeLock() = default;
    ~BuildTreeLock() { Release(); }

    BuildTreeLock(const BuildTreeLock&) = delete;
    BuildTreeLock& operator=(const BuildTreeLock&) = delete;

    bool TryAcquire(const std::filesystem::path& buildDirectory)
    {
        if (m_handle != INVALID_HANDLE_VALUE) return true;

        std::error_code error;
        std::filesystem::create_directories(buildDirectory, error);
        if (error) return false;

        const std::filesystem::path lockPath = buildDirectory / ".engine-build.lock";
        m_handle = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return m_handle != INVALID_HANDLE_VALUE;
    }

    void Release()
    {
        if (m_handle == INVALID_HANDLE_VALUE) return;
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};
}
