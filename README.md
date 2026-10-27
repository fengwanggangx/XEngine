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
