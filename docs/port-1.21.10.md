# 移植到 Minecraft 1.21.10

本文记录把 1.20.1 客户端代理移植到 1.21.10（协议 773，NeoForge 21.10.x）的全部改动、依据和验证状态。
所有结论都来自权威数据，而不是猜测：

- 官方映射 `client.txt`（类名/成员名与签名）——用 `tools/mcport/mcport.py` 查询。
- MCPConfig `joined.tsrg`（SRG 名）。
- 客户端 jar 字节码 `javap -p -c`（构造函数形状、handler 名、调用顺序）。
- NeoForge 21.10.64 universal jar 字节码（确认运行时用 **Mojang 官方名**，不再用 SRG 成员名）。

## 1. 运行环境结论

| 项 | 1.20.1 | 1.21.10 |
| --- | --- | --- |
| Java | 17 | 21 |
| 协议版本 | 763 | 773 |
| 运行时类名 | Mojang 名 | Mojang 名 |
| 运行时成员名 | SRG `m_xxxxx_` / `f_xxxxx_` | **Mojang 名**（SRG 仅用于二进制兼容） |
| 连接阶段 | LOGIN → PLAY | LOGIN → **CONFIGURATION** → PLAY |
| 协议状态存储 | `Connection` 上的 `protocol` 属性 + `setProtocol` | 每个状态的 `ProtocolInfo` 对象，替换 `PacketEncoder`/`PacketDecoder` |

因此 `FindMappedMethod(名, SRG 名, 描述符)` 在 1.21.10 上第一跳就命中；SRG 回退只为兼容 1.20.1 保留，按描述符查找（`findMethodByDescriptor`）在两个版本都可用。

## 2. 类与包的变化（编译器不会告诉你，运行时会炸）

| 1.20.1 | 1.21.10 |
| --- | --- |
| `protocol.login.ClientboundGameProfilePacket` | `protocol.login.ClientboundLoginFinishedPacket` |
| `protocol.game.ClientboundCustomPayloadPacket` | `protocol.common.ClientboundCustomPayloadPacket` |
| `protocol.game.ServerboundKeepAlivePacket` | `protocol.common.ServerboundKeepAlivePacket` |
| `protocol.game.ServerboundCustomPayloadPacket` | `protocol.common.ServerboundCustomPayloadPacket` |
| `protocol.status.ServerboundPingRequestPacket` | `protocol.ping.ServerboundPingRequestPacket` |
| `protocol.status.ClientboundPongResponsePacket` | `protocol.ping.ClientboundPongResponsePacket` |
| `protocol.game.ClientboundAddPlayerPacket` | 删除；玩家也走 `ClientboundAddEntityPacket` |
| `Handshake.ClientIntentionPacket.intention : ConnectionProtocol` | `...intention : ClientIntent` |

`native/server/bindings.cpp` 对以上每个名字都先试新名再退回旧名，因此同一个 DLL 在两个版本上都能装载。

## 3. 网络层：从 ConnectionProtocol 属性到 ProtocolInfo

1.21.9 之后 `configureSerialization` 变成 `(ChannelPipeline, PacketFlow, boolean memoryOnly, BandwidthDebugMonitor)`（**static**），
它只装上框架编解码器（`splitter`/`prepender`）和两个占位 handler `decoder`/`encoder`
（`UnconfiguredPipelineHandler$Inbound/Outbound`）。真正的编解码器要由状态切换时替换：

```text
handshaking   -> decoder = HandshakeProtocols.SERVERBOUND
status        -> decoder = StatusProtocols.SERVERBOUND,     encoder = StatusProtocols.CLIENTBOUND
login         -> decoder = LoginProtocols.SERVERBOUND,      encoder = LoginProtocols.CLIENTBOUND
configuration -> decoder = ConfigurationProtocols.SERVERBOUND, encoder = ConfigurationProtocols.CLIENTBOUND
play          -> GameProtocols.SERVERBOUND_TEMPLATE.bind(RegistryFriendlyByteBuf.decorator(A 的 RegistryAccess), Context)
                 GameProtocols.CLIENTBOUND_TEMPLATE.bind(RegistryFriendlyByteBuf.decorator(A 的 RegistryAccess))
```

- 新路径由 `native/server/configuration.cpp` 的 `applyProtocolState()` 实现：新建 `PacketEncoder`/`PacketDecoder`
  并 `pipeline.replace("decoder"/"encoder", ...)`，找不到时退回 `inbound_config`/`outbound_config`。
- 旧路径（1.20.1）仍走 `setProtocolState()`：设置 `Connection.protocol` 属性并调用 handler 的 `setProtocol`。
- `UnboundProtocol.bind` 的第二个参数是 `GameProtocols$Context`（单方法接口 `hasInfiniteMaterials()`）。
  代理在运行时用 `ClassBuilder` 生成实现（`classfile.h` 新增 `addInterface`），取值与官方客户端一致（true）。

