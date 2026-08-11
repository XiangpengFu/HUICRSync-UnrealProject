# HUICRSync 多人体验拓展计划

## 目标

把当前 `HUICRSync` 从单 PC + 单 HMD 拓展为单 PC + 多 HMD 的多人体验同步插件，同时不破坏已经验证成功的单人体验逻辑。

核心原则：

- 当只有 1 个 HMD 连接 PC 时，继续按当前单人逻辑运行。
- 当第 2 个或更多 HMD 连接同一个 PC 后，PC 进入多 HMD 调度模式。
- PC 始终作为连接、spawn、destroy、tick payload 路由的权威端。
- HMD 之间不直接通信，所有多人同步都经过 PC。
- Transform Convert 在多人架构中只发生在各个 HMD 本地，PC 不再为每个 HMD 做 transform convert。
- 多人拓展按功能模块逐步实现，每一步都必须能回归验证单人流程。

说明：开始 calibration 时，`bGameStopped` 应该变为 `true`；所有有效连接的 HMD 都完成 calibration，并且 initial spawn sync 完成后，`bGameStopped` 才变回 `false`。

## 已确认的多人策略

- 新 HMD 在其他 HMD 游戏过程中加入：第一版采用“全体重新 calibration + resync”。
- PC 不需要 UI 显示每个 HMD 的连接 / calibration / ready 状态；这些状态可以先作为调试日志或蓝图可读数据存在。
- 每个 HMD 的可读名字由 PC 在 connect 后分配，并发送给 HMD，例如 `HMD_1`、`HMD_2`。
- 对于物理交互类 actor，允许抢 ownership，但需要由 PC 权威裁决。
- `LastWriterWins` 不作为第一版重点实现，只记录为后续版本可选功能。

## 当前单人逻辑的保留边界

必须保留的现有行为：

- HMD 输入 PC IP / Port 后连接 PC。
- PC 收到 HMD connection request 后登记 peer，并回复连接成功。
- HMD 可以 start / stop calibration。
- stop calibration 后 HMD 发送 calibration / screen data 到 PC。
- PC 完成 calibration 处理后触发 initial spawn sync。
- HMD 根据 PC 已有的 sync actor 生成 counterpart。
- initial spawn sync 完成后，PC 和 HMD 的 `bGameStopped=false`，游戏开始。
- 游戏中 spawn / destroy sync actor 仍然由 PC 权威分发。
- 单人模式下现有蓝图调用路径尽量不改名、不改调用顺序。

多人改造时，所有新增逻辑都应满足：

- `ConnectedHMDPeers.Num() <= 1` 时，默认行为与当前单人版一致。
- 旧蓝图不需要立刻增加 HMD ID 参数也能继续运行。
- 新功能通过 peer registry、peer id、session epoch、payload source 等内部数据支持多人。

## 架构变化：Transform Convert 只在 HMD 本地发生

这是后续多人版本最重要的架构调整。

当前单人版里，PC 可以统一调用：

- `ConvertHMDTransformToPC`
- `ConvertPCTransformToHMD`

这个方式在单人体验中可用，但多人时会变得危险，因为 PC 必须知道“当前 transform 属于哪个 HMD”，并且不同 HMD 有不同 calibration transform。为了避免 PC 端维护多套 transform 并在蓝图里选择错误 peer，多人版改为：

- HMD 发起数据前，HMD 自己先调用 `ConvertHMDTransformToPC`，把本地 HMD 空间数据转换到 PC 空间。
- PC 收到 HMD 数据后，不再 convert，只把 PC 空间 payload 作为权威数据应用到本地，并转发给其他 HMD。
- 其他 HMD 收到 PC 转发的数据后，各自在本地调用 `ConvertPCTransformToHMD`，把 PC 空间数据转换到自己的 HMD 空间。
- PC 发起的数据同理：PC 直接发送 PC 空间 payload；每个 HMD 收到后自己调用 `ConvertPCTransformToHMD`。

换言之，多人版的网络 payload 标准空间应该是 PC 空间：

```text
HMD Local Space -> HMD Convert -> PC Space -> PC Route -> HMD Convert -> Target HMD Local Space
PC Space -> PC Route -> HMD Convert -> Target HMD Local Space
```

