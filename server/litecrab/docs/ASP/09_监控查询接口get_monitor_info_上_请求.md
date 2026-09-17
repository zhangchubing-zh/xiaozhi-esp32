# 接口名列表

- get_monitor_info.asp

---

# get_monitor_info.asp

### 接口名称

get_monitor_info.asp

### 功能

该接口用于前端（含 APP、WEB、平台前台等）通过 Type 参数区分不同用途，获取监控相关的各类数据信息。接口包含多个 Type 变体，分别实现不同的功能：

| Type | 功能 |
| --- | --- |
| 0 | 获取设备列表信息（app 使用） |
| 1 | 获取告警数量（app 使用） |
| 2 | 获取当前设备的活动告警（app 使用） |
| 3 | 获取采集信号信息（app 使用） |
| 4 | 获取采集信号值（app 使用） |
| 5 | 获取运行参数的信号值（app 使用） |
| 6 | 获取单个设置信号值（app 使用） |
| 7 | 获取运行控制信号值（app 使用） |
| 8 | 获取系统时间（app 使用） |
| 9 | 获取信号值范围 |
| 10 | 刷新设备列表（V2R2C10 增加了字段） |
| 11 | 获取二维表内容（app 使用） |
| 12 | 获取信号分组信息 |
| 13 | 获取一个分组的信号值 |
| 15 | 获取告警关联音响的信息 |
| 16 | 获取实时监控页签显示标识 |
| 17 | 获取采集信号的基本信息 |
| 18 | 获取设置常用信号的基本信息 |
| 19 | 获取多个设备的信号值 |
| 20 | 获取实时监控信号信息（JSON 格式数据）（V2R3C10 版本新增） |
| 21 | 获取当前设备的活动告警（json 形式） |

> 注：文档中未见 Type=14 变体的说明。

**后台接口：**
```
INT32 EMAP_AspGetMonitorInfo(INT32 eid, Webs *wp, INT32 argc, CHAR **argv);
```

**后台是否记录操作记录：** 所有 Type 变体均为"无需记录操作记录"。

**前台请求使用（通用格式）：**
```
get_monitor_info.asp?type=Type&para1=&para2=&para3=&para4=&para5=&para6=
```

**平台前台调用（通用格式）：**
```
<% GetMonitorInfo(type, para1, para2, para3, para4, para5, para6);%>
```

---

## Type=0　获取设备列表信息（app 使用）

### 参数

**请求信息（请求信息）：**

| 参数 | 值 | 说明 |
| --- | --- | --- |
| Type | 0 | 获取设备列表信息 |

**响应信息：**

信息域：
- Error: "ERR"
- Normal:
```
EquipId1~EquipTypeId1~EquipName1~FatherId1~DispAsGroup1~EquipTypeName1|EquipId2~EquipTypeId2~EquipName2~FatherId2~DispAsGroup2~EquipTypeName2|……|EquipIdN~EquipTypeIdN~EquipNameN~FatherIdN~DispAsGroupN~EquipTypeNameN|
```

| 内容 | 分隔符 | 说明 | 变更版本 |
| --- | --- | --- | --- |
| EquipId1 | ~ | 设备Id1 | |
| EquipTypeId1 | ~ | 设备类型Id1 | |
| EquipName1 | ~ | 设备名称1 | |
| FatherId1 | ~ | 父节点Id1 | |
| DispAsGroup1 | ~ | 是否以群展示该设备1类型，1 表示群展示，0 表示普通 | V2R2C10 新增加 |
| EquipTypeName1 | \| | 设备类型名1 | V2R2C10 新增加 |
| …… | …… | …… | |
| EquipIdN | ~ | 设备IdN | |
| EquipTypeIdN | ~ | 设备类型IdN | |
| EquipNameN | ~ | 设备名称N | |
| FatherIdN | ~ | 父节点IdN | |
| DispAsGroupN | ~ | 是否以群展示该设备N类型，1 表示群展示，0 表示普通 | V2R2C10 新增加 |
| EquipTypeNameN | \| | 设备类型名N | V2R2C10 新增加 |

