# 接口名列表

# 接口名: get_common_info.asp

---

### 接口名称

`get_common_info.asp`

### 功能

`get_common_info.asp` 是数字能源 WEB 接口协议中的一个大型通用查询接口，通过 `Type` 参数区分多种查询功能。文档中 Type 的取值为 1~20，各 Type 功能如下：

- **Type=1**：获取当前步数的配置信息（页面等级与页面名称列表）
- **Type=2**：获取设备的信号信息（按当前步数、Asp 页面名称查询设备信号项）
- **Type=3**：获取告警产生/消除延时时间
- **Type=4**：用户点击 NETECO 连接测试时，先获取 NETECO 建链状态；若已处于建链状态，前台提示“当前处于建链状态，测试会导致断链，是否需要继续测试？”
- **Type=5**：用户点击 NETECO 连接测试并启动之后，查询具体建链过程
- **Type=6**：导出已激活的 License 文件
- **Type=7**：查询当前激活的 License 文件信息
- **Type=8**：导入 License 文件进度信息
- **Type=9**：通过 WEB 获取日志同步的配置参数（日志同步状态为使能时才能获取）
- **Type=10**：获取版本号信息（json 格式）
- **Type=11**：查询操作记录信息（V2R3C10 版本新增）
- **Type=12**：进入页面时获取系统中所有需要导出南向数据的设备信息（含设备类型、设备 ID 等）（V2R3C10 新增）
- **Type=13**：发起导出南向设备数据后，定时查询南向设备导出数据的进度（V2R3C10 新增）
- **Type=14**：获取南向设备导出包（V2R3C10 新增）
- **Type=15**：获取全部设备电子标签列表（V2R3C20 版本）
- **Type=16**：获取电子标签内容（V2R3C20 版本）
- **Type=17**：获取操作源信息（V2R5C10 版本）
- **Type=18**：获取当前系统的时间格式（V2R5C10 新增）
- **Type=19**：获取品牌管理信息（V3R21C10 新增）
- **Type=20**：获取安全配置信息（WEB_GET_SECURITY_CONFIG_INFO_JS_E）（V3R21C20 新增）

**后台接口（Backend C 函数）：**

```c
INT32 EMAP_AspGetCommonInfo(INT32 eid, Webs *wp, INT32 argc, CHAR **argv);
```

**后台是否记录操作记录：**

- Type=4：记录操作记录；
- Type=6：会记录操作记录；
- Type=14：会记录操作记录；
- 其余 Type（1、2、3、5、7、8、9、10、11、12、13、15、16、17、18、19、20）：不记录操作记录。

---

## 通用请求/调用方式

**前台请求使用：**

```
get_common_info.asp?Type=type&para1=para1&para2=para2&para3=para3&para4=para4&para5=para5
```

**平台前台调用：**

```
<%GetCommonInfo(type, para1, para2, para3, para4, para5);%>
```

---

## Type = 1

### 功能
获取当前步数的配置信息。

### 参数

#### 请求信息
| 参数 | 分隔符 | 说明 |
| --- | --- | --- |
| type | - | 1：请求类型 |
| para1 | - | Step 步数 |

#### 响应信息
信息域：`Error:"ERR"`；Nomal: `"level1~pageName1|…|levelN~pageNameN|"`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| level1 | ~ | 页面的等级 1 |
| pageName1 | \| | 页面的名称 1 |
| … | … | … |
| levelN | ~ | 页面的等级 N |
| pageNameN | \| | 页面的名称 N |

### 示例
```
get_common_info.asp?Type=1&para1=para1
```

---

## Type = 2

### 功能
获取设备的信号信息。

### 参数

#### 请求信息
| 参数 | 分隔符 | 说明 |
| --- | --- | --- |
| type | - | 2：请求类型 |
| para1 | - | currentStep 当前步数 |
| Para2 | - | Asp 页面的名称，如："config_guide_para_base.asp" |