PC 不再需要为每个 HMD 保存 `PCToHMDTransform` / `HMDToPCTransform` 用于运行时转换。每个 HMD 在本地保存自己的相对 transform。

## 多人 Calibration 的新职责分配

为了支持 HMD 本地 transform convert，calibration 的职责要调整：

- PC 负责把 PC 侧 calibrator / screen / reference data 发送给每个 HMD。
- 每个 HMD 根据 PC 发来的 calibrator 数据和自己的 HMD screen / calibrator 数据，在本地计算自己的 relative transform。
- 每个 HMD 本地保存：
  - `PCToHMDTransform`
  - `HMDToPCTransform`
  - calibration valid state
- PC 只保存每个 HMD 的 calibration 状态，不负责保存或使用每个 HMD 的 transform 做运行时转换。

第一版可以让 HMD 在 stop calibration 时仍然把 calibration 完成消息发给 PC，但 transform 数据不再要求 PC 用于后续 payload convert。

## 阶段 1：HMD Peer 身份与连接管理

### 目标

让 PC 可以同时登记多个 HMD，并能稳定区分每个 HMD。

### 需要新增的数据结构

建议引入：

- `FHUICRSyncHMDPeerId` 或 `FName HMDPeerId`
- `FHUICRSyncHMDPeerInfo`
- `FHUICRSyncHMDPeerRuntimeState`
- `FHUICRSyncHMDPeerCalibrationState`

PC 的 peer registry 可以使用：

```cpp
TMap<FName, FHUICRSyncHMDPeerInfo> ConnectedHMDPeers;
```

每个 HMD peer 至少保存：

- `HMDPeerId`
- `DisplayName`
- `PersistentClientId`
- `IpAddress`
- `SyncPort`
- `bConnected`
- `bIsCalibrating`
- `bCalibrationValid`
- `bInitialSpawnReady`
- `LastHeartbeatFrame`
- `LastHeartbeatTime`
- `MissedHeartbeatCount`
- `LastHandledCalibrationEpoch`
- `LastSeenGameSessionEpoch`

注意：PC registry 里不再要求保存每个 HMD 的 runtime transform convert 数据。transform convert 数据保存在 HMD 本地。

### HMDPeerId 分配

建议：

- HMD 第一次运行时生成 `PersistentClientId`，保存到 SaveGame。
- HMD connect request 携带 `PersistentClientId`。
- PC 根据 `PersistentClientId` 查找是否已有记录。
- 如果是新 HMD，PC 分配新的 `HMDPeerId` 和 `DisplayName`。
- PC 把 `HMDPeerId` / `DisplayName` 回发给 HMD。
- HMD 保存自己的 `HMDPeerId`，后续所有 command / payload 都携带它。

### 连接流程

1. HMD 向 PC 发送 `ConnectionRequest`，携带：
   - `PersistentClientId`
   - HMD IP
   - HMD SyncPort
2. PC 登记或更新 peer。
3. PC 分配或确认：
   - `HMDPeerId`
   - `DisplayName`
4. PC 回复 `ConnectionAccepted` 给该 HMD。
5. HMD 标记连接成功，并保存 `HMDPeerId` / `DisplayName`。
6. PC 广播 peer list changed 给所有 HMD。

### 游戏中加入

如果游戏已经开始后有新 HMD 连接：

- 第一版直接进入全局 calibration / resync。
- PC 广播 `CalibrationState=true` 给所有 HMD。
- 所有 HMD 清理本地 sync actor。
- 所有有效 HMD 完成 calibration 后，PC 再统一 initial spawn sync。

暂不支持新 HMD 静默加入已经运行中的游戏。

## 阶段 2：Heartbeat 与断线处理

### 目标

PC 能检测 HMD 是否仍然连接；断线后从注册列表中移除该 HMD，后续游戏逻辑不再考虑它。

### Heartbeat 策略

HMD 连接成功后，持续向 PC 发送 heartbeat。

可以选择：

- 每帧发送一次 heartbeat。
- 或每隔固定时间发送一次，例如 0.1 秒。

为了降低网络压力，建议第一版用固定频率，例如 5 到 10 Hz，而不是一定每帧。

PC 对每个 HMD 保存：

- `LastHeartbeatFrame`
- `LastHeartbeatTime`
- `MissedHeartbeatCount`

PC 每帧或定时检查：

