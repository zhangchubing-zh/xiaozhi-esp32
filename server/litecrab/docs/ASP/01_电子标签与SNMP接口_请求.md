# 8.1 ASP 请求 —— 全部 HTTP 请求接口文档

**文档来源**：《数字能源WEB接口协议（WEB前后台）》第 8 章「普通消息格式」8.1 节「ASP 请求」。

**请求方式**：ASP 请求 POST 和 GET 方式均支持；建议获取数据用 GET，下发设置用 POST。

**说明**：本节共 **105** 个 ASP 接口。每个接口按四部分整理：**接口名称**、**功能**、**参数**（区分「请求信息」与「响应信息」，并给出分隔符）、**示例**（前台请求使用 URL 与平台前台调用）。原文未给出的字段标注为「（文档未给出）」；标注「退化接口 / 代码已删除」等后缀的为已废弃接口。

## 接口总览（105 个）

| 序号 | 接口 | 序号 | 接口 | 序号 | 接口 |
|---|---|---|---|---|---|
| 1. get_elabel_info_gradually.asp | | 36. set_ntp_server_info.asp | | 71. get_wifi_comp_para.asp |
| 2. get_elable_export_tarname.asp | | 37. get_update_info_ex.asp | | 72. get_func_state.asp |
| 3. get_elable_info.asp | | 38. get_trim_func_state.asp | | 73. set_func_state.asp |
| 4. set_snmp_info.asp | | 39. set_update_clear.asp | | 74. set_signals_info.asp |
| 5. set_snmp_trap.asp | | 40. clear_export_info.asp（退化接口） | | 75. get_modify_pwd_user_list.asp |
| 6. get_snmp_info.asp | | 41. get_data_export_tarname.asp | | 76. set_default_users_pwd.asp |
| 7. get_snmp_trap_info.asp | | 42. get_export_info.asp | | 77. get_monitor_service_status.asp |
| 8. get_bin_connect_mode.asp | | 43. get_export_start.asp | | 78. set_monitor_service_status.asp |
| 9. get_mib_export_tarname.asp | | 44. get_start_export_file_with_attribute.asp | | 79. get_security_log.asp |
| 10. set_debug_info.asp | | 45. set_export_info.asp | | 80. set_sys_domain_info.asp |
| 11. set_long_poll_info.asp | | 46. get_common_info.asp | | 81. start_neteco_connect_test.asp |
| 12. set_long_poll_start_info.asp | | 47. set_common_info.asp | | 82. get_resource_occupancy.asp |
| 13. get_special_implication.asp | | 48. get_cert_info.asp | | 83. get_cipher_suite.asp |
| 14. get_verify_code.asp | | 49. get_language_info.asp | | 84. set_cipher_suite.asp |
| 15. get_verify_display_state.asp | | 50. get_recert_enable.asp | | 85. get_uniform_cert_upload_state.asp |
| 16. get_vlan_para_info.asp | | 51. set_recert.asp | | 86. get_uniform_cert_info.asp |
| 17. request_client_auto_logout_time.asp | | 52. get_history_info.asp | | 87. set_import_uniform_cert.asp |
| 18. set_assigned_neteco_upload_cert_info.asp | | 53. get_monitor_info.asp | | 88. get_ip_whitelist_info.asp |
| 19. set_assigned_upload_cert_info.asp | | 54. get_metadata_info.asp | | 89. set_ip_whitelist_info.asp |
| 20. set_bin_connect_mode.asp | | 55. set_measure_info.asp | | 90. get_unitary_export_para.asp |
| 21. get_multi_sig_value.asp | | 56. set_metadata_info.asp | | 91. set_language_info.asp |
| 22. get_user_info.asp | | 57. set_monitor_info.asp | | 92. get_time_refresh_data.asp |
| 23. get_version_info.asp | | 58. get_res_cfg.asp | | 93. notify_download_packet_suc.asp |
| 24. get_event_notify_info.asp | | 59. get_time_info.asp | | 94. set_performance_collect.asp |
| 25. get_find_equipinfo.asp | | 60. set_reboot_cmd.asp | | 95. proc_sdr_export.asp |
| 26. get_snmp_equip_find_para.asp | | 61. get_measure_info.asp | | 96. get_trace_model_info.asp |
| 27. set_event_notify_info.asp | | 62. set_user_info.asp | | 97. import_ca_cert.asp |
| 28. set_find_equipinfo.asp | | 63. set_alarm_cfg.asp | | 98. delete_ca_cert.asp |
| 29. set_snmp_equip_find_para.asp | | 64. set_alarm_record.asp | | 99. get_ca_cert_details.asp |
| 30. get_alarm_cfg.asp | | 65. get_update_info.asp | | 100. get_access_protocol_info.asp |
| 31. get_alarm_record.asp | | 66. set_neteco_info.asp（目前a8不支持） | | 101. get_reg_cfg.asp |
| 32. get_live_app_auth_hash.asp（退化接口） | | 67. proc_first_login.asp | | 102. set_reg_cfg.asp |
| 33. get_site_info.asp | | 68. change_default_pwd.asp | | 103. get_snmp_equip_export_file_name.asp |
| 34. proc_unauthorized_request.asp | | 69. get_security_check_result.asp | | 104. get_login_display_data.asp |
| 35. request_user_state.asp | | 70. set_wifi_comp_para.asp | | 105. set_security_prompt_cfg.asp |

