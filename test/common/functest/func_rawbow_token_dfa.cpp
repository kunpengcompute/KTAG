/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "rawbow_token_dfa.h"

namespace {

using rawbow_dfa::TokenState;

// 便捷比较: std::vector<std::string> 与初始化列表
void ExpectTokensAre(const std::vector<std::string> &actual,
                     const std::vector<std::string> &expected)
{
    EXPECT_EQ(expected, actual);
}

std::string Repeat(char c, std::size_t n)
{
    return std::string(n, c);
}

}  // namespace

// ---------------------------------------------------------------------------
// 静态数据查询接口
// ---------------------------------------------------------------------------
TEST(KDADK_RAWBOW_DFA, StopwordCountMatchesConstant)
{
    EXPECT_EQ(rawbow_dfa::kStopwordCount, rawbow_dfa::stopword_count());
    EXPECT_EQ(69u, rawbow_dfa::stopword_count());
}

TEST(KDADK_RAWBOW_DFA, StopwordWordsAreValid)
{
    // 每个停用词: 非空、纯小写 ASCII、无重复
    const auto &words = rawbow_dfa::stopword_words();
    for (std::string_view word : words) {
        EXPECT_FALSE(word.empty());
        for (char c : word) {
            const auto uc = static_cast<unsigned char>(c);
            EXPECT_LT(uc, rawbow_dfa::kAsciiAlphabetSize);
            EXPECT_FALSE(uc >= 'A' && uc <= 'Z') << "uppercase in stopword: " << word;
        }
    }
    for (std::size_t i = 0; i < words.size(); ++i) {
        for (std::size_t j = i + 1; j < words.size(); ++j) {
            EXPECT_NE(words[i], words[j]);
        }
    }
}

TEST(KDADK_RAWBOW_DFA, StopwordWordsContainKnownEntries)
{
    const auto &words = rawbow_dfa::stopword_words();
    bool hasHttp = false;
    bool hasHttps = false;
    bool hasCom = false;
    bool hasCn = false;
    bool hasWww = false;
    for (std::string_view word : words) {
        hasHttp = hasHttp || word == "http";
        hasHttps = hasHttps || word == "https";
        hasCom = hasCom || word == "com";
        hasCn = hasCn || word == "cn";
        hasWww = hasWww || word == "www";
    }
    EXPECT_TRUE(hasHttp);
    EXPECT_TRUE(hasHttps);
    EXPECT_TRUE(hasCom);
    EXPECT_TRUE(hasCn);
    EXPECT_TRUE(hasWww);
}

TEST(KDADK_RAWBOW_DFA, StopwordTrieNodeCountMatchesConstant)
{
    EXPECT_EQ(rawbow_dfa::kStopwordTrieNodeCount, rawbow_dfa::stopword_trie_node_count());
    EXPECT_EQ(154u, rawbow_dfa::stopword_trie_node_count());
}

TEST(KDADK_RAWBOW_DFA, StopwordTrieRootIsNotTerminal)
{
    // 空串不是停用词, 根节点不应为终态
    EXPECT_FALSE(rawbow_dfa::stopword_trie()[0].terminal);
}

TEST(KDADK_RAWBOW_DFA, StopwordTrieRecognizesEveryStopword)
{
    const auto &trie = rawbow_dfa::stopword_trie();
    for (std::string_view word : rawbow_dfa::stopword_words()) {
        int node = 0;
        for (char c : word) {
            const auto uc = static_cast<unsigned char>(c);
            ASSERT_LT(uc, rawbow_dfa::kAsciiAlphabetSize);
            node = trie[static_cast<std::size_t>(node)].next[uc];
            ASSERT_GE(node, 0) << "trie path broken at: " << word;
        }
        EXPECT_TRUE(trie[static_cast<std::size_t>(node)].terminal) << "not terminal: " << word;
    }
}

