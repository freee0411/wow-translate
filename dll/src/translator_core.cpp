// translator_core.cpp - Translation functionality for WoWTranslate
// DeepSeek 版改造：把翻译引擎换成 DeepSeek Chat Completions（OpenAI 兼容格式），
// 用 system prompt 锁定「魔兽世界游戏内口语聊天」的译文风格。
//
// 相对上游 paokkerkir/wow-translate main 分支同名文件的改动：
//   1. 端点：fanyi-api.baidu.com / translate.googleapis.com -> api.deepseek.com/chat/completions
//   2. 请求方式：GET+ query -> POST + JSON body，头里带 Authorization: Bearer <key>
//   3. 认证：百度 MD5 签名 / Google 免 key -> 直接 Bearer token（无需签名）
//   4. 响应解析：trans_result[].dst -> choices[0].message.content
//   5. 新增译文风格控制：system prompt（口语化、英文小写开头、保留游戏缩写与占位符）
//   6. 新增输出清洗：剥引号/剥"翻译："前缀/取首行/异常长度丢弃
//   7. 新增容错：401/402 冷却 60 秒、5xx 重试一次、模型名自动回退
// 其余结构（缓存/异步队列/Worker 线程/错误类型）与上游保持一致，接口不变，可直接替换编译。

#include <windows.h>
#include <winhttp.h>
#include <string>
#include <algorithm>
#include <sstream>
#include <vector>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cctype>

#include "../include/translator_core.h"
#include "../include/logging.h"
#include "../include/utils.h"

using namespace std;

// ============================================================================
// DeepSeek 配置
//   API Key 取值优先级：环境变量 DEEPSEEK_API_KEY -> DLL 同级目录 deepseek_key.txt -> 下面的常量
//   这样换 key 不用重新编译，也不会把 key 写进源码里到处传。
// ============================================================================
static const char* DEEPSEEK_HOST  = "api.deepseek.com";
static const char* DEEPSEEK_MODEL = "deepseek-v4-flash";   // 主模型（快、便宜）
static const char* DEEPSEEK_FALLBACK_MODEL = "deepseek-chat"; // 主模型名被拒时自动回退（映射到 v4-flash 非思考模式）
static const char* DEEPSEEK_API_KEY = "";                  // 留空则从环境变量/文件读

// 英文译文是否强制小写开头（去掉书面式的首字母大写）。想保留原样改成 false。
static const bool LOWERCASE_ENGLISH = true;

// 单次请求的硬上限：避免模型话痨时把 token 和时间都耗在废话上
static const int MAX_OUTPUT_TOKENS_CAP = 400;

// ============================================================================
// 风格规则（system prompt 的固定前缀）
// 注意：这段文本必须逐字节稳定，DeepSeek 的上下文缓存按前缀命中，
//       命中价是未命中的 1/50，所以固定内容放前面、变量（目标语言）放最后。
// ============================================================================
static const char* STYLE_RULES =
    "You are a real-time translator for World of Warcraft in-game chat.\n"
    "Follow these rules strictly:\n"
    "1. Translate into the target language as spoken chat, not written prose. "
    "Keep it short, casual and direct, the way players type in guild/party chat. "
    "Never use formal register or written-style connectives (therefore, however, moreover, thus).\n"
    "2. For English output, start with a lowercase letter. Do NOT capitalize the first word. "
    "Do not add a period at the end. Keep any question marks, exclamation marks, ellipses and emoticons that were in the original.\n"
    "3. Keep game abbreviations, slang and jargon untranslated in both directions: "
    "MC BWL AQ AQ40 NAXX ZG ZF SM DM DPS OT AFK GG LOL LFG LFM WTS WTB WTT inc pull buff debuff aggro wipe loot roll gdkp plz thx np brt omw.\n"
    "4. Never translate player names, guild names or pet names - keep them exactly as written. "
    "If unsure about an item or spell name, keep the original.\n"
    "5. Keep numbers, percentages, durations, coordinates, URLs and emoticons unchanged.\n"
    "6. Text in the form http://ph.wt/1 is a placeholder for a clickable item/quest link. "
    "Reproduce it byte-for-byte: same case, no spaces, no line breaks, never translate or reformat it.\n"
    "7. If the input is already in the target language, return it unchanged.\n"
    "8. Output ONLY the translation: no quotes, no 'Translation:' prefix, no explanation, "
    "no romanization, no notes, no extra sentences.\n"
    "\nStyle examples (follow the tone, do not copy the wording):\n"
    "lfm healer for UBRS, pst  ->  UBRS 缺个治疗，密我\n"
    "omw, 2 min  ->  马上到，2分钟\n"
    "anyone selling flasks?  ->  有人卖合剂吗\n"
    "门口有人吗  ->  anyone at the gate?\n"
    "这boss掉啥  ->  what does this boss drop\n"
    "gg wp  ->  gg wp\n";