- 如果连续若干帧 / 若干秒没有收到某 HMD heartbeat，则判定 disconnected。
- disconnected 后从 `ConnectedHMDPeers` 删除。
- PC 广播 peer disconnected 给剩余 HMD。
- 该 HMD 若想再次参与，必须重新执行 connect。

### 断线发生在游戏中

如果某 HMD 在游戏中 disconnected：

- PC 从 peer registry 删除它。
- PC 后续 spawn / destroy / tick payload 不再发给它。
- 其他 HMD 和 PC 不需要因为它断线而暂停。
- 如果它重新连接，第一版按“新 HMD 加入”处理，也就是全体重新 calibration + resync。

### 断线发生在 calibration 中

如果某 HMD 在 calibration 中 disconnected：

1. PC 从 `ConnectedHMDPeers` 删除该 HMD。
2. PC 从 calibration barrier 中移除它。
3. PC 检查剩余 HMD 状态。

如果剩余 HMD 都没有在 calibrating，并且都已经 calibration ready：

- PC 自动进入 initial spawn sync。
- initial spawn sync 完成后，PC 和剩余 HMD 自动开始游戏。

如果还有其他 HMD 正在 calibrating：

- PC 保持 `bGameStopped=true`。
- 等剩余 HMD 都完成后再 start initial spawn sync。

## 阶段 3：多人 Calibration Barrier

### 目标

支持每个 HMD 独立 calibration，但游戏开始由 PC 统一判断。

### PC 全局状态

PC 维护：

- `ConnectedHMDPeers`
- `bAnyHMDCalibrating`
- `bAllConnectedHMDsCalibrationReady`
- `bInitialSpawnSyncInProgress`
- `CalibrationEpoch`
- `GameSessionEpoch`

每个 peer 维护：

- `bIsCalibrating`
- `bCalibrationValid`
- `LastHandledCalibrationEpoch`
- `bInitialSpawnReady`

### Start Calibration

任意 HMD 发起 start calibration：

1. HMD 本地 `bGameStopped=true`。
2. HMD 本地删除所有 sync actor。
3. HMD 向 PC 发送 `CalibrationState=true`，携带自己的 `HMDPeerId`。
4. 如果 PC 当前不在 calibration 状态，PC 执行：
   - `CalibrationEpoch++`
   - `bGameStopped=true`
5. PC 标记该 HMD `bIsCalibrating=true`。
6. PC 广播 `CalibrationState=true` 和 `CalibrationEpoch` 给所有 connected HMD。
7. 所有 HMD 收到广播后：
   - 如果这个 epoch 没处理过，设置 `bGameStopped=true`
   - 触发 `OnCalibrationStateChanged(true)`
   - 删除本地 sync actor
   - 记录 `LastHandledCalibrationEpoch`

### 避免重复删除 sync actor

如果 A 开始 calibration 后，B 也开始 calibration，但仍处于同一个 `CalibrationEpoch`：

- B 不应该触发所有 HMD 再清理一次 sync actor。
- 每个 HMD 对同一个 epoch 只清理一次。

这样可以避免重复 destroy / unregister 导致 registry 状态错乱。

### Stop Calibration

某个 HMD 结束 calibration：

1. HMD 本地计算并保存自己的 `PCToHMDTransform` / `HMDToPCTransform`。
2. HMD 向 PC 发送 calibration ready，携带：
   - `HMDPeerId`
   - `CalibrationEpoch`
   - 可选 screen / calibrator 状态摘要
3. PC 标记该 HMD：
   - `bIsCalibrating=false`
   - `bCalibrationValid=true`
4. PC 检查所有 connected HMD：
   - 是否没有 HMD 正在 calibrating
   - 是否所有 HMD 都 calibration valid

如果还有 HMD 未完成：

- PC 保持 `bGameStopped=true`。
- 不开始 initial spawn sync。

如果所有 HMD 都完成：

- PC 调用 `StartInitialSpawnSyncForAllPeers()`。

## 阶段 4：PC Calibration Reference 分发

### 目标

让每个 HMD 在本地计算自己的 relative transform。

### 流程

PC 在 calibration 阶段向每个 HMD 发送：

- PC calibrator transforms
- PC screen transforms
- screen id / calibrator id
- 必要的 PC reference actor 信息

HMD 收到后：

