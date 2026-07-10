
#include <stdio.h>
#include <stdlib.h>
#include "firmware.h"
#include "logger.h"

int main(int argc, char* argv[]) {
    /* ---- 初始化日志系统 ---- */
    /* 可选: 同时写文件 (NULL=不写文件, LOG_LEVEL_NONE=不写文件) */
    LoggerInit(NULL, LOG_LEVEL_NONE);

    if (argc < 2) {
        printf("client  - block-level migration transfer\n");
        printf("Usage:\n");
        printf("  %s <ip:port> [config.json]    Run migration\n", argv[0]);
        printf("  %s info                       Show disk information\n", argv[0]);
        printf("  %s dcbt_status [disk] [--raw] Show DCBT dirty bitmap and stats\n", argv[0]);
        printf("  %s hash <file>                Compute block hash\n", argv[0]);
        printf("  %s check <disk>               Check disk accessibility\n", argv[0]);
        printf("  %s isbios                     Detect firmware type (UEFI/Legacy)\n", argv[0]);
        printf("  %s begin_session              Mark migration session start\n", argv[0]);
        printf("  %s end_session                Clean block tracking database\n", argv[0]);
        printf("  %s sentbytes                  Print total confirmed bytes\n", argv[0]);
        printf("  %s test_vss                   Test VSS snapshot functionality\n", argv[0]);
        printf("  %s vss_query                  List all existing snapshots\n", argv[0]);
        printf("  %s vss_delete <guid>          Delete specific snapshot\n", argv[0]);
        printf("  %s vss_delete --all           Delete all snapshots\n", argv[0]);
        printf("  %s blockinfo [flags] [db] [devno] [offset]  Query block tracking DB\n", argv[0]);
        printf("        --pending           Show only pending (ack=0) blocks\n");
        printf("        --acked             Show only confirmed (ack=1) blocks\n");
        printf("  %s dryrun [config.json]       Simulate full migration locally\n", argv[0]);
        printf("  %s incsync <ip:port> [config.json]  Single-pass incremental sync\n", argv[0]);
        printf("  %s checkenv [driver_dir]      Full system environment check\n", argv[0]);
        printf("  %s check_drivers [dir]        Check VirtIO driver install status\n", argv[0]);
        printf("  %s inject_driver [dir]        Pre-install VirtIO drivers for KVM\n", argv[0]);
        printf("  %s --help                     Show this help\n", argv[0]);

        LOG_WARN("no command provided, showing help");

        LoggerCleanup();
        return 0;
    }

    const char* cmd = argv[1];

    /* ---- isbios 命令演示: 使用日志模块记录操作过程 ---- */
    if (_stricmp(cmd, "isbios") == 0) {
        LOG_DEBUG("executing command: isbios");

        CbtFwType fwType = CbtDetectFirmware();

        printf("%s\n", CbtFwToString(fwType));
        LOG_DEBUG("isbios command completed. result=%s", CbtFwToString(fwType));

        LoggerCleanup();
        return 0;
    }

    /* TODO: 其他命令实现 ... */
    LOG_WARN("command '%s' not yet implemented", cmd);

    LoggerCleanup();
    return 1;
}
