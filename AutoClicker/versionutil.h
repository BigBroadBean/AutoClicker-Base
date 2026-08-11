#pragma once

// ============================================================
//  版本号工具（纯函数，无依赖，可单测）
// ============================================================

#include <string>
#include <cctype>

// ---- 点分数字版本比较 ----
// 动态解析：忽略 v 前缀、分隔符（. - _）、字母后缀，只比较数字段，
// 各段按数值比较（不是字符串），段数不同时缺段按 0 处理。
// 返回：<0 a<b，0 相等，>0 a>b
//   "v2.5" vs "2.5"     -> 0
//   "2.5"  vs "2.6.0"   -> -1   （小数点位数不同）
//   "2.5.1" vs "2.5"    ->  1
//   "2.10" vs "2.9"     ->  1   （按数值而非字典序）
//   "1.20.1" vs "1.2"   ->  1   （MC 版本号场景）
//   "2.5-beta" vs "2.5" ->  0   （字母后缀被忽略）
//   "2.5.0-rc1" vs "2.5" ->  0  （预发布后缀被忽略）
inline int CompareVersions(const std::string& a, const std::string& b)
{
    size_t ia = 0, ib = 0;
    bool aHasNum = false, bHasNum = false;   // 是否已解析过数字段
    for (;;) {
        // 跳到下一个数字段的开始（忽略 v 前缀 / 分隔符）
        // 已解析过数字段后遇到字母 = 后缀开始（beta/rc 等），忽略其后全部内容
        while (ia < a.size() && !isdigit((unsigned char)a[ia])) {
            if (aHasNum && isalpha((unsigned char)a[ia])) { ia = a.size(); break; }
            ia++;
        }
        while (ib < b.size() && !isdigit((unsigned char)b[ib])) {
            if (bHasNum && isalpha((unsigned char)b[ib])) { ib = b.size(); break; }
            ib++;
        }

        int na = 0, nb = 0;
        while (ia < a.size() && isdigit((unsigned char)a[ia])) {
            na = na * 10 + (a[ia++] - '0');
            aHasNum = true;
        }
        while (ib < b.size() && isdigit((unsigned char)b[ib])) {
            nb = nb * 10 + (b[ib++] - '0');
            bHasNum = true;
        }

        if (na != nb) return na < nb ? -1 : 1;
        // 本段相等：两边都到字符串末尾才认为完全相等，否则继续下一段
        // （某一方先结束：下一轮该方数字段为 0，自然小于另一方）
        if (ia >= a.size() && ib >= b.size()) return 0;
    }
}

// ---- 显示用版本号规范化 ----
// 去掉首尾空白与 v/V 前缀（比较用原串，显示用这个）:
//   "v2.6" -> "2.6"，"V2.6.0" -> "2.6.0"，" 2.5 " -> "2.5"
inline std::string NormalizeVersionDisplay(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    std::string v = s.substr(b, e - b + 1);
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
    return v;
}

// ---- JSON 字符串提取 ----
// 从 JSON 中提取 "key": "value" 的字符串值。
// 处理 \n \r \t \" \\ 转义；\uXXXX 不处理（服务器输出原始 UTF-8，内容受控）。
inline bool GetJsonString(const std::string& json, const std::string& key,
                          std::string& out)
{
    std::string pat = "\"" + key + "\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    // 跳过空白到 ':'
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == '\r' || json[pos] == '\n')) pos++;
    if (pos >= json.size() || json[pos] != ':') return false;
    pos++;
    // 跳过空白到字符串引号
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == '\r' || json[pos] == '\n')) pos++;
    if (pos >= json.size() || json[pos] != '"') return false;
    pos++;

    out.clear();
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '"') return true;   // 字符串结束
        if (c == '\\' && pos < json.size()) {
            char e = json[pos++];
            switch (e) {
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case '\\': out += '\\'; break;
            case '"':  out += '"';  break;
            default:   out += e;    break;   // 其余转义按原样（够用）
            }
        } else {
            out += c;
        }
    }
    return false;   // 未闭合
}
