#!/usr/bin/env python3
"""yolov5 *.pt -> raw-head ONNX（三个 head 的原始卷积输出，不做 sigmoid/decode）

为什么要改导出：
  官方 export.py 的输出是 (1,25200,85)，把 box 坐标(0~640)和置信度(0~1)拼在
  同一个张量里。RKNN int8 对输出按单个 scale 量化（rknn_tensor_attr.scale 是
  标量），坐标把 scale 撑大后置信度会全部量化为 0（实测 NPU 上 obj 全 0）。
  改成输出 3 个 head 原始 logits（1,255,80,80 / 1,255,40,40 / 1,255,20,20），
  取值范围一致，int8 正常；sigmoid/网格解码放到后处理里做。

用法：
    source .venv/bin/activate
    python export_onnx.py --repo /tmp/yolov5-7.0     # yolov5 源码目录（解包 .pt 需要）
"""
import argparse
import sys
import types

import torch


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default="/tmp/yolov5-7.0", help="yolov5 源码目录")
    ap.add_argument("--weights", default="models/yolov5s.pt")
    ap.add_argument("--output", default="models/yolov5s_rawhead.onnx")
    ap.add_argument("--imgsz", type=int, default=640)
    args = ap.parse_args()

    sys.path.insert(0, args.repo)
    from models.yolo import Detect

    model = torch.load(args.weights, map_location="cpu")["model"].float().eval().fuse()
    d = model.model[-1]
    assert isinstance(d, Detect), "只支持 YOLOv5 Detect 头"

    def fwd_raw(self, x):
        for i in range(self.nl):
            x[i] = self.m[i](x[i])      # (bs, 3*85, ny, nx)
        return tuple(x)

    d.forward = types.MethodType(fwd_raw, d)
    torch.onnx.export(model, torch.zeros(1, 3, args.imgsz, args.imgsz), args.output,
                      opset_version=12, input_names=["images"],
                      output_names=["head80", "head40", "head20"],
                      do_constant_folding=True)
    print("saved", args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