// ---------------------------------------------------------------------------
// 字符分类辅助函数
// ---------------------------------------------------------------------------
TEST(KDADK_RAWBOW_DFA, IsDelimiter)
{
    // 7 种分隔符
    for (unsigned char c : {'/', '?', ',', '=', '&', '.', ':'}) {
        EXPECT_TRUE(rawbow_dfa::is_delimiter(c)) << static_cast<char>(c);
    }
    // 非分隔符
    const unsigned char nonDelims[] = {'a', 'Z', '0', '_', '-', '@', ' ',
                                       static_cast<unsigned char>('\x80'),
                                       static_cast<unsigned char>(0xFF)};
    for (unsigned char c : nonDelims) {
        EXPECT_FALSE(rawbow_dfa::is_delimiter(c)) << static_cast<int>(c);
    }
}

TEST(KDADK_RAWBOW_DFA, IsAsciiDigit)
{
    EXPECT_TRUE(rawbow_dfa::is_ascii_digit('0'));
    EXPECT_TRUE(rawbow_dfa::is_ascii_digit('9'));
    EXPECT_FALSE(rawbow_dfa::is_ascii_digit('0' - 1));
    EXPECT_FALSE(rawbow_dfa::is_ascii_digit('9' + 1));
    EXPECT_FALSE(rawbow_dfa::is_ascii_digit('a'));
    EXPECT_FALSE(rawbow_dfa::is_ascii_digit('A'));
    EXPECT_FALSE(rawbow_dfa::is_ascii_digit('/'));
    EXPECT_FALSE(rawbow_dfa::is_ascii_digit('\0'));
}

TEST(KDADK_RAWBOW_DFA, IsAsciiAlpha)
{
    EXPECT_TRUE(rawbow_dfa::is_ascii_alpha('a'));
    EXPECT_TRUE(rawbow_dfa::is_ascii_alpha('z'));
    EXPECT_TRUE(rawbow_dfa::is_ascii_alpha('A'));
    EXPECT_TRUE(rawbow_dfa::is_ascii_alpha('Z'));
    EXPECT_FALSE(rawbow_dfa::is_ascii_alpha('a' - 1));
    EXPECT_FALSE(rawbow_dfa::is_ascii_alpha('z' + 1));
    EXPECT_FALSE(rawbow_dfa::is_ascii_alpha('A' - 1));
    EXPECT_FALSE(rawbow_dfa::is_ascii_alpha('Z' + 1));
    EXPECT_FALSE(rawbow_dfa::is_ascii_alpha('0'));
    EXPECT_FALSE(rawbow_dfa::is_ascii_alpha('_'));
}

TEST(KDADK_RAWBOW_DFA, ToAsciiLower)
{
    EXPECT_EQ('a', rawbow_dfa::to_ascii_lower('A'));
    EXPECT_EQ('z', rawbow_dfa::to_ascii_lower('Z'));
    EXPECT_EQ('m', rawbow_dfa::to_ascii_lower('M'));
    // 小写、数字、符号原样返回
    EXPECT_EQ('a', rawbow_dfa::to_ascii_lower('a'));
    EXPECT_EQ('0', rawbow_dfa::to_ascii_lower('0'));
    EXPECT_EQ('/', rawbow_dfa::to_ascii_lower('/'));
    EXPECT_EQ('@', rawbow_dfa::to_ascii_lower('@'));
    EXPECT_EQ('A' - 1, rawbow_dfa::to_ascii_lower('A' - 1));  // '@'
    EXPECT_EQ('Z' + 1, rawbow_dfa::to_ascii_lower('Z' + 1));  // '['
}

// ---------------------------------------------------------------------------
// TokenState 状态机
// ---------------------------------------------------------------------------
TEST(KDADK_RAWBOW_DFA, UpdateTokenStateTracksStartAndLen)
{
    TokenState state;
    rawbow_dfa::update_token_state(state, 5, 'a');
    EXPECT_EQ(5u, state.start);
    EXPECT_EQ(1u, state.len);
    rawbow_dfa::update_token_state(state, 6, 'b');
    EXPECT_EQ(5u, state.start);  // start 不变
    EXPECT_EQ(2u, state.len);
}