1. 匹配本地 HMD screen / calibrator。
2. 计算该 HMD 自己的 `HMDToPCTransform`。
3. 计算该 HMD 自己的 `PCToHMDTransform`。
4. 保存到 HMD 本地 SaveGame 或 runtime state。
5. 后续所有 transform convert 都使用 HMD 本地这套数据。

### PC 不做的事情

多人模式下 PC 不再做：

- 为每个 HMD 调用 `ConvertPCTransformToHMD`。
- 为每个 HMD 调用 `ConvertHMDTransformToPC`。
- 为每个 HMD 保存并在 tick 转发中使用相对 transform。

PC 只负责：

- 收 HMD 已经转换到 PC 空间的数据。
- 在 PC 空间应用数据。
- 把 PC 空间数据转发给目标 HMD。

## 阶段 5：Initial Spawn Sync 多 HMD 化

### 目标

所有有效连接的 HMD calibration 完成后，PC 把当前 PC 场景中的 sync actor 同步给所有 HMD。只有所有 HMD 都完成 initial spawn sync 后，游戏才开始。

### 流程

1. PC 调用 `StartInitialSpawnSyncForAllPeers()`。
2. PC 设置：
   - `bInitialSpawnSyncInProgress=true`
   - 所有 peer 的 `bInitialSpawnReady=false`
3. PC 对每个 connected HMD 单独发送：
   - `SyncSpawnBegin`
   - 全部 PC sync actor 的 spawn mirror packet
   - `SyncSpawnEnd`
4. 每个 HMD 收到 `SyncSpawnBegin`：
   - 删除本地 sync actor
   - 清空本地 sync actor registry
5. 每个 HMD 根据 PC packet 生成 counterpart。
6. 每个 HMD 完成后回复 `SyncSpawnReady(HMDPeerId)`。
7. PC 收集所有 connected HMD 的 ready。
8. 全部 ready 后：
   - PC `bGameStopped=false`
   - PC 广播 `GameStart`
   - 所有 HMD 收到后 `bGameStopped=false`

### 单人兼容

如果只有一个 HMD，这个流程等价于当前单人 `StartInitialSpawnSync()`。

## 阶段 6：运行时 Spawn / Destroy 多 HMD 化

### Spawn

维持 PC 权威。

PC 发起 spawn：

1. PC 本地 spawn PC actor。
2. PC 分配 `ActorID`。
3. PC 把 spawn mirror packet 发给所有 connected HMD。
4. 所有 HMD 生成自己的 HMD counterpart。

HMD 发起 spawn：

1. HMD 向 PC 发送 spawn request，携带：
   - requester `HMDPeerId`
   - local HMD class / remote PC class 信息
   - transform，应该已经由 HMD 转换到 PC 空间
   - init payload
2. PC spawn PC actor。
3. PC 分配权威 `ActorID`。
4. PC 把 spawn mirror packet 发给所有 HMD。
5. requester HMD 也通过 PC 返回的 mirror packet 生成或确认 counterpart。

第一版建议：无论谁发起 `SpawnSyncActor`，最终都在 PC 和所有 connected HMD 上生成。

### Destroy

维持 PC 权威。

任意端请求 destroy：

1. 如果 PC 发起：PC 本地销毁，然后发给所有 HMD。
2. 如果 HMD 发起：HMD 只发 request 给 PC，不先本地销毁。
3. PC 验证 ActorID 后，广播 destroy 给所有 HMD。
4. 所有 HMD 删除本地 counterpart。

### 幂等要求

所有 spawn / destroy command 都必须可重复处理：

- spawn packet 重复到达时，HMD 不能重复生成 actor。
- destroy packet 重复到达时，HMD 不能报错或影响其他 actor。

建议每个 spawn packet 携带：

- `SpawnRequestId`
- `ActorID`
- `SourcePeerId`
- `GameSessionEpoch`

## 阶段 7：每帧 Payload 同步与控制权

### Payload 标准空间

多人版 tick payload 的标准网络空间是 PC 空间。

PC 发起 payload：

1. PC actor 生成 PC 空间 payload。
2. PC 发送给所有 HMD。
3. 每个 HMD 收到后，自己调用 `ConvertPCTransformToHMD`。
4. HMD 在本地应用 receive payload。

HMD 发起 payload：

