#ifndef LITECRAB_WORKING_MEMORY_H
#define LITECRAB_WORKING_MEMORY_H
#include <stddef.h>

#define WORKING_MEMORY_MAX_OPTIONS 16
#define WORKING_MEMORY_MAX_TOOL_HISTORY 8
#define WORKING_MEMORY_MAX_SKILLS 4

typedef enum {
    WORKFLOW_INACTIVE = 0,
    WORKFLOW_RUNNING,
    WORKFLOW_WAITING_INPUT,
    WORKFLOW_COMPLETED,
    WORKFLOW_FAILED
} WorkflowStatus;

typedef struct {
    char label[128], value[64];
} WorkflowOption;
typedef struct {
    char name[64], input[512], output[2048];
    int exitCode;
} WorkflowToolRecord;
typedef struct {
    char path[256];
    char content[8192];
} WorkingMemorySkill;
typedef struct {
    int active;
    WorkflowStatus status;
    char skillPath[256], scriptPath[256], stepsPath[256];
    char currentStepId[64], currentStepDescription[256];
    WorkflowOption options[WORKING_MEMORY_MAX_OPTIONS];
    int optionCount;
    WorkflowToolRecord toolHistory[WORKING_MEMORY_MAX_TOOL_HISTORY];
    int toolHistoryCount;
} WorkflowState;
typedef struct {
    char task[256], goal[512], currentCommand[256];
    WorkflowState workflow;
    WorkingMemorySkill skills[WORKING_MEMORY_MAX_SKILLS];
    int skillCount;
} WorkingMemory;

void WorkingMemoryInit(void);
void WorkingMemoryClose(void);
void WorkingMemoryBeginRequest(const char* task);
void WorkingMemorySetGoal(const char* goal);
const WorkingMemory* WorkingMemoryGet(void);
int WorkingMemoryActivate(const char* skillPath);
void WorkingMemorySetStatus(WorkflowStatus status);
void WorkingMemorySetStep(const char* id, const char* description);
void WorkingMemorySetPaths(const char* scriptPath, const char* stepsPath);
int WorkingMemoryParseOptions(const char* text);
void WorkingMemoryBeforeTool(const char* name, const char* input);
void WorkingMemoryAfterTool(const char* name, const char* input, const char* output, int exitCode);
void WorkingMemoryCacheSkill(const char* path, const char* output);
int WorkingMemoryAppendPrompt(char* buffer, size_t size);
const char* WorkflowStatusName(WorkflowStatus status);
#endif
