"""Разделить выход YOLO [1,5,8400] на боксы [1,4,8400] и счёты [1,1,8400].

Зачем: в RKNN int8 один тензор получает один масштаб квантизации, и боксы
(значения до ~640) обнуляют вероятности (0…1). Двумя тензорами масштабы
считаются независимо, и вероятности выживают.
"""
import pathlib

import onnx
from onnx import TensorProto, helper

SRC = pathlib.Path("/tmp/split_src.txt").read_text().strip()
DST = "/tmp/best_split.onnx"

m = onnx.load(SRC)
g = m.graph
out_name = g.output[0].name
print("было:", [(o.name, [d.dim_value for d in o.type.tensor_type.shape.dim]) for o in g.output])

boxes_name, scores_name = "boxes", "scores"
nodes = [
    helper.make_node("Slice", [out_name, "slice_b_start", "slice_b_end", "slice_axis"],
                     [boxes_name], name="SliceBoxes"),
    helper.make_node("Slice", [out_name, "slice_s_start", "slice_s_end", "slice_axis"],
                     [scores_name], name="SliceScores"),
]
inits = [
    helper.make_tensor("slice_b_start", TensorProto.INT64, [1], [0]),
    helper.make_tensor("slice_b_end", TensorProto.INT64, [1], [4]),
    helper.make_tensor("slice_s_start", TensorProto.INT64, [1], [4]),
    helper.make_tensor("slice_s_end", TensorProto.INT64, [1], [5]),
    helper.make_tensor("slice_axis", TensorProto.INT64, [1], [1]),
]
g.node.extend(nodes)
g.initializer.extend(inits)

# 8400 = число якорей: берём из формы исходного выхода
dims = [d.dim_value for d in g.output[0].type.tensor_type.shape.dim]
n = dims[-1]

g.output.remove(g.output[0])
for name, ch in ((boxes_name, 4), (scores_name, 1)):
    g.output.append(helper.make_tensor_value_info(name, TensorProto.FLOAT, [1, ch, n]))

onnx.checker.check_model(m)
onnx.save(m, DST)
print("стало:", [(o.name, [d.dim_value for d in o.type.tensor_type.shape.dim]) for o in g.output])
print("сохранено:", DST)
