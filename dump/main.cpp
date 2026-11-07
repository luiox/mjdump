#include <cstdio>
#include <cstring>
#include "libca/log/log_macros.hpp"
#include "mjbase/log/log_init.hpp"

int cmd_collect(int argc, char* argv[]);
int cmd_inject(int argc, char* argv[]);
int cmd_dump(int argc, char* argv[]);
int cmd_inspect(int argc, char* argv[]);
int cmd_list(int argc, char* argv[]);
int cmd_pack(int argc, char* argv[]);
int cmd_sessions(int argc, char* argv[]);
int cmd_status(int argc, char* argv[]);
int cmd_sweep(int argc, char* argv[]);

namespace {

void print_usage()
{
    fprintf(stderr,
            "Usage: jdumper <command> [args]\n\n"
            "Commands:\n"
            "  list           List visible java.exe/javaw.exe processes\n"
            "  collect        Start Named Pipe Server and receive class data\n"
            "  inject  <pid>  Inject jdserver.dll into JVM process (P1)\n"
            "  dump    <pid>  Dump a JVM by injecting jdserver.dll\n"
            "  inspect <pid>  Preview loaded classes and dump filter matches\n"
            "  classes <pid>  Alias for inspect\n"
            "  sessions      List recorded dump/inspect sessions\n"
            "  sweep   <dir>  Re-run dump against a recorded session pid/output\n"
            "  pack    <dir>  Pack a dump class directory into a JAR\n"
            "  status  <dir>  Print dump session metadata\n");
}

}   // namespace

int main(int argc, char* argv[])
{
    // CLI 侧诊断统一走 ca::log 门面（用法说明见 doc:cli-support-commands）。
    mjbase::log::initDefaultLogger();
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage();
        return 0;
    }
    else if (strcmp(argv[1], "list") == 0)
        return cmd_list(argc - 1, argv + 1);
    else if (strcmp(argv[1], "collect") == 0)
        return cmd_collect(argc - 1, argv + 1);
    else if (strcmp(argv[1], "inject") == 0)
        return cmd_inject(argc - 1, argv + 1);
    else if (strcmp(argv[1], "dump") == 0)
        return cmd_dump(argc - 1, argv + 1);
    else if (strcmp(argv[1], "inspect") == 0 || strcmp(argv[1], "classes") == 0)
        return cmd_inspect(argc - 1, argv + 1);
    else if (strcmp(argv[1], "sessions") == 0)
        return cmd_sessions(argc - 1, argv + 1);
    else if (strcmp(argv[1], "sweep") == 0)
        return cmd_sweep(argc - 1, argv + 1);
    else if (strcmp(argv[1], "pack") == 0)
        return cmd_pack(argc - 1, argv + 1);
    else if (strcmp(argv[1], "status") == 0)
        return cmd_status(argc - 1, argv + 1);
    else {
        CA_LOG_ERROR("Unknown command: {}", argv[1]);
        return 1;
    }

    return 1;
}
