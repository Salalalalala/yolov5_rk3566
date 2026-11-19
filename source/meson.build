# source/meson.build

# 先进入各子目录，拿到它们定义的源文件列表
subdir('communication')
subdir('encode')

v4l2_srcs  = files('v4l2_capture.c')    # 摄像头采集
frame_srcs = files('frame_process.c')   # 帧回调分发

# demo1：采集 + 帧回调 + 通信（TCP 客户端 + MJPEG 服务器）
source_srcs = v4l2_srcs + frame_srcs + internet_fns

# mjpeg_preview：采集 + 编码 + 通信
mjpeg_srcs = v4l2_srcs + encode_srcs + internet_fns

# NPU 检测模块：RGA letterbox + RKNN 推理 + 解码/NMS + 画框
detect_srcs = files('detect/rknn_detector.c')