// ============================================================================
// 小工具
// ============================================================================
static string Trim(const string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) e--;
    return s.substr(b, e - b);
}

// UTF-8 字符数（非续字节计数），用来估算 max_tokens 与判断译文长度是否异常
static int Utf8CharCount(const string& s) {
    int n = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) n++;
    }
    return n;
}

// JSON 字符串转义（UTF-8 字节直接透传，这是合法 JSON）
static string EscapeJson(const string& s) {
    string out;
    out.reserve(s.size() + 32);
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    static const char* hexDigits = "0123456789abcdef";
                    out += "\\u00";
                    out += hexDigits[(c >> 4) & 0x0F];
                    out += hexDigits[c & 0x0F];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// 目标语言的人话名字（写进 prompt，比 "zh" 这种代号可靠）
static string LangDisplayName(const string& lang) {
    if (lang == "zh" || lang == "zh-CN" || lang == "zh-cn" || lang == "zh_CN") return "Simplified Chinese";
    if (lang == "en") return "English";
    if (lang == "ja") return "Japanese";
    if (lang == "ko") return "Korean";
    if (lang == "ru") return "Russian";
    return lang;
}

// ---- 风格规则可外部覆盖：DLL 同级目录下的 deepseek_style.txt ------------------
// 有该文件就用文件内容（整段替换内置规则），没有就用内置 STYLE_RULES。
// 这样调风格不用重新编译：改完文件重启游戏即可（内容 UTF-8 或 GBK 都能读）。
static string g_styleRules;
static bool g_styleLoaded = false;

static bool LooksLikeUtf8(const string& s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int need = 0;
        if (c < 0x80) { i++; continue; }
        else if ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else return false;

        if (i + need >= s.size()) return false;
        for (int k = 1; k <= need; k++) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += need + 1;
    }
    return true;
}

// 把系统 ANSI 代码页（中文 Windows 上是 GBK）转成 UTF-8，避免用户用记事本存成 GBK 时乱码
static string AnsiToUtf8(const string& s) {
    int wlen = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (wlen <= 0) return s;
    wstring w(wlen, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &w[0], wlen);

    int ulen = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wlen, nullptr, 0, nullptr, nullptr);
    if (ulen <= 0) return s;
    string u(ulen, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wlen, &u[0], ulen, nullptr, nullptr);
    return u;
}