TEST(KDADK_RAWBOW_DFA, UpdateTokenStateOverflowFlag)
{
    TokenState state;
    for (std::size_t i = 0; i < rawbow_dfa::kMaxTokenLength; ++i) {
        rawbow_dfa::update_token_state(state, i, 'a');
    }
    EXPECT_FALSE(state.overflow_len);  // 20 字符不溢出
    rawbow_dfa::update_token_state(state, rawbow_dfa::kMaxTokenLength, 'a');
    EXPECT_TRUE(state.overflow_len);   // 21 字符溢出
    // 溢出标志不可恢复
    rawbow_dfa::update_token_state(state, rawbow_dfa::kMaxTokenLength + 1, 'a');
    EXPECT_TRUE(state.overflow_len);
}

TEST(KDADK_RAWBOW_DFA, UpdateTokenStateAllDigits)
{
    TokenState state;
    rawbow_dfa::update_token_state(state, 0, '1');
    rawbow_dfa::update_token_state(state, 1, '2');
    EXPECT_TRUE(state.all_digits);
    rawbow_dfa::update_token_state(state, 2, 'a');
    EXPECT_FALSE(state.all_digits);
    // all_digits 一旦为 false 不可恢复
    rawbow_dfa::update_token_state(state, 3, '3');
    EXPECT_FALSE(state.all_digits);
}

TEST(KDADK_RAWBOW_DFA, UpdateTokenStateAlphaCountSaturates)
{
    TokenState state;
    EXPECT_EQ(0, state.alpha_count_state);
    rawbow_dfa::update_token_state(state, 0, 'a');
    EXPECT_EQ(1, state.alpha_count_state);
    rawbow_dfa::update_token_state(state, 1, 'b');
    EXPECT_EQ(2, state.alpha_count_state);
    rawbow_dfa::update_token_state(state, 2, 'c');
    EXPECT_EQ(2, state.alpha_count_state);  // 饱和在 2
}

TEST(KDADK_RAWBOW_DFA, UpdateTokenStateSingleAlphaCandidate)
{
    // 纯数字/下划线/连字符: 保持单字母候选
    {
        TokenState state;
        rawbow_dfa::update_token_state(state, 0, '1');
        rawbow_dfa::update_token_state(state, 1, '_');
        rawbow_dfa::update_token_state(state, 2, '-');
        EXPECT_TRUE(state.single_alpha_candidate);
        EXPECT_EQ(0, state.alpha_count_state);
    }
    // 一个字母: 仍是单字母候选
    {
        TokenState state;
        rawbow_dfa::update_token_state(state, 0, 'a');
        EXPECT_TRUE(state.single_alpha_candidate);
        EXPECT_EQ(1, state.alpha_count_state);
    }
    // 两个字母: 不再是单字母候选
    {
        TokenState state;
        rawbow_dfa::update_token_state(state, 0, 'a');
        rawbow_dfa::update_token_state(state, 1, 'b');
        EXPECT_FALSE(state.single_alpha_candidate);
    }
    // 特殊字符(@ 等): 不再是单字母候选
    {
        TokenState state;
        rawbow_dfa::update_token_state(state, 0, '@');
        EXPECT_FALSE(state.single_alpha_candidate);
    }
}

TEST(KDADK_RAWBOW_DFA, UpdateStopwordStateCaseInsensitive)
{
    TokenState state;
    for (unsigned char c : {'H', 'T', 'T', 'P'}) {
        rawbow_dfa::update_stopword_state(state, c);
    }
    EXPECT_FALSE(state.stopword_dead);
    EXPECT_TRUE(rawbow_dfa::is_stopword_terminal(state));  // "HTTP" 匹配 http
}

TEST(KDADK_RAWBOW_DFA, UpdateStopwordStateDeadOnUnknownTransition)
{
    TokenState state;
    for (char c : std::string_view("www")) {
        rawbow_dfa::update_stopword_state(state, static_cast<unsigned char>(c));
    }
    EXPECT_FALSE(state.stopword_dead);
    // "wwwx" 不构成停用词, x 转移失败进入 dead 态
    rawbow_dfa::update_stopword_state(state, 'x');
    EXPECT_TRUE(state.stopword_dead);
    EXPECT_FALSE(rawbow_dfa::is_stopword_terminal(state));
}

