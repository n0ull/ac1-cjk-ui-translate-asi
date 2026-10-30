// dict 离线测试：与包启动相同的字节入口，不创建词典文件或访问游戏。
// 精确容量边界及拒绝行为另由 tools/tests/test_dictionary_behavior.py 的冻结参照覆盖。
#include "ac1/dict/dict.h"

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <string>

using namespace ac1::dict;

static int g_loggedEllipsisHits = -1;

static void test_log_sink(const char* fmt, ...)
{
    char message[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf_s(message, sizeof(message), _TRUNCATE, fmt, ap);
    va_end(ap);
    printf("      |日志| %s\n", message);
    const char* hits = strstr(message, "U+2026 累计命中 ");
    if (hits) sscanf_s(hits, "U+2026 累计命中 %d", &g_loggedEllipsisHits);
}

static int g_pass = 0, g_fail = 0;

static void check(bool ok, const char* name, const char* detail = "")
{
    if (ok) {
        g_pass++;
        printf("  [通过] %s\n", name);
    }
    else {
        g_fail++;
        printf("  [失败] %s  %s\n", name, detail);
    }
}

static bool wstr_eq(const wchar_t* v, int n, const wchar_t* want)
{
    if (!v) return false;
    int wn = (int)wcslen(want);
    if (wn != n) return false;
    return memcmp(v, want, (size_t)n * sizeof(wchar_t)) == 0;
}

static bool lookup_is(const wchar_t* key, const wchar_t* want)
{
    int     n = (int)wcslen(key);
    DictVal r = lookup(key, n);
    if (!r.p) {
        printf("         （键没查到: %ls）\n", key);
        return false;
    }
    if (!wstr_eq(r.p, r.len, want)) {
        printf("         （译文不符: 实际 %d 码元 =", r.len);
        for (int i = 0; i < r.len; i++) printf(" \\u%04X", (unsigned)(unsigned short)r.p[i]);
        printf("，期望 %d 码元 =", (int)wcslen(want));
        for (size_t i = 0; want[i]; i++) printf(" \\u%04X", (unsigned)(unsigned short)want[i]);
        printf("）\n");
        return false;
    }
    return true;
}

static int g_sinkCalls = 0;
static int g_errorCalls = 0;

static void counting_sink(const char*, ...)
{
    ++g_sinkCalls;
}

static void counting_error(const char*, ...)
{
    ++g_errorCalls;
}

static void test_log_injection()
{
    const unsigned char input[] = "Good\tok\nBad\t\xFF\n";
    set_log(&counting_sink, &counting_error);
    check(g_sinkCalls == 0 && g_errorCalls == 0, "刚装上出口还没有日志行");
    check(!init_bytes(input, sizeof(input) - 1), "坏 UTF-8 行使整份输入拒绝");
    check(g_sinkCalls > 0, "注入的日志出口被调用");
    check(g_errorCalls == 1, "坏 UTF-8 行明确发到错误出口");
    set_log(NULL, &counting_error);
    const int before = g_sinkCalls;
    check(!init_bytes(input, sizeof(input) - 1), "关闭普通日志后仍拒绝坏输入");
    check(g_sinkCalls == before && g_errorCalls == 2, "普通日志关闭不影响错误出口");
    set_log(&counting_sink, NULL);
    check(!init_bytes(input, sizeof(input) - 1), "关闭错误日志后仍拒绝坏输入");
    check(g_sinkCalls > before && g_errorCalls == 2, "错误日志关闭不影响普通出口");
    set_log(NULL, NULL);
    const int silent = g_sinkCalls;
    check(!init_bytes(input, sizeof(input) - 1), "无日志出口仍拒绝坏输入");
    check(g_sinkCalls == silent && g_errorCalls == 2, "无出口时静默");
    set_log(&test_log_sink, &test_log_sink);
}

static void test_utf8_rows()
{
    const unsigned char input[] =
        "Start Game\t開始新局\nEsc Val\t中文\n"
        "# comment\r\n\r\n   \r\nPROFILE\t档案\r\n"
        "Esc\t\\x4E2D\\x6587\r\nSame Key\t第二\r\n"
        "Esc 2\tignored\r\n\\x4E2D\\x6587KEY\t中文键\r\n";
    check(init_bytes(input, sizeof(input) - 1), "UTF-8 行加载成功");
    check(lookup_is(L"PROFILE", L"档案"), "UTF-8 新键");
    check(lookup_is(L"Start Game", L"開始新局"), "规范化后的键精确查询");
    check(lookup_is(L"Esc", L"中文"), "译文中的十六进制转义展开");
    check(lookup_is(L"Same Key", L"第二"), "普通唯一键");
    check(lookup_is(L"Esc 2", L"ignored"), "普通含空格键");
    const DictStats& s = stats();
    check(s.keys == 7 && s.sup_new == 7, "七个有效键，包含非拉丁键");
    check(s.sup_lines == 7, "数据行计数正确");
    check(s.sup_comment == 1 && s.sup_empty == 2, "注释与空白行分别计数");
    check(!s.sup_bad && !s.sup_keybad && !s.sup_valbad, "合法行没有拒绝计数");
    check(lookup_is(L"中文KEY", L"中文键"), "转义后的汉字源键参与查询");
    check(s.sup_keyfull == 0 && s.sup_poolfull == 0, "普通行没有容量失败");
}

static void test_bom_and_hash_keys()
{
    const unsigned char input[] =
        "\xEF\xBB\xBF# comment\r\nBomKey\t由BOM后的第一條\r\n"
        "# Press Half to select your Throwing Knives\t按左鍵選投擲飛刀\n"
        "# Ability Lost: Dodge\t失去能力：閃避\n# another comment\nNormalKey\t正常條目\n";
    check(init_bytes(input, sizeof(input) - 1), "BOM、注释与真实井号键一起加载");
    check(lookup_is(L"BomKey", L"由BOM后的第一條"), "BOM 后的首条数据正确");
    check(lookup_is(L"# Press Half to select your Throwing Knives", L"按左鍵選投擲飛刀"),
          "有 TAB 的井号前缀作为数据");
    check(lookup_is(L"# Ability Lost: Dodge", L"失去能力：閃避"), "第二条井号数据也保留");
    check(lookup_is(L"NormalKey", L"正常條目"), "普通键不受影响");
    check(stats().sup_comment == 2 && stats().sup_bad == 0, "无 TAB 的井号行才是注释");
}

static void test_value_escapes()
{
    const unsigned char input[] =
        "EscNewline\tline1\\nline2\nEscTab\ta\\tb\n"
        "EscBackslash\tpath\\\\to\\\\file\nEscHex\tA\\x00A4B\nEscLiteralBsN\tx\\\\ny\n";
    check(init_bytes(input, sizeof(input) - 1), "转义夹具加载");
    check(lookup_is(L"EscNewline", L"line1\nline2"), "反斜杠 n 成为真实换行");
    check(lookup_is(L"EscTab", L"a\tb"), "反斜杠 t 成为真实 TAB");
    check(lookup_is(L"EscBackslash", L"path\\to\\file"), "双反斜杠还原字面反斜杠");
    check(lookup_is(L"EscHex", L"A\x00A4" L"B"), "十六进制转义保留按键图标码元");
    check(lookup_is(L"EscLiteralBsN", L"x\\ny"), "字面反斜杠 n 不成为换行");
}

static void test_key_column_utf8()
{
    // 键的独立字节夹具用于防止逐字节转换产生两个错误码元。
    const unsigned char input[] =
        "Move \xC6\x92 to WALK.\t走\nAlta\xC3\xAF" "r\t高地\n"
        "Esc \\x0192 Key\t转义键\n%1% \\/ %2% Saved\t已保存\n"
        "Plain ASCII Key\t纯ASCII\n";
    check(init_bytes(input, sizeof(input) - 1), "含 UTF-8 多字节键的夹具加载");
    check(lookup_is(L"Move \x0192 to WALK.", L"走"), "两字节 UTF-8 解为一个 U+0192");
    check(lookup_is(L"Alta\x00EF" L"r", L"高地"), "重音字符解为一个 U+00EF");
    check(lookup_is(L"Esc \x0192 Key", L"转义键"), "键列同样展开十六进制转义");
    check(lookup_is(L"%1% \\/ %2% Saved", L"已保存"), "未识别转义保留反斜杠");
    check(lookup_is(L"Plain ASCII Key", L"纯ASCII"), "ASCII 键行为不变");
    const unsigned char invalid[] = "Good\tok\nBad \xFF Key\t不该出现\n";
    check(!init_bytes(invalid, sizeof(invalid) - 1) && stats().sup_keybad == 1 && !stats().keys,
          "非法 UTF-8 键使整份输入拒绝并清空");
    const wchar_t bad[] = L"Bad \xFFFD Key";
    check(!lookup(bad, (int)wcslen(bad)).p, "坏键不能以替换字符混入");
}

static void test_translation_authority()
{
    const unsigned char input[] = "Start Game\tゲーム開始\nLoad Map\t载入…\nExit\t終了\n";
    check(init_bytes(input, sizeof(input) - 1), "同一入口接受非中文译文");
    check(lookup_is(L"Start Game", L"ゲーム開始"), "只返回本份词典的日语译文");
    check(lookup_is(L"Load Map", L"载入..."), "真实 UTF-8 省略号展开为三个点");
    check(stats().sup_ellipsis == 1, "本次初始化展开一个省略号");
    const wchar_t missing[] = L"Some String Nobody Translated";
    check(!lookup(missing, (int)wcslen(missing)).p, "词典外条目未命中");
}

static void test_length_limits()
{
    const std::string key(4096, 'K');
    const std::string value(8192, 'F');
    std::string input = key + "\t" + value + "\nLongVal\t" + std::string(5000, 'V') + "\n";
    check(init_bytes((const unsigned char*)input.data(), (int)input.size()),
          "4096 键、8192 值及 5000 值均可加载");
    const std::wstring wide_key(4096, L'K');
    DictVal found = lookup(wide_key.data(), 4096);
    check(found.p && found.len == 8192, "边界译文长度完整");
    bool all_f = found.p != NULL;
    for (int i = 0; all_f && i < found.len; ++i) all_f = found.p[i] == L'F';
    check(all_f, "边界译文逐码元正确");
    found = lookup(L"LongVal", 7);
    check(found.p && found.len == 5000, "长译文不受 4096 键上限影响");

    input = "Good\tok\n" + std::string(5000, 'K') + "\tbad\n";
    check(!init_bytes((const unsigned char*)input.data(), (int)input.size()) && !ready(),
          "超长键拒绝整份输入");
    check(stats().keys == 0 && !lookup(L"Good", 4).p, "拒绝不残留先前有效行");
    input = "Good\tok\nTooLong\t" + std::string(9000, 'V') + "\n";
    check(!init_bytes((const unsigned char*)input.data(), (int)input.size()) && !stats().keys,
          "超长译文拒绝整份输入");
}

static void test_pool_rollback()
{
    std::string input;
    for (int i = 0; i < 16; ++i) {
        char key[32];
        _snprintf(key, sizeof(key), "K%d\t", i);
        input += key;
        input += std::string(8192, 'V') + "\n";
    }
    check(init_bytes((const unsigned char*)input.data(), (int)input.size()), "值池恰好装满可加载");
    check(stats().valpool_used == 131072 && stats().keys == 16, "恰好装满的用量正确");
    input += "Extra\tv\n";
    check(!init_bytes((const unsigned char*)input.data(), (int)input.size()), "值池溢出整份拒绝");
    check(!ready() && !stats().keys && !lookup(L"K0", 2).p, "池满后没有部分发布");

    input.clear();
    for (int i = 0; i < 20; ++i) input += "Key\t" + std::string(8192, 'V') + "\n";
    check(!init_bytes((const unsigned char*)input.data(), (int)input.size()),
          "同值重复键也拒绝");
    check(!stats().valpool_used && stats().sup_duplicate == 19, "重复键不发布值池且保留拒因");
    const unsigned char empty_duplicate[] = "Key\told\n Key \told\nKey\t\\n\n";
    check(!init_bytes(empty_duplicate, sizeof(empty_duplicate) - 1) && !lookup(L"Key", 3).p,
          "规范化重复键不保留旧值");
    check(stats().sup_duplicate == 2, "同值和空白目标的重复键均明确拒绝");
}

static void test_case_sensitivity()
{
    const unsigned char input[] = "Start Game\t开始游戏\n";
    check(init_bytes(input, sizeof(input) - 1), "精确匹配夹具加载");
    check(lookup(L"Start Game", 10).p != NULL, "大小写完全相同命中");
    check(!lookup(L"start game", 10).p && !lookup(L"START GAME", 10).p, "大小写不同均不命中");
    check(!lookup(L"", 0).p && !lookup(L"Absent", 6).p, "空键与缺失键不命中");
}

static void test_canon()
{
    printf("\n[2] dict_canon 规范化\n");
    wchar_t out[64];

    int n;
    n = canon(L"  Start   Game  ", -1, out, 64);
    check(n == 10 && wstr_eq(out, n, L"Start Game"), "连续空白折叠 + 掐头去尾");

    n = canon(L"\t\r\n A \n B \t", -1, out, 64);
    check(n == 3 && wstr_eq(out, n, L"A B"), "TAB/CR/LF 全算空白");

    n = canon(L"   ", -1, out, 64);
    check(n == 0, "全空白 ⇒ 长度 0");

    n = canon(L"abc\0def", -1, out, 64);
    check(n == 3 && wstr_eq(out, n, L"abc"), "遇 NUL 视为串尾");

    n = canon(L"abcdefghij", -1, out, 4);
    check(n == -1, "超出 cap ⇒ -1");

    n = canon(L"x", 0, out, 64);
    check(n == 0, "长度 0 ⇒ 空");

    n = canon(L"MiXeD", -1, out, 64);
    check(n == 5 && wstr_eq(out, n, L"MiXeD"), "大小写原样保留（不归一化大小写）");
}

static void test_lookup_bounds()
{
    printf("\n[5b] lookup 长度上界：n > DICT_KEY_MAX ⇒ 未命中\n");
    // 数组尾**正好贴**守卫页：任何越读（哪怕 2 字节）都会立刻 AV，而不是读进垫片里照样返回 NULL。
    const size_t   need = (size_t)(DICT_KEY_MAX + 1) * sizeof(wchar_t);
    const size_t   dataBytes = ((need + 4095) / 4096) * 4096;
    unsigned char* mem =
        (unsigned char*)VirtualAlloc(NULL, dataBytes + 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        check(false, "分配带守卫页的测试缓冲");
        return;
    }
    DWORD old = 0;
    VirtualProtect(mem + dataBytes, 4096, PAGE_NOACCESS, &old); // 守卫页：越界即 AV
    wchar_t* k = (wchar_t*)(mem + dataBytes - need);
    for (int i = 0; i < DICT_KEY_MAX + 1; i++) k[i] = L'Z';

    check(lookup(k, DICT_KEY_MAX + 1).p == NULL, "n = 4097 ⇒ 未命中");
    check(lookup(k, 70000).p == NULL, "★ n = 70000 ⇒ 未命中（不进 u16 窄化比较）");
    check(lookup(k, -1).p == NULL, "n < 0 ⇒ 未命中");
    check(lookup(NULL, 5).p == NULL, "key == NULL ⇒ 未命中");
    VirtualFree(mem, 0, MEM_RELEASE);
}

static void test_ellipsis()
{
    printf("\n[6.5] 译文省略号展开与统计生命周期\n");
    const int before = g_loggedEllipsisHits;
    const unsigned char input[] = "Plain\t开始游戏\nSingle\t载入…\nBetween\t前…中…后\nAdjacent\t………\n";
    check(init_bytes(input, sizeof(input) - 1), "省略号夹具经正式字节入口加载");
    check(lookup_is(L"Plain", L"开始游戏"), "没有省略号时内容不变");
    check(lookup_is(L"Single", L"载入..."), "单个省略号展开且保留前文");
    check(lookup_is(L"Between", L"前...中...后"), "多个省略号展开且保留前后内容");
    check(lookup_is(L"Adjacent", L"........."), "连续三个省略号不相互覆盖");
    check(stats().sup_ellipsis == 6, "本次初始化统计六个展开字符");
    check(g_loggedEllipsisHits == before + 6, "累计日志与本次统计分别记账");

    const unsigned char plain[] = "Key\tok\n";
    check(init_bytes(plain, sizeof(plain) - 1) && stats().sup_ellipsis == 0,
          "再次初始化不带入前轮省略号统计");
    check(g_loggedEllipsisHits == before + 6, "再次初始化不清空进程累计日志");

    const std::string exact = "Limit\t" + std::string(8189, 'a') + "…\n";
    check(init_bytes((const unsigned char*)exact.data(), (int)exact.size()),
          "展开后恰好8192码元可接受");
    const DictVal limit = lookup(L"Limit", 5);
    check(limit.p && limit.len == 8192 && limit.p[0] == L'a' &&
              wmemcmp(limit.p + 8189, L"...", 3) == 0,
          "容量边界保留完整前文与三个点");

    const std::string over = "Good\tok\nLimit\t" + std::string(8190, 'a') + "…\n";
    check(!init_bytes((const unsigned char*)over.data(), (int)over.size()) && !ready(),
          "展开超过容量则整份拒绝并清空旧词典");
    check(stats().sup_ellipsis == 0 && stats().sup_fallback_full == 1,
          "失败展开只记容量拒绝，不记成功命中");
    check(g_loggedEllipsisHits == before + 7, "容量失败不增加进程累计命中");
}

static void test_runtime_bytes()
{
    unsigned char input[] = "\xEF\xBB\xBF# comment\n Key \t\\x4E2D\\n\\x6587\n"
                            "#Key\tvalue\nEllipsis\t\\x2026\n";
    check(init_bytes(input, (int)sizeof(input) - 1) && ready() && stats().keys == 3,
          "runtime bytes initialize an independent ready dictionary");
    check(lookup_is(L"Key", L"中\n文") && lookup_is(L"#Key", L"value") && lookup_is(L"Ellipsis", L"..."),
          "memory input preserves escapes, hash keys and fallback");
    memset(input, 0, sizeof(input));
    check(lookup_is(L"Key", L"中\n文"), "dictionary owns decoded values after input release");
    const unsigned char next[] = "New\tnew\n";
    check(init_bytes(next, (int)sizeof(next) - 1) && !lookup(L"Key", 3).p && lookup_is(L"New", L"new"),
          "runtime initialization resets the previous dictionary");
    const unsigned char invalid[] = "Good\taccepted\nBad\t\\xD800\n";
    check(!init_bytes(invalid, (int)sizeof(invalid) - 1) && !ready() && !stats().keys,
          "unsupported native code units clear partially ingested dictionary entries");
    check(stats().sup_lines == 2 && stats().sup_new == 1,
          "rejected input retains parsing counters after clearing entries");
    check(strstr(status(), "unsupported_value_unit") != NULL,
          "rejected input retains the failure reason");
    static unsigned char too_long[8200];
    memcpy(too_long, "Key\t", 4);
    memset(too_long + 4, 'a', 8193);
    check(!init_bytes(too_long, 8197) && !ready() && !stats().keys,
          "runtime capacity failure leaves no ready or partial dictionary");
}

static void test_rejected_input()
{
    const unsigned char seed[] = "Prior\told\n";
    check(init_bytes(seed, sizeof(seed) - 1) && status()[0], "有效输入发布状态摘要");
    check(!init_bytes(NULL, 0) && !ready() && !stats().keys, "空输入清除旧词典");
    check(strstr(status(), "reason=empty_input") && !lookup(L"Prior", 5).p,
          "失败发布本次原因且不留下旧键");
    check(!init_bytes(NULL, 10) && !ready(), "空指针与非零长度也拒绝");
    check(!init_bytes(seed, -1) && !ready(), "负长度拒绝");
    // 只提供短缓冲，证明超上限判定发生在读取输入前。
    check(!init_bytes(seed, 16 * 1024 * 1024 + 1) && !ready(), "输入超过 16MiB 不解引用");
}

static void test_diagnostics()
{
    const unsigned char partial[] = "# comment\n\nnot a row\nGood\tok\nBad\t\xFF\n";
    check(!init_bytes(partial, sizeof(partial) - 1) && !lookup(L"Good", 4).p,
          "malformed rows reject the whole input");
    check(stats().sup_lines == 3 && stats().rejected_lines == 2 && stats().first_rejected_line == 3 &&
          stats().failure_line == 3 && !strcmp(stats().reason, "malformed_row"),
          "diagnostics retain the first failure and physical line numbers");
    check(strstr(status(), "result=rejected") != NULL, "no partial acceptance");
    const unsigned char fatal[] = "# comment\n\nnot a row\nGood\tok\nBad\t\\xD800\n";
    check(!init_bytes(fatal, sizeof(fatal) - 1) && !lookup(L"Good", 4).p,
          "fatal row clears entries accepted earlier in the same attempt");
    check(stats().rejected_lines == 2 && stats().first_rejected_line == 3 &&
          stats().failure_line == 3 && !strcmp(stats().reason, "malformed_row"),
          "later failures do not hide the first invalid row");
    const unsigned char clean[] = "Good\tok\n";
    check(init_bytes(clean, sizeof(clean) - 1) && !stats().rejected_lines &&
          !stats().first_rejected_line && !stats().failure_line && !strcmp(stats().reason, "none"),
          "next successful initialization clears previous diagnostic evidence");
    check(strstr(status(), "result=clean") != NULL, "clean acceptance is explicit");
}

static void test_stable_mappings()
{
    const unsigned char fixed[] = "A\tB\nB\tB\n日本語\t訳文\n";
    check(init_bytes(fixed, sizeof(fixed) - 1) && lookup_is(L"A", L"B") && lookup_is(L"B", L"B"),
          "one-hop mapping and a fixed point are valid");
    check(lookup_is(L"日本語", L"訳文"), "non-Latin source keys are queried normally");
    const unsigned char chain[] = "# comment\nA\tB\nB\tC\n";
    check(!init_bytes(chain, sizeof(chain) - 1) && !ready() && !lookup(L"A", 1).p,
          "a translation chain rejects and clears the entire dictionary");
    check(!strcmp(stats().reason, "non_idempotent_value") && stats().failure_line == 2,
          "stability failure points to the source of the unstable value");
    const unsigned char cycle[] = "A\tB\nB\tA\n";
    check(!init_bytes(cycle, sizeof(cycle) - 1), "translation cycles reject");
    const unsigned char whitespace[] = "A\tB\\tC\nB C\tB C\n";
    check(!init_bytes(whitespace, sizeof(whitespace) - 1),
          "canonical equality alone cannot replace exact target equality");
    const unsigned char stable_whitespace[] = "A\tB\\tC\nB C\tB\\tC\n";
    check(init_bytes(stable_whitespace, sizeof(stable_whitespace) - 1),
          "canonical lookup is safe when its exact output is unchanged");
    const unsigned char fallback[] = "A\t…\n...\tbad\n";
    check(!init_bytes(fallback, sizeof(fallback) - 1), "stability checks the expanded ellipsis value");
    const unsigned char duplicates[] = "A  B\tone\nA B\ttwo\n";
    check(!init_bytes(duplicates, sizeof(duplicates) - 1) && stats().sup_duplicate == 1 &&
          !strcmp(stats().reason, "duplicate_key"), "canonical duplicate keys reject");
    const std::string long_target = "A\t" + std::string(4097, 'V') + "\n";
    check(init_bytes((const unsigned char*)long_target.data(), (int)long_target.size()),
          "an unqueryably long target is stable");
    const std::string collapsed = "A\tB" + std::string(4096, ' ') + "C\nB C\tbad\n";
    check(!init_bytes((const unsigned char*)collapsed.data(), (int)collapsed.size()),
          "a long value that collapses into a short key is still checked");
    check(init_bytes(fixed, sizeof(fixed) - 1) && ready() && !stats().rejected_lines,
          "a valid initialization recovers after rejected mappings");
}

int main()
{
    SetConsoleOutputCP(65001);
    set_log(&test_log_sink, &test_log_sink);
    printf("ac1-chinese-translate 词典层单测（包内 UTF-8 字节入口）\n");
    test_log_injection();
    test_canon();
    test_utf8_rows();
    test_bom_and_hash_keys();
    test_value_escapes();
    test_key_column_utf8();
    test_translation_authority();
    test_length_limits();
    test_pool_rollback();
    test_case_sensitivity();
    test_lookup_bounds();
    test_ellipsis();
    test_runtime_bytes();
    test_rejected_input();
    test_diagnostics();
    test_stable_mappings();
    printf("\n==== 通过 %d / 失败 %d ====\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