static string ReadWholeFile(const string& path) {
    ifstream f(path.c_str(), ios::in | ios::binary);
    if (!f) return "";
    ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static string LoadStyleOverride() {
    vector<string> candidates;
    char mod[MAX_PATH] = {0};
    HMODULE hSelf = GetModuleHandleA("WoWTranslate.dll");
    if (hSelf && GetModuleFileNameA(hSelf, mod, MAX_PATH)) {
        string p(mod);
        size_t slash = p.find_last_of("\\/");
        if (slash != string::npos) candidates.push_back(p.substr(0, slash + 1) + "deepseek_style.txt");
    }
    candidates.push_back("deepseek_style.txt");

    for (size_t i = 0; i < candidates.size(); i++) {
        string raw = ReadWholeFile(candidates[i]);
        if (raw.empty()) continue;

        // 去 UTF-8 BOM
        if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
            static_cast<unsigned char>(raw[1]) == 0xBB && static_cast<unsigned char>(raw[2]) == 0xBF) {
            raw = raw.substr(3);
        }
        if (!LooksLikeUtf8(raw)) raw = AnsiToUtf8(raw);

        raw = Trim(raw);
        if (!raw.empty()) {
            LOG_INFO("Style rules loaded from: " + candidates[i]);
            return raw;
        }
    }
    return "";
}

static void EnsureStyleLoaded() {
    if (g_styleLoaded) return;
    g_styleLoaded = true;
    string custom = LoadStyleOverride();
    g_styleRules = custom.empty() ? string(STYLE_RULES) : custom;
}

static string BuildSystemPrompt(const string& targetLang) {
    EnsureStyleLoaded();
    return g_styleRules + "\nTarget language: " + LangDisplayName(targetLang) + ".\n";
}

// 估算输出 token 上限：中英互译时输出长度通常不超过输入的 2 倍
static int EstimateMaxTokens(const string& text) {
    int chars = Utf8CharCount(text);
    int tokens = 48 + chars * 2;
    if (tokens < 64) tokens = 64;
    if (tokens > MAX_OUTPUT_TOKENS_CAP) tokens = MAX_OUTPUT_TOKENS_CAP;
    return tokens;
}

// 万一某个模型版本不再接受 max_tokens 这个字段，收到 400 后自动降级，不再带它
static bool g_omitMaxTokens = false;

static string BuildRequestBody(const string& model, const string& sysPrompt,
                               const string& text, int maxTokens) {
    string body = "{\"model\":\"" + model + "\",\"messages\":["
                  "{\"role\":\"system\",\"content\":\"" + EscapeJson(sysPrompt) + "\"},"
                  "{\"role\":\"user\",\"content\":\"" + EscapeJson(text) + "\"}"
                  "],\"temperature\":0,\"stream\":false";
    if (!g_omitMaxTokens) body += ",\"max_tokens\":" + to_string(maxTokens);
    body += "}";
    return body;
}

// UTF-8 codepoint 编码（解码 \uXXXX 用）
static string ConvertCodepointToUTF8(unsigned int codepoint) {
    string result;
    if (codepoint <= 0x7F) {
        result += static_cast<char>(codepoint);
    } else if (codepoint <= 0x7FF) {
        result += static_cast<char>(0xC0 | (codepoint >> 6));
        result += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0xFFFF) {
        result += static_cast<char>(0xE0 | (codepoint >> 12));
        result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0x10FFFF) {
        result += static_cast<char>(0xF0 | (codepoint >> 18));
        result += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    return result;
}

// 从 pos 之后找 "key" 的值，并解析成字符串（处理转义与 \uXXXX）
static string ExtractJsonString(const string& json, size_t from, const string& key) {
    if (from >= json.size()) return "";

    string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey, from);
    if (keyPos == string::npos) return "";

    size_t colon = json.find(":", keyPos + searchKey.size());
    if (colon == string::npos) return "";

    size_t p = colon + 1;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' || json[p] == '\r')) p++;
    if (p >= json.size() || json[p] != '"') return "";  // null 或非字符串
    p++;

    string out;
    while (p < json.size() && json[p] != '"') {
        if (json[p] == '\\' && p + 1 < json.size()) {
            p++;
            switch (json[p]) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'u': {
                    if (p + 4 < json.size()) {
                        string hex = json.substr(p + 1, 4);
                        try {
                            unsigned int cp = static_cast<unsigned int>(stoul(hex, nullptr, 16));
                            out += ConvertCodepointToUTF8(cp);
                            p += 4;
                        } catch (...) {}
                    }
                    break;
                }
                default: out += json[p]; break;
            }
        } else {
            out += json[p];
        }
        p++;
    }
    return out;
}