**JSON 格式返回：**

```json
{
  "errCode": 0,
  "equipList": [
    {
      "equipId": 0,
      "equipTypeId": 0,
      "equipName": "xxx",
      "equipTypeName": "xxx",
      "fatherId": 0,
      "dispAsGroup": 0
    }
  ]
}
```

| 名称 | 值 | 说明 |
| --- | --- | --- |
| errCode | int | 0：查询成功；其他：查询失败，失败时后面的字段没有 |
| equipList | [ ] | 设备列表 |
| equipId | int | 设备 id |
| equipTypeId | int | 设备类型 id |
| equipName | string | 设备名称 |
| equipTypeName | string | 设备类型名称 |
| fatherId | int | 父设备 id，没父设备时为 0 |
| dispAsGroup | int | 是否以群设备展示，0：否，1：是 |

### 示例

**前台请求使用：**
```
get_monitor_info.asp?type=0&para1=&para2=&para3=&para4=&para5=&para6=
```

**平台前台调用：**
```
<% GetMonitorInfo(0, para1, para2, para3, para4, para5, para6);%>
```

后台是否记录操作记录：无需记录操作记录。

---

## Type=1　获取告警数量（app 使用）

### 参数

**请求信息：**

| 参数 | 值 | 说明 |
| --- | --- | --- |
| Type | 1 | 获取告警数量 |

**响应信息：**

信息域：
- Error: "ERR"
- Normal: `CriticalNum|MajorNum|MinorNum|WarningNum`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| CriticalNum | \| | 严重告警数量 |
| MajorNum | \| | 主要告警数量 |
| MinorNum | \| | 一般告警数量 |
| WarningNum | | 次要告警数量 |

### 示例

**前台请求使用：**
```
get_monitor_info.asp?type=1&para1=&para2=&para3=&para4=&para5=&para6=
```

**平台前台调用：**
```
<% GetMonitorInfo(1, para1, para2, para3, para4, para5, para6);%>
```

后台是否记录操作记录：无需记录操作记录。

---

## Type=2　获取当前设备的活动告警（app 使用）

### 参数

**请求信息：**

| 参数 | 参数类型 | 说明 |
| --- | --- | --- |
| Type | 2 | 获取当前设备活动告警 |
| Para1 | EquipId | 设备类型。0：所有活动告警统计；其他：指定设备的活动告警统计 |

**响应信息：**

信息域：
- Error: "ERR"
- Normal:
```
Total|AlarmSeqNo~AlarmId~EquipId~AlarmName~AlarmTime~AlarmLevel~AlarmConfirmState~AlarmReason~AlarmUsrDefine~AlarmDescription~AlarmType~AlarmLocalTime|……|AlarmSeqNo~AlarmId~EquipId~AlarmName~AlarmTime~AlarmLevel~AlarmConfirmState~AlarmReason~AlarmUsrDefine~AlarmDescription~AlarmType~AlarmLocalTime|
```

| 内容 | 分隔符 | 说明 | 变更版本 |
| --- | --- | --- | --- |
| Total | \| | 活动告警总数 | |
| AlarmSeqNo | ~ | 告警序号 | |
| AlarmId | ~ | 告警Id | |
| EquipId | ~ | 设备Id | |
| EquipName | | 设备名称 | |
| AlarmName | ~ | 告警名称 | |
| AlarmTime | ~ | 告警时间 | |
| AlarmLevel | ~ | 告警级别（0:紧急 1:重要 2:次要 3:提示） | |
| AlarmConfirmState | ~ | 告警确认状态 | |
| AlarmReason | ~ | 告警原因 | |
| AlarmUsrDefine | ~ | 告警用户自定义（告警原因码） | 历史版本有此字段，但未使用，V2R3C00 版本被告警合并功能使用 |
| AlarmDescription | ~ | 告警描述 | |
| AlarmType | ~ | 告警类型（0: ADAC，其他:ADMC） | |
| AlarmLocalTime | \| | 告警时间(YY-MM-DD HH:MM:SS) | |
| …… | …… | …… | |

