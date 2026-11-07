#include <Windows.h>
#include "jninfra/dump/class_file.hpp"

#include <cstdio>
#include <cstring>
#include <vector>
#include "libca/log/log_macros.hpp"

namespace {

bool read_exact(HANDLE pipe, void* data, DWORD size)
{
    auto* cursor = static_cast<unsigned char*>(data);
    DWORD total  = 0;
    while (total < size) {
        DWORD chunk = 0;
        if (!ReadFile(pipe, cursor + total, size - total, &chunk, nullptr) || chunk == 0) {
            return false;
        }
        total += chunk;
    }
    return true;
}

void print_usage()
{
    std::fprintf(stderr,
                 "Usage: jdumper collect [-o <dir>] [--pipe <name>] [--once]\n\n"
                 "Receives records encoded as name_len + name + data_len + bytes.\n");
}

}   // namespace

int cmd_collect(int argc, char* argv[])
{
    const char* pipeName  = "\\\\.\\pipe\\jdumper";
    const char* outputDir = "C:\\JavaClassDump";
    bool        once      = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            outputDir = argv[++i];
        else if (strcmp(argv[i], "--pipe") == 0 && i + 1 < argc)
            pipeName = argv[++i];
        else if (strcmp(argv[i], "--once") == 0)
            once = true;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        }
    }

    printf("[collect] Listening on pipe %s\n", pipeName);
    jninfra::dump::FileClassSink sink(outputDir);

    for (;;) {
        HANDLE hPipe = CreateNamedPipeA(pipeName,
                                        PIPE_ACCESS_INBOUND,
                                        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                        1,
                                        0,
                                        65536,
                                        0,
                                        NULL);

        if (hPipe == INVALID_HANDLE_VALUE) {
            CA_LOG_ERROR("CreateNamedPipe failed: {}", GetLastError());
            return 1;
        }

        BOOL connected =
            ConnectNamedPipe(hPipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) {
            CloseHandle(hPipe);
            continue;
        }

        printf("[collect] Client connected\n");

        // 读取数据：name_len(4) + name + data_len(4) + data
        for (;;) {
            int nameLen = 0;
            if (!read_exact(hPipe, &nameLen, sizeof(nameLen))) break;
            if (nameLen <= 0 || nameLen > 4096) {
                CA_LOG_ERROR("[collect] invalid class name length: {}", nameLen);
                break;
            }

            std::vector<char> name(static_cast<std::size_t>(nameLen) + 1);
            if (!read_exact(hPipe, name.data(), static_cast<DWORD>(nameLen))) break;
            name[static_cast<std::size_t>(nameLen)] = '\0';

            int dataLen = 0;
            if (!read_exact(hPipe, &dataLen, sizeof(dataLen))) break;
            if (dataLen <= 0 || dataLen > 256 * 1024 * 1024) {
                CA_LOG_ERROR("[collect] invalid class data length: {}", dataLen);
                break;
            }

            std::vector<unsigned char> data(static_cast<std::size_t>(dataLen));
            if (!read_exact(hPipe, data.data(), static_cast<DWORD>(dataLen))) break;

            jninfra::dump::ClassFileView view{
                name.data(),
                "",
                data.data(),
                static_cast<ca::usize>(data.size()),
            };
            auto result = sink.write(view);
            if (result.is_err()) {
                CA_LOG_ERROR("[collect] write failed: {}", result.unwrap_err().message.c_str());
                continue;
            }
            printf("[collect] %s (%d bytes)\n", name.data(), dataLen);
        }

        printf("[collect] Client disconnected\n");
        FlushFileBuffers(hPipe);
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
        if (once) {
            break;
        }
    }

    return 0;
}
