// CompareVersions 单元测试（临时，不进仓库）
#include <cstdio>
#include <string>
#include "versionutil.h"

static int g_fail = 0;

static void Check(const char* a, const char* b, int expect)
{
    int r = CompareVersions(a, b);
    const char* ok = (r == expect) ? "PASS" : "FAIL";
    if (r != expect) g_fail++;
    printf("[%s] CompareVersions(\"%s\", \"%s\") = %2d (expect %d)\n",
           ok, a, b, r, expect);
}

static void CheckNorm(const char* in, const char* expect)
{
    std::string r = NormalizeVersionDisplay(in);
    const char* ok = (r == expect) ? "PASS" : "FAIL";
    if (r != expect) g_fail++;
    printf("[%s] NormalizeVersionDisplay(\"%s\") = \"%s\" (expect \"%s\")\n",
           ok, in, r.c_str(), expect);
}

int main()
{
    printf("=== 带 v 前缀 ===\n");
    Check("v2.5", "2.5", 0);        // v 前缀忽略
    Check("V2.6", "2.5", 1);        // 大写 V
    Check("2.5", "v2.6", -1);
    Check("v2.5.1", "2.5", 1);

    printf("=== 小数点位数不同 ===\n");
    Check("2.5", "2.6.0", -1);      // 位数不同：2.6.0 更新
    Check("2.5.1", "2.5", 1);
    Check("2.5", "2.5.0", 0);       // 补零等价
    Check("2.5.0.0", "2.5", 0);
    Check("2.5", "2.5.0.1", -1);

    printf("=== 按数值比较（非字典序）===\n");
    Check("2.10", "2.9", 1);        // "2.10" > "2.9"
    Check("1.20.1", "1.2", 1);      // MC 版本
    Check("1.8.9", "1.12.2", -1);
    Check("10.0", "9.9", 1);

    printf("=== 后缀 / 混合格式 ===\n");
    Check("2.5-beta", "2.5", 0);    // 字母后缀忽略
    Check("2.5.0-rc1", "2.5", 0);
    Check("v1.2.3", "1.2.2", 1);
    Check(" 2.5 ", "2.5", 0);       // 空白忽略

    printf("=== 边界 ===\n");
    Check("", "", 0);
    Check("", "1", -1);
    Check("1", "", 1);
    Check("abc", "1", -1);          // 无数字视为 0

    printf("=== 真实场景（本地 2.5）===\n");
    Check("2.5", "2.5", 0);         // 相同 → 不提示
    Check("2.5", "2.4", 1);         // 服务器旧 → 不提示
    Check("2.5", "2.6", -1);        // 服务器新 → 提示
    Check("2.5", "v2.6.0", -1);
    Check("2.5", "2.10", -1);

    printf("=== 显示规范化（NormalizeVersionDisplay）===\n");
    CheckNorm("v2.6", "2.6");
    CheckNorm("V2.6.0", "2.6.0");
    CheckNorm(" 2.5 ", "2.5");
    CheckNorm("2.6", "2.6");
    CheckNorm("", "");

    printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail);
    return g_fail ? 1 : 0;
}
