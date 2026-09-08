/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "nds_platform_bridge.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace
{

using melonDS::Platform::FileMode;

bool HasMode(FileMode mode, FileMode flag)
{
    return (static_cast<unsigned>(mode) & static_cast<unsigned>(flag)) != 0U;
}

bool HasAnyAccess(FileMode mode)
{
    return HasMode(mode, FileMode::Read) || HasMode(mode, FileMode::Write) ||
           HasMode(mode, FileMode::Append);
}

std::string BuildModeString(FileMode mode, bool file_exists)
{
    const bool readable = HasMode(mode, FileMode::Read);
    const bool writable = HasMode(mode, FileMode::Write) ||
                          HasMode(mode, FileMode::Append);
    const bool append = HasMode(mode, FileMode::Append);
    const bool preserve = HasMode(mode, FileMode::Preserve);

    std::string result;
    if (append)
        result.push_back('a');
    else if (!writable)
        result.push_back('r');
    else if (preserve && file_exists)
        result.push_back('r');
    else
        result.push_back('w');

    /* A preserving write has to use update mode with stdio.  It is harmless
     * that such a handle is also readable when the caller only requested
     * writes. */
    if ((readable && writable) || (writable && preserve && file_exists))
        result.push_back('+');

    if (!HasMode(mode, FileMode::Text))
        result.push_back('b');
    return result;
}

std::FILE *OpenNativeFile(const std::string &path, const std::string &mode)
{
#if defined(_WIN32)
    try
    {
        const std::filesystem::path native_path = std::filesystem::u8path(path);
        const std::wstring native_mode(mode.begin(), mode.end());
        return _wfopen(native_path.c_str(), native_mode.c_str());
    }
    catch (...)
    {
        return nullptr;
    }
#else
    return std::fopen(path.c_str(), mode.c_str());
#endif
}

std::int64_t TellFile(std::FILE *stream)
{
#if defined(_WIN32)
    return _ftelli64(stream);
#else
    return static_cast<std::int64_t>(ftello(stream));
#endif
}

bool SeekFile(std::FILE *stream, std::int64_t offset, int origin)
{
#if defined(_WIN32)
    return _fseeki64(stream, offset, origin) == 0;
#else
    const off_t native_offset = static_cast<off_t>(offset);
    if (static_cast<std::int64_t>(native_offset) != offset)
        return false;
    return fseeko(stream, native_offset, origin) == 0;
#endif
}

const std::chrono::steady_clock::time_point &ClockEpoch()
{
    static const auto epoch = std::chrono::steady_clock::now();
    return epoch;
}

} // namespace

namespace melonDS::Platform
{

struct FileHandle
{
    explicit FileHandle(std::FILE *stream_) : stream(stream_) {}

    std::FILE *stream;
};

struct Thread
{
    explicit Thread(std::function<void()> task)
        : worker([task = std::move(task)]() mutable {
              try
              {
                  task();
              }
              catch (const std::exception &error)
              {
                  Platform::Log(LogLevel::Error,
                                "Unhandled melonDS worker exception: %s\n",
                                error.what());
              }
              catch (...)
              {
                  Platform::Log(LogLevel::Error,
                                "Unhandled unknown melonDS worker exception\n");
              }
          })
    {
    }

