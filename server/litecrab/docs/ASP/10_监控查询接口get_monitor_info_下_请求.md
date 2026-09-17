## Type=12　获取信号分组信息

### 参数

**请求信息：**

| 参数 | 参数类型 | 说明 |
| --- | --- | --- |
| Type | 12 | 获取信号分组信息 |
| Para1 | EquipId | 设备Id |
| Para2 | EquipTypeId | 设备类型Id |
| Para3 | SigType | 信号类型。0，采集；1，设置；2，控制；4，显示配置 |

**响应信息：**

信息域：
- Error: "ERR"
- Normal: `"EquipId|SigGroupNum|GroupIndex~GroupName~|…|GroupIndex~GroupName~"`

| 内容 | 分隔符 | 说明 | 修改版本 |
| --- | --- | --- | --- |
| EquipId | \| | 设备Id | |
| SigGroupNum | \| | 信号组数 | |
| GroupIndex1 | ~ | 分组下标 | |
| GroupName1 | ~ | 组名 | |
| GroupLevelState | ~ | 组等级状态。0：分组中信号为常用信号；1：分组中信号全为高级信号；2：分组中信号既有常用信号又有高级信号 | V200R002C10 新增 |
| BivFlag1 | ~\| | 二维表标记，0，不展示二维表；1，展示二维表 | V200R002C10 新增 |
| …… | …… | …… | |
| GroupIndexN | ~ | 分组下标 | |
| GroupNameN | ~ | 组名 | |
| GroupLevelState | ~ | 组等级状态。0：分组中信号为常用信号；1：分组中信号全为高级信号；2：分组中信号既有常用信号又有高级信号 | V200R002C10 新增 |
| BivFlagN | ~ | 二维表标记，0，不展示二维表；1，展示二维表 | V200R002C10 新增 |

**JSON 格式：**

```json
{
  "errCode": 0,
  "groupList": [{
    "groupIndex": 0,
    "groupName": "xxx",
    "groupLevel": 0,
    "bivFlag": 0
  }]
}
```

| 名称 | 值 | 说明 |
| --- | --- | --- |
| errCode | int | 0：成功；其他：失败 |
| groupList | [{...}] | 分组列表 |
| groupIndex | int | 分组下标 |
| groupName | string | 分组名称 |
| groupLevel | int | 0：分组中信号为常用信号；1：信号全为高级信号；2：信号既有常用信号，又有高级信号 |
| bivFlag | int | 是否二维表。0：非二维表；1：二维表 |

### 示例

**前台请求使用：**
```
get_monitor_info.asp?type=12&para1=EquipId&para2=EquipTypeId&para3=SigType&para4=&para5=&para6=
```

**平台前台调用：**
```
<% GetMonitorInfo(12, para1, para2, para3, para4, para5, para6);%>
```

后台是否记录操作记录：无需记录操作记录。

---

## Type=13　获取一个分组的信号值

### 参数

**请求信息：**
```
get_monitor_info.asp?type=Type&para1=EquipId&para2=EquipTypeId&para3=GroupIndex&para4=SigType&para5=&para6=
```

| 参数 | 参数类型 | 说明 |
| --- | --- | --- |
| Type | 13 | 刷新设备列表信息 |
| Para1 | EquipId | 设备Id |
| Para2 | EquipTypeId | 设备类型Id |
| Para3 | GroupIndex | 组序号 |
| Para4 | SigType | 信号类型。0，采集；1，设置；2，控制；4，显示配置 |

**响应信息：**

信息域：
- Error: "ERR"
- Normal: `"EquipId|GroupName~SigNum~SigType1^SigPara1~…SigTypeN~SigParaN~"`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| EquipId | \| | 设备Id |
| GroupName | ~ | 组名 |
| SigNum | ~ | 信号个数 |
| SigType1 | ^ | 信号类型 |
| SigPara1 | ~ | 信号参数 |
| …… | …… | …… |
| SigTypen | ^ | 信号类型 |
| SigParan | ~\| | 信号参数 |

