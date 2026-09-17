# runtime_builtin_tools：内置工具实现

## 1. 模块职责

`runtime/core/builtin_tools.c` 定义一组**内置工具规格**（`g_builtinTools`），注册进注册表供 Agent 调用。每个工具由 `CrabToolSpec{name, description, enabledByDefault, readonly, permissions, concurrency, riskLevel, params, execute, filter}` 描述。

内置工具清单：

| 工具名 | 用途 | readonly | enabled | concurrency | risk |
|--------|------|----------|---------|-------------|------|
| read | 读取文件（行范围/字节） | 是 | 是 | SHARED_READ | LOW |
| csv_read | 读取 CSV 单元格 | 是 | 是 | SHARED_READ | LOW |
| write | 写入文件（create/overwrite/append） | 否 | 是 | PATH_EXCLUSIVE_WRITE | LOW |
| edit | 精确替换文件内容 | 否 | 是 | PATH_EXCLUSIVE_WRITE | LOW |
| grep | 内容搜索 | 是 | 是 | SHARED_READ | LOW |
| glob | 文件名模式匹配 | 是 | 是 | SHARED_READ | LOW |
| shell | 执行 shell 命令 | 否 | **否** | GLOBAL_EXCLUSIVE | HIGH |
| skill_read | 读取技能 SKILL.md | 是 | 是 | SHARED_READ | LOW |
| skill_recover | 恢复技能（空操作反馈） | 是 | 是 | SHARED_READ | LOW |

## 2. 公共辅助工具

### 2.1 file_tool_common（文件工具公共）

```c
int CrabFileToolBuildPath(const CrabRuntime *runtime, const char *path,
                          char *out, size_t outSize);
int CrabFileToolReadAll(const char *path, size_t maxBytes, char **outData, size_t *outSize);
int CrabFileToolWriteAll(const char *path, const char *content, size_t contentSize, const char *mode);
int CrabFileToolMakeParents(const char *path);
```

- **BuildPath**：绝对路径（`/` 开头）原样使用；相对路径拼接为 `<workspaceRoot>/<path>`。这保证所有文件工具限定在工作空间内。
- **ReadAll**：`fopen "rb"`，malloc `maxBytes+1` 读满或到 EOF，末尾补 `\0`；失败返回 `-errno` 或 `EXECUTE_FAILED`。
- **WriteAll**：按 mode 决定打开方式：`create` 要求**文件必须不存在**（存在则报错），`append` 用 `"ab"`，否则 `"wb"`；写入长度错误返回 `EXECUTE_FAILED`。
- **MakeParents**：把目录路径按 `/` 逐段 mkdir（`0775`），忽略 `EXIST`。

### 2.2 search_tool_common（搜索工具公共）

```c
int CrabSearchAppend(char *out, size_t outSize, size_t *offset, const char *format, ...);
int CrabSearchBuildPath(const char *workspaceRoot, const char *path, char *out, size_t outSize);
int CrabSearchWalk(const char *rootPath, const char *relRoot,
                   CrabSearchVisitFn visit, void *userData);
```

- **Append**：把格式化结果安全追加到输出缓冲，返回值表示写入是否成功/溢出。
- **BuildPath**：同 file 的 BuildPath。
- **Walk**：递归遍历目录树，跳过 `.git` / `build` / `dist` / `node_modules` 噪声目录，对每个条目调用 `visit(fullPath, relPath, isDir, userData)`；`visit` 返回非 0 时中断。

## 3. 各工具实现

### 3.1 read —— 文件读取

**参数**：`path`(必填,string)、`startLine`(int,默认1)、`maxLines`(int,默认200)、`maxBytes`(int,默认65536)

```
ReadExecute(runtime, call, raw):
    build fullPath
    ReadAll(fullPath, maxBytes, &fileData, &fileSize)
    out = calloc(maxBytes+1)
    # 逐行扫描，从 startLine 开始输出到 maxLines 行
    for each line (按 \n 分):
        if lineNo >= startLine && emitted < maxLines:
            CrabAppendLine(out, ..., lineNo, line, lineLen)   # "行号: 内容\n"
            emitted++
    raw.exitCode=0
    SetStdout(out)
```

输出格式：每行带行号前缀 `N: 内容\n`。`CrabAppendLine` 保证不超过缓冲。

### 3.2 csv_read —— CSV 单元格读取

**参数**：`path`(必填)、`row`(int,默认1)、`column`(int,默认1)、`columnName`(string,可选)、`maxBytes`(int,默认1MB)

实现一个**流式 CSV 解析器**：

