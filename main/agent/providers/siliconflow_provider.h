#ifndef AGENT_SILICONFLOW_PROVIDER_H
#define AGENT_SILICONFLOW_PROVIDER_H

#include <memory>

#include "provider.h"

std::unique_ptr<AsrProvider> MakeSiliconFlowAsr(const AgentConfig& cfg);
std::unique_ptr<LlmProvider> MakeSiliconFlowLlm(const AgentConfig& cfg);
std::unique_ptr<TtsProvider> MakeSiliconFlowTts(const AgentConfig& cfg);

#endif  // AGENT_SILICONFLOW_PROVIDER_H