---

<!-- CHUNKS_BEGIN -->
# 接口名列表

1. get_elabel_info_gradually.asp
2. get_elable_export_tarname.asp
3. get_elable_info.asp
4. set_snmp_info.asp
5. set_snmp_trap.asp
6. get_snmp_info.asp
7. get_snmp_trap_info.asp
8. get_bin_connect_mode.asp
9. get_mib_export_tarname.asp
10. set_debug_info.asp
11. set_long_poll_info.asp
12. set_long_poll_start_info.asp
13. get_special_implication.asp
14. get_verify_code.asp
15. get_verify_display_state.asp
16. get_vlan_para_info.asp
17. request_client_auto_logout_time.asp
18. set_assigned_neteco_upload_cert_info.asp
19. set_assigned_upload_cert_info.asp
20. set_bin_connect_mode.asp
21. get_multi_sig_value.asp
22. get_user_info.asp
23. get_version_info.asp
24. get_event_notify_info.asp

---

### 接口名称

get_elabel_info_gradually.asp

### 功能

电子标签导出（分批/渐进式导出）接口，按 Type 区分不同的导出步骤。

- Type=0：发送请求启动电子标签导出，后台将显示电子标签的设备列表与导出用户绑定。
- Type=1：每发送一条请求导出 30 个电子标签，并显示导出进度；对于非信号的电子标签，一个设备类型算一个电子标签。
- Type=2：电子标签导出结果进度。
- Type=3：电子标签导出结果文件（返回导出的包名）。

后台接口：`INT32 EMAP_AspGetElabelGradually (INT32 eid, Webs *wp, INT32 argc, CHAR **argv);`

后台是否记录操作记录：
- Type=0/1/2：无需记录操作记录；
- Type=3：会记录导出完成操作记录。

### 参数

前台请求 URL 固定为：
`get_elabel_info_gradually.asp?type=0&para1=&para2=&para3=&para4=`

**请求信息（各 Type 的 Type 参数说明）：**

| Type | 分隔符 | 说明 |
| --- | --- | --- |
| Type=0 | （文档未给出） | 0：后台将显示电子标签的设备列表与导出用户绑定 |
| Type=1 | （文档未给出） | 1：每发送一条请求导出30个电子标签，并显示导出进度 |
| Type=2 | （文档未给出） | 2：电子标签导出结果 |
| Type=3 | 参数名为 sType | 3：电子标签导出结果文件 |

（para1/para2/para3/para4 含义文档未给出）

**响应信息（各 Type）：**

- Type=0：`Error: ERR`；`Normal: U16Ret | exportedEalbelNum | ElabelTotal`

| 参数 | 分隔符 | 说明 |
| --- | --- | --- |
| U16Ret | （文档未给出） | 0表示导出正常；2：其他用户正在导出 |
| exportedEalbelNum | （文档未给出） | 已经导出的电子标签数量（当U16Ret为0时有该字段） |
| ElabelTotal | （文档未给出） | 总共需要导出的电子标签数量（当U16Ret为0时有该字段） |

- Type=1：`Error: ERR`；`Normal: U16Ret`

| 参数 | 分隔符 | 说明 |
| --- | --- | --- |
| U16Ret | （文档未给出） | 0：表示导出正常；1:当前用户正在导出；2：其他用户正在导出；其他：参数错误 |

- Type=2：`Error: ERR`；`Normal: U16Ret | exportedEalbelNum | ElabelTotal`

| 参数 | 分隔符 | 说明 |
| --- | --- | --- |
| U16Ret | （文档未给出） | 0表示导出正常；1:当前用户正在导出；2：其他用户正在导出；其他：导出失败 |
| exportedEalbelNum | （文档未给出） | 已经导出的电子标签数量（当U16Ret为0时有该字段） |
| ElabelTotal | （文档未给出） | 总共需要导出的电子标签数量（当U16Ret为0时有该字段） |

