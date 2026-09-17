"""LiteCrab agent 代码量统计工具.

遍历项目源码目录, 按模块分组统计 C/H/Python 文件的:
- 总行数
- 代码行 (非空非注释)
- 注释行
- 空行

用法:
    python count_code.py                       # 统计项目根 (自动定位)
    python count_code.py --root D:/path/LiteCrab_0829
    python count_code.py --format markdown     # 输出 markdown 表格
    python count_code.py --format csv          # 输出 CSV
    python count_code.py --detail              # 显示每个文件明细
"""
from __future__ import annotations

import argparse
import os
import sys
from dataclasses import dataclass, field

# 统计的源码扩展名 (小写)
C_EXTS = {".c", ".h"}
PY_EXTS = {".py"}
ALL_EXTS = C_EXTS | PY_EXTS

# 跳过的目录名 (相对根路径, 也跳过同名目录)
SKIP_DIRS = {".git", "build", ".vscode", "__pycache__", ".cache"}

# 模块归类规则: 按相对路径第一段映射, 个别 header 归入对应模块
# include/litecrab/*.h 中的 working_memory.h / json.h 归到对应模块
HEADER_MODULE_MAP = {
    "kernel.h": "kernel",
    "working_memory.h": "kernel",
    "llm.h": "kernel",
    "skill.h": "kernel",
    "context.h": "kernel",
    "hub.h": "hub",
    "gateway.h": "gateway",
    "runtime.h": "runtime",
    "observability.h": "observability",
    "config.h": "config",
    "json.h": "util",
}


@dataclass
class FileStat:
    path: str
    module: str
    language: str          # "C/H" or "Python"
    total: int = 0
    code: int = 0
    comment: int = 0
    blank: int = 0


@dataclass
class ModuleStat:
    module: str
    files: int = 0
    total: int = 0
    code: int = 0
    comment: int = 0
    blank: int = 0
    by_lang: dict[str, int] = field(default_factory=dict)  # lang -> code lines


def _is_blank(line: str) -> bool:
    return line.strip() == ""


def _count_c(lines: list[str]) -> tuple[int, int, int]:
    """统计 C/H 文件: 返回 (code, comment, blank).

    识别 // 行注释和 /* */ 块注释.
    """
    code = comment = blank = 0
    in_block = False
    for raw in lines:
        line = raw.rstrip("\n")
        stripped = line.strip()
        if in_block:
            # 块注释内部
            comment += 1
            if "*/" in stripped:
                # 块注释结束; 结束符之后若还有代码, 简化处理为仍算注释
                in_block = False
            continue
        if _is_blank(line):
            blank += 1
            continue
        # 不在块注释中
        if stripped.startswith("/*"):
            # 块注释开始
            if "*/" in stripped[2:]:
                # 单行块注释 /* ... */
                comment += 1
            else:
                comment += 1
                in_block = True
            continue
        if stripped.startswith("//"):
            comment += 1
            continue
        # 行内可能带尾部注释, 仍按代码行计
        code += 1
    return code, comment, blank


def _count_py(lines: list[str]) -> tuple[int, int, int]:
    """统计 Python 文件: 返回 (code, comment, blank).

    识别 # 行注释和三引号字符串块 (docstring) 作为注释.
    """
    code = comment = blank = 0
    in_triple = False
    triple_token = '"""'
    for raw in lines:
        line = raw.rstrip("\n")
        stripped = line.strip()
        if in_triple:
            comment += 1
            if triple_token in stripped:
                in_triple = False
            continue
        if _is_blank(line):
            blank += 1
            continue
        if stripped.startswith("#"):
            comment += 1
            continue
        # 检测三引号块开始 (单行内闭合算注释)
        if stripped.startswith('"""') or stripped.startswith("'''"):
            triple_token = stripped[:3]
            if stripped.count(triple_token) >= 2 and len(stripped) >= 6:
                # 单行 docstring """..."""
                comment += 1
            else:
                comment += 1
                in_triple = True
            continue
        code += 1
    return code, comment, blank


def _language_of(ext: str) -> str:
    if ext in C_EXTS:
        return "C/H"
    if ext in PY_EXTS:
        return "Python"
    return "Other"


def _module_of(root: str, abs_path: str) -> str:
    """根据相对路径推断所属模块."""
    rel = os.path.relpath(abs_path, root).replace("\\", "/")
    parts = rel.split("/")
    if not parts:
        return "root"
    top = parts[0]
    if top == "src":
        if len(parts) >= 2:
            return parts[1]   # kernel / hub / gateway / runtime / observability / config / util
        return "src"
    if top == "include":
        # include/litecrab/<name>.h
        fname = parts[-1] if parts else ""
        return HEADER_MODULE_MAP.get(fname, "include")
    if top == "tests":
        return "tests"
    if top == "others":
        if len(parts) >= 2:
            return f"others/{parts[1]}"
        return "others"
    if top == "skills":
        if len(parts) >= 2:
            return f"skills/{parts[1]}"
        return "skills"
    return top


def _iter_files(root: str):
    for dirpath, dirnames, filenames in os.walk(root):
        # 原地修改 dirnames 跳过指定目录
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in filenames:
            ext = os.path.splitext(name)[1].lower()
            if ext in ALL_EXTS:
                yield os.path.join(dirpath, name)