### 示例

**前台请求使用：**
```
get_monitor_info.asp?type=2&para1=&para2=&para3=&para4=&para5=&para6=
```

**平台前台调用：**
```
<% GetMonitorInfo(2, para1, para2, para3, para4, para5, para6);%>
```

后台是否记录操作记录：无需记录操作记录。

---

## Type=3　获取采集信号信息（app 使用）

### 参数

**请求信息：**
```
get_monitor_info.asp?type=Type&para1=EquipId&para2=EquipTypeId&para3=&para4=&para5=&para6=
```

| 参数 | 参数类型 | 说明 |
| --- | --- | --- |
| Type | 3 | 获取采集信号的信息 |
| Para1 | EquipId | 设备Id |
| Para2 | EquipTypeId | 设备类型Id |
| Para3 | GetSigNumFlag | 是否只获取信号分组个数，需要时仅返回 SampSigGroupNum（该字段可省略，默认不需要）。0：不需要 1：需要 |
| Para4 | CheckIsDisplayTable | 是否以表格显示（该字段可省略，默认不需要）。0：不需要 1：需要 |

**响应信息：**

信息域：
- Error: "ERR"
- Normal:
```
Total|AlarmSeqNo~AlarmId~EquipId~AlarmName~AlarmTime~AlarmLevel~AlarmConfirmState~AlarmReason~AlarmUsrDefine~AlarmDescription~AlarmType~AlarmLocalTime|……|AlarmSeqNo~AlarmId~EquipId~AlarmName~AlarmTime~AlarmLevel~AlarmConfirmState~AlarmReason~AlarmUsrDefine~AlarmDescription~AlarmType~AlarmLocalTime|
```
（注：文档中响应 Normal 格式与 Type=2 相同，疑为文档笔误，实际响应应为采集信号分组信息。）

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| SampSigGroupNum | \| | 采集信号的组数。当（组数为 0）或者（GetSigNumFlag == 1 且组数 > 1）时，无后续字段 |
| SigGroupName | ^ | 信号组名 |
| IsDispTable | ~ | 是否以表格显示（CheckIsDisplayTable = 0 时不显示该字段）。EMAP V2R2C10 修改：IsDispTable 为 1 时此分组后续信息不带 |
| SameSigNum | ~ | 采集信号个数（如果 SameSigNum=0，无后续字段） |
| SigId1 | ^ | 信号Id |
| SigFullName1 | ^ | 信号全名 |
| SigUnit1 | ~ | 信号单位 |
| SigId2 | ^ | 信号Id |
| SigFullName2 | ^ | 信号全名 |
| SigUnit2 | ~ | 信号单位 |
| …… | …… | …… |
| SigIdM | ^ | 信号Id |
| SigFullNameM | ^ | 信号全名 |
| SigUnitM | \| | 信号单位 |
| SigGroupName | ^ | 信号组名（第二组开始） |
| IsDispTable | ~ | 是否以表格显示（CheckIsDisplayTable = 0 时不显示该字段） |
| SameSigNum | ~ | 采集信号个数（如果 SameSigNum=0，无后续字段） |
| …… | …… | …… |

### 示例

**前台请求使用：**
```
get_monitor_info.asp?type=3&para1=EquipId&para2=EquipTypeId&para3=&para4=&para5=&para6=
```

**平台前台调用：**
```
<% GetMonitorInfo(3, para1, para2, para3, para4, para5, para6);%>
```

后台是否记录操作记录：无需记录操作记录。

---

## Type=4　获取采集信号值（app 使用）

### 参数

**请求信息：**
```
get_monitor_info.asp?type=Type&para1=EquipId&para2=EquipTypeId&para3=SigType&para4=&para5=&para6=
```