TEST(KDADK_RAWBOW_DFA, UpdateStopwordStateDeadIsSticky)
{
    TokenState state;
    rawbow_dfa::update_stopword_state(state, '\x80');  // 非 ASCII -> dead
    EXPECT_TRUE(state.stopword_dead);
    const int node_before = state.stopword_node;
    rawbow_dfa::update_stopword_state(state, 'w');
    EXPECT_TRUE(state.stopword_dead);
    EXPECT_EQ(node_before, state.stopword_node);  // dead 后状态不再变化
}

TEST(KDADK_RAWBOW_DFA, UpdateStopwordStateNonAsciiIsDead)
{
    TokenState state;
    rawbow_dfa::update_stopword_state(state, static_cast<unsigned char>(0xFF));
    EXPECT_TRUE(state.stopword_dead);
}

TEST(KDADK_RAWBOW_DFA, IsStopwordTerminalFalseForNonStopword)
{
    // "example" 第二个字符即走出停用词 trie 前缀空间, 进入 dead 态
    // dead 表示该 token 不可能是停用词, 因此绝非终态
    TokenState state;
    for (char c : std::string_view("example")) {
        rawbow_dfa::update_stopword_state(state, static_cast<unsigned char>(c));
    }
    EXPECT_TRUE(state.stopword_dead);
    EXPECT_FALSE(rawbow_dfa::is_stopword_terminal(state));
}

TEST(KDADK_RAWBOW_DFA, ShouldAcceptTokenRejectsShortTokens)
{
    TokenState state;
    rawbow_dfa::update_token_state(state, 0, 'a');
    EXPECT_EQ(1u, state.len);
    EXPECT_FALSE(rawbow_dfa::should_accept_token(state));  // len < 2
}

TEST(KDADK_RAWBOW_DFA, ShouldAcceptTokenRejectsOverflowTokens)
{
    TokenState state;
    for (std::size_t i = 0; i <= rawbow_dfa::kMaxTokenLength; ++i) {
        rawbow_dfa::update_token_state(state, i, 'a');
    }
    EXPECT_TRUE(state.overflow_len);
    EXPECT_FALSE(rawbow_dfa::should_accept_token(state));
}

TEST(KDADK_RAWBOW_DFA, ShouldAcceptTokenRejectsAllDigitTokens)
{
    TokenState state;
    for (std::size_t i = 0; i < 4; ++i) {
        rawbow_dfa::update_token_state(state, i, '7');
    }
    EXPECT_TRUE(state.all_digits);
    EXPECT_FALSE(rawbow_dfa::should_accept_token(state));
}

TEST(KDADK_RAWBOW_DFA, ShouldAcceptTokenRejectsSingleAlphaTokens)
{
    // "v1": 一个字母 + 数字, 仍是单字母候选
    {
        TokenState state;
        rawbow_dfa::update_token_state(state, 0, 'v');
        rawbow_dfa::update_token_state(state, 1, '1');
        EXPECT_FALSE(rawbow_dfa::should_accept_token(state));
    }
    // "a_123": 一个字母 + 下划线/数字
    {
        TokenState state;
        for (std::size_t i = 0; i < 5; ++i) {
            rawbow_dfa::update_token_state(state, i, "a_123"[i]);
        }
        EXPECT_FALSE(rawbow_dfa::should_accept_token(state));
    }
}

TEST(KDADK_RAWBOW_DFA, ShouldAcceptTokenAcceptsMultiAlphaTokens)
{
    // "ab": 两个字母
    {
        TokenState state;
        rawbow_dfa::update_token_state(state, 0, 'a');
        rawbow_dfa::update_token_state(state, 1, 'b');
        EXPECT_TRUE(rawbow_dfa::should_accept_token(state));
    }
    // "ab1": 两个字母 + 数字
    {
        TokenState state;
        rawbow_dfa::update_token_state(state, 0, 'a');
        rawbow_dfa::update_token_state(state, 1, 'b');
        rawbow_dfa::update_token_state(state, 2, '1');
        EXPECT_TRUE(rawbow_dfa::should_accept_token(state));
    }
}