- Type=3：`Error: ERR`；`Normal:`

| 参数 | 分隔符 | 说明 |
| --- | --- | --- |
| elabelTarName | （文档未给出） | 包名：获取导出的包名；ERR：获取导出包失败 |

### 示例

前台请求使用：
```
get_elabel_info_gradually.asp?type=0&para1=&para2=&para3=&para4=
```

平台前台调用：
```
<% ExportElabelByProgress (type, para1, para2, para3, para4);%>
```

---

### 接口名称

get_elable_export_tarname.asp

### 功能

导出电子标签（导出完成后获取导出的包名）。

后台接口：`INT32 WebAspExportElableData(INT32 eid, Webs *wp, INT32 argc, CHAR **argv)`

后台是否记录操作记录：会记录导出完成操作记录。

### 参数

**请求信息（发送信息）：** 发送请求时不需要携带参数（发送请求时无需携带参数）。

**响应信息：**

信息域：`Error: ERR`；`Normal:`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| TarName | 无 | 导出的包名 |

### 示例

前台请求使用：
```
./get_elable_export_tarname.asp?exporttype=&para1=&para2=&para3=&para4=
```

平台前台调用：
```
<%ExportElableData(exporttype, para1, para2, para3, para4);%>
```

---

### 接口名称

get_elable_info.asp

### 功能

获取电子标签内容。

后台接口：`extern INT32 EMAP_AspGetElableInfo( INT32 eid, Webs *wp, INT32 argc, CHAR **argv);`

后台是否记录操作记录：不记录操作记录。

### 参数

**请求信息：**

| 参数 | 参数类型 | 说明 |
| --- | --- | --- |
| equipId | Id | 设备Id |
| sigId | Id | 信号Id |
| equipName | Name | 设备名（V300R021C00及之后版本此字段保留，但无需赋具体值） |
| sigName | sName | 信号名（V300R021C00及之后版本此字段保留，但无需赋具体值） |

**响应信息：**

`Error: ERR`；`Normal: EquipId|SigId|ElableInfo`

| 参数 | 分隔符 | 说明 |
| --- | --- | --- |
| EquipId | （文档未给出） | 设备ID |
| SigId | （文档未给出） | 信号ID |
| ElableInfo | （文档未给出） | 返回信息 |

### 示例

前台请求使用：
```
get_elable_info.asp?equipId=Id&sigId=Id&equipName=name&sigName=name
```

平台前台调用：
```
<% GetElableInfo(equipId, sigId, equipName, sigName); %>
```

---

### 接口名称

set_snmp_info.asp

### 功能

设置 SNMP V3 用户信息接口，按 Type 区分不同操作。

- Type=0：增加 V3 用户信息（增加 SNMPV3 用户）。
- Type=1：修改 V3 用户信息（修改 SNMPV3 用户）。
- Type=2：删除 V3 用户信息（删除 SNMPV3 用户）。

后台接口：`Extern INT32 EMAP_AspSetSnmpV3User( INT32 eid, Webs *wp, INT32 argc, CHAR **argv);`

后台是否记录操作记录：Type=0/1/2 均会记录操作记录。

### 参数

前台请求为 POST 请求（$.post），Request 参数通过 post 体传递。

**请求信息（Type=0，增加 SNMPV3 用户）：**

| 参数 | 参数类型 | 说明 |
| --- | --- | --- |
| Type | （文档未给出） | 0：增加SNMPV3用户 |
| userName | （文档未给出） | 用户名 |
| MD5SHAEncrypt | （文档未给出） | MD5SHA加密 |
| MD5SHAPwd | （文档未给出） | MD5SHA密码 |
| DESAESEncrypt | （文档未给出） | DESAES加密 |
| DESAESPwd | （文档未给出） | DESAES密码 |
| PwdActiveTime | （文档未给出） | 密码有效期 |
| PwdNotifyTime | （文档未给出） | 密码到期前几天的提醒时间 |

**响应信息（Type=0）：**

`Error: "ERR"或者" "`（//添加失败）；`Normal:`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| Reslut | 无 | 0：添加成功；9：用户已存在；10：密码周期参数错误；23:认证密码密码由重复字符串组成,如a1a1a1a1a1；24:认证密码为弱口令；33:私有密码由重复字符串组成,如a1a1a1a1a1；34:私有密码为弱口令 |

