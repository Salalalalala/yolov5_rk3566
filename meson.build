project('yolov5_rk3566', 'c','cpp',
        version : '0.0.1')

# sysroot 路径（项目约定：仓库根下的 sysroot/，由 utils.sh 的 pullsysroot 拉取）
sys_root = meson.source_root() + '/sysroot'

# 让交叉编译器/链接器用这个 sysroot（头文件查找 + 库链接）。
# Meson 0.53 不会从 cross 文件自动传 --sysroot，必须在这里显式加。
add_project_arguments('--sysroot=' + sys_root, language: ['c', 'cpp'])
add_project_link_arguments('--sysroot=' + sys_root, language: ['c', 'cpp'])

# include 路径：本工程 include/（交叉编译时 sysroot 由 --sysroot 生效）
inc = include_directories('include')

subdir('source')

# 板端 MPP（Rockchip 媒体处理平台）硬件编码库，JPEG 编码走 VPU 硬件
mpp_dep = declare_dependency(link_args: ['-lrockchip_mpp'])

demo1_srcs = [
    'main.c',
    source_srcs,
]

cc = meson.get_compiler('c')

# 板端有 libv4l2，但纯 V4L2 ioctl 只需要内核头 <linux/videodev2.h>，
# 无需链接额外库。这里探测一下（为后续 v4lconvert 预留），
# 找不到也不报错。
libv4l2_dep = cc.find_library('v4l2', required : false)
demo1_deps = []
if libv4l2_dep.found()
    demo1_deps += [libv4l2_dep]
    message('libv4l2 found, linking')
else
    message('libv4l2 not found, using raw V4L2 ioctls only')
endif

executable('demo1', demo1_srcs,
    include_directories : inc,
    dependencies : demo1_deps)

# mjpeg_preview：摄像头 → JPEG(硬件 MPP) → HTTP multipart → VLC 播放
executable('mjpeg_preview', ['source/preview/main.c', mjpeg_srcs],
    include_directories : inc,
    dependencies : [mpp_dep])