```
ReadExecute:
    读文件全部内容到 fileData
    初始化 CrabCsvReadState{row, column, columnName, hasColumnName}
    CrabCsvParse(fileData, size, &state, error, ...)   # 逐字符扫描

CsvParse(data, size, state):
    维护 fieldLen / row / column / inQuotes / justEndedRecord
    逐字符：
      inQuotes 状态: 处理 "" 转义 & '"' 结束
      字符 '"' 且 fieldLen==0: 进入 quoted
      字符 ',': 提交字段 -> ProcessField; column++
      字符 \r 或 \n: 提交字段(ProcessField); 处理 \r\n; row++; column=1
      其他: 追加到 field
    # 提交已捕获的目标单元格即完成（DONE）
```

`CsvProcessField`：
- 若用 columnName 且在第 1 行，匹配表头确定 `resolvedColumn`。
- 当 `row==目标行` 且（columnName 时 `column==resolvedColumn` / 否则 `column==目标column`），捕获该值并返回 `DONE`。

结果 success 输出：`path/row/column[/column_name]/value:` 文本。失败场景：columnName 未找到、单元格不在 maxBytes 内、未闭合引号等。

### 3.3 write —— 文件写入

**参数**：`path`(必填)、`content`(string,默认"")、`mode`(enum: create/overwrite/append,默认overwrite)、`createDirs`(bool,默认0)

```
WriteExecute:
    build fullPath
    if createDirs: MakeParents(fullPath)
    WriteAll(fullPath, content, strlen(content), mode)
    输出 "path: ...\nmode: ...\nbytes: <len>"
```

### 3.4 edit —— 精确文本替换

**参数**：`path`(必填)、`oldText`(必填)、`newText`(string,默认"")、`expectedOccurrences`(int,默认1)

```
EditExecute:
    ReadAll(fullPath, 1MB, &fileData, &fileSize)
    count = CrabCountOccurrences(fileData, oldText)   # 统计匹配次数
    if count != expectedOccurrences:
        raw.exitCode=EXECUTE_FAILED; stderr="expected %lld occurrences, found %lld"
        return EXECUTE_FAILED
    newData = CrabReplaceAll(fileData, oldText, newText, count)  # 全量替换
    WriteAll(fullPath, newData, strlen(newData), "overwrite")
    输出 "path: ...\nreplacements: <count>"
```

- **CountOccurrences**：用 `strstr` 循环统计，不重叠。
- **ReplaceAll**：预分配 `textLen + (newLen-oldLen)*count + 1`，逐段 memcpy 重建。
- **安全**：必须严格等于 expectedOccurrences，避免误替换。

### 3.5 grep —— 内容搜索

**参数**：`pattern`(必填)、`path`(默认".")、`maxMatches`(int,默认100)、`caseSensitive`(bool,默认1)

```
GrepExecute:
    build rootPath
    CrabSearchWalk(rootPath, ".", CrabGrepVisit, &ctx)
    ...
GrepVisit(fullPath, relPath, isDir, ctx):
    if isDir || matchCount>=maxMatches: return OK
    fopen 逐行 fgets:
        if LineMatches(line, pattern, caseSensitive):
            去掉行尾 \r\n
            CrabSearchAppend("%s:%d:%s\n", relPath, lineNo, line)
            matchCount++
    fclose
LineMatches: caseSensitive? strstr : 不敏感子串匹配(tolower)
```

**退出码约定**：`matchCount==0` 时 `exitCode=1`、stdout 空；有匹配时 `exitCode=0`。

### 3.6 glob —— 文件名模式匹配

**参数**：`pattern`(必填)、`path`(默认".")、`maxMatches`(int,默认100)、`includeDirs`(bool,默认1)

```
GlobExecute:
    build rootPath
    CrabSearchWalk(rootPath, ".", CrabGlobVisit, &ctx)
GlobVisit:
    if matchCount>=maxMatches: return OK
    if isDir && !includeDirs: return OK
    # 用 fnmatch 分别匹配完整 relPath 与 basename（'/'后部分）
    if fnmatch(pattern, relPath)!=0 && fnmatch(pattern, basename(relPath))!=0: 不匹配
    CrabSearchAppend("%s\t%s\n", isDir?"dir":"file", relPath)
```

模式支持 `*` / `?`（依赖平台的 `fnmatch`）。

### 3.7 shell —— 执行 shell 命令

**参数**：`script`(必填)、`path`(默认".")

**特性**：`enabledByDefault=0`（默认不启用，需显式配置）、`GLOBAL_EXCLUSIVE`、`HIGH` 风险、`SCHEDULER` 权限。

