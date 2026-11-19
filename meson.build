project('yolov5_rk3566', 'c','cpp',
        version : '0.0.1')

# =============================================================================
# C/C++ 编译选项
# =============================================================================
# sysroot 路径（项目约定：仓库根下的 sysroot/，由 utils.sh 的 pullsysroot 拉取）
cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')
sys_root = meson.source_root() + '/sysroot'

# 让交叉编译器/链接器用这个 sysroot（头文件查找 + 库链接）。
# Meson 0.53 不会从 cross 文件自动传 --sysroot，必须在这里显式加。
add_project_arguments('--sysroot=' + sys_root, language: ['c', 'cpp'])
add_project_link_arguments('--sysroot=' + sys_root, language: ['c', 'cpp'])

# include 路径：本工程 include/（交叉编译时 sysroot 由 --sysroot 生效）
inc = include_directories('include')

# =============================================================================
#                                   源文件
# =============================================================================
subdir('source')


# =============================================================================
#                                   链接库设置
# =============================================================================
# 板端 MPP（Rockchip 媒体处理平台）硬件编码库，JPEG 编码走 VPU 硬件
mpp_dep = declare_dependency(link_args: ['-lrockchip_mpp'])

# NPU 推理运行时（librknnrt，1.5.0）
rknn_dep = declare_dependency(link_args: ['-lrknnrt'])
# 2D 加速（librga，letterbox/颜色转换）
rga_dep  = declare_dependency(link_args: ['-lrga'])

# 后处理用到 expf/powf，需要显式链接 libm（--no-undefined 不会自动带）
math_dep = declare_dependency(link_args: ['-lm'])



# =============================================================================
#                                   输出项设置
# ============================================================================
# mjpeg_preview：摄像头 → (RGA + RKNN 检测画框) → JPEG(硬件 MPP) → HTTP → VLC
executable('mjpeg_preview',
    ['main.c', mjpeg_srcs],
    include_directories : inc,
    dependencies : [mpp_dep, rknn_dep, rga_dep, math_dep])