**SigType 为 Enum 类型时**，SigPara 格式：
```
SigId^SigFullName^SigUnit^EnumNum&EnumRes1=EnumValue1&…&EnumResN=EnumValN
```

| SigPara 内容 | 分隔符 | 说明 | 修改版本 |
| --- | --- | --- | --- |
| SigId | ^ | 信号Id | |
| SigFullName | ^ | 信号全名 | |
| SigUnit | ^ | 信号单位 | |
| EnumNum | & | 枚举个数 | |
| EnumRes1 | = | 枚举结果 | |
| EnumVal1 | & | 枚举值 | |
| …… | …… | …… | |
| EnumResN | = | 枚举结果 | |
| EnumValN | ^ | 枚举值 | |
| EnumValue | ^ | 信号枚举值 | |
| SigAuthority | ^ | 信号权限 | |
| SigCtrlFlag | ^ | 信号控制标识 | |
| SigLevel | | 信号等级。0：常用信号 1：高级信号 | V2R2C10 新增 |

**SigType 为 String 类型时**，SigPara 格式：
```
SigId^SigFullName^SigValLen^SigValue^SigAuthority^SigCtrlFlag^SigUnit
```

| SigPara 内容 | 分隔符 | 说明 | 修改版本 |
| --- | --- | --- | --- |
| SigId | ^ | 信号Id | |
| SigFullName | ^ | 信号全名 | |
| SigValLen | ^ | 信号值长度 | |
| SigValue | ^ | 信号值 | |
| SigAuthority | ^ | 4 | |
| SigCtrlFlag | ^ | 信号控制标志 | |
| SigUnit | ^ | 信号单位 | |
| SigLevel | | 信号等级。0：常用信号 1：高级信号 | V2R2C10 新增 |

**SigType 为其他类型时**，SigPara 格式：
```
SigId^SigFullName^SigPrecision^SigUnit^SigValue^SigAuthority^SigValStep^SigCtrlFlag
```

| SigPara 内容 | 分隔符 | 说明 | 修改版本 |
| --- | --- | --- | --- |
| SigId | ^ | 信号Id | |
| SigFullName | ^ | 信号全名 | |
| SigPrecision | ^ | 信号精度 | |
| SigUnit | ^ | 信号单位 | |
| SigValueMin | ^ | 信号最小值 | |
| SigValueMax | ^ | 信号最大值 | |
| SigValue | ^ | 信号值 | |
| SigAuthority | ^ | 信号权限 | |
| SigValStep | ^ | 信号值步长 | |
| SigCtrlFlag | ^ | 信号控制标志 | |
| SigLevel | | 信号等级。0：常用信号 1：高级信号 | V2R2C10 新增 |

**JSON 格式：**

```json
{
  "errCode": 0,
  "sigList": [{
    "sigName": "xxx",
    "sigLevel": 0,
    "sigId": 0,
    "defaultVal": "xxx",
    "sigUnit": "xxx",
    "ctrlSigFlag": 0,
    "sigAuth": 0,
    "sigValue": "xxx",
    "valType": 0,
    "enumList": [{"enumName": "", "enumVal": 0}],
    "maxValLen": 0,
    "precision": 0,
    "minVal": "xxx",
    "maxVal": "xxx",
    "valStep": "xxx"
  }]
}
```

