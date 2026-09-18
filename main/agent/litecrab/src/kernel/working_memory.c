#include "litecrab/working_memory.h"

#include "litecrab/kernel.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static WorkingMemory fallbackMemory;
static WorkingMemory* current_memory(void) {
    WorkingMemory* memory = AgentSessionStateWorkingMemory();
    return memory ? memory : &fallbackMemory;
}
#define memory (*current_memory())
static size_t utf8_prefix_length(const char* value, size_t limit) {
    size_t length = 0;
    const unsigned char* text = (const unsigned char*) value;
    while (text[length] && length < limit) {
        unsigned char first = text[length];
        size_t width = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2
                                      : first >= 0xe0 && first <= 0xef   ? 3
                                      : first >= 0xf0 && first <= 0xf4   ? 4
                                                                          : 1;
        if (length + width > limit)
            break;
        int valid = 1;
        for (size_t i = 1; i < width; i++)
            if ((text[length + i] & 0xc0) != 0x80) {
                valid = 0;
                break;
            }
        if (!valid)
            width = 1;
        length += width;
    }
    return length;
}
static void copy(char* out, size_t size, const char* value) {
    if (!out || !size)
        return;
    value = value ? value : "";
    size_t length = utf8_prefix_length(value, size - 1);
    memcpy(out, value, length);
    out[length] = 0;
}
void WorkingMemoryInit(void) {
    memset(&memory, 0, sizeof memory);
}
void WorkingMemoryClose(void) {
    memset(&memory, 0, sizeof memory);
}
const WorkingMemory* WorkingMemoryGet(void) {
    return &memory;
}
void WorkingMemoryBeginRequest(const char* task) {
    copy(memory.task, sizeof memory.task, task);
    memory.currentCommand[0] = 0;
    if (memory.workflow.status != WORKFLOW_WAITING_INPUT)
        memory.workflow.optionCount = 0;
    AgentSessionStateMarkDirty();
}
void WorkingMemorySetGoal(const char* goal) {
    copy(memory.goal, sizeof memory.goal, goal);
    AgentSessionStateMarkDirty();
}
int WorkingMemoryActivate(const char* skillPath) {
    if (!skillPath || !*skillPath)
        return -1;
    memory.workflow.active = 1;
    memory.workflow.status = WORKFLOW_RUNNING;
    copy(memory.workflow.skillPath, sizeof memory.workflow.skillPath, skillPath);
    AgentSessionStateMarkDirty();
    return 0;
}
void WorkingMemorySetStatus(WorkflowStatus status) {
    memory.workflow.status = status;
    memory.workflow.active = status == WORKFLOW_RUNNING || status == WORKFLOW_WAITING_INPUT;
    AgentSessionStateMarkDirty();
}
void WorkingMemorySetStep(const char* id, const char* description) {
    copy(memory.workflow.currentStepId, sizeof memory.workflow.currentStepId, id);
    copy(memory.workflow.currentStepDescription,
         sizeof memory.workflow.currentStepDescription,
         description);
    AgentSessionStateMarkDirty();
}
void WorkingMemorySetPaths(const char* scriptPath, const char* stepsPath) {
    copy(memory.workflow.scriptPath, sizeof memory.workflow.scriptPath, scriptPath);
    copy(memory.workflow.stepsPath, sizeof memory.workflow.stepsPath, stepsPath);
    AgentSessionStateMarkDirty();
}
const char* WorkflowStatusName(WorkflowStatus status) {
    static const char* names[] = {"inactive", "running", "waiting_input", "completed", "failed"};
    return status >= WORKFLOW_INACTIVE && status <= WORKFLOW_FAILED ? names[status] : "unknown";
}