**请求信息（Type=1，修改 SNMPV3 用户）：**

| 参数 | 参数类型 | 说明 |
| --- | --- | --- |
| Type | （文档未给出） | 1：修改SNMPV3用户 |
| userName | （文档未给出） | 用户名 |
| modifyType | （文档未给出） | 1：修改MD5/SHA密码；3：修改MD5/SHA密码、DES/AES密码；7：修改MD5/SHA密码、DES/AES密码、SNMP口令周期；15：修改MD5/SHA密码、DES/AES密码、SNMP口令周期、到期提醒时间（使用标志位进行或运算得出，修改MD5/SHA密码、DES/AES密码、口令周期、到期提醒时间对应的标志位分别为1、2、4、8） |
| MD5SHAEncrypt | （文档未给出） | MD5SHA加密 |
| MD5SHAPwd | （文档未给出） | MD5SHA密码 |
| DESAESEncrypt | （文档未给出） | DESAES加密 |
| DESAESPwd | （文档未给出） | DESAES密码 |
| PwdActiveTime | （文档未给出） | 密码有效期 |
| PwdNotifyTime | （文档未给出） | 密码到期前几天的提醒时间 |

**响应信息（Type=1）：**

`Error: "ERR"或者" "`（//修改失败）；`Normal:`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| Reslut | 无 | 0：修改成功；23:认证密码密码由重复字符串组成,如a1a1a1a1a1；24:认证密码为弱口令；33:私有密码由重复字符串组成,如a1a1a1a1a1；34:私有密码为弱口令 |

**请求信息（Type=2，删除 SNMPV3 用户）：**

| 参数 | 参数类型 | 说明 |
| --- | --- | --- |
| Type | （文档未给出） | 2：删除SNMPV3用户 |
| userName | （文档未给出） | 用户名 |
| port | （文档未给出） | 端口 |

**响应信息（Type=2）：**

`Error: "ERR"或者" "`（//删除失败）；`Normal:`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| Reslut | 无 | 0：删除成功；5：用户已关联SNMP TRAP，请先清除关联关系 |

### 示例

前台请求使用（POST）：
```
$.post("./set_snmp_info.asp", { type: Type, para1: userName, para2: MD5SHAEncrypt, para3: MD5SHAPwd, para4: DESAESEncrypt, para5: DESAESPwd, para6: PwdActiveTime, para7: PwdNotifyTime, para8:" ", para9:" ", para10:" "}
```

平台前台调用：
```
<% SetSnmpV3(type, para1, para2, para3, para4, para5, para6, para7, para8, para9, para10); %>
```

---

### 接口名称

set_snmp_trap.asp

### 功能

设置 SNMP Trap 信息接口，按 Type 区分不同操作。

- Type=0：增加 snmp trap 信息。
- Type=1：删除 snmp trap 信息。
- Type=2：修改 snmp trap 信息。

后台接口：`Extern INT32 EMAP_AspSetSnmpTrapInfo( INT32 eid, Webs *wp, INT32 argc, CHAR **argv);`

后台是否记录操作记录：Type=0/1/2 均会记录操作记录。

### 参数

前台请求为 POST 请求.

**请求信息（Type=0，增加 snmp trap 信息）：**

| 参数类型 | 参数 | 说明 |
| --- | --- | --- |
| type | typeId | 0：增加snmp trap信息 |
| para1 | ipAddress | IP地址 |
| para2 | port | 端口 |
| para3 | version | 端口版本 |
| para4 | trapRelation | Trap名 |

**响应信息（Type=0）：**

`Error: "ERR"或者" "`（//增加失败）；`Normal:`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| Reslut | 无 | 0：增加成功，其他：添加失败；4：v3用户不存在；10：trap已存在；11: trap共同体为弱口令 |

**请求信息（Type=1，删除 snmp trap 信息）：**

| 参数 | 参数类型 | 说明 |
| --- | --- | --- |
| type | （文档未给出） | 1：删除snmp trap信息 |
| ipAddress | （文档未给出） | IP地址 |
| port | （文档未给出） | 端口 |

**响应信息（Type=1）：**

`Error: "ERR"或者" "`（//删除失败）；`Normal:`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| Reslut | 无 | 0：删除成功 |

**请求信息（Type=2，修改 snmp trap 信息）：**

| 参数类型 | 参数 | 说明 |
| --- | --- | --- |
| type | typeId | 2：修改snmp trap信息 |
| para1 | ipAddress | IP地址 |
| para2 | port | 端口 |
| para3 | version | 端口版本 |
| para4 | trapRelation | Trap名 |