| 名称 | 值 | 说明 | 变更版本 |
| --- | --- | --- | --- |
| errCode | int | 0：成功；其他：失败 | |
| sigList | [{...}] | 信号列表 | |
| sigName | string | 信号名称 | |
| sigLevel | int | 0：分组中信号为常用信号；1：信号全为高级信号；2：信号既有常用信号，又有高级信号 | |
| sigId | int | 信号 id | |
| defaultVal | string | 默认值。EMAP_WEB_MODIFY_SIG_DEFAULT_VALUE_ID 未使能则不带 | 当前未实现 |
| sigUnit | string | 单位 | |
| ctrlSigFlag | Int | 控制信号标识。值为 3 代表有提示信息 | |
| sigAuth | Int | 信号操作权限。使能 bit 位控制权限，则取低四位，例如 0x07；其他取高四位 | |
| sigValue | String/int | 信号值。枚举类型信号值为 int，其他为 string | |
| valType | Int | 信号值类型。按 emap 定义的值类型枚举 | |
| enumList | array | [{enumName:"", enumVal:0},…]。enumName：枚举项名；enumVal：枚举项值。仅信号为枚举类型时有该字段 | |
| maxValLen | int | 最大信号值长度。仅信号类型为 string/memory 类型时有该字段 | |
| precision | int | 信号值精度。信号类型非枚举、字符串时返回 | |
| minVal | string | 最小值。信号类型非枚举、字符串时返回 | |
| maxVal | string | 最大值。信号类型非枚举、字符串时返回 | |
| valStep | string | 信号值步长。信号类型非枚举、字符串时返回 | |

### 示例

**前台请求使用：**
```
get_monitor_info.asp?type=13&para1=EquipId&para2=EquipTypeId&para3=GroupIndex&para4=SigType&para5=&para6=
```

**平台前台调用：**
```
<% GetMonitorInfo(13, para1, para2, para3, para4, para5, para6);%>
```

后台是否记录操作记录：无需记录操作记录。

---

## Type=15　获取告警关联音响的信息

### 参数

**请求信息：**
```
get_monitor_info.asp?type=Type&para1=EquipId&para2=EquipTypeId&para3=SigType&para4=GroupIndex&para5=&para6=
```

| 参数 | 参数类型 | 说明 |
| --- | --- | --- |
| Type | 15 | 按类型获取告警关联音响信息 |
| Para1 | EquipId | 设备Id |
| Para2 | EquipTypeId | 设备类型Id |
| Para3 | SigType | 信号类型。0，采集；1，设置；2，控制；4，显示配置 |

**响应信息：**

信息域：
- Error: "ERR"
- Normal: `"alarmNum | EquipId~AlarmId~RelationFlag|…|EquipId~AlarmId~RelationFlag|"`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| AlarmNum | \| | 告警个数 |
| EquipId | ~ | 设备Id |
| AlarmId | ~ | 告警Id |
| RelationFlag | \| | 关联标记 |
| …… | …… | …… |
| EquipId | ~ | 设备Id |
| AlarmId | ~ | 告警Id |
| RelationFlag | \| | 关联标记 |

### 示例

**前台请求使用：**
```
get_monitor_info.asp?type=15&para1=EquipId&para2=EquipTypeId&para3=SigType&para4=GroupIndex&para5=&para6=
```

**平台前台调用：**
```
<% GetMonitorInfo(15, para1, para2, para3, para4, para5, para6);%>
```

后台是否记录操作记录：无需记录操作记录。

---

## Type=16　获取实时监控页签显示标识

### 参数

**请求信息：**
```
get_monitor_info.asp?type=Type&para1=EquipId&para2=EquipTypeId&para3=&para4=&para5=&para6=
```

| 参数 | 参数类型 | 说明 |
| --- | --- | --- |
| Type | 16 | 查询实时监控页签显示标识 |
| Para1 | EquipId | 设备 id |
| Para2 | EquipTypeId | 当前的设备类型 id |

**响应信息：**

信息域：
- Error: "ERR"
- Normal: `RunInfoDispFlag | RunParaDispFlag | RunCtrolInfoDispFlag | DispCfgDispFlag |`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| RunInfoDispFlag | \| | 运行信息显示标识 |
| RunParaDispFlag | \| | 运行参数显示标识 |
| RunCtrolInfoDispFlag | \| | 运行控制显示标识 |
| DispCfgDispFlag | \| | 显示配置显示标识 |