| 参数 | 参数类型 | 说明 |
| --- | --- | --- |
| Type | 4 | 获取采集信号的值 |
| Para1 | EquipId | 设备Id |
| Para2 | EquipTypeId | 设备类型Id |
| Para3 | SigType | 信号类型。0，采集；1，设置；2，控制；4，显示配置 |

**响应信息：**

信息域：
- Error: "ERR"
- Normal: `"EquipId|SigId~SigValue|..."`

| 内容 | 分隔符 | 说明 | 修改版本 |
| --- | --- | --- | --- |
| EquipId | \| | 设备Id | |
| SigId1 | ~ | 信号Id1 | |
| SigVal1 | ~ | 信号值1 | |
| SigLevel1 | \| | 信号级别。0：常用信号 1：高级信号 | V200R002C10 新增 |
| …… | …… | …… | |
| SigIdN | ~ | 信号IdN | |
| SigValN | \| | 信号值N | |
| SigLevelN | \| | 信号级别。0：常用信号 1：高级信号 | V200R002C10 新增 |

### 示例

**前台请求使用：**
```
get_monitor_info.asp?type=4&para1=EquipId&para2=EquipTypeId&para3=SigType&para4=&para5=&para6=
```

**平台前台调用：**
```
<% GetMonitorInfo(4, para1, para2, para3, para4, para5, para6);%>
```

后台是否记录操作记录：无需记录操作记录。

---

## Type=5　获取运行参数的信号值（app 使用）

### 参数

**请求信息：**
```
get_monitor_info.asp?type=Type&para1=EquipId&para2=EquipTypeId&para3=&para4=&para5=&para6=
```

| 参数 | 参数类型 | 说明 |
| --- | --- | --- |
| Type | 5 | 获取采集信号的值 |
| Para1 | EquipId | 设备Id |
| Para2 | EquipTypeId | 设备类型Id |

**响应信息：**

信息域：
- Error: "ERR"
- Normal:
```
"EquipId|SigGroupNum|SigGroupName1~SigNum1~SigType11^SigPara11……SigType1^SigPara1n~……|SigGroupNamem~SigNumm~SigTypem1^SigParam1……SigTypemn~"
```

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| EquipId | \| | 设备Id |
| SigGroupNum | \| | 信号组数 |
| SigGroupName1 | ~ | 组名 |
| SigNum1 | ~ | 信号个数 |
| SigType11 | ^ | 信号类型 |
| SigPara11 | ~ | 信号参数 |
| …… | …… | …… |
| SigType1n | ^ | 信号类型 |
| SigPara1n | ~\| | 信号参数 |
| …… | | …… |
| SigGroupNamem | ~ | 组名 |
| SigNumm | ~ | 信号个数 |
| SigTypem1 | ^ | 信号类型 |
| SigParam1 | ~ | 信号参数 |
| …… | …… | …… |
| SigTypemn | ^ | 信号类型 |
| SigParamn | ~ | 信号参数 |

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
| EnumSigValue | ^ | 枚举信号值 | |
| SigAuthority | ^ | 信号显示权限 | |
| SigCtrlFlag | ^ | 信号控制标记 | |
| SigGrade | ~ | 信号级别。0：常用信号 1：高级信号 | V200R002C10 新增 |

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
| SigAuthority | ^ | 信号权限 | |
| SigCtrlFlag | ^ | 信号控制标志 | |
| SigUnit | ^ | 信号单位 | |
| SigGrade | ~ | 信号级别。0：常用信号 1：高级信号 | V200R002C10 新增 |

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
| SigGrade | ~ | 信号级别。0：常用信号 1：高级信号 | V200R002C10 新增 |

### 示例

**前台请求使用：**
```
get_monitor_info.asp?type=5&para1=EquipId&para2=EquipTypeId&para3=&para4=&para5=&para6=
```