#### 响应信息
信息域：`Error:"ERR"`；Normal: `tableNum|equipName1~Number1^equipTypeId1^dataType1^SignalId1^SignalName1^Accuracy1^Unit1^MinValue1^MaxValue1^Value1^Right1^Step length1^ControlSignal1^SignalLevel1~…~equipNameN…`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| tableNum | \| | 表格个数 |
| equipName1 | ~ | 设备名称 1 |
| Number1 | ~ | 无作用 1 |
| equipTypeId1 | ^ | 设备类型 Id 1 |
| equipId1 | ^ | 设备 ID 1 |
| dataType1 | ^ | 数据类型 1 |
| SignalId1 | ^ | 信号 ID 1 |
| SignalName1 | ^ | 信号名称 1 |
| Accuracy1 | ^ | 精度 1 |
| Unit1 | ^ | 单位 1 |
| MinValue1 | ^ | 最小值 1 |
| MaxValue1 | ^ | 最大值 1 |
| Value1 | ^ | 信号值 1 |
| Right1 | ^ | 权限 1 |
| Step length1 | ^ | 步长 1 |
| ControlSignal1 | ^ | 是否是控制信号 1 |
| SignalLevel1 | ~ | 信号等级 1 |
| … | ~ | … |
| equipNameN | ~ | 设备名称 N |
| NumberN | ~ | 无作用 N |
| EquipTypeIdN | ^ | 设备类型 Id N |
| EquipIdN | ^ | 设备 ID N |
| DataTypeN | ^ | 数据类型 N |
| SignalIdN | ^ | 信号 ID N |
| SignalNameN | ^ | 信号名称 N |
| AccuracyN | ^ | 精度 N |
| UnitN | ^ | 单位 N |
| MinValueN | ^ | 最小值 N |
| MaxValueN | ^ | 最大值 N |
| ValueN | ^ | 信号值 N |
| RightN | ^ | 权限 N |
| Step lengthN | ^ | 步长 N |
| ControlSignalN | ^ | 是否是控制信号 N |
| SignalLevelN | ~ | 信号等级 N |

### 示例
```
get_common_info.asp?Type=2&para1=para1&para2=para2
```

---

## Type = 3

### 功能
获取告警产生/消除延时时间。

### 参数

#### 请求信息
| 参数 | 分隔符 | 说明 |
| --- | --- | --- |
| type | - | 3：请求类型 |

#### 响应信息
信息域：`Error:"ERR"`；NORMAL: `AlarmDelayTime|AlarmCancelDelayTime|`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| AlarmDelayTime | \| | 告警产生延时时间 |
| AlarmCancelDelayTime | \| | 告警消除延时时间 |

### 示例
```
get_common_info.asp?Type=3
```

---

## Type = 4

### 功能
在用户点击 NETECO 连接测试时，先通过该请求获取 NETECO 建链状态，如果获取到当前 NETECO 处于已经建链状态的，则需要前台给出用户提示，“当前处于建链状态，测试会导致断链，是否需要继续测试？”

### 参数

#### 请求信息
| 参数 | 分隔符 | 说明 |
| --- | --- | --- |
| type | - | 4：请求类型 |
| para1（portId） | - | 端口 ID |
| para2（IpAddr） | 无 | 网管的 IP 地址 |
| para3（BinModId） | 无 | BIN 的模块 ID |
| para4 | ~ | 见下方子字段 |
| - ConnectIpExistFlag | ~ | 监控建链 IP 是否存在，当只有一个时，认为不存在；1：存在，0：不存在 |
| - ConnectIpEnum | ~ | 监控建链 IP 的枚举值，从 1 开始枚举，依次增加 |
| para5 | 无 | 见下方子字段 |
| - addrType | 无 | 网管地址类型；0：ipv4，2：域名 |
| - domainName | - | 域名（addrType 为 2 时有该字段） |
| - gateWay | - | 网关，可选参数，可不传 |
| - subnetMask | - | 子网掩码，可选参数，字符串格式，如：“255.255.254.0”，不传默认为全 0 |