**响应信息（Type=2）：**

`ERR: "ERR"或者" "`（//修改失败）；`Normal:`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| Reslut | 无 | 0：修改成功；11: trap共同体为弱口令 |

### 示例

前台请求使用（POST）：
```
"./set_snmp_trap.asp", { type: ParaValue0, para1:ParaValue1, para2:ParaValue2, para3:ParaValue3, para4:ParaValue4, para5:ParaValue5, para6:ParaValue6,}
```

平台前台调用：
```
<% SetSnmpTrap(type, para1, para2, para3, para4, para5, para6); %>
```

---

### 接口名称

get_snmp_info.asp

### 功能

获取 Snmp 信息（SNMP 版本、端口、读/写共同体名、用户列表及口令周期信息等）。

后台接口：`extern INT32 EMAP_AspGetSnmpInfo( INT32 eid, Webs *wp, INT32 argc, CHAR **argv);`

后台是否记录操作记录：无需记录操作记录。

### 参数

**请求信息：** 文档未给出（前台直接请求 ./get_snmp_info.asp，无参数）。

**响应信息：**

信息域：`Error: "ERR"`；`Normal: SNMP version| SNMP port| SNMP read name| SNMP write name| User num| User name1^ Password1 DesAesType1^ Password info 1^ LeaveDay1^ LeaveHour1^ PawdCycleTime1^ PawdCycleNotifyTime1~ ……`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| SNMP version | （文档未给出） | SNMP版本信息 |
| SNMP port | （文档未给出） | SNMP端口号 |
| SNMP read name | （文档未给出） | SNMP读共同体名 |
| SNMP write name | （文档未给出） | SNMP 写共同体名 |
| User num | （文档未给出） | 用户数量 |
| User name1 | ^ | 用户名称 |
| Password1 | ^ | 密码 |
| DesAesType1 | （文档未给出） | 加密类型（^：不使能snmp口令管理周期功能，不存在该字段） |
| Password info 1 | ^ | 口令信息（-1：获取失败；0：不需要提醒；1：即将过期；2：过期。不使能snmp口令管理周期功能，不存在该字段） |
| LeaveDay1 | ^ | 到期剩余天数（不使能snmp口令管理周期功能，不存在该字段） |
| LeaveHour1 | ^ | 到期剩余小时数（不使能snmp口令管理周期功能，不存在该字段） |
| PawdCycleTime1 | ^ | 口令有效期（不使能snmp口令管理周期功能，不存在该字段） |
| PawdCycleNotifyTime1 | （文档未给出） | 到期提醒时间（不使能snmp口令管理周期功能，不存在该字段） |
| …… | ~ | …… |
| User nameN | ^ | 用户名称 |
| PasswordN | ^ | 密码 |
| DesAesTypeN | （文档未给出） | 加密类型（^：不使能snmp口令管理周期功能，不存在该字段） |
| Password info N | ^ | 口令信息（-1：获取失败；0：不需要提醒；1：即将过期；2：过期。不使能snmp口令管理周期功能，不存在该字段） |
| LeaveDayN | ^ | 到期剩余天数（不使能snmp口令管理周期功能，不存在该字段） |
| LeaveHourN | ^ | 到期剩余小时数（不使能snmp口令管理周期功能，不存在该字段） |
| PawdCycleTimeN | ^ | 口令有效期（不使能snmp口令管理周期功能，不存在该字段） |
| PawdCycleNotifyTime N | （文档未给出） | 到期提醒时间（不使能snmp口令管理周期功能，不存在该字段） |
| （组间分隔符） | ~ | （文档未给出） |

### 示例

前台请求使用：
```
./get_snmp_info.asp
```

平台前台调用：
```
<% GetSnmpInfo(); %>
```

---

### 接口名称

get_snmp_trap_info.asp

### 功能

获取 SnmpTrap 信息（Trap 个数、地址、端口、版本、用户名称/共同体名称列表）。

后台接口：`extern INT32 EMAP_AspGetSnmpTrap( INT32 eid, Webs *wp, INT32 argc, CHAR **argv);;`

后台是否记录操作记录：无需记录操作记录。

### 参数

**请求信息：** 文档未给出（前台直接请求 ./get_snmp_trap_info.asp，无参数）。

**响应信息：**