### 示例

**前台请求使用：**
```
get_monitor_info.asp?type=16&para1=EquipId&para2=EquipTypeId&para3=&para4=&para5=&para6=
```

**平台前台调用：**
```
<% GetMonitorInfo(16, para1, para2, para3, para4, para5, para6);%>
```

后台是否记录操作记录：无需记录操作记录。

---

## Type=17　获取采集信号的基本信息

### 参数

**请求信息：**
```
get_monitor_info.asp?type=Type&para1=equipNum~equipType~equipId~0~&para2=&para3=&para4=&para5=&para6=
```

**Type 参数：**

| 参数 | 参数类型 | 说明 |
| --- | --- | --- |
| Type | 17 | 查询实时监控页签显示标识 |

**Para1 参数：**

| 参数 | 分隔符 | 说明 |
| --- | --- | --- |
| equipNum | ~ | 设备数量。1：目前只考虑一个设备的场景 |
| equipType | ~ | 设备类型 |
| equipId | ~ | 设备ID |
| 0 | ~ | 0：标识符 |

**Para2 参数（该参数产品可以不携带，不携带场景均按照默认值进行处理）：**

```json
{
  "precisionflag": 0
}
```

| 名称 | 值 | 说明（初始版本：V2R5C00） | 变更版本 |
| --- | --- | --- | --- |
| precisionflag | | 携带精度信息标记，取值范围[0~1]。0：不携带；1：携带。默认不携带 | |

**响应信息：**

信息域：
- Error: "ERR"
- Normal:
```
equipNum | equipId ~MoreSigFlag~WebShowSigNum~SigId1^SigFullName1^SigUnit1^SigValueType1^SigPrecision1……~SigIdN^SigFullNameN^SigUnitN^SigValueTypeN^SigPrecision1
```

| 内容 | 分隔符 | 说明 | 变更版本 |
| --- | --- | --- | --- |
| equipNum | \| | 设备数量。1：目前只支持一个设备 | |
| equipId | ~ | 设备ID | |
| MoreSigFlag | ~ | 是否有更多信号标识。1：存在；0：不存在 | |
| WebShowSigNum | ~ | Web 显示信号的数量 | |
| SigId1 | ^ | 信号ID1 | |
| SigFullName1 | ^ | 信号名称1 | |
| SigUnit1 | ^ | 信号单位1 | |
| SigValueType1 | ^ | 信号值类型1 | |
| SigPrecision1 | | 信号值精度1 | V2R5C00 版本新增精度信息，根据请求数据中 Para2-precisionflag 字段决定是否携带该字段 |
| …… | | …… | |
| SigIdN | ^ | 信号ID N | |
| SigFullNameN | ^ | 信号名称N | |
| SigUnitN | ^ | 信号单位N | |
| SigValueTypeN | | 信号值类型N | |
| SigPrecisionN | | 信号值精度N | V2R5C00 版本新增精度信息，根据请求数据中 Para2-precisionflag 字段决定是否携带该字段 |

### 示例

**前台请求使用：**
```
get_monitor_info.asp?type=17&para1=equipNum~equipType~equipId~0~&para2=&para3=&para4=&para5=&para6=
```

**平台前台调用：**
```
<% GetMonitorInfo(17, para1, para2, para3, para4, para5, para6);%>
```

后台是否记录操作记录：无需记录操作记录。

---

## Type=18　获取设置常用信号的基本信息

功能：获取设置常用信号的基本信息，包括信号名、信号单位、信号类型、信号精度、信号的范围等，该请求获取的信号为该用户权限下的信号。

### 参数

**请求信息：**
```
get_monitor_info.asp?type=18&para1=para1Info&para2=para2Info&para3=&para4=&para5=&para6=
```

**para1Info 参数：**

| 参数 | 分割符 | 说明 |
| --- | --- | --- |
| CtrlFlag | ~ | 控制信号标志：1 表示控制信号；0 表示设置信号 |

