# yolov5_rk3566

基于 Rockchip RK3566（LubanCat 鲁班猫卡片电脑）的 YOLOv5s 实时目标检测：摄像头采集 → RGA 预处理 → NPU int8 推理 → 解码/NMS → 画框 → MJPEG 预览。

```
V4L2(NV12 640x480) ──RGA letterbox──▶ RGB888 640x640 ──RKNN(NPU)──▶ 3×raw head
                                                                      │
        VLC ◀──HTTP MJPEG◀── MPP 硬件 JPEG ◀── 在 NV12 Y 平面画框 ◀── 解码/NMS
```

## 功能

- 摄像头采集：V4L2 multiplanar，rkisp 输出单 plane NV12
- 预处理：RGA 硬件 letterbox（等比缩放 + 灰边填充，640x640）
- NPU 推理：RKNN int8（librknnrt 1.5.0，RKNPU2）
- 后处理：sigmoid + 网格/anchor 解码、按类 NMS，坐标反映射回原帧
- 画框：直接写 NV12 的 Y 平面，零额外转换开销
- 预览：MPP/VPU 硬件 JPEG + HTTP multipart，VLC 直接播放
- 检测耗时统计：每 30 帧打印目标数与 NPU+RGA 耗时

## 目录结构

```
main.c                  demo1 入口（V4L2 取帧 + 可选 TCP 推流）
include/                公共头文件（v4l2_capture / frame_process / tcp_sender /
                        mjpeg_server / jpeg_encoder / rknn_detector / rknn_api）
source/
  v4l2_capture.c        摄像头采集（rkisp multiplanar NV12）
  frame_process.c       帧处理回调框架
  communication/        TCP 客户端、MJPEG HTTP 服务器
  encode/               jpeg_encoder.c（MPP 硬件 JPEG）
  detect/               rknn_detector.c（RGA + RKNN + 解码/NMS + 画框）
  preview/              mjpeg_preview 入口
export_onnx.py          .pt → 3 个 raw head 的 ONNX
convert_rknn.py         ONNX → RK3566 int8 RKNN（摄像头校准图）
models/                 模型（*.onnx / *.rknn / calib/ 不入库，脚本重新生成）
```

## 环境

- 开发机：Ubuntu 20.04，`aarch64-linux-gnu` 交叉工具链、meson + ninja
- 目标板：LubanCat（RK3566，aarch64），默认用户 `cat`，IP 见 `utils.sh` 的 `lbcip`
- 板端运行时：`librknnrt.so` 1.5.0（RKNPU 驱动 0.8.2）、`librga.so` 2.1.0、`librockchip_mpp`
- 摄像头：`/dev/video0`（rkisp_mainpath，NV12）
- SSH 免密已配置

## 构建与运行

```bash
# 1) 首次：从板子拉取交叉编译 sysroot（约 3.9G）
bash -c 'source utils.sh && pullsysroot'

# 2) 编译并推送到板子 ~/ywj_demo1/
bash -c 'source utils.sh && build'     # target 默认为 mjpeg_preview

# 3) 板子上运行（第三个参数传模型即开启检测）
cd ~/ywj_demo1
./mjpeg_preview /dev/video0 8080 ./yolov5s.rknn

# 4) 宿主机用 VLC 打开
vlc http://192.168.137.200:8080
```

不传模型参数则退化为纯预览。

## 模型转换

模型产物不入库（见 `.gitignore`），需要在本机重新生成：

```bash
# 一次性环境：Python 3.8 + rknn-toolkit2 1.5.0（版本必须与板端 runtime 一致）
python3 -m venv .venv
source .venv/bin/activate
# 安装 rknn-toolkit2 1.5.0（wheel 在 airockchip/rknn-toolkit2 v1.5.0 仓库 packages/ 下）

# 1) 导出 raw-head ONNX（需要 yolov5 源码目录来反序列化 .pt）
python export_onnx.py --repo /path/to/yolov5-7.0

# 2) 采集校准图（板上抓 100 帧 NV12），转成 letterbox 640x640 的 JPG 存 models/calib/
#    并生成 models/calib.txt

# 3) 转 int8 RKNN
python convert_rknn.py          # → models/yolov5s.rknn
```

### 为什么不用官方单输出 ONNX

官方 `export.py` 输出 `(1,25200,85)`，把 box 坐标(0~640)和置信度(0~1)拼在同一张量。RKNN 1.5.0 的输出量化是 per-tensor（`scale` 为标量），坐标会把量化步长撑大，置信度被全部压成 0（板端实测 obj 恒为 0）。

因此 `export_onnx.py` 改为输出 3 个 head 的原始 logits，sigmoid/网格解码在 C 后处理完成。

### 解码公式

```
xy = (sigmoid(logit) * 2 - 0.5 + 网格坐标) * stride
wh = (sigmoid(logit) * 2)^2 * anchor        # anchor 为像素值，不再乘 stride
score = sigmoid(obj) * sigmoid(max_cls)
```

## 性能（RK3566 单核 NPU，默认 600MHz）

| 阶段 | 耗时 |
|---|---|
| rknn_run | 69 ms |
| 输出获取（int8） | 5.6 ms |
| RGA + 解码/NMS | ~35 ms |
| 整帧 | ~110 ms（约 9 fps） |

NPU 最高 900MHz，需要提速可切性能模式（约快 25%）：

```bash
echo performance > /sys/class/devfreq/fde40000.npu/governor
```

## 调试

- `utils.sh` 里 `dbg` 函数可用 gdb-multiarch 连板子 gdbserver 远程调试，详见 `AGENT.md`
- 不依赖摄像头的检测验证：用任意 640x480 NV12 裸帧喂给检测函数并 dump 带框帧（见 `AGENT.md`）

更细的工程约定、踩坑记录和性能分析见 [AGENT.md](AGENT.md)。