    std::thread worker;
};

struct Semaphore
{
    std::mutex mutex;
    std::condition_variable changed;
    std::uint64_t count = 0;
};

struct Mutex
{
    std::mutex mutex;
};

struct DynamicLibrary
{
#if defined(_WIN32)
    HMODULE handle;
#else
    void *handle;
#endif
};

void SignalStop(StopReason reason, void *userdata)
{
    dualboy_melonds_platform::SignalStop(reason, userdata);
}

std::string GetLocalFilePath(const std::string &filename)
{
    try
    {
        const std::filesystem::path path = std::filesystem::u8path(filename);
        if (path.is_absolute())
            return path.u8string();

        const std::string directory =
            dualboy_melonds_platform::SystemDirectory();
        if (directory.empty())
            return filename;

        return (std::filesystem::u8path(directory) / path).u8string();
    }
    catch (...)
    {
        return filename;
    }
}

FileHandle *OpenFile(const std::string &path, FileMode mode)
{
    if (!HasAnyAccess(mode))
    {
        Log(LogLevel::Error,
            "Attempted to open '%s' without read or write access\n",
            path.c_str());
        return nullptr;
    }

    const bool existed = FileExists(path);
    if (HasMode(mode, FileMode::NoCreate) && !existed)
        return nullptr;

    const std::string native_mode = BuildModeString(mode, existed);
    std::FILE *stream = OpenNativeFile(path, native_mode);

    /* stdio cannot express "preserve and create, but do not read" in one
     * mode.  BuildModeString selects w/w+ for the missing-file case, so there
     * is no second, racy open needed here. */
    if (!stream)
        return nullptr;

    std::unique_ptr<FileHandle> result(new (std::nothrow) FileHandle(stream));
    if (!result)
    {
        std::fclose(stream);
        return nullptr;
    }
    return result.release();
}

FileHandle *OpenLocalFile(const std::string &path, FileMode mode)
{
    return OpenFile(GetLocalFilePath(path), mode);
}

bool FileExists(const std::string &name)
{
    try
    {
        std::error_code error;
        return std::filesystem::exists(std::filesystem::u8path(name), error) &&
               !error;
    }
    catch (...)
    {
        return false;
    }
}

bool LocalFileExists(const std::string &name)
{
    return FileExists(GetLocalFilePath(name));
}

bool CheckFileWritable(const std::string &filepath)
{
    const FileMode mode = static_cast<FileMode>(
        static_cast<unsigned>(FileMode::Write) |
        static_cast<unsigned>(FileMode::Preserve) |
        static_cast<unsigned>(FileMode::Append));
    FileHandle *file = OpenFile(filepath, mode);
    if (!file)
        return false;
    return CloseFile(file);
}

bool CheckLocalFileWritable(const std::string &filepath)
{
    return CheckFileWritable(GetLocalFilePath(filepath));
}

bool CloseFile(FileHandle *file)
{
    if (!file)
        return false;
    const bool success = file->stream && std::fclose(file->stream) == 0;
    delete file;
    return success;
}

bool IsEndOfFile(FileHandle *file)
{
    return file && file->stream && std::feof(file->stream) != 0;
}

bool FileReadLine(char *str, int count, FileHandle *file)
{
    return str && count > 0 && file && file->stream &&
           std::fgets(str, count, file->stream) != nullptr;
}

u64 FilePosition(FileHandle *file)
{
    if (!file || !file->stream)
        return 0;
    const std::int64_t position = TellFile(file->stream);
    return position < 0 ? 0 : static_cast<u64>(position);
}

bool FileSeek(FileHandle *file, s64 offset, FileSeekOrigin origin)
{
    if (!file || !file->stream)
        return false;

    int native_origin = SEEK_SET;
    switch (origin)
    {
    case FileSeekOrigin::Start:
        native_origin = SEEK_SET;
        break;
    case FileSeekOrigin::Current:
        native_origin = SEEK_CUR;
        break;
    case FileSeekOrigin::End:
        native_origin = SEEK_END;
        break;
    }
    return SeekFile(file->stream, offset, native_origin);
}

void FileRewind(FileHandle *file)
{
    if (file && file->stream)
        std::rewind(file->stream);
}

u64 FileRead(void *data, u64 size, u64 count, FileHandle *file)
{
    if (!file || !file->stream || !data || size == 0 || count == 0 ||
        size > std::numeric_limits<std::size_t>::max() ||
        count > std::numeric_limits<std::size_t>::max())
        return 0;

    return static_cast<u64>(std::fread(data,
                                       static_cast<std::size_t>(size),
                                       static_cast<std::size_t>(count),
                                       file->stream));
}

bool FileFlush(FileHandle *file)
{
    return file && file->stream && std::fflush(file->stream) == 0;
}

u64 FileWrite(const void *data, u64 size, u64 count, FileHandle *file)
{
    if (!file || !file->stream || !data || size == 0 || count == 0 ||
        size > std::numeric_limits<std::size_t>::max() ||
        count > std::numeric_limits<std::size_t>::max())
        return 0;

    return static_cast<u64>(std::fwrite(data,
                                        static_cast<std::size_t>(size),
                                        static_cast<std::size_t>(count),
                                        file->stream));
}

u64 FileWriteFormatted(FileHandle *file, const char *fmt, ...)
{
    if (!file || !file->stream || !fmt)
        return 0;

    va_list arguments;
    va_start(arguments, fmt);
    const int written = std::vfprintf(file->stream, fmt, arguments);
    va_end(arguments);
    return written < 0 ? 0 : static_cast<u64>(written);
}

u64 FileLength(FileHandle *file)
{
    if (!file || !file->stream)
        return 0;

    const std::int64_t original = TellFile(file->stream);
    if (original < 0)
        return 0;
    if (!SeekFile(file->stream, 0, SEEK_END))
    {
        (void)SeekFile(file->stream, original, SEEK_SET);
        return 0;
    }

    const std::int64_t length = TellFile(file->stream);
    const bool restored = SeekFile(file->stream, original, SEEK_SET);
    if (length < 0 || !restored)
        return 0;
    return static_cast<u64>(length);
}

void Log(LogLevel level, const char *fmt, ...)
{
    if (!fmt)
        return;

    va_list arguments;
    va_start(arguments, fmt);
    va_list count_arguments;
    va_copy(count_arguments, arguments);
    const int required = std::vsnprintf(nullptr, 0, fmt, count_arguments);
    va_end(count_arguments);

    if (required < 0)
    {
        va_end(arguments);
        dualboy_melonds_platform::Log(level, fmt);
        return;
    }

    std::vector<char> message;
    try
    {
        message.resize(static_cast<std::size_t>(required) + 1U);
    }
    catch (...)
    {
        va_end(arguments);
        dualboy_melonds_platform::Log(level, fmt);
        return;
    }

    std::vsnprintf(message.data(), message.size(), fmt, arguments);
    va_end(arguments);
    dualboy_melonds_platform::Log(level, message.data());
}

Thread *Thread_Create(std::function<void()> func)
{
    if (!func)
        return nullptr;
    try
    {
        return new Thread(std::move(func));
    }
    catch (...)
    {
        return nullptr;
    }
}

void Thread_Free(Thread *thread)
{
    if (!thread)
        return;
    if (thread->worker.joinable())
    {
        if (thread->worker.get_id() == std::this_thread::get_id())
            thread->worker.detach();
        else
            thread->worker.join();
    }
    delete thread;
}

void Thread_Wait(Thread *thread)
{
    if (!thread || !thread->worker.joinable() ||
        thread->worker.get_id() == std::this_thread::get_id())
        return;
    thread->worker.join();
}

Semaphore *Semaphore_Create()
{
    return new (std::nothrow) Semaphore();
}

void Semaphore_Free(Semaphore *sema)
{
    delete sema;
}

void Semaphore_Reset(Semaphore *sema)
{
    if (!sema)
        return;
    std::lock_guard<std::mutex> lock(sema->mutex);
    sema->count = 0;
}

void Semaphore_Wait(Semaphore *sema)
{
    if (!sema)
        return;
    std::unique_lock<std::mutex> lock(sema->mutex);
    sema->changed.wait(lock, [sema] { return sema->count != 0; });
    --sema->count;
}

bool Semaphore_TryWait(Semaphore *sema, int timeout_ms)
{
    if (!sema)
        return false;

    std::unique_lock<std::mutex> lock(sema->mutex);
    if (timeout_ms <= 0)
    {
        if (sema->count == 0)
            return false;
    }
    else if (!sema->changed.wait_for(lock,
                                     std::chrono::milliseconds(timeout_ms),
                                     [sema] { return sema->count != 0; }))
    {
        return false;
    }

    --sema->count;
    return true;
}

void Semaphore_Post(Semaphore *sema, int count)
{
    if (!sema || count <= 0)
        return;

    {
        std::lock_guard<std::mutex> lock(sema->mutex);
        const std::uint64_t amount = static_cast<std::uint64_t>(count);
        if (amount > std::numeric_limits<std::uint64_t>::max() - sema->count)
            sema->count = std::numeric_limits<std::uint64_t>::max();
        else
            sema->count += amount;
    }

    if (count == 1)
        sema->changed.notify_one();
    else
        sema->changed.notify_all();
}

Mutex *Mutex_Create()
{
    return new (std::nothrow) Mutex();
}

void Mutex_Free(Mutex *mutex)
{
    delete mutex;
}

void Mutex_Lock(Mutex *mutex)
{
    if (mutex)
        mutex->mutex.lock();
}

void Mutex_Unlock(Mutex *mutex)
{
    if (mutex)
        mutex->mutex.unlock();
}

bool Mutex_TryLock(Mutex *mutex)
{
    return mutex && mutex->mutex.try_lock();
}

void Sleep(u64 usecs)
{
    constexpr u64 maximum =
        static_cast<u64>(std::numeric_limits<std::int64_t>::max());
    while (usecs > maximum)
    {
        std::this_thread::sleep_for(
            std::chrono::microseconds(std::numeric_limits<std::int64_t>::max()));
        usecs -= maximum;
    }
    std::this_thread::sleep_for(
        std::chrono::microseconds(static_cast<std::int64_t>(usecs)));
}

u64 GetMSCount()
{
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - ClockEpoch())
                                .count());
}

