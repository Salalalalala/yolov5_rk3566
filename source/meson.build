# source/meson.build
# 先进入各子目录，拿到它们定义的源文件列表
subdir('communication')
subdir('encode')
subdir('detect')

v4l2_srcs  = files('v4l2_capture.c')    # 摄像头采集
frame_srcs = files('frame_process.c')   # 帧回调分发

# mjpeg_preview：采集 + 编码 + 通信
mjpeg_srcs = v4l2_srcs + frame_srcs + encode_srcs + communication_srcs + detect_srcs