1. HMD actor 生成本地 HMD 空间 payload。
2. HMD 在发送前调用 `ConvertHMDTransformToPC`。
3. HMD 把 PC 空间 payload 发送给 PC。
4. PC 在 PC 空间应用到本地 actor。
5. PC 把同一份 PC 空间 payload 转发给除 source HMD 外的其他 HMD。
6. 其他 HMD 收到后，各自调用 `ConvertPCTransformToHMD`。

PC 不负责对每个目标 HMD 做 payload convert。

### 必须新增的元数据

每个 actor payload 需要携带：

- `ActorID`
- `TypeCode`
- `SourcePeerId`
- `TargetPeerId` 或 broadcast 标记
- `SequenceNumber`
- `ServerFrame` 或 `ServerTimestamp`
- `ControlMode`
- `CurrentOwnerPeerId`
- `GameSessionEpoch`

### 控制权模型

第一版建议只实现：

1. `PCAuthority`
   - 只有 PC payload 有效。
   - HMD 只接收。
   - 适合 PC 驱动的全局物体。

2. `OwnerHMD`
   - 某个 HMD 是当前 owner。
   - 只有 owner HMD 可以发送有效 payload。
   - PC 接收 owner payload 后应用到本地，并转发给其他 HMD。
   - 非 owner HMD 的 payload 默认忽略，或先发起 ownership request。

后续版本记录：

- `LastWriterWins` 可作为可选模式，但不建议第一版使用。
- 更复杂的 prediction / rollback 暂不实现。

### 允许抢 ownership

对于物理交互类 actor，允许其他 HMD 抢 ownership，但必须由 PC 裁决。

建议第一版规则：

- 非 owner HMD 发生交互时，发送 `OwnershipRequest`。
- PC 判断该 actor 是否允许抢占。
- 如果允许，PC 更新 `CurrentOwnerPeerId`。
- PC 广播 `OwnershipGranted` / `OwnerChanged` 给所有 HMD。
- 新 owner 的 payload 开始有效。
- 旧 owner 收到 owner changed 后停止发送或其 payload 被 PC 忽略。

可选裁决条件：

- actor 是否 `bAllowOwnershipSteal`
- 当前 owner 是否超时
- 当前 owner 是否仍在交互
- 新请求是否来自更高优先级交互

### HMD 是否执行 receive

建议规则：

- 如果 payload 的 `SourcePeerId == LocalHMDPeerId`，HMD 忽略，不执行 receive。
- 如果 payload 来源是 PC，HMD 执行 receive。
- 如果 payload 来源是其他 HMD，经 PC 转发，HMD 执行 receive。
- 如果 actor 当前本地由自己控制，忽略其他普通 state payload，但仍处理 PC authority correction / ownership changed。

### 发送频率与 dirty 判断

不要默认所有 HMD 每帧都发送所有 actor payload。

建议增加：

- `bHasLocalControl`
- `bPayloadDirty`
- transform movement threshold
- send rate limit

蓝图可以每帧准备 payload，但插件层可以根据 dirty / ownership / rate limit 决定是否真正发送。

## 阶段 8：Actor Blueprint 数据结构调整

多人后同一个 HMD actor 蓝图需要同时具备：

- 本地控制时写 `ActorSendPayloadData`
- 远端控制时读 `ActorReceivePayloadData`

但是否执行 receive 不应该只靠蓝图自己猜，插件应提供状态。

建议给 `AHUICRSyncActor` 增加蓝图可读字段：

- `LocalPeerId`
- `LastPayloadSourcePeerId`
- `LastPayloadWasFromSelf`
- `bHasLocalControl`
- `CurrentOwnerPeerId`
- `SyncControlMode`
- `LastPayloadSequenceNumber`
- `LastDroppedPayloadReason`

蓝图 Tick 推荐结构：

1. 如果 `bGameStopped`，不处理游戏同步。
2. 如果 `bHasLocalControl`，写 send payload。
3. 如果收到新 receive payload，并且不是自己发出的，应用 receive payload。
4. 如果 ownership changed，切换本地控制状态。

## 阶段 9：可靠命令与状态广播

多人需要新增或扩展的 command：