// 解析数字字段（用于读取 usage.prompt_tokens / completion_tokens）
static long long ExtractJsonInt(const string& json, const string& key) {
    string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == string::npos) return -1;

    size_t colon = json.find(":", keyPos + searchKey.size());
    if (colon == string::npos) return -1;

    size_t p = colon + 1;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) p++;
    size_t start = p;
    while (p < json.size() && (isdigit(static_cast<unsigned char>(json[p])) || json[p] == '-')) p++;
    if (p == start) return -1;

    return atoll(json.substr(start, p - start).c_str());
}

// 解析 Chat Completions 响应，取出 choices[0].message.content
static string ParseChatCompletionContent(const string& json) {
    size_t choices = json.find("\"choices\"");
    if (choices == string::npos) return "";

    size_t message = json.find("\"message\"", choices);
    if (message == string::npos) return "";

    return ExtractJsonString(json, message, "content");
}

// 清洗模型输出：游戏聊天只有一行，别把模型的口头禅带进聊天框
static string CleanTranslation(const string& raw, const string& sourceText, const string& targetLang) {
    string s = Trim(raw);
    if (s.empty()) return "";

    // 只取第一行（模型偶尔会补一句解释）
    size_t nl = s.find('\n');
    if (nl != string::npos) s = Trim(s.substr(0, nl));
    if (s.empty()) return "";

    // 剥掉包裹的引号：ASCII 双引号、中文引号、直角引号
    static const char* pairs[][2] = {
        {"\"", "\""}, {"'", "'"},
        {"\xE2\x80\x9C", "\xE2\x80\x9D"},   // “ ”
        {"\xE3\x80\x8C", "\xE3\x80\x8D"}    // 「 」
    };
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        string l = pairs[i][0], r = pairs[i][1];
        if (s.size() > l.size() + r.size() &&
            s.compare(0, l.size(), l) == 0 &&
            s.compare(s.size() - r.size(), r.size(), r) == 0) {
            s = Trim(s.substr(l.size(), s.size() - l.size() - r.size()));
            break;
        }
    }

    // 剥掉 "翻译：/译文：/Translation:" 之类的前缀
    static const char* prefixes[] = {
        "\xE7\xBF\xBB\xE8\xAF\x91\xEF\xBC\x9A",  // 翻译：
        "\xE8\xAF\x91\xE6\x96\x87\xEF\xBC\x9A",  // 译文：
        "Translation:", "Translation :", "Translated text:"
    };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        string p = prefixes[i];
        if (s.size() > p.size() && s.compare(0, p.size(), p) == 0) {
            s = Trim(s.substr(p.size()));
            break;
        }
    }

    if (s.empty()) return "";

    // 明显不是译文（模型在拒绝/解释）就直接判失败，让上层回退原文
    if (s.find("As an AI") != string::npos ||
        s.find("I cannot") != string::npos ||
        s.find("\xE6\x8A\xB1\xE6\xAD\x89") == 0) {   // 以「抱歉」开头
        return "";
    }

    int srcChars = Utf8CharCount(sourceText);
    if (Utf8CharCount(s) > srcChars * 6 + 80) return "";  // 长度离谱，丢弃

    // 英文：去掉书面式的首字母大写（玩家名/全大写缩写不动）
    if (LOWERCASE_ENGLISH && (targetLang == "en" || targetLang == "en-US" || targetLang == "en-GB")) {
        if (s.size() > 1 && s[0] >= 'A' && s[0] <= 'Z') {
            size_t endWord = 0;
            while (endWord < s.size() && isalpha(static_cast<unsigned char>(s[endWord]))) endWord++;
            bool restLower = true;
            for (size_t i = 1; i < endWord; i++) {
                if (s[i] >= 'A' && s[i] <= 'Z') { restLower = false; break; }
            }
            if (restLower) s[0] = static_cast<char>(s[0] - 'A' + 'a');
        }
    }

    return s;
}