#### 响应信息
信息域：`NORMAL: u16Ret|ConnectState|SameFlag|ProcessNum|`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| u16Ret | \| | 响应码，0 表示 OK，其他表示错误 |
| ConnectState | \| | 当前监控与网管建链状态；0：已经与网管连接成功；1：没有与网管建链成功，且处于测试空闲状态；2：当前 IP 处于测试中；3：当前 IP 测试成功；4：其他 IP 正在测试中；5：当前 IP 测试失败 |
| SameFlag | \| | 测试的 IP 和端口号与已经建链的是否一致；0：一致；1：不一致 |
| ProcessNum | \| | 测试过程数量，该请求一定是 0 |

### 示例
```
get_common_info.asp?Type=4&para1=para1&para2=para2&para3=para3&para4=para4&para5=para5
```

---

## Type = 5

### 功能
在用户点击 NETECO 连接测试时，用于发起 NETECO 链接测试启动之后，查询具体建链的过程。

### 参数

#### 请求信息
| 参数 | 分隔符 | 说明 |
| --- | --- | --- |
| type | - | 5：请求类型 |
| para1（portId） | - | 端口 ID |
| para2（IpAddr） | - | 网管的 IP 地址 |
| para3（BinModId） | - | BIN 的模块 ID |
| para4 | ~ | 见下方子字段 |
| - ConnectIpExistFlag | ~ | 监控建链 IP 是否存在，当只有一个时，认为不存在；1：存在，0：不存在 |
| - ConnectIpEnum | ~ | 监控建链 IP 的枚举值，从 1 开始枚举，依次增加 |
| para5 | - | 见下方子字段 |
| - addrType | 无 | 网管地址类型；0：ipv4，2：域名 |
| - domainName | - | 域名（addrType 为 2 时有该字段） |
| - gateWay | - | 网关，可选参数，可不传 |
| - subnetMask | - | 子网掩码，可选参数，字符串格式，如：“255.255.254.0”，不传默认为全 0 |

#### 响应信息
信息域：`NORMAL: u16Ret|ConnectState|SameFlag|ProcessNum|Process1~ProcessSate1|…ProcessN~ProcessSateN|`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| u16Ret | \| | 响应码，0 表示 OK，其他表示错误 |
| ConnectState | \| | 当前监控与网管建链状态；0：已经与网管连接成功；1：没有与网管建链成功，且处于测试空闲状态；2：当前 IP 处于测试中；3：当前 IP 测试完成；4：其他 IP 正在测试中 |
| SameFlag | \| | 该字段只有在 ConnectState 为 0 时，才有意义；测试的 IP 和端口号与已经建链的是否一致；0：一致；1：不一致 |
| ProcessNum | \| | 建链过程数目，包括已经连接成功的，和正在连接的 |
| Process1 | ~ | 过程 1 序号 |
| ProcessSate1 | \| | 过程 1 连接状态；0：测试 OK；1：测试中；其他：测试失败 |
| … | … | … |
| ProcessN | ~ | 过程 N 序号 |
| ProcessSateN | \| | 过程 N 连接状态；0：测试 OK；1：测试中；其他：测试失败 |

### 示例
```
get_common_info.asp?Type=5&para1=para1&para2=para2&para3=para3&para4=para4&para5=para5
```

---

## Type = 6

### 功能
导出已激活的 License 文件。

### 参数

#### 请求信息
| 参数 | 分隔符 | 说明 |
| --- | --- | --- |
| type | - | 6：请求类型 |

#### 响应信息
信息域：`Error:"ERR"`；Normal: `Result|FileName|`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| Result | \| | 导出结果；0：成功；3：其他用户正在导入；其他：失败 |
| FileName | \| | 导出文件名称 |

### 示例
```
get_common_info.asp?Type=6
```

---

## Type = 7

### 功能
查询当前激活的 License 文件信息。

### 参数