**para2Info 参数：**

| 参数 | 分割符 | 说明 |
| --- | --- | --- |
| EquipNum | ~ | 设备数量，目前只支持查询一个，即填 1 |
| EquipTypeId1 | ~ | 设备类型ID 1 |
| EquipId1 | ~ | 设备ID 1 |
| Reserve1 | ~ | 保留字1 |
| … | … | … |
| EquipTypeIdn | ~ | 设备类型ID n |
| EquipIdn | | 设备ID n |
| ReserveN | | 保留字N |

**响应信息：**

信息域：
- Error: "ERR"
- Normal:
```
EquipNum|EquipId1~MoreSigFlag1~EquipSigNum1~sigId1^sigName1^sigUnit1^sigType1^setSigPromptType1^SigAuthority1^diff1（差异化字段，根据信号类型的不同而不同，详见后面说明）
…
|EquipIdn~MoreSigFlagn~EquipSigNumn~sigIdn^sigNamen^sigUnitn^sigTypeN^setSigPromptTypeN^SigAuthorityN^diffN（差异化字段，根据信号类型的不同而不同，详见后面说明）
```

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| EquipNum | \| | 设备数量 |
| EquipId1 | ~ | 设备Id1 |
| MoreSigFlag1 | ~ | 是否有更多信号标记1，其中 1 表示有更多信号 |
| EquipSigNum1 | ~ | 设备1的常用信号数量 |
| sigId1 | ^ | 信号ID1 |
| sigName1 | ^ | 信号名1 |
| sigUnit1 | ^ | 信号单位1 |
| sigType1 | ^ | 信号类型值的类型。6：枚举类型；14：字符串类型；15：内存 MEM 类型；其余数值为其他类型的信号 |
| setSigPromptType1 | ^ | 设置信号提示类型：0：设置信号，不需要提示语，不需要二次认证；1：控制信号，不需要提示语，不需要二次认证；2：控制信号且需要提示语；3：设置信号且需要提示语；4：设置信号且需要密码认证；5：控制信号且需要密码认证 |
| SigAuthority1 | ^ | 信号权限1。0：访客权限；1：操作员；2：工程师；3：管理员 |
| 差异化字段1 | ^ | 差异化字段详见后面说明 |
| … | … | … |
| EquipIdn | ~ | 设备Idn |
| MoreSigFlagn | ~ | 是否有更多信号标记 n，其中 1 表示有更多信号 |
| EquipSigNumn | ~ | 设备 n 的常用信号数量 |
| sigIdn | ^ | 信号IDn |
| sigNamen | ^ | 信号名 n |
| sigUnitn | ^ | 信号单位 n |
| sigTypeN | ^ | 信号值的类型 N。6：枚举类型；14：字符串类型；15：内存 MEM 类型；其余数值为其他类型的信号 |
| setSigPromptTypeN | ^ | 设置信号提示类型：0~5，含义同上 |
| SigAuthorityN | ^ | 信号权限 N。0~3，含义同上 |
| 差异化字段N | ^ | 差异化字段详见后面说明 |

**差异化字段按信号类型区分：**

当信号类型为枚举 6 时，差异化字段为：

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| enumNum | = | 枚举数量 |
| enumRes1 | = | 枚举资源1 |
| enumVal1 | = | 枚举资源1对应的值 |
| … | … | … |
| enumResN | = | 枚举资源N |
| enumValN | = | 枚举资源N对应的值 |

当信号类型为 MEM 15 或者字符型 14 时，差异化字段为：

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| sigMaxLen | | 信号最大长度 |

当信号类型为除了 6、14、15 之外的类型时，差异化字段为：

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| sigPrecision1 | = | 精度1 |
| sigMinLimit1 | = | 信号下限1 |
| sigMaxLimit1 | = | 信号上限1 |
| sigStep1 | = | 信号步长1 |

### 示例