## 4. 配置阶段交接

官方客户端在 `ClientboundLoginFinishedPacket` 之后才发送 `ServerboundLoginAcknowledgedPacket`，
**服务端必须等到这个确认再切换编解码器**，否则确认包会被当成配置包解码而报错。代理照做：

```text
B 发 ServerboundHelloPacket
  -> 代理回 ClientboundLoginFinishedPacket（伪造 GameProfile）
  -> 状态 Configuring，编解码器保持 LOGIN
B 发 ServerboundLoginAcknowledgedPacket
  -> 切到 CONFIGURATION
     · 中途接入（A 已在游戏内）：用 A 的 RegistryAccess 重建配置阶段
     · A 尚未入服：镜像 A 自己的配置流（放行主线程门，让 A 去连接）
B 发 ServerboundFinishConfigurationPacket
  -> 切到 PLAY，随后进入快照交接 / 直接转发 A 的 PLAY 流
```

「重建配置阶段」与官方服务端一致，使用官方助手而不是手写编码：

```text
ops       = A.registryAccess().createSerializationContext(NbtOps.INSTANCE)
consumer  = 代理生成的 java.util.function.BiConsumer（每个注册表一次回调）
RegistrySynchronization.packRegistries(ops, A.registryAccess(), Set.of(), consumer)
  -> consumer.accept(key, list) => new ClientboundRegistryDataPacket(key, list) => 写给 B
ClientboundUpdateTagsPacket(TagNetworkSerialization.serializeToNetwork(registry) ...)
ClientboundUpdateEnabledFeaturesPacket(FeatureFlags.REGISTRY.toNames(A.enabledFeatures()))
ClientboundFinishConfigurationPacket
```

`ClientboundCodeOfConductPacket`、`ClientboundSelectKnownPacks` 对客户端都是可选的：
客户端只要收到 `ClientboundFinishConfigurationPacket` 就能离开配置阶段，因此代理不发送它们（也不等待 `ServerboundSelectKnownPacks`）。

## 5. 世界快照：逐个包对齐 1.21.10

`native/world_snapshot.cpp` 的每个包都用「先试新构造，再退回旧构造」：

| 包 | 1.20.1 | 1.21.10 |
| --- | --- | --- |
| `ClientboundLoginPacket` | `(IZGameType;GameType;Set;RegistryAccess$Frozen;ResourceKey;ResourceKey;JIIIZZZZOptional;I)V` | `(IZSet;IIIZZZLCommonPlayerSpawnInfo;Z)V`（新增 `CommonPlayerSpawnInfo`） |
| 实体出生 | `ClientboundAddPlayerPacket(Player)` / `Entity.getAddEntityPacket()` | `ClientboundAddEntityPacket(ILjava/util/UUID;DDDFFLEntityType;ILVec3;D)V` |
| `ClientboundContainerSetContentPacket` | `(IILNonNullList;LItemStack;)V` | `(IILjava/util/List;LItemStack;)V` |
| `ClientboundSetDefaultSpawnPositionPacket` | `(LBlockPos;F)V` | `(LLevelData$RespawnData;)V`（取 `Level.getRespawnData()`） |
| `ClientboundPlayerPositionPacket` | `(DDDFFLSet;I)V` | `(ILPositionMoveRotation;LSet;)V`（`PositionMoveRotation.of(Entity)`） |
| `ClientboundPlayerInfoUpdatePacket$Entry` | 7 参数 | 9 参数（多了 showHat、listOrder） |
| `Scoreboard` 队伍 / 时间 / 区块 / 半径 / 中心 | 不变 | 不变（`ClientboundLevelChunkWithLightPacket` 构造仍在） |

`sendSelfInfoToB` 和 `mirrorPlayerInfoUpdateToB` 不再手写字节流，而是用
`native/server/player_info.cpp: buildPlayerInfoUpdate()` 通过公开构造函数组装（EnumSet + Entry + `entries` 字段）。
这样 1.20.5 的动作位掩码、1.21.9 的 profile payload 变化都不会再让代理编码出错。
（1.21.9 去掉了 `Minecraft.getProfileProperties()`，所以皮肤属性只在旧版可用；皮肤渲染改由客户端缓存负责。）

## 6. 版本判定与回退

没有编译期宏，全部是运行时能力探测（`native/server/bindings.cpp`）：

- `refs.modernNetworking`：`ProtocolInfo` + `PacketEncoder/PacketDecoder(ProtocolInfo)` 全部存在。
- `refs.configurationPhase`：`ConnectionProtocol.CONFIGURATION` 存在。
- `refs.hasCommonSpawnInfo`、`refs.addEntityPacketCtor`、`refs.positionMoveRotationOfEntityMid`、
  `refs.levelGetRespawnDataMid`、`refs.playerInfoEntryCtorModern`、`refs.loginAckPacketCls` 等：逐项探测。