信息域：`Error: "ERR"`；`Normal: Trap num| Trap addr1~ Trap port1~ TrapVersion1~ Name1|……`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| Trap num | （文档未给出） | Trap 个数 |
| Trap addr1 | ~ | Trap地址 |
| Trap port1 | ~ | Trap端口 |
| TrapVersion1 | ~ | Trap 版本号 |
| Name1 | （文档未给出） | 用户名称或者共同提名 |
| …… | …… | …… |
| Trap addrN | ~ | Trap地址 |
| Trap portN | ~ | Trap端口 |
| TrapVersionN | ~ | Trap 版本号 |
| NameN | （文档未给出） | 用户名称或者共同提名 |

### 示例

前台请求使用：
```
./get_snmp_trap_info.asp
```

平台前台调用：
```
<% GetSnmpTrap (); %>
```

---

### 接口名称

get_bin_connect_mode.asp

### 功能

获取 BIN 和网管的连接方式。

后台接口：`extern INT32 EMAP_AspGetBinConnectMode(INT32 eid, Webs *wp, INT32 argc, CHAR **argv);`

后台是否记录操作记录：无需记录操作记录。

### 参数

**请求信息：** 需要发送请求（./get_bin_connect_mode.asp?para1=binMoudle&para2=）

| 参数 | 参数类型 | 说明 |
| --- | --- | --- |
| para1 | binMoudle | Bin模式 |
| para2 | （文档未给出） | 无 |

**响应信息：**

`Error: "ERR"`；`Normal: ReturnCode|ConnectMode`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| ReturnCode | ~ | 返回码 |
| ConnectMode | ~ | 连接模式 |

### 示例

前台请求使用：
```
./get_bin_connect_mode.asp?para1=" + binMoudle + "&para2=
```

平台前台调用：
```
<% GetBinConnectMode(para1, para2); %>
```

---

### 接口名称

get_mib_export_tarname.asp

### 功能

导出 mib 文件（该请求用来导出mib文件，不返回空白页；返回导出的包名）。

后台接口：`INT32 WebAspExportMibData(INT32 eid, Webs *wp, INT32 argc, CHAR **argv)`

后台是否记录操作记录：会记录导出完成操作记录。

### 参数

**请求信息：** 文档未给出具体请求参数。

**响应信息：**

`Error: ERR`；`Normal:`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| TarName | 无 | 导出的包名 |

### 示例

前台请求使用：
```
./get_mib_export_tarname.asp?exporttype=&para1=&para2=&para3=&para4=
```

平台前台调用：
```
<%ExportMibData(exporttype, para1, para2, para3, para4);%>
```

---

### 接口名称

set_debug_info.asp

### 功能

串口调试接口，按 Type 区分不同调试操作。

- Type=0：获取串口配置参数。
- Type=2：打开调试串口。
- Type=3：关闭调试串口。
- Type=4：发送调试数据。
- Type=5：获取上报的串口数据。
- Type=6：打开监听串口。
- Type=7：关闭监听串口。

（文档中未出现 Type=1 的说明）

后台接口：`extern INT32 EMAP_AspGetComDebugInfo(INT32 eid, Webs *wp, int argc, CHAR **argv);`

后台是否记录操作记录：
- Type=0/4/5：无需记录操作记录；
- Type=2/3/6/7：会记录操作记录。

### 参数

**请求信息（Type=0，获取串口配置参数）：**

| 参数 | 参数类型 | 说明 |
| --- | --- | --- |
| type | type | type=0 获取串口配置参数 |
| para1 | （文档未给出） | （文档未给出） |
| …… | …… | …… |
| Para8 | （文档未给出） | （文档未给出） |

**响应信息（Type=0）：**

`Error: ERR,参数错误`；`Normal:`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| currentState | （文档未给出） | 当前串口调试状态。0：未打开串口；2：已有其他用户打开串口；3：已有相同用户打开串口 |

**请求信息（Type=2，打开调试串口）：**

| 参数 | 参数类型 | 说明 |
| --- | --- | --- |
| type | type | type=2打开调试串口 |
| para1 | comNum | 端口ID |
| para2 | baudRate | 波特率 |
| para3 | 无 | 无 |
| para4 | stopBit | 停止位 |
| para5 | checkBit | 校验位 |
| para6 | 9bit | 第9数据位 |
| para7 | cutframe | 自动断帧使能 |
| para8 | timespace | 断帧时间间隔 |

**响应信息（Type=2）：**

`Error: ERR,参数错误`；`Normal: 0表示设置成功，其他为失败`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| result | 无 | 打开结果。0：打开成功；2：已有其他用户打开串口；3：已有相同用户打开串口 |

**请求信息（Type=3，关闭调试串口）：**

