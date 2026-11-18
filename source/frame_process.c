/* frame_process.c —— 帧处理模块（回调注册 + 分发）
 *
 * 把"拿到一帧后做什么"从采集循环里抽出来单独成文件，好处：
 *   - 采集循环（main.c）只管取帧/还帧，不关心业务逻辑
 *   - 以后 RGA letterbox → RKNN 推理 → 画框 都加在回调里
 *   - 想换处理逻辑（比如换模型）只需换回调，不动采集代码
 */
#include "frame_process.h"

/* 当前注册的回调；未注册时为空，帧会被跳过 */
static frame_callback_t g_cb   = NULL;
static void *usr_ctx = NULL;

void frame_set_handler(frame_callback_t cb, void *user)
{
    g_cb   = cb;
    usr_ctx = user;
}

int frame_process(const void *data, size_t size, uint32_t seq)
{
    if (!g_cb)
        return 0;                 /* 没注册回调：跳过这帧 */
    return g_cb(data, size, seq, usr_ctx);
}