```
ShellExecute:
    if !script 或空: INVALID_ARG
    build fullPath (工作目录)
    if strlen(script)+... > cmd上限: "script too long"
    cmd = "sh -c <script>"           # 组装
    pipe(pipeFd)                     # 创建管道
    pid = fork()
    if child:
        close(pipeFd[0])
        dup2(pipeFd[1], STDOUT_FILENO)
        dup2(pipeFd[1], STDERR_FILENO)   # stdout+stderr 合并
        close(pipeFd[1])
        chdir(fullPath)
        execlp("sh","sh","-c",script,NULL)
        _exit(-errno)
    # 父进程
    close(pipeFd[1])
    pipeFile = fdopen(pipeFd[0],"r")
    循环 fread 读取输出（动态扩展缓冲，默认 128KB 起翻倍）
    fclose; waitpid(pid,&status,0)
    退出码：
      WIFEXITED:   WEXITSTATUS
      WIFSIGNALED: 128+WTERMSIG
      其他: 1
    if outputSize>0: SetStdout(output)
```

要点：**stdout 与 stderr 合并输出**；`chdir` 到工作目录；脚本长度上限 4096。

### 3.8 skill_read —— 读取技能文件

**参数**：`skillPath`(必填)、`fileName`(默认"SKILL.md")

```
SkillReadExecute(runtime, call, raw):
    fullPath = "<skillPath>/<fileName>"
    if !IsPathSafe(SKILLS_ROOT, fullPath): INVALID_ARG("path traversal not allowed")

    # IsPathSafe 检查：
    #   含 ".." 拒绝；绝对路径('/','\','C:')拒绝；
    #   解析 realpath 后必须处于 SKILLS_ROOT 前缀内

    fullPath = "<SKILLS_ROOT>/<skillPath>/<fileName>"   # SKILLS_ROOT="skills"
    fopen "rb"; 读最多 64KB
    body = GetContentAfterSecondSeparator(content)      # 取第二个 '---' 之后
    TrimWhitespace(body)
    SetStdout(body)
```

- **GetContentAfterSecondSeparator**：逐行找两个独占一行且内容恰为 `---` 的行，返回第二个 `---` 之后的内容。
- 路径穿越防护是本工具的安全关键点。

### 3.9 skill_recover —— 恢复技能（反馈）

**参数**：`skillPath`(必填)

```
SkillRecoverExecute(runtime, call, raw):
    校验 skillPath 非空
    输出 "Skill '<skillPath>' has been recovered. You can now continue using this skill."
    exitCode=0
```

真正的状态恢复逻辑不在本工具，而由 `SkillHandlerOnSkillStart` 完成（见 kernel_skill_handler.md）。

## 4. 工具规格注册（builtin_tools.c）

`g_builtinTools[]` 为静态数组，每项一个 `CrabToolSpec`，含完整参数规格（`CrabToolParamSpec` 数组）。`CrabRuntimeRegisterBuiltinTools` 遍历数组逐个 `CrabRegistryRegister`。

参数规格示例（read）：

```c
{
  "name": "read",
  "description": "读取文件内容，支持行范围。",
  "readonly": 1,
  "concurrency": CRAB_TOOL_CONCURRENCY_SHARED_READ,
  "riskLevel": CRAB_TOOL_RISK_LOW,
  "params": (CrabToolParamSpec[]){
    {"path","文件路径",STRING,1,0,{0},0,0,NULL,0},
    {"startLine","起始行",INT,0,1,{INT,1,1},0,0,NULL,0},
    ...
  },
  "execute": CrabToolReadExecute,
  "filter": CrabToolReadFilter
}
```

## 5. 复现要点（检查清单）

- [ ] 所有工具的参数规格（类型/必填/默认/枚举/范围）与上述一致。
- [ ] file 工具路径全部经 BuildPath 限定在工作空间内。
- [ ] write 的 create 模式要求文件不存在，append 追加。
- [ ] edit 严格检查 expectedOccurrences，否则失败。
- [ ] shell 默认不启用，stdout/stderr 合并，chdir 生效，退出码映射正确。
- [ ] skill_read 做路径穿越防护，只返回第二个 `---` 之后内容。
- [ ] grep 无匹配退出码为 1；glob 支持 fnmatch 通配。
- [ ] csv_read 支持列名解析与 quoted 字段。

## 6. 相关文档

- `runtime_contracts.md`：工具规格数据结构。
- `runtime_execution.md`：参数校验与结果构建。
- `runtime_overview.md`：调用主流程。