| 参数 | 参数类型 | 说明 |
| --- | --- | --- |
| type | type | type=3关闭调试串口 |
| para1 | comNum | 端口ID |
| para2 | baudRate | 波特率 |
| para3 | 无 | 无 |
| …… | …… | …… |
| para8 | 无 | 无 |

**响应信息（Type=3）：**

`Error: ERR,参数错误`；`Normal: 0表示关闭成功，其他为失败`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| result | 无 | 结果。0：关闭成功；其他：关闭失败 |

**请求信息（Type=4，发送调试数据）：**

| 参数 | 参数类型 | 说明 |
| --- | --- | --- |
| type | type | type=4发送调试数据 |
| para1 | data | 发送的数据。16进制格式：xx\|xx\|xx\|…，例如01\|02\|03\|；ASCII格式：字符串"xxxxx"，例如"12345" |
| para2 | dataType | 数据格式。0：表示16进制，上面data字段按16进制的格式；1：表示ASCII码格式，此类型下dataNum填零 |
| para3 | dataNum | dataType 等于0时，para1中16进制的数据数量 |
| …… | …… | …… |
| para8 | 无 | 无 |

**响应信息（Type=4）：**

`Error: ERR,参数错误`；`Normal: result|time`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| result | （文档未给出） | 发送结果。0：成功；其他：失败 |
| time | 无 | 发送时间（%d-%02d-%02d %02d:%02d:%02d） |

**请求信息（Type=5，获取上报的串口数据）：**

| 参数 | 参数类型 | 说明 |
| --- | --- | --- |
| type | type | type=5获取上报的串口数据 |
| Para1 | dataType | 数据格式。0：表示16进制格式；1：表示ASCII码格式 |

**响应信息（Type=5）：**

`Error: ERR`；`Normal: "timeout"（串口超时关闭）；ComState|SysTime|Direction|ComData~ComState|SysTime|Direction|ComData`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| ComState | （文档未给出） | 端口状态。0：打开；其他：关闭 |
| SysTime | （文档未给出） | 接收时间（%d-%02d-%02d %02d:%02d:%02d） |
| Direction | （文档未给出） | 数据流向。0：发送；1：接收 |
| ComData | ~ | 16进制格式：xx xx xx …~，例如01 02 03~；ASCII格式：字符串"xxxxx~"，例如"12345~" |

**请求信息（Type=6，打开监听串口）：**

| 参数 | 参数类型 | 说明 |
| --- | --- | --- |
| type | type | type=6打开监听串口 |
| para1 | comNum | 端口ID |
| para2 | baudRate | 波特率 |
| para3 | 无 | 无 |
| para4 | stopBit | 停止位 |
| para5 | checkBit | 校验位 |
| para6 | 9bit | 第9数据位 |
| para7 | cutframe | 自动断帧使能 |
| para8 | timespace | 断帧时间间隔 |

**响应信息（Type=6）：**

`Error: ERR,参数错误`；`Normal: 0表示设置成功，其他为失败`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| result | 无 | 打开结果。0：打开成功；2：已有其他用户打开串口；3：已有相同用户打开串口 |

**请求信息（Type=7，关闭监听串口）：**

| 参数 | 参数类型 | 说明 |
| --- | --- | --- |
| type | type | type=7关闭监听串口 |
| para1 | 无 | 无 |
| para2 | 无 | 无 |
| para3 | 无 | 无 |
| …… | …… | …… |
| para8 | 无 | 无 |

**响应信息（Type=7）：**

`Error: ERR,参数错误`；`Normal: 0表示设置成功，其他为失败`

| 内容 | 分隔符 | 说明 |
| --- | --- | --- |
| result | 无 | 结果。0：关闭成功；其他：关闭失败 |

### 示例

前台请求使用：
```
./set_debug_info.asp?type=0&para1=&para2=&para3=&para4=&para5=&para6=&para7=&para8=
```

平台前台调用：
```
<% SetDebugInfo(Type, para1, para2, para3, para4, para5, para6, para7, para8); %>
```

---

### 接口名称

set_long_poll_info.asp

### 功能

长链接（long poll）管理接口，按 Type 区分不同操作。

- 功能1（type=0）：清理活动告警长链接。
- 功能2（type=1）：去注册监控信息主动上报长链接。

后台接口：`INT32 EMAP_AspSetLongPollInfo(INT32 eid, Webs *wp, int argc, CHAR **argv);`

后台是否记录操作记录：无需记录操作记录。

### 参数

**请求信息（type=0，清理活动告警长链接）：**

| 参数 | 参数类型 | 说明 | 变更版本 |
| --- | --- | --- | --- |
| type | 0 | Type类型 | （文档未给出） |