**平台前台调用：**
```
<% GetMonitorInfo(5, para1, para2, para3, para4, para5, para6);%>
```

后台是否记录操作记录：无需记录操作记录。

---

## Type=6　获取单个设置信号值（app 使用）

### 参数

**请求信息：**
```
get_monitor_info.asp?type=Type&para1=EquipId&para2=EquipTypeId&para3=SigId=SigVal&para4=&para5=&para6=
```

| 参数 | 参数类型 | 说明 |
| --- | --- | --- |
| Type | 6 | 获取采集信号的值 |
| Para1 | EquipId | 设备Id |
| Para2 | EquipTypeId | 设备类型Id |
| Para3 | SigId 和 SigVal | 信号Id 和信号值 |

**响应信息：**

信息域：
- Error: "ERR"
- Normal: `"ReturnCode | EquipId | SigId | SigSetRes"`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| ReturnCode/FaiulReson | \| | 返回值/失败原因 |
| EquipId | \| | 设备Id |
| SigId | \| | 信号Id |
| SigSetRes | | 信号设置结果 |

### 示例

**前台请求使用：**
```
get_monitor_info.asp?type=6&para1=EquipId&para2=EquipTypeId&para3=SigId=SigVal&para4=&para5=&para6=
```

**平台前台调用：**
```
<% GetMonitorInfo(6, para1, para2, para3, para4, para5, para6);%>
```

后台是否记录操作记录：无需记录操作记录。

---

## Type=7　获取运行控制信号值（app 使用）

### 参数

**请求信息：**
```
get_monitor_info.asp?type=Type&para1=EquipId&para2=EquipTypeId&para3=&para4=&para5=&para6=
```

| 参数 | 参数类型 | 说明 |
| --- | --- | --- |
| Type | 7 | 获取采集信号的值 |
| Para1 | EquipId | 设备Id |
| Para2 | EquipTypeId | 设备类型Id |

**响应信息：**

信息域：
- Error: "ERR"
- Normal:
```
"EquipId|SigGroupNum|SigGroupName1~SigNum1~SigType11^SigPara11……SigType1^SigPara1n~……|SigGroupNamem~SigNumm~SigTypem1^SigParam1……SigTypemn~"
```

字段说明与 Type=5 相同，包括 SigType 为 Enum 类型、String 类型、其他类型时对应的 SigPara 格式（详见 Type=5 部分）。此处完整列出各字段：

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| EquipId | \| | 设备Id |
| SigGroupNum | \| | 信号组数 |
| SigGroupName1 | ~ | 组名 |
| SigNum1 | ~ | 信号个数 |
| SigType11 | ^ | 信号类型 |
| SigPara11 | ~ | 信号参数 |
| …… | …… | …… |
| SigType1n | ^ | 信号类型 |
| SigPara1n | ~\| | 信号参数 |
| …… | | …… |
| SigGroupNamem | ~ | 组名 |
| SigNumm | ~ | 信号个数 |
| SigTypem1 | ^ | 信号类型 |
| SigParam1 | ~ | 信号参数 |
| …… | …… | …… |
| SigTypemn | ^ | 信号类型 |
| SigParamn | ~ | 信号参数 |

**SigType 为 Enum 类型时**，SigPara 格式（与 Type=5 一致）：
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
| EnumSigValue | ^ | 枚举信号值 | |
| SigAuthority | ^ | 信号显示权限 | |
| SigCtrlFlag | ^ | 信号控制标记 | |
| SigGrade | ~ | 信号级别。0：常用信号 1：高级信号 | V200R002C10 新增 |

**SigType 为 String 类型时**，SigPara 格式（与 Type=5 一致）：
```
SigId^SigFullName^SigValLen^SigValue^SigAuthority^SigCtrlFlag^SigUnit
```