// ============================================================================
// Windows 用户名下的 HTTP POST（JSON）
// ============================================================================
static bool HttpsPostJson(HINTERNET hConnect, const wstring& wPath, const string& body,
                          const string& headers, string& response, DWORD& statusCode) {
    response.clear();
    statusCode = 0;
    if (!hConnect) return false;

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"POST", wPath.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        LOG_ERROR("Failed to open POST request");
        return false;
    }

    wstring wHeaders(headers.begin(), headers.end());
    WinHttpAddRequestHeaders(hRequest, wHeaders.c_str(), (DWORD)-1L,
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    BOOL sent = WinHttpSendRequest(hRequest,
                                   WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   (LPVOID)body.c_str(), (DWORD)body.size(),
                                   (DWORD)body.size(), 0);

    if (!sent) {
        LOG_ERROR("POST send failed: " + to_string(GetLastError()));
        WinHttpCloseHandle(hRequest);
        return false;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        LOG_ERROR("POST receive failed: " + to_string(GetLastError()));
        WinHttpCloseHandle(hRequest);
        return false;
    }

    DWORD dwSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &dwSize,
                        WINHTTP_NO_HEADER_INDEX);

    DWORD bytesAvailable = 0;
    char buffer[8192];
    while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
        DWORD bytesRead = 0;
        DWORD bytesToRead = bytesAvailable < (sizeof(buffer) - 1)
                            ? bytesAvailable : (DWORD)(sizeof(buffer) - 1);
        if (WinHttpReadData(hRequest, buffer, bytesToRead, &bytesRead)) {
            buffer[bytesRead] = '\0';
            response.append(buffer, bytesRead);
        } else {
            break;
        }
    }

    WinHttpCloseHandle(hRequest);
    return true;
}

// ============================================================================
// API Key 解析：环境变量 -> 同级目录 deepseek_key.txt -> 源码常量
// ============================================================================
static string TrimKey(const string& in) {
    string s = in;
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF) {
        s = s.substr(3);   // 去 UTF-8 BOM（记事本存文件会带）
    }
    return Trim(s);
}

static bool KeyLooksValid(const string& key) {
    if (key.size() < 20) return false;          // 占位符/空串一律不合格
    if (key.compare(0, 3, "sk-") != 0) return false;
    return true;
}

static string ResolveApiKey(string& source) {
    char env[512] = {0};
    DWORD n = GetEnvironmentVariableA("DEEPSEEK_API_KEY", env, sizeof(env));
    if (n > 0 && n < sizeof(env)) {
        string key = TrimKey(string(env, n));
        if (KeyLooksValid(key)) { source = "env DEEPSEEK_API_KEY"; return key; }
    }

    vector<string> candidates;
    char mod[MAX_PATH] = {0};
    HMODULE hSelf = GetModuleHandleA("WoWTranslate.dll");
    if (hSelf && GetModuleFileNameA(hSelf, mod, MAX_PATH)) {
        string p(mod);
        size_t slash = p.find_last_of("\\/");
        if (slash != string::npos) candidates.push_back(p.substr(0, slash + 1) + "deepseek_key.txt");
    }
    candidates.push_back("deepseek_key.txt");

    for (size_t i = 0; i < candidates.size(); i++) {
        ifstream f(candidates[i].c_str());
        if (!f) continue;
        string line;
        if (!getline(f, line)) continue;
        string key = TrimKey(line);
        if (KeyLooksValid(key)) { source = candidates[i]; return key; }
    }

    string key = TrimKey(string(DEEPSEEK_API_KEY));
    if (KeyLooksValid(key)) { source = "compiled-in constant"; return key; }

    source = "not configured";
    return "";
}

