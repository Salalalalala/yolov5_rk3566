# 项目简介

基于 Rockchip RK3566（LubanCat 鲁班猫卡片电脑，arm64）的视觉识别项目：目标是把 YOLOv5s 模型部署到板端运行。

当前进度：
- 已完成摄像头采集（V4L2 multiplanar，NV12）、帧处理回调框架、TCP 推流
- 已完成 MJPEG 预览：摄像头 → 硬件 JPEG（MPP/VPU）→ HTTP multipart → VLC 播放
- 已完成模型转换：yolov5s.pt → raw-head ONNX → int8 RKNN（models/yolov5s.rknn），
  校准图来自摄像头截图，板端 NPU 实测推理正常（obj 分数合理）
- 已完成检测集成：RGA letterbox(NV12→RGB888 640x640) → RKNN 推理 → 3 head 解码/NMS
  → 在 NV12 上画框，接在 mjpeg_preview 的编码前，VLC 可看到带框画面
- 待做：性能优化（当前 rknn_run 69ms@600MHz，整帧约 110ms）、画面内文字标签

# 目录结构

    AGENT.md              本文件，工程说明
    README.md             标题占位（yolov5_rk3566）
    main.c                demo1 入口：V4L2 取帧 + 可选 TCP 推流
    meson.build           构建定义：两个可执行 demo1、mjpeg_preview；--sysroot 接线
    cross-rk3566.ini      Meson 交叉编译配置（aarch64-linux-gnu 工具链）
    utils.sh              封装 shell 函数（构建/传板/拉库/远程调试）
    include/              头文件：v4l2_capture / frame_process / tcp_sender / mjpeg_server /
                          jpeg_encoder / rknn_detector / rknn_api（1.5.0 vendor 头）
    source/
        v4l2_capture.c     摄像头采集（rkisp multiplanar NV12）
        frame_process.c    帧处理回调框架
        communication/     tcp_sender.c（TCP 客户端）、mjpeg_server.c（MJPEG HTTP 服务器）
        encode/            jpeg_encoder.c（MPP 硬件 JPEG 编码）
        detect/            rknn_detector.c（RGA letterbox + RKNN + 解码/NMS + 画框）
        preview/           main.c（mjpeg_preview 入口，可选传入 .rknn 开启检测）
    models/               模型文件：yolov5s.pt、yolov5s.onnx（标准单输出，仅参考）、
                          yolov5s_rawhead.onnx（部署用，3 个 head 原始输出）、yolov5s.rknn（int8）、
                          calib/（摄像头校准图 100 张，letterbox 640x640）+ calib.txt
    export_onnx.py        .pt → raw-head ONNX（改 Detect 输出，见文件头注释）
    convert_rknn.py       raw-head ONNX → int8 RKNN（RK3566，摄像头校准图）
    .venv/                Python 3.8 虚拟环境（rknn-toolkit2 1.5.0 + torch，git 忽略）
    build_arm64/          Meson/Ninja 构建输出（git 忽略）
    sysroot/              板端拉取的 sysroot（git 忽略，约 3.9G）
    .vscode/              VSCode cppdbg 远程调试配置

# 两个可执行

    demo1          V4L2 取帧演示：./demo1 [/dev/video0] [ip port]
                   （给了 ip port 就把每帧经 TCP 发到该地址，带 4 字节长度前缀）
    mjpeg_preview  MJPEG 预览：./mjpeg_preview [/dev/video0] [port] [model.rknn]
                   给了模型就叠加检测框；VLC 打开 http://<板子IP>:<port> 看实时画面

# 环境约定

- 开发机：VMware 虚拟机（Ubuntu 20.04），用户 jaja
- 目标板：LubanCat（RK3566, aarch64），用户 cat，IP 192.168.137.200（见 utils.sh 的 lbcip）
- 摄像头：/dev/video0 = rkisp_mainpath（MIPI CSI 摄像头经 ISP 处理后的输出），
  multiplanar 采集设备，输出单 plane NV12（非 YUYV）
- SSH 免密已配置（cat@192.168.137.200）

# 构建与部署（utils.sh 函数）

    build        首次 meson setup build_arm64 --cross-file cross-rk3566.ini，然后 ninja，最后 sendsw
    sendsw       rsync 把 build_arm64/$target 推到板子 ~/ywj_demo1/
    pullbd       scp 从板子拉文件：pullbd <远程路径> <本地路径>
    pullsysroot  从板子拉取完整 sysroot 到 ./sysroot
    dbg          gdb-multiarch 远程调试，连板子 gdbserver_port 端口

使用方式（仓库根目录）：

    bash -c 'source utils.sh && build'

注意：utils.sh 函数不会被自动加载，直接 ./utils.sh 不执行任何东西；target 变量目前是 mjpeg_preview（demo1 被注释掉）。

