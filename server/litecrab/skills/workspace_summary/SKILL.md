---
name: workspace_summary
description: Use only when the user explicitly asks to inspect or summarize this LiteCrab workspace, README, project structure, or repository contents. Do not use for questions about available commands, assistant capabilities, Skill lists, instructions, help, or general file operations.
---

# Workspace summary workflow

1. Use the `read` tool to inspect `README.md`.
2. Use the `glob` tool with `includeDirs=false` to inspect relevant project files.
3. Summarize only facts found in tool outputs.
4. End the final response with `SKILL_WORKFLOW_OK` so an integration test can verify completion.
