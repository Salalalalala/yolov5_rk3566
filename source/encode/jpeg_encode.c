/* jpeg_encode.c —— NV12 → JPEG 编码（基于 libjpeg-turbo）
 *
 * 设计要点:
 *   - libjpeg 的 jpeg_write_raw_data 吃的是 I420（Y、U、V 三个独立平面），
 *     而摄像头给的是 NV12（Y 平面 + 交错 UV 平面）。所以先做一步
 *     "NV12 → I420" 的去交错转换（U、V 各抽一半字节），几乎零成本。
 *   - 高度补边到 16 的倍数：JPEG 按 16x16 的 MCU 块编码，每次必须写满
 *     16 行，底部不足的用最后一行填充，libjpeg 只编码真实高度。
 *   - 用 setjmp 错误处理器：libjpeg 默认出错直接 exit()，这里改成
 *     长跳转回来返回 -1，避免一个编码错误干掉整个进程。
 */
#include "jpeg_encode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#include <jpeglib.h>

/* 自定义错误处理器：把 libjpeg 的"出错即 exit"改成"长跳转回编码函数" */
struct jpeg_err_mgr {
    struct jpeg_error_mgr pub;   /* 必须放在第一个成员，libjpeg 要求 */
    jmp_buf jmp;
};

static void jpeg_error_exit(j_common_ptr cinfo)
{
    struct jpeg_err_mgr *mgr = (struct jpeg_err_mgr *)cinfo->err;
    (*cinfo->err->output_message)(cinfo);   /* 打印错误信息 */
    longjmp(mgr->jmp, 1);                    /* 跳回 setjmp 处 */
}

int jpeg_encode_nv12(const uint8_t *nv12, int w, int h, int quality,
                     uint8_t **jpeg_out, size_t *jpeg_len)
{
    if (!nv12 || !jpeg_out || !jpeg_len || w <= 0 || h <= 0)
        return -1;
    if (quality < 1)   quality = 1;
    if (quality > 100) quality = 100;

    /* 1. 高度向上取整到 16 的倍数（JPEG MCU 行高 = 16） */
    int padded_h = ((h + 15) / 16) * 16;

    /* I420 布局: Y = w*padded_h，U = (w/2)*(padded_h/2)，V 同 U */
    size_t y_size  = (size_t)w * padded_h;
    size_t uv_size = (size_t)(w / 2) * (padded_h / 2);
    uint8_t *i420 = malloc(y_size + uv_size * 2);
    if (!i420) {
        fprintf(stderr, "[jpeg] malloc I420 失败\n");
        return -1;
    }
    uint8_t *y_plane = i420;
    uint8_t *u_plane = i420 + y_size;
    uint8_t *v_plane = i420 + y_size + uv_size;

    /* 2. NV12 → I420：Y 平面整块拷，UV 交错平面拆成 U、V 两个平面 */
    const uint8_t *y_src  = nv12;
    const uint8_t *uv_src = nv12 + (size_t)w * h;
    for (int r = 0; r < h; r++) {
        memcpy(y_plane + (size_t)r * w, y_src + (size_t)r * w, (size_t)w);
        if ((r & 1) == 0) {          /* 每 2 行 Y 对应 1 行色度 */
            int cr = r / 2;
            for (int c = 0; c < w / 2; c++) {
                u_plane[(size_t)cr * (w/2) + c] = uv_src[(size_t)cr * w + 2*c];
                v_plane[(size_t)cr * (w/2) + c] = uv_src[(size_t)cr * w + 2*c + 1];
            }
        }
    }
    /* 3. 底部补边：最后一行复制到 padding 区域（Y、U、V 都要） */
    for (int r = h; r < padded_h; r++)
        memcpy(y_plane + (size_t)r * w, y_plane + (size_t)(h - 1) * w, (size_t)w);
    for (int r = h / 2; r < padded_h / 2; r++) {
        memcpy(u_plane + (size_t)r * (w/2), u_plane + (size_t)(h/2 - 1) * (w/2), (size_t)(w/2));
        memcpy(v_plane + (size_t)r * (w/2), v_plane + (size_t)(h/2 - 1) * (w/2), (size_t)(w/2));
    }

    /* 4. libjpeg 压缩到内存缓冲区 */
    struct jpeg_compress_struct cinfo;
    struct jpeg_err_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;

    unsigned char *outbuf = NULL;
    unsigned long  outsize = 0;

    if (setjmp(jerr.jmp)) {
        /* 编码中途出错会 longjmp 到这里 */
        jpeg_destroy_compress(&cinfo);
        free(i420);
        free(outbuf);
        return -1;
    }

    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &outbuf, &outsize);   /* 输出到内存，自动增长 */

    cinfo.image_width      = w;
    cinfo.image_height     = h;
    cinfo.input_components = 3;
    cinfo.in_color_space   = JCS_YCbCr;
    jpeg_set_defaults(&cinfo);                  /* YCbCr 默认 4:2:0 采样 */
    jpeg_set_quality(&cinfo, quality, TRUE);
    cinfo.raw_data_in = TRUE;                   /* 告诉它：我喂的是平面数据 */

    jpeg_start_compress(&cinfo, TRUE);

    /* 每次写 16 行（一个 MCU 行）；Y 16 行，U/V 各 8 行 */
    while (cinfo.next_scanline < (JDIMENSION)h) {
        int start = (int)cinfo.next_scanline;
        JSAMPROW y_rows[16], u_rows[8], v_rows[8];
        for (int i = 0; i < 16; i++)
            y_rows[i] = (JSAMPROW)(y_plane + (size_t)(start + i) * w);
        for (int i = 0; i < 8; i++) {
            u_rows[i] = (JSAMPROW)(u_plane + (size_t)(start / 2 + i) * (w / 2));
            v_rows[i] = (JSAMPROW)(v_plane + (size_t)(start / 2 + i) * (w / 2));
        }
        JSAMPARRAY planes[3] = { y_rows, u_rows, v_rows };
        jpeg_write_raw_data(&cinfo, planes, 16);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    free(i420);

    *jpeg_out = outbuf;
    *jpeg_len = (size_t)outsize;
    return 0;
}