**前台请求使用：**
```
get_monitor_info.asp?type=18&para1=para1Info&para2=para2Info&para3=&para4=&para5=&para6=
```

**平台前台调用：**
```
<% GetMonitorInfo(18, para1, para2, para3, para4, para5, para6);%>
```

后台是否记录操作记录：无需记录操作记录。

---

## Type=19　获取多个设备的信号值

### 参数

**请求信息：**
```
get_monitor_info.asp?type=19&para1=Para1Info&para2=Para2Info&para3=&para4=&para5=&para6=
```

**Para1Info 参数：**

| 参数 | 分割符 | 说明 |
| --- | --- | --- |
| SigType | ~ | 信号类型。0，采集；1，设置；2，控制 |
| Reserve | ~ | 保留字 |

**Para2Info 参数：**

| 参数 | 分割符 | 说明 |
| --- | --- | --- |
| reserve | ~ | 保留字 |
| EquipTypeId | ~ | 设备类型ID |
| EquipNum | ~ | 设备数量 |
| EquipId1 | ~ | 设备ID 1 |
| Reserve1 | ~ | 保留字1 |
| … | … | … |
| EquipIdn | ~ | 设备ID n |
| reserven | ~ | 保留字 n |

**Para3 参数（该参数产品可以不携带，不携带场景均按照默认值进行处理）：**

```json
{
  "precisionflag": 0
}
```

| 名称 | 值 | 说明（初始版本：V2R5C00） | 变更版本 |
| --- | --- | --- | --- |
| precisionflag | | 携带精度信息标记，取值范围[0~1]。0：不携带；1：携带。默认不携带 | |

**响应信息：**

信息域：
- Error: "ERR"
- Normal:

由于获取多个设备的消息是以信号维度回复的，为了减少 for，提高效率，响应格式采用**格式二**。当前代码实现为格式二。修改消息时，不需要考虑格式一。

**格式一（目前不用该格式）：**
```
SigType |EquipNum|EquipId1~sigId1~sigType1~sigValue1~…|EquipIdN~sigIdN~sigTypeN~sigValueN~
```

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| SigType | \| | 信号类型 |
| EquipNum | \| | 设备数量 |
| EquipId1 | ~ | 设备Id1 |
| sigId1 | ~ | 信号ID1 |
| sigType1 | ~ | 信号类型1 |
| sigValue1 | ~ | 信号值1，当信号值为枚举时，该值的格式为：信号值=枚举资源；其他信号类型时，为实际信号值 |
| … | … | … |
| EquipIdN | ~ | 设备IdN |
| sigIdN | ~ | 信号IDN |
| sigValueN | ~ | 信号值N |

**格式二：**
```
SigType |SigNum|EquipId1~sigId1~sigType1~sigValue1~…|EquipIdN~sigIdN~sigTypeN~sigValueN~
```

| 内容 | 分隔符 | 说明 | 变更版本 |
| --- | --- | --- | --- |
| SigType | \| | 信号类型 | |
| SigNum | \| | 信号数量 | |
| EquipId1 | ~ | 设备Id1 | |
| sigId1 | ~ | 信号ID1 | |
| sigType1 | ~ | 信号类型1，其中：6：枚举类型；14：字符串类型；15：内存 MEM 类型；其余数值为其他类型的信号，不单独区分 | |
| sigValue1 | ~ | 信号值1，当信号值类型 sigType1 为：枚举 6：信号值=枚举资源；其他信号类型：信号值 | |
| sigDefaultVal1 | ~ | 信号默认值1。产品使能设置信号恢复默认值功能时存在该字段 | |
| sigPrecision1 | | 信号值精度1 | V2R5C00 版本新增精度信息，根据请求数据中 Para3-precisionflag 字段决定是否携带该字段 |
| … | … | … | |
| EquipIdN | ~ | 设备IdN | |
| sigIdN | ~ | 信号IDN | |
| sigTypeN | ~ | 信号类型N | |
| sigValueN | ~ | 信号值N，当信号值类型 sigTypeN 为：枚举 6：信号值^枚举资源；其他信号类型：信号值 | |
| sigDefaultValN | ~ | 信号默认值N。产品使能设置信号恢复默认值功能时存在该字段 | |
| sigPrecisionN | | 信号值精度N | V2R5C00 版本新增精度信息，根据请求数据中 Para3-precisionflag 字段决定是否携带该字段 |