- `ConnectionRequest`
- `ConnectionAccepted`
- `PeerListChanged`
- `Heartbeat`
- `PeerDisconnected`
- `CalibrationStateChanged`
- `PCCalibrationReference`
- `CalibrationReadyFromHMD`
- `AllCalibrationReady`
- `SyncSpawnBegin`
- `SyncSpawnPacket`
- `SyncSpawnEnd`
- `SyncSpawnReady`
- `GameStart`
- `SpawnRequest`
- `SpawnMirror`
- `DestroyRequest`
- `DestroyMirror`
- `OwnershipRequest`
- `OwnershipGranted`
- `OwnershipRejected`
- `OwnershipReleased`
- `OwnerChanged`
- `ActorPayloadForward`

所有 command 都应该携带：

- `SourcePeerId`
- `TargetPeerId` 或 broadcast
- `CommandSequence`
- `CalibrationEpoch` 或 `GameSessionEpoch`

如果 command 的 epoch 和当前 epoch 不一致，应该丢弃或记录为 stale command。

## 阶段 10：PC Registry 与路由列表

PC 需要有明确的 connected peer 列表，connect 时添加，disconnect 时删除。

建议基础结构：

```cpp
TMap<FName, FHUICRSyncHMDPeerInfo> ConnectedHMDPeers;
```

PC 转发时不靠蓝图猜目标，而是根据 registry 和 payload metadata 判断：

- 如果是 PC 发起 payload：发给所有 connected HMD。
- 如果是 HMD 发起 payload：发给除 `SourcePeerId` 外的所有 connected HMD。
- 如果是 unicast command：只发给 `TargetPeerId`。
- 如果 peer 已 disconnected：不再发送。
- 如果 peer 不属于当前 `GameSessionEpoch`：不发送或等待 resync。

这个列表也是 calibration barrier、initial spawn barrier、heartbeat disconnected 判断的基础。

## 阶段 11：SaveGame 与持久化

HMD 本地保存：

- PC IP
- PC SyncPort
- HMD `PersistentClientId`
- PC 分配的最近一次 `HMDPeerId`
- PC 分配的 `DisplayName`
- HMD 本地计算出的 `PCToHMDTransform`
- HMD 本地计算出的 `HMDToPCTransform`
- 上次连接信息

PC 本地可选保存：

- 每个 HMD 的 `PersistentClientId`
- PC 分配的 `DisplayName`
- 最近 IP
- 最近 connected time

PC 第一版不需要保存每个 HMD 的 transform，因为运行时 transform convert 已经转移到 HMD 本地。

## 阶段 12：验证路线

### 每个阶段都必须回归单人

每做完一个模块，至少验证：

- 单 HMD connect PC 成功。
- 单 HMD calibration 成功。
- 单 HMD initial spawn sync 成功。
- 单 HMD runtime spawn / destroy 成功。
- 单 HMD tick payload 仍按旧逻辑工作。

### 多人最小验证

两台 HMD：

1. HMD A 连接 PC。
2. HMD B 连接 PC。
3. PC 分配 `HMD_1` / `HMD_2` 并分别发送给 A/B。
4. A start calibration，PC/A/B 全部 `bGameStopped=true`。
5. A/B 本地 sync actor 都被清理。
6. B 再 start calibration，不重复清理同一 epoch。
7. A stop calibration，A 本地计算并保存自己的 relative transform，PC 记录 A ready，但不开始游戏。
8. B stop calibration，B 本地计算并保存自己的 relative transform。
9. PC 对 A/B 同时 initial spawn sync。
10. A/B 都 ready 后，PC/A/B 同时 `bGameStopped=false`。
11. PC spawn actor，A/B 都生成 counterpart。
12. A 请求 spawn actor，PC 生成并广播给 A/B。
13. A destroy actor，PC 权威删除并广播给 A/B。

### Disconnect 验证

1. A/B 都连接并进入游戏。
2. A 断开 heartbeat。
3. PC 在阈值后移除 A。
4. PC 后续 payload 只发给 B。
5. A 重新连接后，触发全体 calibration + resync。

Calibration 中断线：

1. A/B 都连接。
2. A start calibration。
3. B disconnect。
4. PC 从 barrier 移除 B。
5. A stop calibration 后，PC 自动 initial spawn sync 并开始游戏。

### Tick payload 验证

1. PCAuthority actor：
   - PC 发送 PC 空间 payload。
   - A/B 收到后各自执行 `ConvertPCTransformToHMD`。
   - A/B 发送 payload 被忽略。