u64 GetUSCount()
{
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - ClockEpoch())
                                .count());
}

void WriteNDSSave(const u8 *savedata,
                  u32 savelen,
                  u32 writeoffset,
                  u32 writelen,
                  void *userdata)
{
    dualboy_melonds_platform::WriteNDSSave(
        savedata, savelen, writeoffset, writelen, userdata);
}

void WriteGBASave(const u8 *savedata,
                  u32 savelen,
                  u32 writeoffset,
                  u32 writelen,
                  void *userdata)
{
    dualboy_melonds_platform::WriteGBASave(
        savedata, savelen, writeoffset, writelen, userdata);
}

void WriteFirmware(const Firmware &firmware,
                   u32 writeoffset,
                   u32 writelen,
                   void *userdata)
{
    dualboy_melonds_platform::WriteFirmware(
        firmware, writeoffset, writelen, userdata);
}

void WriteDateTime(int year,
                   int month,
                   int day,
                   int hour,
                   int minute,
                   int second,
                   void *userdata)
{
    dualboy_melonds_platform::WriteDateTime(
        year, month, day, hour, minute, second, userdata);
}

void MP_Begin(void *userdata)
{
    dualboy_melonds_platform::MPBegin(userdata);
}

void MP_End(void *userdata)
{
    dualboy_melonds_platform::MPEnd(userdata);
}