### 示例

**前台请求使用：**
```
get_monitor_info.asp?type=19&para1=Para1Info&para2=Para2Info&para3=&para4=&para5=&para6=
```

**平台前台调用：**
```
<% GetMonitorInfo(19, para1, para2, para3, para4, para5, para6);%>
```

后台是否记录操作记录：无需记录操作记录。

---

## Type=20　获取实时监控信号信息（JSON 格式数据）（V2R3C10 版本新增）

### 参数

**请求信息：**
```
get_monitor_info.asp?type=type&para1=value&para2=&para3=&para4=&para5=&para6=
```

**Type 参数：**

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| Type | | 20：查询实时监控信号信息，json 格式 |

**Para1 参数（JSON）：

```json
{
  "equiptypeid": 0,
  "equipid": 0,
  "type": 0,
  "startindex": 0
}
```

| 名称 | 值 | 说明（初始版本：V2R3C10） | 变更版本 |
| --- | --- | --- | --- |
| equiptypeid | | 设备类型 id | |
| equipid | | 设备 id | |
| type | | 信号类型。0：采集参数；1：运行参数；2：控制参数；4：显示配置 | |
| startindex | | 起始分组下标（包含起始下标所在分组，取值范围（0~当前设备组数-1）） | |

**响应信息（采集信号参数响应格式）：**

```json
{
  "errcode": 0,
  "isfinished": 1,
  "groupinfo": [
    {
      "groupname": "",
      "groupindex": 0,
      "isbiv": 0,
      "siginfo": [  // 采集信号信息（非二维表展示场景下）
        {
          "id": 0,
          "name": "",
          "unit": "",
          "val": "",
          "enumkey": 0,
          "type": 0,
          "level": 0,
          "dispflag": 0,
          "precision": 0
        }
      ]
    },
    {
      "groupname": "",
      "groupindex": 0,
      "isbiv": 1,
      "siginfo": [  // 采集信号信息（二维表展示场景下）
        {
          "dispflag": 0,
          "titlename": "",
          "rowtitle": [
            {"titlename": "", "titlelevel": 0, "dispflag": 0}
          ],
          "coltitle": [
            {"titlename": "", "titlelevel": 0, "dispflag": 0}
          ],
          "tableinfo": [
            [
              {"id": 0, "val": ""}
            ]
          ]
        }
      ]
    }
  ]
}
```

| 名称 | 值 | 说明 | 变更版本 |
| --- | --- | --- | --- |
| errcode | int | 0：成功；其他：失败 | |
| isfinished | int | 是否所有分组都获取完成。0：没有获取完成；1：获取完成 | |
| groupinfo | [ ] | 分组信息数组 | |
| groupname | string | 分组名称 | |
| groupindex | int | 分组下标 | |
| isbiv | int | 是否以二维表展示。0：非二维表展示；1：二维表展示 | |

**siginfo 字段（非二维表展示场景下）：**

| 名称 | 值 | 说明 |
| --- | --- | --- |
| id | int | 信号 id |
| name | string | 信号名称 |
| unit | string | 信号单位 |
| val | string | 信号值 |
| enumkey | | 枚举信号的原始值（非枚举信号不携带该字段） |
| type | | 信号数据类型 |
| level | | 信号等级。0：常用；1：高级 |
| dispflag | | 信号显示标记。0：不显示；1：显示 |
| precision | | 信号值精度 |

**siginfo 字段（二维表展示场景下）：**