**请求信息（type=1，去注册监控信息主动上报长链接）：**

| 参数 | 参数类型 | 说明 | 变更版本 |
| --- | --- | --- | --- |
| type | 1 | Type类型 | V300R021C00新增 |
| Id | （文档未给出） | 0：去注册长链接；1：重新连接长链接 | （文档未给出） |

**响应信息：** 不返回信息（type=0 与 type=1 均不返回信息）。

### 示例

前台请求使用：
```
./set_long_poll_info.asp?type=typeId&id=id
```

平台前台调用：
```
<%SetLongPollInfo(type, id);%>
```

---

### 接口名称

set_long_poll_start_info.asp

### 功能

长链接注册接口，按 Type 区分不同注册。

- 功能1（type=0）：活动告警，注册告警变化主动上报长链接。
- 功能2（type=1）：注册按照设备id主动上报监控信息长链接。

后台接口：`INT32 EMAP_AspSetLongPollStart(INT32 eid, Webs *wp, INT32 argc, CHAR **argv);`

后台是否记录操作记录：无需记录操作记录。

### 参数

**请求信息（type=0，活动告警主动上报）：**

| 参数 | 参数类型 | 说明 | 变更版本 |
| --- | --- | --- | --- |
| type | 0 | Type类型 | （文档未给出） |

Id（预留）参数：

| 参数 | 参数类型 | 说明 | 变更版本 |
| --- | --- | --- | --- |
| Id | 24 | Id标识 | （文档未给出） |

**响应信息（type=0）：**

`Error: "ERR"`；`Normal: Id = 24(活动告警主动上报使能)`

| 内容 | 分隔符 | 说明 | 变更版本 |
| --- | --- | --- | --- |
| CriticalNum | （文档未给出） | （文档未给出） | （文档未给出） |
| MajorNum | （文档未给出） | （文档未给出） | （文档未给出） |
| MinorNum | （文档未给出） | （文档未给出） | （文档未给出） |
| WarningNum | （文档未给出） | （文档未给出） | （文档未给出） |
| AllAlmNum | （文档未给出） | 告警关联音响未使能，无此字段及以下字段 | V200R003C00新增 |
| EquipId1 | ~ | 设备ID1 | （文档未给出） |
| AlmId1 | ~ | 告警ID1.. | （文档未给出） |
| AlmSoundFlg1 | （文档未给出） | 关联音响标志 | （文档未给出） |
| EquipId2 | ~ | 设备ID2 | （文档未给出） |
| AlmId2 | ~ | 告警ID2.. | （文档未给出） |
| AlmSoundFlg2 | （文档未给出） | 关联音响标志 | （文档未给出） |
| ………. | （文档未给出） | （文档未给出） | （文档未给出） |
| EquipIdN | ~ | 设备IDn | （文档未给出） |
| AlmIdN | ~ | 告警IDn.. | （文档未给出） |
| AlmSoundFlgN | （文档未给出） | 关联音响标志 | （文档未给出） |

（注意：如果没有活动告警暂时是不给前台回复的，等产生活动告警会通过长链接给前台回复。）

**请求信息（type=1，注册按照设备id主动上报监控信息，json格式）：**

| 参数 | 参数类型 | 说明 | 变更版本 |
| --- | --- | --- | --- |
| Type | （文档未给出） | 1：注册按照设备id主动上报监控信息，json格式 | （文档未给出） |

Para1 请求参数（json，初始版本：V2R3C10）：

| 名称 | 值 | 说明 | 变更版本 |
| --- | --- | --- | --- |
| { | （文档未给出） | （文档未给出） | V300R021C00新增 |
| "id" | 24 | 长链接id | （文档未给出） |
| "equipid" | （文档未给出） | 设备id | （文档未给出） |
| equiptypeid | （文档未给出） | 设备类型id | （文档未给出） |
| } | （文档未给出） | （文档未给出） | （文档未给出） |

**响应信息（type=1）：**

`Error: "ERR"`；`Normal:`（json 格式）

| 名称 | 值 | 说明 |
| --- | --- | --- |
| "errcode" | "" | （文档未给出） |
| "siginfo" | [ { "equipId": "", "signalId": "", "value": "" } ] | 信号信息列表 |
| "equipId" | "" | 设备ID |
| "signalId" | "" | 信号ID |
| "value" | "" | 信号值 |

### 示例

前台请求使用：
```
./set_long_poll_start_info.asp?type=typeId&id=id
```

平台前台调用：
```
<%SetLongPollStart(type, id);%>
```

---

