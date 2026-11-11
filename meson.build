project('yolov5_rk3566', 'c','cpp',
        version : '0.0.1')

# include 路径：本工程的 include/（交叉编译时 sysroot 由 cross-rk3566.ini 生效）
inc = include_directories('include')

# meson.build
subdir('source')

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