# 远程调试

1. 板上启动：gdbserver :12345 ./<target>
2. 本地执行：bash -c 'source utils.sh && dbg'（或用 .vscode/launch.json 的 cppdbg 配置）

# 注意事项（给 AI 助手/后续开发）

- sysroot 接线：Meson 0.53 不会从 cross 文件自动传 --sysroot（cross-rk3566.ini 的
  sys_root 键只供 pkg-config 查找用），必须在 meson.build 里用
  add_project_arguments / add_project_link_arguments 显式加 --sysroot。
  改动 cross 文件后必须 rm -rf build_arm64 重新 setup。
- 摄像头是 multiplanar 设备（V4L2_CAP_VIDEO_CAPTURE_MPLANE），v4l2_capture.c 全程用
  _MPLANE 结构（fmt.pix_mp / v4l2_plane / V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE）。
  rkisp 输出单 plane NV12，这是 RGA/RKNN 的原生输入格式，后面无需转 RGB。
- JPEG 编码走 MPP 硬件（librockchip_mpp，板端装了 -dev 包，头文件在
  sysroot/usr/include/rockchip/）。两个坑：①同步接口 mpi->encode 对 JPEG 返回
  NULL 包，必须用 encode_put_frame + encode_get_packet；②包含 MPP 头文件前要先
  #define MODULE_TAG。配置用字符串键（mpp_enc_cfg_set_s32 "prep:width" 等）。
- 板子上 /lib/aarch64-linux-gnu/qt-default 有失效软链接，rsync 需 --exclude='qt-default'。
- build_arm64/ 与 sysroot/ 已 gitignore，勿提交。
- 模型转换链路必须用 rknn-toolkit2 **1.5.0**（PyPI 只有 2.x；板端 librknnrt 1.5.0 +
  RKNPU 驱动 0.8.2，2.x 要求更新驱动）。工具装在仓库根的 .venv（py3.8），
  轮子来自 GitHub airockchip/rknn-toolkit2 v1.5.0 的 packages/ 目录。
- int8 量化的坑：官方 export.py 的单输出 (1,25200,85) 把 box 坐标和置信度拼在一个
  张量，RKNN 输出按单 scale 量化（rknn_tensor_attr.scale 是标量），置信度会被全部
  压成 0（板端实测 obj 恒为 0）。所以用 export_onnx.py 导出 3 个 head 原始 logits，
  sigmoid + 网格/anchor 解码放到 C 后处理；换算：
  xy=(sigmoid*2-0.5+grid)*stride, wh=(sigmoid*2)^2*anchor（anchor 用像素值，不乘 stride，见下）。
- 校准图：板上 v4l2-ctl 抓 100 帧 NV12（640x480），本机 NV12→BGR 后 letterbox 到
  640x640 存 models/calib/。画面内容要覆盖实际场景，纯墙面会导致量化退化。
- 板端验证模型：装了 python3 rknn-toolkit-lite2 1.5.0 + numpy 1.19.5（离线 wheel
  在 /tmp/armwheels_new），可直接 load_rknn 跑 NPU；C 侧头文件在 include/rknn_api.h，
  sysroot 里有 librknnrt.so，链接 -lrknnrt。
- 解码锚点坑：这个 checkpoint 的 Detect.anchors 存的是 grid 单位（1.25/1.625…），
  模型内 anchor_grid = anchors*stride。所以 C 后处理用像素 anchor 表时，
  wh = (2*sigmoid)^2 * anchor_px，**不要再乘 stride**（否则框会大 8/16/32 倍，铺满全屏）。
  xy 仍然是 (2*sigmoid - 0.5 + cell) * stride。
- 性能（RK3566 单核 NPU，rknpu_ondemand）：rknn_run 69ms@600MHz、输出获取 5.6ms、
  RGA+解码约 35ms，整帧约 110ms（约 9fps）。NPU 最高 900MHz，需要提速可
  `echo performance > /sys/class/devfreq/fde40000.npu/governor`（约 -25%）。
  解码已按"通道优先"重排（否则 80 次跨 6.4KB 的 cache miss 会让单帧多 50ms+），
  输出用 want_float=0 手工按 scale/zp 反量化，省掉每帧 8.5MB 的 int8→float。
- 画框直接改 NV12 的 Y 平面（白色框，只写亮度），不额外做 RGB 叠加；
  yolo_draw_nv12 与检测分离，demo1 等其他程序也能复用。
- 验证方法：板端 /tmp/det_test 可加载任意 640x480 NV12 裸帧跑一次检测并 dump
  带框帧（`det_test model.rknn in.nv12 out.nv12`），不依赖摄像头。
- 旧的 scripts/（export.py、run.sh）是废弃结构，新结构以根目录为准。