// Global variables
unique_ptr<TranslationClient> g_translator = nullptr;
char g_translation_buffer[4096] = {0};
char g_error_buffer[256] = {0};

// 鉴权/欠费后的冷却截止时间：避免每条聊天都撞一次 401/402
static DWORD g_apiBlockedUntil = 0;

// 运行期只解析一次 key
static string g_apiKey;
static bool g_apiKeyResolved = false;

// ============================================================================
// TranslationClient
// ============================================================================
TranslationClient::TranslationClient()
    : hSession(nullptr), hConnect(nullptr), initialized(false), running(false) {
}

TranslationClient::~TranslationClient() {
    Cleanup();
}

bool TranslationClient::Initialize() {
    if (initialized) Cleanup();

    if (!g_apiKeyResolved) {
        string source;
        g_apiKey = ResolveApiKey(source);
        g_apiKeyResolved = true;
        if (g_apiKey.empty()) {
            LOG_ERROR("DeepSeek API key not configured. Set env DEEPSEEK_API_KEY, "
                      "or put deepseek_key.txt next to WoWTranslate.dll (one line: sk-xxxx).");
            return false;
        }
        LOG_INFO("DeepSeek API key loaded from: " + source);
    }

    // 在起工作线程之前把风格规则读好，避免多线程下读到半初始化的 prompt
    EnsureStyleLoaded();

    const std::string host = DEEPSEEK_HOST;
    const int port = 443;

    LOG_INFO("Initializing DeepSeek API client");

    hSession = WinHttpOpen(L"WoWTranslate/1.0",
                           WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS,
                           0);
    if (!hSession) {
        LOG_ERROR("Failed to initialize WinHTTP session");
        return false;
    }

    // 模型首字延迟通常在 300~800ms，给足接收时间，避免长句被截断
    WinHttpSetTimeouts(hSession, 5000, 5000, 10000, 20000);

    wstring wHost(host.begin(), host.end());
    hConnect = WinHttpConnect(hSession, wHost.c_str(),
                              static_cast<INTERNET_PORT>(port), 0);
    if (!hConnect) {
        LOG_ERROR("Failed to create WinHTTP connection handle");
        WinHttpCloseHandle(hSession);
        hSession = nullptr;
        return false;
    }

    running = true;
    workerThread = thread(&TranslationClient::WorkerThreadFunc, this);
    initialized = true;
    LOG_INFO("DeepSeek API client initialized");
    return true;
}

void TranslationClient::Cleanup() {
    if (running) {
        running = false;
        if (workerThread.joinable()) {
            workerThread.join();
        }
    }

    if (hConnect) {
        WinHttpCloseHandle(hConnect);
        hConnect = nullptr;
    }

    if (hSession) {
        WinHttpCloseHandle(hSession);
        hSession = nullptr;
    }

    cache.clear();
    initialized = false;
    LOG_INFO("Translation client cleanup complete");
}

// 上游接口保留：DeepSeek 不需要语言码转换，这里只做直通
string TranslationClient::MapLangCode(const string& lang) {
    return lang;
}

string TranslationClient::GenerateCacheKey(const string& text, const string& sourceLang, const string& targetLang) {
    return sourceLang + "->" + targetLang + ":" + text;
}

void TranslationClient::CleanExpiredCache() {
    DWORD currentTime = GetTickCount();
    auto it = cache.begin();

    while (it != cache.end()) {
        if (currentTime - it->second.timestamp > CACHE_EXPIRY_MS) {
            it = cache.erase(it);
        } else {
            ++it;
        }
    }

    if (cache.size() > MAX_CACHE_SIZE) {
        size_t removeCount = cache.size() - MAX_CACHE_SIZE / 2;
        for (size_t i = 0; i < removeCount && !cache.empty(); ++i) {
            cache.erase(cache.begin());
        }
    }
}