TEST(KDADK_RAWBOW_DFA, ShouldAcceptTokenRejectsStopwordsAnyCase)
{
    // 小写停用词
    {
        TokenState state;
        for (char c : std::string_view("www")) {
            rawbow_dfa::update_token_state(state, 0, static_cast<unsigned char>(c));
        }
        EXPECT_FALSE(rawbow_dfa::should_accept_token(state));
    }
    // 大写停用词
    {
        TokenState state;
        for (char c : std::string_view("WWW")) {
            rawbow_dfa::update_token_state(state, 0, static_cast<unsigned char>(c));
        }
        EXPECT_FALSE(rawbow_dfa::should_accept_token(state));
    }
}

TEST(KDADK_RAWBOW_DFA, ShouldAcceptTokenAcceptsStopwordPrefixPlusSuffix)
{
    // "wwwx": 走出停用词路径(dead), 应接受
    TokenState state;
    for (char c : "wwwx") {
        rawbow_dfa::update_token_state(state, 0, static_cast<unsigned char>(c));
    }
    EXPECT_TRUE(state.stopword_dead);
    EXPECT_TRUE(rawbow_dfa::should_accept_token(state));
}

TEST(KDADK_RAWBOW_DFA, TokenStateResetRestoresInitialValues)
{
    TokenState state;
    for (std::size_t i = 0; i < 30; ++i) {
        rawbow_dfa::update_token_state(state, i, i % 2 == 0 ? 'a' : '\x80');
    }
    state.reset();
    EXPECT_EQ(0u, state.start);
    EXPECT_EQ(0u, state.len);
    EXPECT_FALSE(state.overflow_len);
    EXPECT_TRUE(state.all_digits);
    EXPECT_EQ(0, state.alpha_count_state);
    EXPECT_TRUE(state.single_alpha_candidate);
    EXPECT_EQ(0, state.stopword_node);
    EXPECT_FALSE(state.stopword_dead);
}

// ---------------------------------------------------------------------------
// finalize_token
// ---------------------------------------------------------------------------
TEST(KDADK_RAWBOW_DFA, FinalizeTokenAppendsAcceptedSubstring)
{
    TokenState state;
    const std::string_view input = "xx/hello/yy";
    for (std::size_t i = 3; i < 8; ++i) {
        rawbow_dfa::update_token_state(state, i, static_cast<unsigned char>(input[i]));
    }
    std::vector<std::string> out;
    rawbow_dfa::finalize_token(input, state, out, rawbow_dfa::kUnlimitedTokens);
    ExpectTokensAre(out, {"hello"});
}

TEST(KDADK_RAWBOW_DFA, FinalizeTokenKeepsOriginalCase)
{
    // DFA 停用词匹配大小写不敏感, 但输出保留原始大小写
    TokenState state;
    const std::string_view input = "Example";
    for (std::size_t i = 0; i < input.size(); ++i) {
        rawbow_dfa::update_token_state(state, i, static_cast<unsigned char>(input[i]));
    }
    std::vector<std::string> out;
    rawbow_dfa::finalize_token(input, state, out, rawbow_dfa::kUnlimitedTokens);
    ExpectTokensAre(out, {"Example"});
}

TEST(KDADK_RAWBOW_DFA, FinalizeTokenSkipsZeroLength)
{
    TokenState state;  // len == 0
    std::vector<std::string> out;
    rawbow_dfa::finalize_token("whatever", state, out, rawbow_dfa::kUnlimitedTokens);
    EXPECT_TRUE(out.empty());
}

