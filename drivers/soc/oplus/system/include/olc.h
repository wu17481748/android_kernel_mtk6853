#ifndef _LIBOLC_H_
#define _LIBOLC_H_

#include <linux/types.h>

#define LOG_KERNEL (0x1)
#define LOG_SYSTEM (0x1<<1)
#define LOG_MAIN   (0x1<<2)
#define LOG_EVENTS (0x1<<3)
#define LOG_RADIO  (0x1<<4)

#define EXP_LEVEL_CRITICAL  1
#define EXP_LEVEL_IMPORTANT 2
#define EXP_LEVEL_GENERAL   3
#define EXP_LEVEL_INFO      4
#define EXP_LEVEL_DEBUG     5

enum exception_type {
    EXCEPTION_KERNEL,
    EXCEPTION_NATIVE,
    EXCEPTION_FRAMEWROK,
    EXCEPTION_APP,
};

struct exception_info {
    uint64_t time;              // 异常发生时间
    uint32_t id;                // 异常编号，用于标识异常检测点
    uint32_t pid;               // 异常来源
    uint32_t exceptionType;     // 异常类型, enum exception_type
    uint32_t faultLevel;        // 异常严重等级
    uint64_t logOption;         // 异常日志收集选项，LOG_KERNEL/LOG_SYSTEM/LOG_MAIN...
    char module[128];           // 模块名称
    char logPath[256];          // 模块私有日志路径
    char summary[256];          // 异常概要说明
};


int olc_raise_exception(struct exception_info *exp);

#endif