#### 请求信息
| 参数 | 分隔符 | 说明 |
| --- | --- | --- |
| type | - | 7：请求类型 |
| para | ~ | 见下方子字段 |
| - Num | ~ | 查询当前激活信息类型个数（1~3） |
| - Type1 | ~ | 0：证书基本信息；1：失效码信息；2：控制项对比信息 |
| - …… | …… | …… |
| - TypeN | ~ | 0：证书基本信息；1：失效码信息；2：控制项对比信息 |

#### 响应信息
信息域：`Error:"ERR"`；Normal: `Result|LicenseInfo1|LicenseInfo2|LicenseInfo3|`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| Result | \| | 查询结果；0：成功；其他：失败 |
| LicenseInfo1 | \| | 证书信息 1 |
| LicenseInfo2 | \| | 证书信息 2 |
| LicenseInfo3 | \| | 证书信息 3 |

**LicenseInfo 信息格式（如果为证书信息，LicenseInfo 格式如下）：**

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| InfoType | ~ | 信息类型 0：证书信息类型 |
| InfoExist | ~ | 信息内容是否存在；1：存在；0：不存在（如果为 0，后续文字字段不存在，结束时会带一个“\|”） |
| LicenseFileId | ~ | License ID 号 |
| LSN | ^ | License 文件生成序列号 |
| Creat time | ^ | License 文件创建时间 |
| Nation | ^ | 国家信息 |
| Operator | ^ | 运营商信息 |
| Local Info | ^ | 局点信息 |
| Certificategraceperiod | ^ | 证书宽限期 |
| Certificatevalidity | ^ | 证书有效性；0：有效；非 0：失效 |
| itemNum | ~ | item 的个数 |
| itemName1 | ^ | 控制项 1 名称 |
| itemVal1 | ^ | 控制项 1 的值 |
| itemtype1 | ^ | 控制项 1 类型：功能项/资源项 |
| itemFeatureName1 | ^ | 控制项 1 Feature 名字 |
| Item Deadline1 | ^ | 控制项 1 截止日期 |
| ItemState1 | ^ | 控制项 1 的状态 |
| …… | …… | …… |
| itemNameN | ^ | 控制项 N 名称 |
| itemValN | ^ | 控制项 N 的值 |
| itemtypeN | ^ | 控制项 N 类型：功能项/资源项 |
| itemFeatureNameN | ^ | 控制项 N Feature 名字 |
| Item DeadlineN | ^ | 控制项 N 截止日期 |
| ItemStateN | ^ | 控制项 N 的状态 |

**LicenseInfo 信息格式（如果为失效码信息，LicenseInfo 格式如下）：**

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| InfoType | ~ | 信息类型 1：失效码信息类型 |
| InfoExist | ~ | 信息内容是否存在；1：存在；0：不存在（如果为 0，后续文字字段不存在，结束时会带一个“\|”） |
| InvalidCodeNum | ~ | 失效码的个数 |
| InvalidCode1 | ~ | 失效码 1 |
| InvalidCode2 | ~ | 失效码 2 |
| …… | …… | …… |
| InvalidCodeN | ~ | 失效码 N |

**LicenseInfo 信息格式（如果为导入证书对比信息，LicenseInfo 格式如下）：**

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| InfoType | ~ | 信息类型 2：导入证书对比信息类型 |
| InfoExist | ~ | 如果为 0，后续文字字段不存在，结束时会带一个“\|” |
| LicenseFileId | ~ | 导入未激活文件 id 号 |
| ControlTermNum | ~ | 对比控制项的个数 |
| ControlTermName1 | ^ | 对比控制项 1 名称 |
| ControlTermCurrentVal1 | ^ | 对比控制项 1 当前值 |
| ControlTermImportVal1 | ^ | 对比控制项 1 导入文件值 |
| ControlTermName2 | ^ | 对比控制项 2 名称 |
| ControlTermCurrentVal2 | ^ | 对比控制项 2 当前值 |
| ControlTermImportVal2 | ^ | 对比控制项 2 导入文件值 |
| …… | …… | …… |
| ControlTermNameN | ^ | 对比控制项 N 名称 |
| ControlTermCurrentValN | ^ | 对比控制项 N 当前值 |
| ControlTermImportValN | ^ | 对比控制项 N 导入文件值 |