TEST(KDADK_RAWBOW_DFA, FinalizeTokenSkipsRejected)
{
    // 全数字 token 被拒绝
    TokenState state;
    const std::string_view input = "12345";
    for (std::size_t i = 0; i < input.size(); ++i) {
        rawbow_dfa::update_token_state(state, i, static_cast<unsigned char>(input[i]));
    }
    std::vector<std::string> out;
    rawbow_dfa::finalize_token(input, state, out, rawbow_dfa::kUnlimitedTokens);
    EXPECT_TRUE(out.empty());
}

TEST(KDADK_RAWBOW_DFA, FinalizeTokenRespectsMaxTokens)
{
    TokenState state;
    for (std::size_t i = 0; i < 4; ++i) {
        rawbow_dfa::update_token_state(state, i, "abcd"[i]);
    }
    std::vector<std::string> out{"pre"};
    // out 已有 1 个, max_tokens = 1: 不再追加
    rawbow_dfa::finalize_token("abcd", state, out, 1);
    ExpectTokensAre(out, {"pre"});
    // max_tokens = 2: 可追加
    rawbow_dfa::finalize_token("abcd", state, out, 2);
    ExpectTokensAre(out, {"pre", "abcd"});
}

// ---------------------------------------------------------------------------
// collect_filtered_tokens 端到端
// ---------------------------------------------------------------------------
TEST(KDADK_RAWBOW_DFA, CollectEmptyInput)
{
    EXPECT_TRUE(rawbow_dfa::collect_filtered_tokens("").empty());
}

TEST(KDADK_RAWBOW_DFA, CollectDelimitersOnly)
{
    EXPECT_TRUE(rawbow_dfa::collect_filtered_tokens("////??,,==&&..::").empty());
    EXPECT_TRUE(rawbow_dfa::collect_filtered_tokens("/:.,=&?").empty());
}

TEST(KDADK_RAWBOW_DFA, CollectPlainText)
{
    ExpectTokensAre(rawbow_dfa::collect_filtered_tokens("helloworld"), {"helloworld"});
}

TEST(KDADK_RAWBOW_DFA, CollectStandardUrl)
{
    // https/www/com/api 为停用词, v1 为单字母 token, 123 为全数字
    ExpectTokensAre(rawbow_dfa::collect_filtered_tokens("https://www.example.com/api/v1/users?ID=123"),
                    {"example", "users", "ID"});
}

TEST(KDADK_RAWBOW_DFA, CollectContinuousDelimiters)
{
    // name 为停用词被过滤
    ExpectTokensAre(rawbow_dfa::collect_filtered_tokens("user//name==id??"), {"user", "id"});
}

TEST(KDADK_RAWBOW_DFA, CollectLeadingDelimiters)
{
    ExpectTokensAre(rawbow_dfa::collect_filtered_tokens(":://example.com/path/file"),
                    {"example", "path", "file"});
}

TEST(KDADK_RAWBOW_DFA, CollectTrailingDelimiters)
{
    // query 为停用词被过滤
    ExpectTokensAre(rawbow_dfa::collect_filtered_tokens("query=param/.,&?"), {"param"});
}

TEST(KDADK_RAWBOW_DFA, CollectTokensWithSpecialChars)
{
    ExpectTokensAre(rawbow_dfa::collect_filtered_tokens("user_name123:pass-word,my@data"),
                    {"user_name123", "pass-word", "my@data"});
}

TEST(KDADK_RAWBOW_DFA, CollectUppercaseStopwords)
{
    // 停用词匹配大小写不敏感
    ExpectTokensAre(rawbow_dfa::collect_filtered_tokens("HTTP://WWW.Example.COM/API/Users"),
                    {"Example", "Users"});
}

TEST(KDADK_RAWBOW_DFA, CollectSingleAlphaTokens)
{
    // v1 / a_123 / x-99 均为单字母 token 被过滤, 仅 ab1 保留
    ExpectTokensAre(rawbow_dfa::collect_filtered_tokens("v1/a_123/x-99/ab1"), {"ab1"});
}

