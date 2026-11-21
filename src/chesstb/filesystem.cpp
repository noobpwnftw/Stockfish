#include "filesystem.h"

#include <cerrno>
#include <cstdint>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#else
    #include <fcntl.h>
    #include <sys/mman.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

namespace Stockfish::ChessTB {

bool Memory_Mapped_File::open_readonly(const std::filesystem::path& path) {
#ifdef _WIN32
    HANDLE fd = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (fd == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER bytes;
    GetFileSizeEx(fd, &bytes);
    HANDLE mmap = CreateFileMapping(fd, nullptr, PAGE_READONLY, 0, 0, nullptr);
    CloseHandle(fd);
    if (!mmap)
        return false;
    void* view = MapViewOfFile(mmap, FILE_MAP_READ, 0, 0, 0);
    if (!view)
    {
        CloseHandle(mmap);
        return false;
    }
    m_handle = mmap;
    m_size   = size_t(bytes.QuadPart);
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd == -1)
        return false;
    struct stat st;
    void*       view = fstat(fd, &st) == 0
                       ? mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0)
                       : MAP_FAILED;
    ::close(fd);
    if (view == MAP_FAILED)
        return false;
    m_size = size_t(st.st_size);
    madvise(view, m_size, MADV_RANDOM);
#endif
    m_data = static_cast<const uint8_t*>(view);
    return true;
}

void Memory_Mapped_File::close_file() {
    if (!m_data)
        return;
#ifdef _WIN32
    UnmapViewOfFile(m_data);
    CloseHandle(m_handle);
#else
    munmap(const_cast<uint8_t*>(m_data), m_size);
#endif
    m_data = nullptr;
    m_size = 0;
}

}  // namespace Stockfish::ChessTB
