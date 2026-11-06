Dock v1.12 定制补丁 —— 让"停稳唤出 / 无操作收回"变成配置项
============================================================

上游：https://github.com/TheWay1LovedU/windows-dock （MIT）
基线：dock.rc 声明 1.12.0.0，与你机器上 D:\Dock\Dock.exe 的版本一致。

为什么走这条路
--------------
上游源码把两个时间写成 constexpr 常量：

  src/main.cpp:99   constexpr DWORD kRevealDwellMs  = 450;   // 需在边缘停稳多久才浮出
  src/main.cpp:104  constexpr DWORD kManualRevealMs = 3000;  // 无操作多久自动收起

外部接管（之前的 DockEdge）只能瞬移窗口，必然跳过它的动画管线
（SetAutoHidden 里：opacityTarget = 0 → 滑出工作区 → 动画收尾才 SW_HIDE），
所以动画丢失。改源码则完整保留动画，只是把两个数字换成配置值。

改了什么（4 处，共 +10 行）
---------------------------
1. Settings 结构新增两个字段，默认值仍取原常量：
       int revealDwellMs = (int)kRevealDwellMs;
       int retractMs     = (int)kManualRevealMs;

2. LoadSettings() 解析两个新键，并做范围钳制
   （revealDwellMs: 80–5000；retractMs: 300–20000，越界回落默认）。

3. SaveSettings() 写回这两个键，所以运行后 ini 自动带上它们。

4. 三处使用点改为读配置：
   - UpdateReveal() 的停稳判断：`now - g.edgeEnterTick >= (DWORD)s.revealDwellMs`
   - RefreshRevealDeadline()：`GetTickCount() + (DWORD)s.retractMs`
   - UpdateSmartHide() 的续期：`now + (DWORD)s.retractMs`

5. 附带一处精度修正：边缘热区检测的定时器间隔原本固定 100ms，
   对 300ms 这种设定太粗糙，改为 `min(100, max(20, revealDwellMs / 4))`。

新增配置项（dock-settings.ini）
------------------------------
  revealDwellMs=300       停稳多久才浮出
  retractMs=1500          鼠标离开 Dock 后多久收回

不写这两行 = 保持上游默认 450 / 3000，行为与原版完全一致。

三种落地方式
------------
A. GitHub Actions 云端编译（推荐，本机不用装任何东西）
   1) 在 GitHub 新建仓库（或 Fork 上游）
   2) 把本目录全部文件推上去，推到 main 分支
   3) Actions 里 build 工作流会自动跑（.github/workflows/build.yml），
      用 MSVC + Ninja，产物 Dock-portable.zip 在 run 页面的 Artifacts 里
   4) 解压得到 Dock.exe，替换 D:\Dock\Dock.exe
   注意：Fork 首次推送后可能需要在 Settings → Actions 里允许工作流运行。

B. 本地编译（需要管理员装 VS Build Tools）
   winget install --id Microsoft.VisualStudio.2022.BuildTools --override "--quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
   然后：cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
         cmake --build build
   产物 build\Dock.exe

C. 二进制补丁（不推荐，但可行，不需要任何工具链）
   - kRevealDwellMs 的 450 在 .text 里唯一命中：cmp r/m32,450 @ RVA 0x1400124A3
   - kManualRevealMs 的 3000 有 5 处 `add eax,3000`（加法，不是比较，所以
     按 cmp 搜索找不到），需要逐个验证属于哪条路径
   缺点：只能改成固定值，改一次打一次补丁，不可配置。

部署步骤
--------
1. 托盘 → 退出（先停掉 Dock）
2. 备份：把 D:\Dock\Dock.exe 复制为 Dock.exe.v1.12.orig
3. 用新编出的 Dock.exe 覆盖
4. 在 D:\Dock\dock-settings.ini 追加：
       revealDwellMs=300
       retractMs=1500
5. 启动 Dock.exe

回滚：删掉新 exe，把 Dock.exe.v1.12.orig 改回 Dock.exe 即可（ini 多出的两行会被旧版忽略）。
