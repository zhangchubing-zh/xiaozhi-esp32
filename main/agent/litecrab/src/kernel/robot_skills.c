/*
 * Embedded robot skill definitions.
 *
 * These are compile-time replacements for the file-based per-skill SKILL.md
 * discovery used by the PLC-diagnosis variant. Embedding the definitions in
 * rodata keeps the skill subsystem free of filesystem access (directory
 * scanning, openat traversal, SHA-256 file verification), which is
 * a prerequisite for the ESP32 port.
 *
 * Each definition provides:
 *   - name:        router/catalog identity (front-matter name equivalent)
 *   - description: selection metadata for the router (front-matter
 *                  description equivalent)
 *   - body:        SKILL.md body equivalent: workflow instructions the LLM
 *                  loads via skill_read after the router selects the skill.
 */
#include "litecrab/kernel.h"

#include <stddef.h>

const RobotSkillDef kRobotSkills[] = {
    {
        .name = "Robot.Movement",
        .description =
            "机器人运动控制。指导LLM使用 otto 工具完成前进、后退、转向、停止等动作，"
            "支持组合动作顺序执行、执行前电量检查和执行后状态确认。",
        .body =
            "# 机器人运动控制\n\n"
            "## 职责\n"
            "你负责把用户的运动意图翻译成一次或多次工具调用，并在执行后确认结果。\n\n"
            "## 可用工具\n"
            "- `self.otto.action`：执行单个动作。参数：`action`（如 walk/turn）、`direction`（方向）、"
            "`amount`（幅度）、`arm_swing`（摆臂）。参数含义以工具 schema 为准。\n"
            "- `self.otto.stop`：立即停止当前动作。用户喊停、动作异常或需要打断时优先调用。\n"
            "- `self.battery.get_level`：查询电量。首次运动前必查；电量过低时拒绝执行并告知用户。\n"
            "- `self.otto.get_status`：查询当前状态。组合动作完成后调用一次确认最终状态。\n\n"
            "## 执行流\n"
            "1. 首次运动请求：先 `self.battery.get_level`，电量不足则说明情况并结束。\n"
            "2. 单动作：直接调用 `self.otto.action`，读取结果判断成败。\n"
            "3. 组合动作（如\"向前走三步然后左转\"）：按用户表述的顺序逐个调用，每步确认成功再进行下一步；"
            "中途失败立即停止，不继续后续动作。\n"
            "4. 全部完成后调用 `self.otto.get_status` 确认，然后向用户报告实际执行了什么。\n\n"
            "## 规则\n"
            "- 不要在一次回复里虚构未执行的动作；只报告工具结果支持的事实。\n"
            "- 幅度类参数拿不准时取保守值，宁可分多次小步执行。\n"
            "- 用户要求停止时，先 `self.otto.stop`，再回复。\n\n"
            "## 完成判据\n"
            "所有动作调用返回成功、状态确认完成、用户意图达成，即可总结收尾。",
    },
    {
        .name = "Robot.Pose",
        .description =
            "机器人预设动作表演。指导LLM根据情境从 servo_sequences 预设序列中选择合适的舞蹈、"
            "姿势或庆祝动作并执行。",
        .body =
            "# 机器人预设动作表演\n\n"
            "## 职责\n"
            "根据用户情绪或情境描述，从预设动作序列中选择并执行最匹配的一项。\n\n"
            "## 可用工具\n"
            "- `self.otto.servo_sequences`：执行预设动作序列。可用序列及参数以工具 schema 为准。\n"
            "- `self.otto.stop`：表演途中用户要求停止时调用。\n\n"
            "## 执行流\n"
            "1. 识别情境：庆祝、打招呼、跳舞、展示等。\n"
            "2. 对照工具 schema 中列出的序列清单，选择最贴切的一项；不确定时选通用性最强的序列。\n"
            "3. 调用 `self.otto.servo_sequences` 执行，确认成功后简短回应（不要重复描述动作细节）。\n\n"
            "## 规则\n"
            "- 一次只执行一个序列，不叠加。\n"
            "- 用户只问\"你会什么动作\"时，列出能力即可，不要直接执行。\n\n"
            "## 完成判据\n"
            "序列调用返回成功且已向用户确认。",
    },
    {
        .name = "Robot.Status",
        .description =
            "机器人状态查询。指导LLM使用电量、IP、设备状态等查询工具，一次性给出简洁的状态播报。",
        .body =
            "# 机器人状态查询\n\n"
            "## 职责\n"
            "回答关于机器人自身状态的问题：电量、IP 地址、网络、音量、屏幕亮度等。\n\n"
            "## 可用工具\n"
            "- `self.battery.get_level`：电量。\n"
            "- `self.otto.get_ip`：IP 地址。\n"
            "- `self.otto.get_status`：机器人运动控制器状态。\n"
            "- `self.get_device_status`：整机状态（音量、屏幕、网络等）。\n\n"
            "## 执行流\n"
            "1. 按问题选择工具，只查被问到的项；明确要求\"整体状态\"时优先 `self.get_device_status`。\n"
            "2. 一次查询即可回答，不要为单个问题连环调用多个工具。\n\n"
            "## 规则\n"
            "- 播报格式：一句话结论优先（如\"电量 78%\"），需要时再补充细节。\n"
            "- 查询失败时如实说明，不要编造数值。\n\n"
            "## 完成判据\n"
            "查询成功并以简洁结论回应。",
    },
    {
        .name = "Robot.Calibration",
        .description =
            "机器人行走校准。指导LLM读取舵机微调值、分析跑偏方向、小步修正并复走验证，形成校准闭环。",
        .body =
            "# 机器人行走校准\n\n"
            "## 职责\n"
            "处理\"走路跑偏/歪/往一边拐\"类问题：读取当前 trim，分析偏差，保守修正，复走验证。\n\n"
            "## 可用工具\n"
            "- `self.otto.get_trims`：读取左右腿舵机微调值。\n"
            "- `self.otto.set_trim`：设置单个舵机微调。参数以工具 schema 为准。\n"
            "- `self.otto.action`：执行一小段直线行走作为验证。\n"
            "- `self.otto.stop`：异常时停止。\n\n"
            "## 执行流\n"
            "1. 读取：`self.otto.get_trims` 获取当前两侧微调值。\n"
            "2. 分析：根据用户描述的跑偏方向判断需要增大还是减小哪一侧的值。\n"
            "3. 修正：调用 `self.otto.set_trim` 做小步调整（一次只改一个舵机、小幅度）。\n"
            "4. 验证：执行一小段直线行走，请用户确认是否仍偏。\n"
            "5. 若仍偏，回到第 3 步继续小幅修正；最多三轮，避免大幅来回改动。\n\n"
            "## 规则\n"
            "- 修正量保守：小幅多次优于一次大改。\n"
            "- 不确定偏哪边时先问用户，不要盲调。\n"
            "- 每轮修正后复述当前 trim 值，方便用户掌握进度。\n\n"
            "## 完成判据\n"
            "用户确认走直，或已达到三轮上限时如实说明当前状态与建议。",
    },
};
const size_t kRobotSkillCount = sizeof kRobotSkills / sizeof kRobotSkills[0];

const RobotSkillDef* RobotSkillsAll(int* count) {
    if (count)
        *count = (int) kRobotSkillCount;
    return kRobotSkills;
}
