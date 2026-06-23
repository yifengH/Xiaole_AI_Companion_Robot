#ifndef _COMPANION_OTA_H
#define _COMPANION_OTA_H

#include <functional>
#include <string>

#include <esp_err.h>
#include "board.h"

// CompanionOta 只负责固件「刷写」这一件事:下载 → 校验(sha256 + 大小)→ 写分区 → 切启动分区。
// 「要不要升级、升到哪个版本」由服务端经 WSS 的 checkUpdate/update 帧回答(见 docs/device-access.md 第三部分),
// 设备不做任何版本比较、不走任何 HTTP 配置/激活接口。
class CompanionOta {
public:
    CompanionOta() = default;
    ~CompanionOta() = default;

    // 标记当前运行固件有效(取消回滚)。OTA 后首次正常启动时调用。
    void MarkCurrentVersionValid();

    // 当前运行固件版本(来自编译进固件的 app description),随 status 帧上报。
    std::string GetCurrentVersion();

    // 下载固件并校验后刷写。expected_sha256_hex / expected_size 来自服务端 update 帧
    // (packageHash / packageSize);校验不过即丢弃不装(契约硬性要求)。expected_size 为 0 表示不校验大小。
    static bool Upgrade(const std::string& firmware_url,
                        const std::string& expected_sha256_hex,
                        size_t expected_size,
                        std::function<void(int progress, size_t speed)> callback);
};

#endif // _COMPANION_OTA_H