### 示例
```
get_common_info.asp?Type=7&para=para
```

---

## Type = 8

### 功能
导入 License 文件进度信息。

### 参数

#### 请求信息
| 参数 | 分隔符 | 说明 |
| --- | --- | --- |
| type | - | 8：请求类型 |

#### 响应信息
信息域：`Error:"ERR"`；Normal: `State|Result|InfoType~InfoExist~ControlTermNum~ControlTermName1^ControlTermCurrentVal1^ControlTermImportVal1~……ControlTermNameN^ControlTermCurrentValN^ControlTermImportValN~|`（证书对比信息，LicenseInfo 格式）

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| State | \| | 导入状态；0：导入初始化；1：导入中；2：导入完成（仅导入完成状态，才有后续字段） |
| Result | \| | 导入结果；0：导入成功【导入成功，携带 CompareInfo 字段信息】；2：上传初始化；3：其他用户正在上传；27695：存在次要错误【该错误出现时，后续只有存在 SecondaryErrorCode、SecondaryErrorItemNum 和 SecondaryErrorInfo 字段，其他错误出现时，SecondaryErrorCode、SecondaryErrorItemNum 和 SecondaryErrorInfo 字段不存在】；其他：导入失败 |
| SecondaryErrorCode | \| | 次要错误码 |
| SecondaryErrorItemNum | \| | 存在次要错误的控制项信息个数 |
| SecondaryErrorInfo | \| | 次要错误信息 |
| CompareInfo | \| | 导入证书对比信息 |

**SecondaryErrorInfo 格式：**

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| itemName1 | ^ | 控制项 1 名称 |
| itemErrCode1 | ~ | 错误码 |
| …… | …… | …… |
| itemNameN | ^ | 控制项 N 名称 |
| itemErrCodeN | ~ | 错误码 |

**CompareInfo 格式：**

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| InfoType | ~ | 信息类型 2：导入证书对比信息类型 |
| InfoExist | ~ | 信息内容是否存在；1：存在；0：不存在（如果为 0，后续文字字段不存在，结束时会带一个“\|”） |
| LicenseFileId | ~ | 导入未激活文件 id 号 |
| ControlTermNum | ~ | 对比控制项的个数 |
| ControlTermName1 | ^ | 对比控制项 1 名称 |
| ControlTermCurrentVal1 | ^ | 对比控制项 1 当前值 |
| ControlTermImportVal1 | ^ | 对比控制项 1 导入文件值 |
| ControlTermName2 | ^ | 对比控制项 2 名称 |
| ControlTermCurrentVal2 | ^ | 对比控制项 2 当前值 |
| ControlTermImportVal2 | ^ | 对比控制项 2 导入文件值 |
| …… | …… | …… |
| ControlTermNameN | ^ | 对比控制项 N 名称 |
| ControlTermCurrentValN | ^ | 对比控制项 N 当前值 |
| ControlTermImportValN | ^ | 对比控制项 N 导入文件值 |

### 示例
```
get_common_info.asp?Type=8
```

---

## Type = 9

### 功能
通过 WEB 获取日志同步的配置参数（日志同步状态为使能时才能获取）。

### 参数

#### 请求信息
| 参数 | 分隔符 | 说明 | 变更版本 |
| --- | --- | --- | --- |
| type | - | 9：请求类型 | V2R3C00 |

#### 响应信息（JSON 格式）
| 名称 | 值 | 说明 | 变更版本 |
| --- | --- | --- | --- |
| errcode | "" | 0：获取成功；非 0：获取失败 | - |
| synenable | "" | 日志同步功能是否使能，0：不使能，1：使能 | - |
| serveripv4 | "" | 服务器 ip，x.x.x.x 格式 | V2R3C10 修改：将 serverip 修改为 serveripv4 |
| serveripv6 | "" | 服务器 ip，x.x.x.x 格式 | V2R3C10 新增 |
| serverportid | "" | 服务器端口号 | - |
| encryptenable | "" | 加密使能标记：0 为禁止，1 为使能 | - |
| iptype | "" | Ip 类型：0 为 ipv4，1 为 ipv6 | V2R3C10 新增 |