2. OwnerHMD actor：
   - A 获得 ownership。
   - A 发送前执行 `ConvertHMDTransformToPC`。
   - PC 收到 PC 空间 payload 并应用。
   - PC 转发给 B。
   - B 收到后执行 `ConvertPCTransformToHMD`。
   - A 不接收自己经 PC 转发的数据。
   - B 请求 ownership 成功后，B 控制，PC/A 跟随。

## 推荐实现顺序

### 第一组：连接、Peer Registry、Heartbeat

1. 新增 HMD peer 数据结构。
2. PC 支持多个 HMD connection request。
3. PC 分配 `HMDPeerId` / `DisplayName`。
4. HMD 保存自己的 `HMDPeerId` / `DisplayName`。
5. HMD 开始 heartbeat。
6. PC 实现 heartbeat timeout 和 disconnected 移除。
7. 单人回归测试。

### 第二组：多人 Calibration Barrier

1. command 增加 `HMDPeerId` 和 `CalibrationEpoch`。
2. PC 维护每个 HMD 的 calibration 状态。
3. 任意 HMD start calibration，PC 广播给所有 HMD。
4. 所有 HMD 按 epoch 清理 sync actor。
5. disconnected peer 从 barrier 移除。
6. 所有有效 HMD stop calibration 后才进入 spawn sync。
7. 单人和双 HMD calibration 测试。

### 第三组：HMD 本地 Transform Convert

1. PC 发送 PC calibrator / screen reference 给 HMD。
2. HMD 本地计算并保存 `PCToHMDTransform` / `HMDToPCTransform`。
3. HMD 发送前执行 `ConvertHMDTransformToPC`。
4. HMD 接收后执行 `ConvertPCTransformToHMD`。
5. PC 只路由 PC 空间 payload，不做 per-HMD convert。
6. 验证 A/B 使用不同 calibration transform 时各自转换正确。

### 第四组：Initial Spawn Sync For All Peers

1. 当前 `StartInitialSpawnSync` 拓展为按 peer 发送。
2. PC 收集每个 HMD 的 `SyncSpawnReady`。
3. 全部 ready 后再 `GameStart`。
4. 单人流程保持兼容。

### 第五组：Runtime Spawn / Destroy 广播

1. PC 发起 spawn 广播到所有 HMD。
2. HMD 发起 spawn request，由 PC 广播到所有 HMD。
3. Destroy 改为所有端都经 PC 权威确认。
4. 加入 spawn / destroy 幂等处理。
5. 验证运行中加入 / 删除 actor 不影响 ActorID 对齐。

### 第六组：Tick Payload 路由

1. payload 增加 `SourcePeerId`、`SequenceNumber`、`ControlMode`、`GameSessionEpoch`。
2. HMD 发送前 convert 到 PC 空间。
3. PC 接收 HMD payload 后按控制权验证。
4. PC 转发给除 source 外的其他 HMD。
5. HMD 接收后 convert 到本地空间。
6. HMD 忽略自己 source 的 payload。
7. 单人 tick 逻辑回归。

### 第七组：Ownership

1. 新增 `SyncControlMode`。
2. 实现 `PCAuthority`。
3. 实现 `OwnerHMD`。
4. 实现 ownership request / granted / rejected / owner changed。
5. 支持允许抢 ownership。
6. `LastWriterWins` 只记录为后续可选模式。

## 第一版多人体验范围

第一版建议做到：

- 多 HMD connect。
- PC 分配 HMD 名字。
- heartbeat / disconnected 移除。
- 多 HMD calibration barrier。
- PC calibration reference 分发。
- HMD 本地独立 transform convert。
- 所有 HMD 完成后统一 initial spawn sync。
- runtime spawn / destroy 广播到所有 HMD。
- tick payload 支持 `PCAuthority` 和简单 `OwnerHMD`。
- ownership 可以被抢，但由 PC 裁决。

暂时不做：

- HMD 中途无缝加入已经运行的游戏。
- `LastWriterWins` 默认模式。
- 复杂物理冲突合并。
- 多 PC。
- HMD 之间直接通信。
- 完整 rollback / prediction。

这样可以在现有插件架构上稳步拓展，并且每一步都能用当前单人流程做回归验证。