| 名称 | 值 | 说明 |
| --- | --- | --- |
| dispflag | | 二维表表显示标记。0：不显示；1：显示 |
| titlename | string | 表格第一行第一列标题 |
| rowtitle | [ ] | 行标题数组，元素为 {titlename（标题名称）, titlelevel（标题所在行等级，0：常用行；1：高级行）, dispflag（行显示标记，0：不显示；1：显示）} |
| coltitle | [ ] | 列标题数组，元素为 {titlename（标题名称）, titlelevel（标题所在列等级，0：常用列；1：高级列）, dispflag（列显示标记，0：不显示；1：显示）} |
| tableinfo | [ ] | 二维表数据数组，每个元素为一行，行由 {id（信号 id）, val（信号值）} 数组组成 |

### 示例

**前台请求使用：**
```
get_monitor_info.asp?type=20&para1=value&para2=&para3=&para4=&para5=&para6=
```

**平台前台调用：**
```
<% GetMonitorInfo(20, para1, para2, para3, para4, para5, para6);%>
```

后台是否记录操作记录：无需记录操作记录。

---

## Type=21　获取当前设备的活动告警（json 形式）

### 参数

**请求信息：**
```
get_monitor_info.asp?Type=type&para1=para1&para2=&para3=&para4=&para5=&para6=
```

**Type 参数：**

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| Type | | 21：查询当前设备所有活动告警信息，json 格式 |

**Para1 参数（JSON）：

```json
{
  "equipid": 0,
  "equipidlist": []
}
```

| 名称 | 值 | 说明 | 变更版本（初始版本：V2R3C20） |
| --- | --- | --- | --- |
| equipid | "" | 0：所有设备；非0：指定设备Id | |
| equipidlist | [] | 优先使用 equipid 字段获取设备；若无 equipid，则使用 equipidlist 字段获取设备列表 | |

**响应信息：**

```json
{
  "errcode": "OK",
  "almlist": [
    {
      "seqno": 0,
      "almid": 0,
      "almname": "",
      "equipid": 0,
      "equiptypeid": 0,
      "equipname": "",
      "level": 0,
      "confirmstate": 0,
      "reason": "",
      "reasonum": 0,
      "description": "",
      "type": 0,
      "localtime": "YY-MM-DD HH:MM:SS",
      "faultDesc": "",
      "subReasonList": [{"subReason": "", "subRepair": ""}],
      "locationInfo": "",
      "counterNo": 0,
      "slotNo": 0
    }
  ]
}
```

| 名称 | 值 | 说明 | 变更版本 |
| --- | --- | --- | --- |
| errcode | | ERR：查询失败，无后面字段；OK：查询 ok | |
| almlist | [ ] | 活动告警数组 | |
| seqno | | 告警序列号 | |
| almid | | 告警ID | |
| almname | | 告警名称 | |
| equipid | | 设备ID | |
| equiptypeid | | 设备类型 id | |
| equipname | | 设备名称 | |
| level | | 告警级别 | |
| confirmstate | | 告警确认状态 | |
| reason | | 告警原因 | |
| reasonum | | 告警用户自定义（告警原因码） | |
| description | | 告警描述 | |
| type | | 告警类型 | |
| localtime | | 告警时间(YY-MM-DD HH:MM:SS) | |
| faultDesc | | 故障描述 | |
| subReasonList | [{subReason,subRepair}] | 可能原因及修复建议列表 | |
| locationInfo | str | 告警定位信息 | |
| counterNo | int | 柜号 | |
| slotNo | int | 槽位号 | |

### 示例

**前台请求使用：**
```
get_monitor_info.asp?Type=21&para1=para1&para2=&para3=&para4=&para5=&para6=
```

**平台前台调用：**
```
<% GetMonitorInfo(21, para1, para2, para3, para4, para5, para6);%>
```

后台是否记录操作记录：无需记录操作记录。

