# Source attribution

ASAP observation construction, default joint configurations and policy-switch timing were adapted into C++ from the RoboJuDo/Anakin Python research source, copyright (c) 2025 HansZ8, at reference commit `4d58df1bae9fe3358b3d92353e23a579f31a7a62`. Its supplied CC BY 4.0 notice is preserved in LICENSE-Anakin; license link: https://creativecommons.org/licenses/by/4.0/ . Changes include C++/ONNX Runtime implementations, independent per-model contexts, explicit ROS2 epoch transactions, local execution gates and bounded launch cleanup. The original Python source and model assets are not included in this repository.

New interface/runtime implementation is supplied with the Apache-2.0 notice in LICENSE. Official ROS packages, Unitree SDK2 and ONNX Runtime retain their own licenses and are separate dependencies. zdev was an architectural reference only; no zdev implementation was copied or executed.