| SigPara 内容 | 分隔符 | 说明 | 修改版本 |
| --- | --- | --- | --- |
| SigId | ^ | 信号Id | |
| SigFullName | ^ | 信号全名 | |
| SigValLen | ^ | 信号值长度 | |
| SigValue | ^ | 信号值 | |
| SigAuthority | ^ | 信号权限 | |
| SigCtrlFlag | ^ | 信号控制标志 | |
| SigUnit | ^ | 信号单位 | |
| SigGrade | ~ | 信号级别。0：常用信号 1：高级信号 | V200R002C10 新增 |

**SigType 为其他类型时**，SigPara 格式（与 Type=5 一致）：
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
| SigGrade | ~ | 信号级别。0：常用信号 1：高级信号 | V200R002C10 新增 |

### 示例

**前台请求使用：**
```
get_monitor_info.asp?type=7&para1=EquipId&para2=EquipTypeId&para3=&para4=&para5=&para6=
```

**平台前台调用：**
```
<% GetMonitorInfo(7, para1, para2, para3, para4, para5, para6);%>
```

后台是否记录操作记录：无需记录操作记录。

---

## Type=8　获取系统时间（app 使用）

### 参数

**请求信息：**

| 参数 | 值 | 说明 |
| --- | --- | --- |
| Type | 8 | 获取系统时间 |

**响应信息：**

信息域：
- Error: "ERR"
- Normal: `"TimePara"`

TimePara 字段：

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| Year | - | 年 |
| Month | - | 月 |
| Day | | 日 |
| Hour | ： | 时 |
| Minute | | 分 |

### 示例

**前台请求使用：**
```
get_monitor_info.asp?type=8&para1=&para2=&para3=&para4=&para5=&para6=
```

**平台前台调用：**
```
<% GetMonitorInfo(8, para1, para2, para3, para4, para5, para6);%>
```

后台是否记录操作记录：无需记录操作记录。

---

## Type=9　获取信号值范围

### 参数

**请求信息：**

| 参数 | 参数类型 | 说明 |
| --- | --- | --- |
| Type | 9 | 获取设置信号范围 |
| Para1 | EquipId | 设备Id |
| Para2 | EquipTypeId | 设备类型Id |

**响应信息：**

信息域：
- Error: "ERR"
- Normal: `"EquipId|SigId1~SigValMin^SigValMax&……SigIdN~SigValMin^SigValMax&"`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| EquipId | \| | 设备Id |
| SigId1 | ~ | 信号Id |
| SigValMin | ^ | 信号最大值 |
| SigValMax | & | 信号最小值 |
| …… | …… | …… |
| SigIdN | ~ | 信号Id |
| SigValMin | ^ | 信号最大值 |
| SigValMax | & | 信号最小值 |

> 注：文档中 SigValMin 说明为"信号最大值"、SigValMax 说明为"信号最小值"，疑为文档笔误（按字段名应为 SigValMin=信号最小值、SigValMax=信号最大值）。

### 示例

**前台请求使用：**
```
get_monitor_info.asp?type=9&para1=EquipId&para2=EquipTypeId&para3=&para4=&para5=&para6=
```

**平台前台调用：**
```
<% GetMonitorInfo(9, para1, para2, para3, para4, para5, para6);%>
```

后台是否记录操作记录：无需记录操作记录。

---

## Type=10　刷新设备列表（V2R2C10 增加了字段）

### 参数

**请求信息：**

| 参数 | 值 | 说明 |
| --- | --- | --- |
| Type | 10 | 刷新设备列表 |

**响应信息：**

信息域：
- Error: "ERR"
- Normal: `EquipId~EquipTypeId~EquipName~FatherId~DispAsGroup~EquipTypeName|`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| EquipId | ~ | 设备ID |
| EquipTypeId | ~ | 设备类型ID |
| EquipName | ~ | 设备名称 |
| FatherId | ~ | 所属ID |
| DispAsGroup | ~ | 群设备展示标识：0：非群展示设备；1：群展示设备 |
| EquipTypeName | \| | 设备类型名称 |

### 示例

**前台请求使用：**
```
get_monitor_info.asp?type=10&para1=&para2=&para3=&para4=&para5=&para6=
```