### 示例
```
get_common_info.asp?Type=9
```

---

## Type = 10

### 功能
获取版本号信息（json 格式）(V2R3C10 版本)。

### 参数

#### 请求信息
| 参数 | 分隔符 | 说明 | 变更版本 |
| --- | --- | --- | --- |
| type | - | 10：请求类型 | V2R3C00 |
| para1 | - | 见下方 JSON 子字段 | - |
| - equipversion | - | 设备版本号；0：不携带设备版本信息；1：携带设备版本信息 | - |
| - bspversion | - | Bsp 版本号；0：不携带 BSP 版本信息；1：携带 BSP 版本信息 | - |
| - appversion | - | App 版本号；0：不携带 APP 版本信息；1：携带 APP 版本信息 | - |

#### 响应信息（JSON 格式）
初始版本：V2R3C10

| 名称 | 值 | 说明 |
| --- | --- | --- |
| errcode | "" | Bit0：0 解析参数正常 / 1 解析参数失败；Bit1：0 设备版本信息获取成功 / 1 获取失败；Bit2：0 BSP 版本信息获取成功 / 1 获取失败；Bit3：0 APP 版本信息获取成功 / 1 获取失败 |
| equipversionlist | [ ] | 设备版本号信息（请求消息要求携带设备版本信息时携带该组数据） |
| 　- 每项 | | equiptypeid：设备类型 id；equipid：设备 id；equipname：设备名称；svid：软件版本信号 id；svname：软件版本号；hvid：硬件版本信号 id；hvname：硬件版本号 |
| bspversion | "" | Bsp 版本号（请求消息要求携带 BSP 版本信息时携带该数据） |
| appversionlist | [ ] | APP 版本号（请求消息要求携带 APP 版本信息时携带该组数据）|
| 　- 每项 | | name：特性名称；version：特性版本号 |

### 示例
```
get_common_info.asp?Type=10&para1=para1
```

---

## Type = 11

### 功能
查询操作记录信息（V2R3C10 版本新增）。

### 参数

#### 请求信息
| 参数 | 分隔符 | 说明 |
| --- | --- | --- |
| type | - | 11：请求类型 |
| para1 | - | 0：查询操作记录日志 |
| para2 | - | 【Para1 = 0：查询操作记录日志时，Para2 内容如下】JSON：{ "pageindex": 查询第 n 页数据(n≥1)，数据值类型为数字；"queryNum": [1,500]，每页的条数，未传则默认原来的 30 条（V300R022C10 新增）；"totalnumflag": 获取操作记录总数量的标记，0：获取，1：不获取（默认必须要查询操作记录总数量，不支持不获取场景） } |
| para3 | - | 【Para1=0：查询操作记录日志时，如果按登录源或用户名查询操作记录，Para3 内容如下，V2R5C10 版本开始支持】JSON：{ "operatesource": 操作源；"username": 用户名称 } |

#### 响应信息（JSON 格式）
初始版本：V2R3C10

| 名称 | 值 | 说明 |
| --- | --- | --- |
| errcode | - | 0：查询成功；其他：查询失败，失败时后面的字段没有 |
| totalnum | - | 操作记录总数量（根据请求中 para2 中的"totalnumflag"参数值确认是否携带该数据），为 0 时后面的字段没有 |
| operatloglist | [ ] | 操作记录列表；每项包含：username（用户名）、operatetime（操作时间）、operatesrc（操作源）、operatecontent（操作内容） |

### 示例
```
get_common_info.asp?Type=11&para1=para1&para2=para2&para3=para3
```

---

## Type = 12

### 功能
进入页面时，先发送该请求获取系统中所有的需要导出南向数据的设备信息，包括设备类型、设备 ID 等（V2R3C10 新增）。

