/**
 * 上一次传的图的 Flash 存储实现
 * ----------------------------------------------------------------------------
 * 分区：直接用 platformio.ini 指定的 minimal.csv 里那块 spiffs 数据区
 *       （0x150000，640KB）。这里不起文件系统 —— 全部内容就是 5.7KB 的点阵，
 *       套一层 LittleFS 要多背 40KB 代码并多一次挂载，不值当。
 *       所以只借它的地址，用 esp_partition 读写原始扇区。
 *
 * 布局：分区前 64KB 切成 8 个 8KB 的槽，每次上传轮着写下一个槽。
 *       槽 0            槽 1                       槽 7
 *       +-------------+ +-------------+   ......    +-------------+
 *       | 头 | 黑 | 红 | | 头 | 黑 | 红 |             | 头 | 黑 | 红 |
 *       +-------------+ +-------------+             +-------------+
 *       读的时候扫 8 个头，取「magic 对、长度对、CRC 对、seq 最大」的那一份。
 *
 * 为什么要轮着写：每保存一次要擦 2 个 4KB 扇区，固定写一个槽的话寿命只有
 *       10 万次；摊到 8 个槽上就够用到板子报废了。
 * 为什么要 CRC：擦完、写头、写数据是三步，中途断电会留下「头是新的、数据
 *       是半截」的槽。CRC 不过就当这槽没写过，上一张图还在别的槽里。
 * ==========================================================================*/
#include "photo_store.h"

#include <esp_partition.h>
#include <esp_rom_crc.h>

#include <string.h>

#include "log.h"
#include "photo_server.h"     // PHOTO_FRAME_BYTES / PHOTO_BODY_BYTES

namespace
{
    constexpr uint32_t kMagic = 0x504B454E; // 'NEKP'
    constexpr size_t kSlotSize = 0x2000; // 8KB = 2 个扇区，够放 5776 字节
    constexpr size_t kSlotCount = 8; // 共用 64KB，其余 576KB 留着不用
    constexpr size_t kHeader = 16; // 头长度，同时保证后面 4 字节对齐

    /** 槽头：16 字节，紧跟其后是黑层、红层 */
    struct SlotHeader
    {
        uint32_t magic;
        uint32_t seq; // 递增序号，越大越新
        uint32_t len; // 恒为 PHOTO_BODY_BYTES，兼作版本校验
        uint32_t crc; // 覆盖黑层 + 红层
    };

    // 头长度写死在偏移计算里，结构体被改动（加字段/换平台对齐）时这里要报错
    static_assert(sizeof(SlotHeader) == kHeader, "槽头必须是 16 字节");

    /** 找那块数据分区，找不到就一直返回 nullptr（只探测一次） */
    const esp_partition_t* dataPart()
    {
        static const esp_partition_t* part = nullptr;
        static bool probed = false;
        if (probed) return part;

        probed = true;
        part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                        ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
        if (!part)
        {
            logLine("[STORE] no spiffs partition: last photo cannot be stored");
        }
        else if (part->size < kSlotSize * kSlotCount)
        {
            logf("[STORE] partition only %u bytes, cannot hold %u slots\n",
                 (unsigned)part->size, (unsigned)(kSlotSize * kSlotCount));
            part = nullptr;
        }
        return part;
    }

    /** 两层分别过 CRC，链式接起来算一个（省一块 5.7KB 的拼接缓冲） */
    uint32_t frameCrc(const uint8_t* black, const uint8_t* red)
    {
        const uint32_t c = esp_rom_crc32_le(0, black, PHOTO_FRAME_BYTES);
        return esp_rom_crc32_le(c, red, PHOTO_FRAME_BYTES);
    }

    bool readHeader(const esp_partition_t* p, size_t slot, SlotHeader& h)
    {
        return esp_partition_read(p, slot * kSlotSize, &h, sizeof(h)) == ESP_OK;
    }
} // namespace

bool photoStoreSave(const uint8_t* black, const uint8_t* red)
{
    const esp_partition_t* p = dataPart();
    if (!p) return false;

    // 先找出当前最大的序号，新的一份接在后面
    bool found = false;
    uint32_t maxSeq = 0;
    for (size_t i = 0; i < kSlotCount; i++)
    {
        SlotHeader h;
        if (!readHeader(p, i, h)) continue;
        if (h.magic != kMagic || h.len != PHOTO_BODY_BYTES) continue;
        if (!found || h.seq > maxSeq)
        {
            maxSeq = h.seq;
            found = true;
        }
    }

    const uint32_t seq = found ? maxSeq + 1 : 1;
    const size_t slot = seq % kSlotCount;
    const size_t base = slot * kSlotSize;
    const SlotHeader h = {kMagic, seq, PHOTO_BODY_BYTES, frameCrc(black, red)};

    if (esp_partition_erase_range(p, base, kSlotSize) != ESP_OK)
    {
        logf("[STORE] erase slot %u failed\n", (unsigned)slot);
        return false;
    }

    // 头 + 黑 + 红分三次写：省掉一块 5776 字节的临时缓冲，偏移都在 4 字节边界上
    esp_err_t err = esp_partition_write(p, base, &h, sizeof(h));
    if (err == ESP_OK)
        err = esp_partition_write(p, base + kHeader, black, PHOTO_FRAME_BYTES);
    if (err == ESP_OK)
        err = esp_partition_write(p, base + kHeader + PHOTO_FRAME_BYTES, red, PHOTO_FRAME_BYTES);

    if (err != ESP_OK)
    {
        logf("[STORE] write slot %u failed (%d)\n", (unsigned)slot, (int)err);
        return false;
    }
    logf("[STORE] photo #%u stored in slot %u\n", (unsigned)seq, (unsigned)slot);
    return true;
}

bool photoStoreLoad(uint8_t* black, uint8_t* red)
{
    const esp_partition_t* p = dataPart();
    if (!p) return false;

    int best = -1;
    uint32_t bestSeq = 0;
    uint32_t bestCrc = 0;
    for (size_t i = 0; i < kSlotCount; i++)
    {
        SlotHeader h;
        if (!readHeader(p, i, h)) continue;
        if (h.magic != kMagic || h.len != PHOTO_BODY_BYTES) continue;
        if (best < 0 || h.seq > bestSeq)
        {
            best = (int)i;
            bestSeq = h.seq;
            bestCrc = h.crc;
        }
    }
    if (best < 0)
    {
        logLine("[STORE] no photo stored yet");
        return false;
    }

    const size_t base = (size_t)best * kSlotSize;
    if (esp_partition_read(p, base + kHeader, black, PHOTO_FRAME_BYTES) != ESP_OK ||
        esp_partition_read(p, base + kHeader + PHOTO_FRAME_BYTES, red,
                           PHOTO_FRAME_BYTES) != ESP_OK)
    {
        logf("[STORE] read slot %d failed\n", best);
        return false;
    }
    if (frameCrc(black, red) != bestCrc)
    {
        // 断电留下的半截数据：当作没存过，让调用方回落到扫码页
        logf("[STORE] slot %d failed CRC, skipped\n", best);
        return false;
    }
    logf("[STORE] loaded photo #%u from slot %d\n", (unsigned)bestSeq, best);
    return true;
}
