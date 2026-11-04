# XEngine

XTrade 的策略运行与执行服务，原项目名 Wind。解决方案为 `XEngine.sln`，Linux 输出程序为 `XEngine`。构建配置使用 `XENGINE_DEPS`、`XENGINE_MODE`。现有数据库名称及业务配置保持兼容。

## HQMarket 行情客户端

`CBootLoader` 创建并运行 `CTcpClient`，作为 XEngine 到 HQMarket 的 TCP 桥梁。
连接地址通过 `HQMARKET_HOST`、`HQMARKET_PORT` 配置，默认使用
`127.0.0.1:9901`。
XEngine 自身的 TCP、HTTP 监听端口通过 `XENGINE_TCP_PORT`、`XENGINE_HTTP_PORT`
配置，默认使用 `9801`、`9802`，避免与本机 HQMarket 的 `9901/9902` 冲突。

`XEngine/hqmarket/CHQMarket` 使用传入的 `CTcpClient` 管理认证、协议收发和响应回调，
不直接管理 libevent 对象或网络线程；业务请求由
`CHQRequest` 独立组装，再交给 `CHQMarket::SendRequest()`。请求工厂包括
`Subscribe()`、`Unsubscribe()`、`QueryQuote()`、`QueryBars()` 和
`Heartbeat()`，返回类型均为 `CRequest*`。`CHQMarket::SendRequest()` 会接管并
释放传入的请求指针，调用方不要再次释放。

HQMarket 认证 Token 从 `XEngine/ini/system.ini` 读取：

```ini
[HQMarket]
token=replace-with-your-token
```

## XTrader 登录与注册

XEngine TCP 服务兼容 XTrader 的 `auth` / `register` 请求，账号与密码字段分别为
`user` / `password`。启动时 XEngine 连接 MySQL 并自动创建 `table_user`；密码使用
每用户随机盐和 SHA-256 摘要保存，不存储明文。

在运行目录的 `ini/system.ini` 中配置数据库：

```ini
[MySQL]
host=127.0.0.1
port=3306
account=root
password=your-password
database=wind
pool_size=4
```

数据库 `wind` 需要预先存在，配置账号需要拥有建表、查询和写入权限。账号允许
3-64 位字母、数字及 `_-.@`，密码长度为 8-128 位。

订阅股票当日实时行情时，代码和交易所分开传入：

```cpp
hqMarket.SubscribeQuote("600519", market::Exchange::sse);
```

响应仍以 `CRequest` 回调：`cmd` 是 HQMarket 消息类型，`ret["request_id"]`
用于关联请求。`CRequest::GetData()` 返回运行期 `CData` 指针，其中 `type` 标识具体
行情结构，`payload` 是对应 Protobuf 的序列化内容；在线路上分别存放于
`ret["data_type"]` 和 `ret["data"]`。错误响应另含 `error_code` 和 `error_message`。


## XBroker 动态账户

XEngine 通过 `[Broker] host/port/token` 连接 XBroker。账户不再使用数字 account_id，而以券商 ID、站点、账号共同标识。策略绑定将这一 tuple 无损持久化为 account_identity；协议仍传递独立的 broker_id、site、account 字段。默认数据库改为 engine_accounts_v2，使用新库，不迁移旧策略绑定。

券商目录和开户/登录请求由 XEngine 转发；Broker 只发布运行期间成功登录的账户。XEngine 在目录发现账户后依次查询资金、持仓、委托和成交，按各类水位衔接推送。断线使缓存失效，恢复查询完成后策略才可使用账户；交易请求不自动重发，不提供一次返回全部账户数据的请求。

配置与集成测试参见同级 XBroker/README.md。三端必须配套升级；账号是字符串，前导零及券商/站点隔离必须保留。


## 交易服务分层

`CAccountService` 仅管理账户身份、目录、登录连接状态、占用计数及退出保护。`IsConnected(account)` 表示账户已经连接，不表示资金和持仓等数据已经同步。账户状态变化通过通知使交易服务立即取消旧同步并失效旧快照；实际柜台连接和会话由 XBroker 管理。

`CTradeService` 保存资金、持仓、委托、成交、同步水位和数据就绪标记，按资金、持仓、委托、成交顺序查询并衔接增量推送，提供 `Snapshot()`、`GetAccount()`、`GetPosition()` 和 `GetOrders()`。`IsReady(account)` 同时检查账户连接和交易数据同步状态；策略启动及报单要求交易数据就绪。交易请求和委托成交通知也由交易服务处理。

`CBootLoader` 独立创建 HQMarket 和 XBroker 两个 `CSession`。账户服务和交易服务借用同一个 Broker Session。`CEngineCenter` 统一注册两个 Session 的响应及连接状态回调，以 `OnRequest` 分发客户端请求，以 `OnReply` 按来源和业务类型分发上游消息；业务服务不直接注册 Session 回调。账户服务处理券商目录、账户登录、退出和账户目录请求；交易服务处理资金、持仓、委托、成交、报单和撤单。两者各自管理请求关联、超时和断线完成，Session 负责认证、心跳、重连及响应广播，不再使用独立的 Broker 客户端。账户服务通知终端账户状态，交易服务通知终端资金、持仓、委托和成交变化。

`CStrategyService` 管理运行策略和订单归属，从账户服务申请或释放占用，从交易服务读取快照并报撤单。策略上下文直接借用交易服务查询资金持仓。资金查询等交易数据请求直接路由至交易服务。

退出先停止接收请求和策略，停止两个上游 Session 并排空回调，再停止两个服务的刷新任务、完成待响应请求并清除 Session 回调，随后销毁调用方、账户与交易服务，最后释放两个 Session。`CBootLoader` 的成员保持裸指针并显式释放和置空。

系统登录统一使用 `user`：认证请求和成功响应的身份字段为 `user`，`table_user` 的用户名列为 `user`，上游行情登录配置为 `[HQMarket] user`。交易账户身份字段仍为 `account`。不兼容旧用户名字段或数据库列，不迁移旧数据。


## 请求中心与终端行情服务

`CEngineCenter` 负责客户端认证、注册、心跳和消息分发。行情、账户、交易和策略服务均使用 `OnRequest` 接收业务请求、`OnReply` 接收响应或推送；账户请求的用户身份由中心认证后提供。

`CHQService` 管理终端行情查询合并、请求关联、订阅与退订、确认及回滚、行情推送、超时和断线恢复。策略仍独立管理自身行情订阅；行情消息由中心同时交给行情服务和策略服务，不因一方处理成功而跳过另一方。策略交易响应继续由交易服务完成关联与解码，再调用策略服务的 `OnReply` 重载。

BootLoader 显式持有中心和各业务服务。初始化完成并绑定回调后启动 Session；退出先排空终端请求、停止策略和上游 Session，再停止业务任务并清除两个 Session 回调，最后销毁借用方和 Session。