TEST(KDADK_RAWBOW_DFA, CollectTokenLengthBoundary)
{
    // 20 字符恰好保留, 21 字符溢出被过滤
    const std::string input =
        "short/" + Repeat('a', 20) + "/" + Repeat('b', 21) + "/tail";
    ExpectTokensAre(rawbow_dfa::collect_filtered_tokens(input),
                    {"short", Repeat('a', 20), "tail"});
}

TEST(KDADK_RAWBOW_DFA, CollectAllDigits)
{
    EXPECT_TRUE(rawbow_dfa::collect_filtered_tokens("0/123/456789").empty());
}

TEST(KDADK_RAWBOW_DFA, CollectStopwordPrefixExtendedIsKept)
{
    // httpx / httpsapi 不是停用词, 应保留
    ExpectTokensAre(rawbow_dfa::collect_filtered_tokens("httpx/httpsapi"),
                    {"httpx", "httpsapi"});
}

TEST(KDADK_RAWBOW_DFA, CollectUnderscoreOnlyToken)
{
    // "__" 长度 2、非全数字、无字母: 保留
    ExpectTokensAre(rawbow_dfa::collect_filtered_tokens("__"), {"__"});
}

TEST(KDADK_RAWBOW_DFA, CollectMixedRealisticUrl)
{
    // token 不是停用词, 应保留
    ExpectTokensAre(rawbow_dfa::collect_filtered_tokens(
                        "https://cdn.example.net/resource/path/to/file-name_20260701.jpg?token=abc123"),
                    {"cdn", "example", "resource", "path", "to", "file-name_20260701", "jpg",
                     "token", "abc123"});
}

TEST(KDADK_RAWBOW_DFA, CollectMaxTokensZero)
{
    EXPECT_TRUE(rawbow_dfa::collect_filtered_tokens("hello/world", 0).empty());
}

TEST(KDADK_RAWBOW_DFA, CollectMaxTokensLimitReached)
{
    // 上限 2: 收满后提前返回
    ExpectTokensAre(rawbow_dfa::collect_filtered_tokens("xx/yy/zz", 2), {"xx", "yy"});
}

TEST(KDADK_RAWBOW_DFA, CollectMaxTokensEarlyStopSkipsRest)
{
    // 达到上限后, 后续 token(含超长串)不再处理
    const std::string input = "xx/yy/" + Repeat('c', 300) + "/zz";
    ExpectTokensAre(rawbow_dfa::collect_filtered_tokens(input, 2), {"xx", "yy"});
}

TEST(KDADK_RAWBOW_DFA, CollectMaxTokensLargerThanResult)
{
    ExpectTokensAre(rawbow_dfa::collect_filtered_tokens("aa/bb/cc", 100), {"aa", "bb", "cc"});
}

TEST(KDADK_RAWBOW_DFA, CollectSkippedTokensDoNotConsumeLimit)
{
    // 被过滤的 token 不占用 max_tokens 名额: www/123 被过滤, 仍有 2 个名额
    ExpectTokensAre(rawbow_dfa::collect_filtered_tokens("www/123/xx/yy/zz", 2), {"xx", "yy"});
}

TEST(KDADK_RAWBOW_DFA, CollectDefaultMaxIsUnlimited)
{
    // 默认参数 = 不限数量
    std::string input;
    for (int i = 0; i < 100; ++i) {
        if (!input.empty()) {
            input += '/';
        }
        input += "tok" + std::to_string(i);  // tok0, tok1, ... 均为合法 token
    }
    std::vector<std::string> expected;
    for (int i = 0; i < 100; ++i) {
        expected.push_back("tok" + std::to_string(i));
    }
    ExpectTokensAre(rawbow_dfa::collect_filtered_tokens(input), expected);
}

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------
TEST(KDADK_RAWBOW_DFA, Constants)
{
    EXPECT_EQ(20u, rawbow_dfa::kMaxTokenLength);
    EXPECT_EQ(std::numeric_limits<std::size_t>::max(), rawbow_dfa::kUnlimitedTokens);
    EXPECT_EQ(128u, rawbow_dfa::kAsciiAlphabetSize);
}