### 参数

#### 请求信息
| 参数 | 分隔符 | 说明 |
| --- | --- | --- |
| type | - | 12：请求类型 |

#### 响应信息（JSON 格式）
初始版本：V2R3C10

| 名称 | 值 | 说明 |
| --- | --- | --- |
| enableflag | - | 1：使能；0：禁止 |
| errcode | "" | 0：获取成功；其他：获取失败 |
| equipInfo | [ ] | 设备信息数组；每项包含：equipid（设备 ID）、equiptypeid（设备类型 ID）、equiptypename（设备类型名称，文档原文省略）、equipname（设备名称） |
| specification | [ ] | 导出规格信息数组；每项包含：equiptypeid（设备类型 id）、maxnum（设备类型下可以导出南向数据的设备最大数量） |

### 示例
```
get_common_info.asp?Type=12
```

---

## Type = 13

### 功能
在发起导出南向设备数据后，需要定时查询南向设备导出南向设备的数据进度（V2R3C10 新增）。

### 参数

#### 请求信息
| 参数 | 分隔符 | 说明 |
| --- | --- | --- |
| type | - | 13：请求类型 |

#### 响应信息（JSON 格式）
初始版本：V2R3C10

| 名称 | 值 | 说明 |
| --- | --- | --- |
| errcode | - | 0：获取成功；其他：获取失败 |
| totalstate | - | 导出总状态：0：空闲；1：导出中；2：导出成功；3：导出失败；4：其他用户导出中 |
| exportinfolist | [ ] | 导出信息数组；每项包含：equiptypeid（设备类型 ID）、equipid（设备 ID）、state（0：上传成功；1：进行中；2：失败；3：等待上传中）、progress（进度，如 90 表示 90%） |

### 示例
```
get_common_info.asp?Type=13
```

---

## Type = 14

### 功能
获取南向设备导出包（V2R3C10 新增）。

### 参数

#### 请求信息
| 参数 | 分隔符 | 说明 |
| --- | --- | --- |
| type | - | 14：请求类型 |

#### 响应信息（JSON 格式）
| 名称 | 值 | 说明 |
| --- | --- | --- |
| errcode | - | 0：成功，其他：失败 |
| packagename | - | 包名 |

### 示例
```
get_common_info.asp?Type=14
```

---

## Type = 15

### 功能
获取全部设备电子标签列表（V2R3C20 版本）。

### 参数

#### 请求信息
| 参数 | 分隔符 | 说明 |
| --- | --- | --- |
| type | - | 15：请求类型 |

#### 响应信息（JSON 格式）
| 名称 | 值 | 说明 |
| --- | --- | --- |
| errcode | "" | 1：ERR 获取失败，无后面所有字段；0：OK，获取 OK，带后面所有字段 |
| elabelist | [ ] | 设备电子标签数组；每项包含：equipid（设备 ID）、equipname（设备名称）、sigid（信号 ID）、signame（信号名称）、sigindex（信号 Index） |

### 示例
```
get_common_info.asp?Type=15
```

---

## Type = 16

### 功能
获取电子标签内容（V2R3C20 版本）。

### 参数

#### 请求信息
| 参数 | 分隔符 | 说明 |
| --- | --- | --- |
| type | - | 16：请求类型 |
| para1 | - | 见下方 JSON 子字段 |
| - equipid | "" | 设备 Id |
| - sigid | "" | 信号 ID |
| - sigindex | "" | 信号 Index |
| - equipname | "" | 设备名称 |
| - signame | "" | 信号名称 |

#### 响应信息（JSON 格式）
| 名称 | 值 | 说明 |
| --- | --- | --- |
| errcode | "" | 1：ERR 获取失败，无后面所有字段；0：OK，获取 OK，带后面所有字段 |
| equipid | "" | 设备 ID |
| sigid | "" | 信号 ID |
| sigindex | "" | 信号 Index |
| elableInfo | "" | 电子标签内容 |