任何一项缺失就退回 1.20.1 行为，日志会打印
`BServer: networking model=ProtocolInfo configurationPhase=yes` 或 `ConnectionProtocol/no`。

## 7. 验证状态

已在本机完成：

- **MSVC 正式构建**：`./scripts/build.ps1 -VisualStudio 'D:/Program Files/vs_2026/main' -WithoutJvmTests`
  49/49 步骤成功，产物落在 `proxy/`：`Aurora.dll`（415 KB）、`Prism.dll`（143 KB）、`Compass.exe`（152 KB）；
  `ctest` 6/6 通过（`login_handoff`、`runtime_gate`、`packet_policy`、`team_state`、`class_editor`、`class_interface`）。
  工具链：MSVC 14.51.36231 (x64) + Windows SDK 10.0.26100（装在 `D:\Windows Kits\10`）+ VS 自带 CMake 4.3 / Ninja。
- **MinGW 回归路径**：`./scripts/build_mingw.ps1` 用同一份 `native/sources.txt` 编译链接到
  `build/mingw-verify/` 并跑同一批原生测试，便于在没有 VS C++ 工作负载的机器上做编译级回归。
- `tools/mcport/check_sources.py --version 1.20.1` 基线为 19 ok / 0 问题（证明扫描器把旧实现认得全），
  1.21.10 报出的差异已逐条处理（报告见 `.portcache/check-1.21.10.txt`）。

补充说明：VS 2026 初始安装里没有 C++ 工具集，已用 Visual Studio Installer 补装
（工作负载 `Microsoft.VisualStudio.Workload.NativeDesktop`；组件 `VC.Tools.x86.x64`、
`Windows11SDK.26100`、`VC.CMake.Project`）。注意该安装器要求 `--quiet/--passive` 必须**一开始就提权**
（否则报 5007），并且不支持 `--wait`；Windows SDK 默认装到 `D:\Windows Kits\10`。


**尚未完成（必须在真实客户端上做，不能由本机替代）**：

- 没有启动过 1.21.10 客户端，因此 DLL 的实际注入、协议交互、快照渲染都还没有端到端验证；
  编译、链接与单元测试通过不等于这些流程已经跑通。
- 端到端清单：A 入服前注入 → B 登录 → A 入服 → B 进入世界；A 已在服注入 → B 加入 → 位置/区块一致；
  B 断开重连；同步中断开；DEL 停用与再次启用。日志应依次出现
  `BServer: networking model=ProtocolInfo`、`B acknowledged the login phase; channel switched to CONFIGURATION`、
  `configuration: synthesized configuration phase for B`、`B acknowledged configuration; switching channel to PLAY`。

### 已知取舍

- `CommonPlayerSpawnInfo.seaLevel`：1.21.10 的 `Level` 没有 `getSeaLevel()`，取不到时写 0（只影响海平面雾/地平线表现）。
- 生成的 `GameProtocols$Context` 固定返回 `true`，与官方客户端一致；服务端侧按玩家游戏模式判断，理论上存在差异，
  但服务端解码依赖的是客户端用的同一套编解码器，因此以客户端为准更安全。
- 配置阶段不发送 `ClientboundSelectKnownPacks`/`CodeOfConduct`：官方客户端两者都不强制，
  唯一离开配置阶段的条件是 `ClientboundFinishConfigurationPacket`。
- 未实现 Bundle 的打包 handler（`bundler`/`unbundler`）：代理始终把 bundle 展开成单个包再转发。


## 8. 复现工具

```powershell
# 抓取某版本的官方映射 + MCPConfig；--with-jar 同时下载客户端 jar 供 javap 校验
python tools/mcport/mcport.py fetch 1.21.10 --with-jar

# 查类/成员/构造：名称 + 描述符 + obf + SRG
python tools/mcport/mcport.py ctors net.minecraft.network.protocol.game.ClientboundLoginPacket
python tools/mcport/mcport.py members net.minecraft.network.protocol.game.ClientboundPlayerInfoUpdatePacket -all

# 对比两个版本之间的成员签名变化
python tools/mcport/mcport.py diff 1.20.1 1.21.10 net.minecraft.network.protocol.game.ClientboundSetTimePacket

# 把 native/ 里所有 JNI 引用对着某个版本体检（ok / renamed / desc-changed / missing / class-missing）
python tools/mcport/check_sources.py --version 1.21.10 --report .portcache/check-1.21.10.txt
python tools/mcport/check_sources.py --version 1.21.10 --only missing,desc-changed,renamed,class-missing
```

缓存目录 `.portcache/` 已在 `.gitignore` 中；`python tools/mcport/mcport.py fetch <版本>` 可随时重建。
