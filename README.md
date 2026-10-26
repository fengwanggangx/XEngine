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


## 独立 XBroker 交易服务

模拟柜台、交易账户和券商登录管理已迁入同级 XBroker 工程。XEngine 通过 `[Broker] host/port/token` 连接 XBroker；未配置时非交易功能仍可启动。原模拟账本不迁移，旧策略需显式修改并重新选择 XBroker 账户，账户来源标记由 XEngine 自动维护。具体构建、配置、数据切换和测试参见 `../XBroker/README.md`。

Linux CMake 构建：`cmake -S XEngine/XEngine -B /tmp/xengine-build && cmake --build /tmp/xengine-build -j4`。协议转换测试使用 `ctest --test-dir /tmp/xengine-build`；运行时为现有二进制依赖设置 `LD_LIBRARY_PATH` 为本项目 `XEngine/libs/debug`。

### 单账户柜台数据协议

柜台请求必须指定非零账户编号。`CAccountData` 是单账户完整数据；`CCounterReply` 按操作返回回执、资金、持仓、委托、成交或完整账户数据。普通查询不覆盖完整缓存。

连接及重连通过完整账户数据恢复状态，再应用水位之后的实时事件，不支持 `after_sequence` 历史补取。资金查询使用单个 `account_id`，不再接受 `account_ids`。账户目录查询仍返回基础信息列表。凭据登录由服务端解析到唯一账户后提交柜台。

本次协议使用 `counter_reply` 的 `oneof` 数据字段，旧 `account_info_list`、`trade_sync` 字段已保留编号并废弃。XBroker、XEngine、XTrader 必须配套升级；HQMarket 同步协议定义。
