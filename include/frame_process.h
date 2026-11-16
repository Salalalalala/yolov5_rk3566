#ifndef FRAME_PROCESS_H
#define FRAME_PROCESS_H

#include <stddef.h>
#include <stdint.h>

/*
 * 帧处理回调：采集循环每取到一帧调用一次。
 *   data : 帧数据首地址（NV12，mmap 映射，只读勿越界写）
 *   size : 帧字节数
 *   seq  : 帧序号（V4L2 给的）
 *   user : 注册时透传的用户数据，可放统计结构/模型句柄等
 * 返回 0 继续采集，非 0 停止采集。
 */
typedef int (*frame_callback_t)(const void *data, size_t size,
                                uint32_t seq, void *user);

/* 注册帧处理回调，采集循环开始前调用一次（等价于 signal() 的注册） */
void frame_set_handler(frame_callback_t cb, void *user);

/* 处理一帧：采集循环每取一帧调用，内部转发给已注册的回调。
 * 未注册回调时直接跳过（返回 0）。 */
int frame_process(const void *data, size_t size, uint32_t seq);

#endif /* FRAME_PROCESS_H */