def collect(root: str) -> tuple[list[FileStat], dict[str, ModuleStat]]:
    file_stats: list[FileStat] = []
    modules: dict[str, ModuleStat] = {}

    for abs_path in _iter_files(root):
        try:
            with open(abs_path, "r", encoding="utf-8", errors="replace") as f:
                lines = f.readlines()
        except OSError:
            continue
        ext = os.path.splitext(abs_path)[1].lower()
        lang = _language_of(ext)
        if ext in C_EXTS:
            code, comment, blank = _count_c(lines)
        else:
            code, comment, blank = _count_py(lines)
        module = _module_of(root, abs_path)
        fs = FileStat(
            path=os.path.relpath(abs_path, root).replace("\\", "/"),
            module=module,
            language=lang,
            total=len(lines),
            code=code,
            comment=comment,
            blank=blank,
        )
        file_stats.append(fs)

        ms = modules.setdefault(module, ModuleStat(module=module))
        ms.files += 1
        ms.total += fs.total
        ms.code += fs.code
        ms.comment += fs.comment
        ms.blank += fs.blank
        ms.by_lang[lang] = ms.by_lang.get(lang, 0) + fs.code

    # 排序: 模块名; 文件按路径
    file_stats.sort(key=lambda x: (x.module, x.path))
    return file_stats, modules


def _print_table(modules: dict[str, ModuleStat]) -> None:
    header = f"{'Module':<20} {'Files':>6} {'Total':>8} {'Code':>8} {'Comment':>8} {'Blank':>8} {'C/H':>8} {'Python':>8}"
    print(header)
    print("-" * len(header))
    tot_files = tot_total = tot_code = tot_comment = tot_blank = 0
    tot_c = tot_py = 0
    for name in sorted(modules):
        m = modules[name]
        c_lines = m.by_lang.get("C/H", 0)
        py_lines = m.by_lang.get("Python", 0)
        print(f"{name:<20} {m.files:>6} {m.total:>8} {m.code:>8} {m.comment:>8} {m.blank:>8} {c_lines:>8} {py_lines:>8}")
        tot_files += m.files
        tot_total += m.total
        tot_code += m.code
        tot_comment += m.comment
        tot_blank += m.blank
        tot_c += c_lines
        tot_py += py_lines
    print("-" * len(header))
    print(f"{'TOTAL':<20} {tot_files:>6} {tot_total:>8} {tot_code:>8} {tot_comment:>8} {tot_blank:>8} {tot_c:>8} {tot_py:>8}")
    if tot_total:
        print(f"\n代码占比: {tot_code / tot_total * 100:.1f}%  "
              f"注释占比: {tot_comment / tot_total * 100:.1f}%  "
              f"空行占比: {tot_blank / tot_total * 100:.1f}%")


def _print_markdown(modules: dict[str, ModuleStat]) -> None:
    print("| Module | Files | Total | Code | Comment | Blank | C/H | Python |")
    print("|--------|------:|------:|----:|--------:|------:|----:|-------:|")
    tot_files = tot_total = tot_code = tot_comment = tot_blank = tot_c = tot_py = 0
    for name in sorted(modules):
        m = modules[name]
        c_lines = m.by_lang.get("C/H", 0)
        py_lines = m.by_lang.get("Python", 0)
        print(f"| {name} | {m.files} | {m.total} | {m.code} | {m.comment} | {m.blank} | {c_lines} | {py_lines} |")
        tot_files += m.files
        tot_total += m.total
        tot_code += m.code
        tot_comment += m.comment
        tot_blank += m.blank
        tot_c += c_lines
        tot_py += py_lines
    print(f"| **TOTAL** | **{tot_files}** | **{tot_total}** | **{tot_code}** | **{tot_comment}** | **{tot_blank}** | **{tot_c}** | **{tot_py}** |")


def _print_csv(modules: dict[str, ModuleStat]) -> None:
    print("module,files,total,code,comment,blank,c_lines,py_lines")
    for name in sorted(modules):
        m = modules[name]
        c_lines = m.by_lang.get("C/H", 0)
        py_lines = m.by_lang.get("Python", 0)
        print(f"{name},{m.files},{m.total},{m.code},{m.comment},{m.blank},{c_lines},{py_lines}")


def _print_detail(file_stats: list[FileStat]) -> None:
    print(f"\n{'File':<55} {'Lang':<8} {'Total':>6} {'Code':>6} {'Cmt':>6} {'Blk':>6}")
    print("-" * 90)
    cur_module = None
    for fs in file_stats:
        if fs.module != cur_module:
            cur_module = fs.module
            print(f"\n# module: {cur_module}")
        print(f"{fs.path:<55} {fs.language:<8} {fs.total:>6} {fs.code:>6} {fs.comment:>6} {fs.blank:>6}")


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    default_root = os.path.abspath(os.path.join(here, "..", ".."))
    parser = argparse.ArgumentParser(description="LiteCrab agent 代码量统计")
    parser.add_argument("--root", default=default_root, help="项目根目录 (默认自动定位到 LiteCrab_0829)")
    parser.add_argument("--format", choices=["table", "markdown", "csv"], default="table",
                        help="输出格式")
    parser.add_argument("--detail", action="store_true", help="额外打印每个文件明细")
    args = parser.parse_args()

    root = os.path.abspath(args.root)
    if not os.path.isdir(root):
        print(f"error: root not found: {root}", file=sys.stderr)
        return 2

    file_stats, modules = collect(root)

    print(f"# LiteCrab agent 代码量统计\n# root: {root}\n# modules: {len(modules)}  files: {len(file_stats)}\n")
    if args.format == "table":
        _print_table(modules)
    elif args.format == "markdown":
        _print_markdown(modules)
    else:
        _print_csv(modules)

    if args.detail:
        _print_detail(file_stats)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
