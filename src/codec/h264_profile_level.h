#ifndef H264_PROFILE_LEVEL_H
#define H264_PROFILE_LEVEL_H

#include <QString>
#include <algorithm>
#include <cstdint>

namespace H264ProfileLevel
{
struct LevelLimit
{
    int levelIdc;
    int maxMacroblocksPerFrame;
    int maxMacroblocksPerSecond;
};

inline int macroblocksPerFrame(int width, int height)
{
    const int mbW = std::max(1, (width + 15) / 16);
    const int mbH = std::max(1, (height + 15) / 16);
    return mbW * mbH;
}

inline int levelIdcFor(int width, int height, int fps)
{
    static constexpr LevelLimit kLimits[] = {
        {10, 99, 1485},
        {11, 396, 3000},
        {12, 396, 6000},
        {13, 396, 11880},
        {20, 396, 11880},
        {21, 792, 19800},
        {22, 1620, 20250},
        {30, 1620, 40500},
        {31, 3600, 108000},
        {32, 5120, 216000},
        {40, 8192, 245760},
        {41, 8192, 245760},
        {42, 8704, 522240},
        {50, 22080, 589824},
        {51, 36864, 983040},
        {52, 36864, 2073600},
    };

    const int mbpf = macroblocksPerFrame(width, height);
    const std::int64_t mbps = static_cast<std::int64_t>(mbpf) * std::max(1, fps);
    for (const LevelLimit &limit : kLimits)
    {
        if (mbpf <= limit.maxMacroblocksPerFrame && mbps <= limit.maxMacroblocksPerSecond)
            return limit.levelIdc;
    }
    return 52;
}

inline QString constrainedBaselineProfileLevelId(int width, int height, int fps)
{
    return QStringLiteral("42e0%1").arg(levelIdcFor(width, height, fps), 2, 16, QLatin1Char('0'));
}
} /* namespace H264ProfileLevel */

#endif /* H264_PROFILE_LEVEL_H */