int MP_SendPacket(u8 *data, int len, u64 timestamp, void *userdata)
{
    return dualboy_melonds_platform::MPSendPacket(
        data, len, timestamp, userdata);
}

int MP_RecvPacket(u8 *data, u64 *timestamp, void *userdata)
{
    return dualboy_melonds_platform::MPRecvPacket(data, timestamp, userdata);
}

int MP_SendCmd(u8 *data, int len, u64 timestamp, void *userdata)
{
    return dualboy_melonds_platform::MPSendCmd(data, len, timestamp, userdata);
}

int MP_SendReply(u8 *data,
                 int len,
                 u64 timestamp,
                 u16 aid,
                 void *userdata)
{
    return dualboy_melonds_platform::MPSendReply(
        data, len, timestamp, aid, userdata);
}

int MP_SendAck(u8 *data, int len, u64 timestamp, void *userdata)
{
    return dualboy_melonds_platform::MPSendAck(data, len, timestamp, userdata);
}

int MP_RecvHostPacket(u8 *data, u64 *timestamp, void *userdata)
{
    return dualboy_melonds_platform::MPRecvHostPacket(
        data, timestamp, userdata);
}

u16 MP_RecvReplies(u8 *data, u64 timestamp, u16 aidmask, void *userdata)
{
    return dualboy_melonds_platform::MPRecvReplies(
        data, timestamp, aidmask, userdata);
}