**平台前台调用：**
```
<% GetMonitorInfo(10, para1, para2, para3, para4, para5, para6);%>
```

后台是否记录操作记录：无需记录操作记录。

---

## Type=11　获取二维表内容（app 使用）

### 参数

**请求信息：**

| 参数 | 参数类型 | 说明 |
| --- | --- | --- |
| Type | 11 | 获取信号分组信息 |
| Para1 | EquipId | 设备Id |
| Para2 | EquipTypeId | 设备类型Id |
| Para3 | SigType | 信号类型。1，采集；2，设置；3，控制；4，显示配置 |
| Para4 | GroupIndex | 组序号 |

**响应信息：**

信息域：
- Error: "ERR"
- Normal: `" "`

**SigType 为采集、显示配置信号时，格式如下：**

| 内容 | 分隔符 | 说明 | 修改版本 |
| --- | --- | --- | --- |
| SigGroupNum | \| | 信号分组数量 | |
| GroupIndex | ^ | 配置表中 index | |
| R1C1 name | ~ | 第一行第一列名称 | |
| R1C2 name | ~ | 第一行第二列名称 | |
| …… | …… | …… | |
| R1CN name | ~ | 第一行第 N 列名称 | |
| R2C1 linenum | ~ | 第二行第一列行号 | |
| R2C1 name | ~ | 第二行第一列名称 | |
| R2C2 sigId | ~ | 第二行第二列信号 id | |
| R2C2 sigvalue | ~ | 第二行第二列信号值 | |
| …… | …… | …… | |
| R2CN sigid | ~ | 第二行第 N 列信号 id | |
| R2CN sigvalue | ~ | 第二行第 N 列信号值 | |
| R2 sig level | ~ | 第二行信号级别。0：常用信号 1：高级信号 | V200R002C10 新增，对应 R2 信号的信号级别 |
| RMC1 linenum | ~ | 第 M 行第一列行号 | |
| RMC1 name | ~ | 第 M 行第一列名称 | |
| RMC2 sigid | ~ | 第 M 行第二列信号 id | |
| RMC2 sigvalue | ~ | 第 M 行第二列信号值 | |
| …… | …… | …… | |
| RMCN sigid | ~ | 第 M 行第 N 列信号 id | |
| RMCN sigvalue | ~ | 第 M 行第 N 列信号值 | |
| RM sig level | ~ | 第 M 行信号级别。0：常用信号 1：高级信号 | V200R002C10 新增，对应 RM 信号的信号级别 |
| C1 sig level | ~ | 第 1 列信号级别。0：常用信号 1：高级信号 | V200R002C10 新增，对应 C1 信号的信号级别 |
| C2 sig level | ~ | 第 2 列信号级别。0：常用信号 1：高级信号 | V200R002C10 新增，对应 C2 信号的信号级别 |
| …… | …… | …… | |
| CN sig level | ~ | 第 N 列信号级别。0：常用信号 1：高级信号 | V200R002C10 新增，对应 CN 信号的信号级别 |
| GroupIndex | ^ | 配置表中 index（下一组开始） | |
| …… | …… | …… | |

**SigType 为设置信号时（V2R2C10 版本新增），格式如下：**

