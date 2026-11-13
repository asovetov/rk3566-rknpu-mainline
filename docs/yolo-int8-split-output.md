# YOLO INT8 on RKNPU2: split the head output into two tensors

## Symptom

A YOLOv8/YOLO11 model converted with `do_quantization=True` runs fast
(`yolov8n` @640: **77–81 ms/frame** on RK3566) but produces **zero detections**:
the confidence channel of the `[1, 5, 8400]` output is `0.000` for every anchor,
while the four box channels look sane (values up to ~640 px). The same weights as
`fp16` detect normally (conf up to 0.5), so the model is fine.

## Why

RKNPU2 quantizes each *tensor* with one scale. `[1, 5, 8400]` mixes box
coordinates (0…640) with class scores (0…1): the scale is set by the boxes, and
every score rounds to zero.

What does **not** help (measured):

- `quantized_algorithm='mmse'`;
- `quantized_method='channel'`;
- more/better calibration images.

## Fix: two output tensors

Split the concatenated head output in the ONNX graph, so boxes and scores get
independent scales:

```python
# scripts/split_output.py
Slice(output0, starts=[0], ends=[4], axes=[1]) -> "boxes"   # [1, 4, 8400]
Slice(output0, starts=[4], ends=[5], axes=[1]) -> "scores"  # [1, 1, 8400]
```

Then convert as usual. Result on RK3566 (yolov8n @640, ROI-crop tracking of a
10–15 px drone in 1080p60):

| variant | NPU ms/frame | FPS | frames with target | max conf |
|---|---|---|---|---|
| int8, single output | 81 | 12.3 | 0 % | 0.000 |
| int8, **split output** | 77 | 12.9 | 49 % | 0.50 |
| fp16, single output | 291 | 3.4 | 39 % | 0.51 |

End-to-end (h264 decode + crop + infer, `ffmpeg | C tracker`): **10.4 FPS** with
the split int8 model versus 2.7 FPS on fp16.

## Reading the two outputs in C

`scripts/drone_track_roi.c` shows the pattern: take the argmax over the scores
tensor, then read `boxes[0..3][k]` for that anchor (`[1, 4, N]`, channel-major).
Coordinates are in the model's input space; map them back to frame coordinates by
adding the crop origin.