TranslationResult TranslationClient::TranslateText(const string& text, string& result,
                                                    const string& sourceLang,
                                                    const string& targetLang) {
    if (!initialized) return TranslationResult::INVALID_PARAMS;
    if (Trim(text).empty()) return TranslationResult::INVALID_PARAMS;

    // DLL 侧缓存
    string cacheKey = GenerateCacheKey(text, sourceLang, targetLang);
    auto cacheIt = cache.find(cacheKey);
    if (cacheIt != cache.end() &&
        (GetTickCount() - cacheIt->second.timestamp) < CACHE_EXPIRY_MS) {
        result = cacheIt->second.translation;
        LOG_DEBUG("Cache hit: " + text.substr(0, 50));
        return TranslationResult::SUCCESS;
    }

    CleanExpiredCache();

    // key 无效/余额不足后的冷却期内直接跳过，不再逐条撞墙
    if (GetTickCount() < g_apiBlockedUntil) {
        return TranslationResult::API_ERROR;
    }

    string sysPrompt = BuildSystemPrompt(targetLang);
    int maxTokens = EstimateMaxTokens(text);
    string headers = "Content-Type: application/json\r\n"
                     "Accept: application/json\r\n"
                     "Authorization: Bearer " + g_apiKey + "\r\n";

    // 慢请求不叠加：单线程串行，给个宽松上限即可
    DWORD deadline = GetTickCount() + 25000;

    string response;
    DWORD status = 0;
    bool ok = false;
    bool modelRejected = false;

    const char* models[2] = { DEEPSEEK_MODEL, DEEPSEEK_FALLBACK_MODEL };

    for (int mi = 0; mi < 2 && !ok; ++mi) {
        if (mi == 1) {
            LOG_ERROR(string("Model '") + models[0] + "' rejected, retrying with '" + models[1] + "'");
        }

        for (int attempt = 0; attempt < 2; ++attempt) {
            string body = BuildRequestBody(models[mi], sysPrompt, text, maxTokens);

            bool sent = HttpsPostJson(hConnect, L"/chat/completions", body, headers, response, status);

            if (!sent) {
                if (attempt == 0 && GetTickCount() < deadline) { Sleep(400); continue; }
                break;
            }
            if (status == 200) { ok = true; break; }

            // 模型不认 max_tokens：去掉这个字段再来一次
            if (status == 400 && !g_omitMaxTokens && response.find("max_tokens") != string::npos) {
                LOG_ERROR("DeepSeek rejected max_tokens, retrying without it");
                g_omitMaxTokens = true;
                continue;
            }

            // 5xx 可能是抖动，重试一次；429/4xx 立即返回
            if (status >= 500 && attempt == 0 && GetTickCount() < deadline) {
                Sleep(700);
                continue;
            }
            break;
        }

        if (ok) break;

        // 模型名不被接受（改名/下线）→ 换回退模型再试
        if (status == 400 && response.find("model") != string::npos) {
            modelRejected = true;
            continue;
        }
        break;
    }

    if (!ok) {
        if (status == 401 || status == 403) {
            LOG_ERROR("DeepSeek auth failed (HTTP " + to_string(status) +
                      "). Check API key. Skipping requests for 60s.");
            g_apiBlockedUntil = GetTickCount() + 60000;
            return TranslationResult::API_ERROR;
        }
        if (status == 402) {
            LOG_ERROR("DeepSeek insufficient balance (HTTP 402). Top up at platform.deepseek.com.");
            g_apiBlockedUntil = GetTickCount() + 60000;
            return TranslationResult::API_ERROR;
        }
        if (status == 429) {
            LOG_ERROR("DeepSeek rate limited (HTTP 429).");
            return TranslationResult::RATE_LIMITED;
        }
        if (status == 0) {
            LOG_ERROR("DeepSeek request failed: no response (network/DNS/TLS).");
            return TranslationResult::NETWORK_ERROR;
        }
        LOG_ERROR("DeepSeek HTTP " + to_string(status) + ": " + response.substr(0, 300) +
                  (modelRejected ? " (both model names rejected)" : ""));
        return TranslationResult::API_ERROR;
    }

    LOG_DEBUG("Response: " + response.substr(0, 200));

    string content = ParseChatCompletionContent(response);
    string translation = CleanTranslation(content, text, targetLang);

    if (translation.empty()) {
        LOG_ERROR("DeepSeek returned unusable content: " + response.substr(0, 300));
        return TranslationResult::API_ERROR;
    }

    // 记一下用量，便于估算成本（v4-flash：输入 0.14/M，输出 0.28/M 美元）
    long long inTok = ExtractJsonInt(response, "prompt_tokens");
    long long outTok = ExtractJsonInt(response, "completion_tokens");
    if (inTok >= 0 && outTok >= 0) {
        LOG_DEBUG("tokens in/out: " + to_string(inTok) + "/" + to_string(outTok));
    }

    cache[cacheKey] = CacheEntry(translation);
    result = translation;
    LOG_DEBUG("Translated: " + text.substr(0, 30) + " -> " + translation.substr(0, 50));
    return TranslationResult::SUCCESS;
}

