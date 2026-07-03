#ifndef _COMPANION_MCP_TOOLS_H_
#define _COMPANION_MCP_TOOLS_H_

// 我方设备能力 MCP 工具(从上游 mcp_server.cc 的 AddCommonTools 抽出)。
// mcp_server.cc 是通用框架,工具本应外部注册:在 application 初始化时、AddCommonTools 之后调用本函数。
void AddCompanionMcpTools();

#endif // _COMPANION_MCP_TOOLS_H_