| 内容 | 分隔符 | 说明 | 修改版本 |
| --- | --- | --- | --- |
| EquipId | \| | 设备ID | |
| GroupName | \| | 组名 | |
| R1C1 name | ~ | 第一行第一列名称 | |
| R1C2 name | ~ | 第一行第二列名称 | |
| …… | …… | …… | |
| R1CN name | ~ | 第一行第 N 列名称 | |
| R2C1 linenum | ~ | 第二行第一列行号 | |
| R2C1 name | ~ | 第二行第一列名称 | |
| R2C2 sigType | ^ | 第二行第二列信号类型 | |
| R2C2 sigPara | ~ | 第二行第二列信号参数 | |
| …… | …… | …… | |
| R2CN sigType | ^ | 第二行第 N 列信号类型 | |
| R2CN sigPara | ~ | 第二行第 N 列信号参数 | |
| R2 siglevel | ~ | 第二行信号级别 | |
| R2 auth | ~ | 第二行权限 | |
| R2 Other Data | \| | 第二行其他信息 | V2r2c20 新增，修改前无该字段，前台获取字符串尾部为"~\|"，修改后字符串尾部为"~R2 Other Data\|" |
| RMC1 linenum | ~ | 第 M 行第一列行号 | |
| RMC1 name | ~ | 第 M 行第一列名称 | |
| RMC2 sigType | ^ | 第 M 行第二列信号类型 | |
| RMC2 sigPara | ~ | 第 M 行第二列信号参数 | |
| …… | …… | …… | |
| RMLN sigType | ^ | 第 M 行第 N 列信号类型 | |
| RMLN sigPara | ~ | 第 M 行第 N 列信号参数 | |
| RM sig level | ~ | 第 M 行信号级别。0：常用信号 1：高级信号 | V200R002C10 新增，对应 RMCN sigId 信号的信号级别 |
| RM auth | ~ | 第 M 行权限 | |
| RM Other Data | \| | 第 M 行其他信息 | V2r2c20 新增，修改前无该字段，前台获取字符串尾部为"~\|"，修改后字符串尾部为"~RM Other Data\|" |
| C1 sig level | ~ | 第 1 列信号级别。0：常用信号 1：高级信号 | V200R002C10 新增，对应 C1 信号的信号级别 |
| …… | …… | …… | |
| CN sig level | ~ | 第 N 列信号级别。0：常用信号 1：高级信号 | V200R002C10 新增，对应 CN 信号的信号级别 |
| C1 auth | ~ | 第 1 列权限 | |
| …… | …… | | |
| CN auth | ~ | 第 N 列权限 | |

> 其中 sigType 为 Enum 类型、String 类型、其他类型时，对应的 SigPara 与 Type=5（获取一个分组的信号值）所描述的一致。

**RM Other Data 说明：**
该字段中不允许使用 "~"、"|" 作为分隔符。数据类型和数据信息按照类 TLV 的格式进行填充，每一种 T 对应一种 DataType Info。

| 内容 | 分隔符 | 说明 | 修改版本 |
| --- | --- | --- | --- |
| DataTypeNum | ^ | 表示后续有多少种数据类型。每种类型需按照"^"符号进行分割 | |
| DataType1 | ^ | 数据类型1。1：当前表格内显示数值信息的列信号的显示表达式计算结果 | |
| DataType1 Num | ^ | 数据类型1 个数 | |
| DataType1 Info | ^ | 数据类型1 信息 | |
| …… | …… | …… | |
| DataTypeN | ^ | 数据类型N | |
| DataTypeN Num | ^ | 数据类型N 个数 | |
| DataTypeN Info | ^ | 数据类型M 信息。1：当前表格内显示数值信息的列信号的显示表达式计算结果 | |

**DataType = 1（当前行每列信号的显示表达式计算结果）：**

| 内容 | 分隔符 | 说明 | 修改版本 |
| --- | --- | --- | --- |
| Sig1 Exp Flag | = | 第一列信号的信号显示表达式结果。0：不显示；1：显示 | |
| Sig2 Exp Flag | = | 第二列信号的信号显示表达式结果 | |
| …… | …… | …… | |
| SigN Exp Flag | = | 第 N 列信号的信号显示表达式结果 | |

### 示例

**前台请求使用：**
```
get_monitor_info.asp?type=11&para1=EquipId&para2=EquipTypeId&para3=SigType&para4=GroupIndex&para5=&para6=
```

**平台前台调用：**
```
<% GetMonitorInfo(11, para1, para2, para3, para4, para5, para6);%>
```

后台是否记录操作记录：无需记录操作记录。

---