static int option_line(
    const char* line, size_t length, char* value, size_t valueSize, char* label, size_t labelSize) {
    size_t i = 0;
    while (i < length && isspace((unsigned char) line[i]))
        i++;
    if (i == length)
        return 0;
    size_t numberStart = i;
    if (line[i] == '[') {
        i++;
        numberStart = i;
        while (i < length && isdigit((unsigned char) line[i]))
            i++;
        if (i == numberStart || i == length || line[i] != ']')
            return 0;
        size_t n = i - numberStart;
        if (n >= valueSize)
            n = valueSize - 1;
        memcpy(value, line + numberStart, n);
        value[n] = 0;
        i++;
    } else {
        while (i < length && isdigit((unsigned char) line[i]))
            i++;
        if (i == numberStart || i == length || (line[i] != '.' && line[i] != ')'))
            return 0;
        size_t n = i - numberStart;
        if (n >= valueSize)
            n = valueSize - 1;
        memcpy(value, line + numberStart, n);
        value[n] = 0;
        i++;
    }
    while (i < length && (isspace((unsigned char) line[i]) || line[i] == '-' || line[i] == ':'))
        i++;
    while (length > i && isspace((unsigned char) line[length - 1]))
        length--;
    if (i >= length)
        return 0;
    size_t n = length - i;
    if (n >= labelSize)
        n = labelSize - 1;
    memcpy(label, line + i, n);
    label[n] = 0;
    return 1;
}
int WorkingMemoryParseOptions(const char* text) {
    memory.workflow.optionCount = 0;
    if (!text)
        return 0;
    const char* p = text;
    while (*p && memory.workflow.optionCount < WORKING_MEMORY_MAX_OPTIONS) {
        const char* end = strchr(p, '\n');
        size_t n = end ? (size_t) (end - p) : strlen(p);
        WorkflowOption* o = &memory.workflow.options[memory.workflow.optionCount];
        if (option_line(p, n, o->value, sizeof o->value, o->label, sizeof o->label))
            memory.workflow.optionCount++;
        if (!end)
            break;
        p = end + 1;
    }
    if (memory.workflow.optionCount)
        WorkingMemorySetStatus(WORKFLOW_WAITING_INPUT);
    AgentSessionStateMarkDirty();
    return memory.workflow.optionCount;
}
void WorkingMemoryBeforeTool(const char* name, const char* input) {
    copy(memory.currentCommand, sizeof memory.currentCommand, name);
    if (input && *input) {
        size_t used = strlen(memory.currentCommand);
        if (used + 2 < sizeof memory.currentCommand)
            snprintf(
                memory.currentCommand + used, sizeof memory.currentCommand - used, " %s", input);
    }
}
static void cache_skill(const char* input, const char* output) {
    char path[256];
    if (!JsonExtractSkillPath(input, path, sizeof path))
        return;
    int index = -1;
    for (int i = 0; i < memory.skillCount; i++)
        if (!strcmp(memory.skills[i].path, path)) {
            index = i;
            break;
        }
    if (index < 0) {
        if (memory.skillCount < WORKING_MEMORY_MAX_SKILLS)
            index = memory.skillCount++;
        else {
            memmove(memory.skills,
                    memory.skills + 1,
                    (WORKING_MEMORY_MAX_SKILLS - 1) * sizeof memory.skills[0]);
            index = WORKING_MEMORY_MAX_SKILLS - 1;
        }
    }
    copy(memory.skills[index].path, sizeof memory.skills[index].path, path);
    copy(memory.skills[index].content, sizeof memory.skills[index].content, output);
    WorkingMemoryActivate(path);
    AgentSessionStateMarkDirty();
}
void WorkingMemoryCacheSkill(const char* path, const char* output) {
    if (!path || !*path)
        return;
    int index = -1;
    for (int i = 0; i < memory.skillCount; i++)
        if (!strcmp(memory.skills[i].path, path)) {
            index = i;
            break;
        }
    if (index < 0) {
        if (memory.skillCount < WORKING_MEMORY_MAX_SKILLS)
            index = memory.skillCount++;
        else {
            memmove(memory.skills,
                    memory.skills + 1,
                    (WORKING_MEMORY_MAX_SKILLS - 1) * sizeof memory.skills[0]);
            index = WORKING_MEMORY_MAX_SKILLS - 1;
        }
    }
    copy(memory.skills[index].path, sizeof memory.skills[index].path, path);
    copy(memory.skills[index].content, sizeof memory.skills[index].content, output);
    WorkingMemoryActivate(path);
    AgentSessionStateMarkDirty();
}
void WorkingMemoryAfterTool(const char* name, const char* input, const char* output, int exitCode) {
    if (!name)
        return;
    if (!strcmp(name, "skill_read")) {
        if (!exitCode)
            cache_skill(input, output);
        return;
    }
    WorkflowState* w = &memory.workflow;
    if (w->toolHistoryCount == WORKING_MEMORY_MAX_TOOL_HISTORY) {
        memmove(w->toolHistory,
                w->toolHistory + 1,
                (WORKING_MEMORY_MAX_TOOL_HISTORY - 1) * sizeof w->toolHistory[0]);
        w->toolHistoryCount--;
    }
    WorkflowToolRecord* r = &w->toolHistory[w->toolHistoryCount++];
    memset(r, 0, sizeof *r);
    copy(r->name, sizeof r->name, name);
    copy(r->input, sizeof r->input, input);
    copy(r->output, sizeof r->output, output);
    r->exitCode = exitCode;
    AgentSessionStateMarkDirty();
}
int WorkingMemoryAppendPrompt(char* buffer, size_t size) {
    if (!buffer || !size)
        return -1;
    size_t used = strlen(buffer);
    if (used >= size)
        return -1;
    int n = snprintf(buffer + used,
                     size - used,
                     "\n# Working memory\ntask: %s\ngoal: %s\ncurrent_command: "
                     "%s\nworkflow_status: %s\n",
                     memory.task,
                     memory.goal,
                     memory.currentCommand,
                     WorkflowStatusName(memory.workflow.status));
    if (n < 0 || (size_t) n >= size - used)
        return -1;
    used += (size_t) n;
    if (memory.workflow.active) {
        n = snprintf(buffer + used,
                     size - used,
                     "skill: %s\nstep: %s %s\n",
                     memory.workflow.skillPath,
                     memory.workflow.currentStepId,
                     memory.workflow.currentStepDescription);
        if (n < 0 || (size_t) n >= size - used)
            return -1;
        used += (size_t) n;
    }
    for (int i = 0; i < memory.skillCount; i++) {
        n = snprintf(buffer + used,
                     size - used,
                     "\n## Loaded skill: %s\n%s\n",
                     memory.skills[i].path,
                     memory.skills[i].content);
        if (n < 0 || (size_t) n >= size - used)
            return -1;
        used += (size_t) n;
    }
    return 0;
}
