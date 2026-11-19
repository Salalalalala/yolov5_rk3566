#!/usr/bin/env python3
"""yolov5s_rawhead.onnx -> yolov5s.rknn（RK3566，int8 量化）

输入必须是 export_onnx.py 导出的 raw-head 模型（3 个输出）。官方单输出
(1,25200,85) 的 ONNX 在 int8 下置信度会被量化成 0，见 export_onnx.py 注释。

用法（仓库根目录）：
    source .venv/bin/activate
    python convert_rknn.py [--onnx models/yolov5s_rawhead.onnx] [--dataset models/calib.txt]

校准图：models/calib/*.jpg（摄像头 NV12 截图，已 letterbox 到 640x640）
"""
import argparse
import sys

from rknn.api import RKNN


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--onnx", default="models/yolov5s_rawhead.onnx")
    ap.add_argument("--dataset", default="models/calib.txt")
    ap.add_argument("--output", default="models/yolov5s.rknn")
    ap.add_argument("--no-quant", action="store_true", help="关闭量化（fp16）")
    args = ap.parse_args()

    rknn = RKNN(verbose=False)

    # YOLOv5 预处理：RGB / 255。量化用板端摄像头画面，非对称 int8 + 按通道量化。
    if rknn.config(
        mean_values=[[0, 0, 0]],
        std_values=[[255, 255, 255]],
        target_platform="rk3566",
        quantized_dtype="asymmetric_quantized-8",
        quantized_algorithm="normal",
        quantized_method="channel",
        optimization_level=3,
    ) != 0:
        print("rknn.config failed")
        return 1

    if rknn.load_onnx(model=args.onnx) != 0:
        print("load_onnx failed")
        return 1

    if rknn.build(do_quantization=not args.no_quant, dataset=args.dataset) != 0:
        print("build failed")
        return 1

    if rknn.export_rknn(args.output) != 0:
        print("export_rknn failed")
        return 1

    rknn.release()
    print("saved", args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