// Queue async translation request
bool TranslationClient::TranslateAsync(const string& requestId, const string& text,
                                       const string& sourceLang, const string& targetLang) {
    if (!initialized || !running) {
        return false;
    }

    lock_guard<mutex> lock(requestMutex);
    requestQueue.push(AsyncRequest(requestId, text, sourceLang, targetLang));
    LOG_DEBUG("Async request queued: " + requestId + " (" + sourceLang + " -> " + targetLang + ")");
    return true;
}

// Poll for completed translation
bool TranslationClient::PollResult(string& requestId, string& translation, string& error) {
    lock_guard<mutex> lock(resultMutex);

    if (resultQueue.empty()) {
        return false;
    }

    AsyncResult result = resultQueue.front();
    resultQueue.pop();

    requestId = result.requestId;
    translation = result.translation;
    error = result.error;

    return true;
}

// Get count of pending requests
size_t TranslationClient::GetPendingCount() {
    lock_guard<mutex> lock(requestMutex);
    return requestQueue.size();
}

// Worker thread for async translations
void TranslationClient::WorkerThreadFunc() {
    LOG_INFO("Worker thread started");

    while (running) {
        AsyncRequest request;
        bool hasRequest = false;

        {
            lock_guard<mutex> lock(requestMutex);
            if (!requestQueue.empty()) {
                request = requestQueue.front();
                requestQueue.pop();
                hasRequest = true;
            }
        }

        if (hasRequest) {
            LOG_DEBUG("Processing async request: " + request.requestId);

            string translation;
            string error;

            TranslationResult tr = TranslateText(request.text, translation,
                                                  request.sourceLang, request.targetLang);

            if (tr != TranslationResult::SUCCESS) {
                switch (tr) {
                    case TranslationResult::NETWORK_ERROR:  error = "network error"; break;
                    case TranslationResult::API_ERROR:      error = "API error";     break;
                    case TranslationResult::ENCODING_ERROR: error = "encoding error"; break;
                    case TranslationResult::TIMEOUT_ERROR:  error = "timeout";       break;
                    case TranslationResult::RATE_LIMITED:   error = "rate limited";  break;
                    default:                                error = "unknown error";  break;
                }
                translation = "";
            }

            {
                lock_guard<mutex> lock(resultMutex);
                resultQueue.push(AsyncResult(request.requestId, translation, error));
            }

            LOG_DEBUG("Async request completed: " + request.requestId);
        } else {
            Sleep(50);
        }
    }

    LOG_INFO("Worker thread stopped");
}