int Net_SendPacket(u8 *data, int len, void *userdata)
{
    (void)data;
    (void)len;
    (void)userdata;
    return 0;
}

int Net_RecvPacket(u8 *data, void *userdata)
{
    (void)data;
    (void)userdata;
    return 0;
}

void Camera_Start(int num, void *userdata)
{
    (void)num;
    (void)userdata;
}

void Camera_Stop(int num, void *userdata)
{
    (void)num;
    (void)userdata;
}

void Camera_CaptureFrame(int num,
                         u32 *frame,
                         int width,
                         int height,
                         bool yuv,
                         void *userdata)
{
    (void)num;
    (void)yuv;
    (void)userdata;
    if (!frame || width <= 0 || height <= 0)
        return;

    const auto unsigned_width = static_cast<std::size_t>(width);
    const auto unsigned_height = static_cast<std::size_t>(height);
    if (unsigned_width >
        std::numeric_limits<std::size_t>::max() / unsigned_height)
        return;
    std::fill_n(frame, unsigned_width * unsigned_height, 0U);
}

void Mic_Start(void *userdata)
{
    (void)userdata;
}

void Mic_Stop(void *userdata)
{
    (void)userdata;
}

int Mic_ReadInput(s16 *data, int maxlength, void *userdata)
{
    (void)userdata;
    if (!data || maxlength <= 0)
        return 0;
    std::fill_n(data, static_cast<std::size_t>(maxlength), static_cast<s16>(0));
    return maxlength;
}

AACDecoder *AAC_Init()
{
    return nullptr;
}

void AAC_DeInit(AACDecoder *dec)
{
    (void)dec;
}

bool AAC_Configure(AACDecoder *dec, int frequency, int channels)
{
    (void)dec;
    (void)frequency;
    (void)channels;
    return false;
}

bool AAC_DecodeFrame(AACDecoder *dec,
                     const void *input,
                     int inputlen,
                     void *output,
                     int outputlen)
{
    (void)dec;
    (void)input;
    (void)inputlen;
    (void)output;
    (void)outputlen;
    return false;
}

bool Addon_KeyDown(KeyType type, void *userdata)
{
    (void)type;
    (void)userdata;
    return false;
}

void Addon_RumbleStart(u32 len, void *userdata)
{
    (void)len;
    (void)userdata;
}

void Addon_RumbleStop(void *userdata)
{
    (void)userdata;
}

float Addon_MotionQuery(MotionQueryType type, void *userdata)
{
    (void)type;
    (void)userdata;
    return 0.0F;
}

DynamicLibrary *DynamicLibrary_Load(const char *lib)
{
    if (!lib || !*lib)
        return nullptr;

#if defined(_WIN32)
    HMODULE handle = nullptr;
    try
    {
        const std::filesystem::path path = std::filesystem::u8path(lib);
        handle = LoadLibraryW(path.c_str());
    }
    catch (...)
    {
        return nullptr;
    }
#else
    void *handle = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
#endif
    if (!handle)
        return nullptr;

    std::unique_ptr<DynamicLibrary> result(
        new (std::nothrow) DynamicLibrary{handle});
    if (!result)
    {
#if defined(_WIN32)
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        return nullptr;
    }
    return result.release();
}

void DynamicLibrary_Unload(DynamicLibrary *lib)
{
    if (!lib)
        return;
    if (lib->handle)
    {
#if defined(_WIN32)
        FreeLibrary(lib->handle);
#else
        dlclose(lib->handle);
#endif
    }
    delete lib;
}

void *DynamicLibrary_LoadFunction(DynamicLibrary *lib, const char *name)
{
    if (!lib || !lib->handle || !name || !*name)
        return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void *>(GetProcAddress(lib->handle, name));
#else
    return dlsym(lib->handle, name);
#endif
}

} // namespace melonDS::Platform
