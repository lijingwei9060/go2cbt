#include "DiskMap.h"
#include "Logger.h"
#include "Server.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <getopt.h>

static std::atomic<Server*> g_server{nullptr};

static void OnSignal(int sig)
{
    (void)sig;
    Server* s = g_server.load();
    if (s)
        s->Stop();
}

static void PrintUsage(const char* prog)
{
    std::fprintf(stderr,
        "Usage: %s --listen <ip:port> --disk <devno:path> [--disk ...] [--verbose]\n"
        "\n"
        "Options:\n"
        "  --listen <ip:port>   TCP listen address, e.g. 0.0.0.0:9000\n"
        "  --disk <devno:path>  disk mapping, repeatable. e.g. 0:/data/disk0.raw\n"
        "  --verbose            enable DEBUG level logs\n"
        "  --help               show this help\n",
        prog);
}

struct CliArgs
{
    std::string listen;
    std::vector<std::pair<uint32_t, std::string>> disks;
    bool verbose = false;
};

static bool ParseDisk(const std::string& s, uint32_t& devno, std::string& path)
{
    auto pos = s.find(':');
    if (pos == std::string::npos)
        return false;
    try
    {
        unsigned long v = std::stoul(s.substr(0, pos));
        if (v > 0xFFFFFFFFUL)
            return false;
        devno = static_cast<uint32_t>(v);
    }
    catch (...)
    {
        return false;
    }
    path = s.substr(pos + 1);
    return !path.empty();
}

int main(int argc, char** argv)
{
    CliArgs args;

    static option longOpts[] = {
        {"listen",  required_argument, nullptr, 'l'},
        {"disk",    required_argument, nullptr, 'd'},
        {"verbose", no_argument,       nullptr, 'v'},
        {"help",    no_argument,       nullptr, 'h'},
        {nullptr,   0,                 nullptr, 0},
    };

    int opt;
    int idx = 0;
    while ((opt = getopt_long(argc, argv, "l:d:vh", longOpts, &idx)) != -1)
    {
        switch (opt)
        {
            case 'l':
                args.listen = optarg;
                break;
            case 'd':
            {
                uint32_t devno;
                std::string path;
                if (!ParseDisk(optarg, devno, path))
                {
                    std::fprintf(stderr, "invalid --disk value: %s\n", optarg);
                    return 1;
                }
                args.disks.emplace_back(devno, path);
                break;
            }
            case 'v':
                args.verbose = true;
                break;
            case 'h':
                PrintUsage(argv[0]);
                return 0;
            default:
                PrintUsage(argv[0]);
                return 1;
        }
    }

    if (args.listen.empty() || args.disks.empty())
    {
        PrintUsage(argv[0]);
        return 1;
    }

    Logger::Init(args.verbose ? Logger::Level::DEBUG : Logger::Level::INFO);

    // 忽略 SIGPIPE，避免 send 到已关闭连接导致进程退出
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT,  OnSignal);
    std::signal(SIGTERM, OnSignal);

    LOG_INFO("[cbt-server] starting listen=%s verbose=%d disks=%zu",
             args.listen.c_str(), args.verbose ? 1 : 0, args.disks.size());

    DiskMap diskMap;
    for (const auto& kv : args.disks)
    {
        if (!diskMap.Add(kv.first, kv.second))
        {
            LOG_ERROR("[cbt-server] failed to open devno=%u path=%s, exit", kv.first, kv.second.c_str());
            return 1;
        }
    }

    Server server(diskMap, args.listen);
    g_server.store(&server);

    if (!server.Start())
    {
        LOG_ERROR("[cbt-server] start failed, exit");
        return 1;
    }

    server.Run();

    g_server.store(nullptr);
    diskMap.CloseAll();
    LOG_INFO("[cbt-server] exited cleanly");
    return 0;
}
