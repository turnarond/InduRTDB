"""InduRTDB 运行时诊断工具包（巡检 / 冒烟 / 泄漏检测）。

不依赖 C 库，纯 Python 复刻共享内存 v2 Header 布局（见 _layout），在线读取
/dev/shm 段进行健康检查。仅依赖标准库，可独立运行，可接入 CI。
"""

__version__ = "0.1.0"