### 示例
```
get_common_info.asp?Type=16&para1=para1
```

---

## Type = 17

### 功能
获取操作源信息（V2R5C10 版本）。

### 参数

#### 请求信息
| 参数 | 分隔符 | 说明 |
| --- | --- | --- |
| type | - | 17：请求类型 |

#### 响应信息（JSON 格式）
| 名称 | 值 | 说明 |
| --- | --- | --- |
| errcode | - | ERR：获取失败，无后面字段；OK：查询 ok |
| operatesrclist | [ ] | 操作源列表；每项包含：operatesrcid（操作源 id）、operatesrcname（操作源名称） |

### 示例
```
get_common_info.asp?Type=17
```

---

## Type = 18

### 功能
获取当前系统的时间格式（V2R5C10 新增）。

### 参数

#### 请求信息
| 参数 | 分隔符 | 说明 | 变更版本 |
| --- | --- | --- | --- |
| type | - | 18：请求类型 | V2R5C10 新增 |

#### 响应信息（JSON 格式）
| 名称 | 值 | 说明 |
| --- | --- | --- |
| errcode | "" | 1：ERR 获取失败，无后面所有字段；0：OK，获取 OK，带后面所有字段 |
| dateformatflag | "" | 日期格式标记（0：年月日，1：日月年） |

### 示例
```
get_common_info.asp?Type=18
```

---

## Type = 19

### 功能
获取品牌管理信息（V3R21C10 新增）。

### 参数

#### 请求信息
| 参数 | 分隔符 | 说明 | 变更版本 |
| --- | --- | --- | --- |
| type | - | 19：请求类型 | V3R21C10 新增 |

#### 响应信息（JSON 格式）
| 名称 | 值 | 说明 |
| --- | --- | --- |
| errcode | "" | 1：ERR 获取失败，无后面所有字段；0：OK，获取 OK，带后面所有字段 |
| brandFlag | - | 品牌标志；0：华为品牌；1：多品牌；2：品牌替换；3：白牌化（白牌化——后续字段不带） |
| brandList | [ ] | 品牌信息数组；每项包含：logoPicName（登录 logo 名称）、smallLogoPicName（关于弹框 logo 名称）、copyrightPicName（copyright logo 名称）、website（官网）、mailbox（售后邮箱）、copyright（版权信息） |

### 示例
```
get_common_info.asp?Type=19
```

---

## Type = 20

### 功能
获取安全配置信息（WEB_GET_SECURITY_CONFIG_INFO_JS_E）（V3R21C20 新增）。

### 参数

#### 请求信息
| 参数 | 分隔符 | 说明 | 变更版本 |
| --- | --- | --- | --- |
| type | - | 20：请求类型 | V3R21C20 新增 |

#### 响应信息（JSON 格式）
| 名称 | 值 | 说明 |
| --- | --- | --- |
| errcode | "" | 1：ERR 获取失败，安全页面所有配置项参数均获取失败；0：OK，获取 OK，有一个配置项获取到数据即为成功 |
| safeinfoList | [ ] | 安全配置项组数；每个配置项是一个单独的对象，各对象格式如下： |

**safeinfoList 中入侵组件对象：**

| 名称 | 值 | 说明 |
| --- | --- | --- |
| errcode | "" | 0：ok，入侵组件初始状态获取成功；1：err，入侵组件初始状态获取失败，无后续 intrusioncontol 字段 |
| intrusioncontol | "" | 0：启动；1：停止 |

**safeinfoList 中可扩展配置项对象（每个配置项是一个单独对象）：**

| 名称 | 值 | 说明 |
| --- | --- | --- |
| errcode | "" | 0：ok，初始值获取成功；1：err，初始值获取失败，无后续 something1/something2 等字段 |
| something1 | "" | （文档未给出具体含义，为可扩展配置项示例字段） |
| something2 | "" | （文档未给出具体含义，为可扩展配置项示例字段） |

### 示例
```
get_common_info.asp?Type=20
```

