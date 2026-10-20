#include "rawbow_token_dfa.h"

#include <algorithm>

namespace rawbow_dfa {
namespace {

constexpr std::array<std::string_view, kStopwordCount> kStopwords = {
    "http", "https", "com", "cn", "www", "net", "org", "resolve", "host", "query", "api", "app",
    "ftp", "ftps", "ssh", "telnet", "smtp", "pop3", "imap", "dns", "dhcp", "tcp", "udp", "icmp",
    "edu", "gov", "mil", "int", "info", "biz", "name", "pro", "museum", "aero", "coop", "jobs",
    "travel", "mobi", "asia", "cat", "tel", "xxx", "post", "geo", "us", "uk", "de", "fr", "jp", "kr",
    "ru", "au", "ca", "in", "br", "mx", "it", "es", "nl", "se", "no", "dk", "fi", "pl", "tr", "th",
    "sg", "hk", "tw",
};

struct StopwordTrieTable {
    std::array<StopwordNode, kStopwordTrieNodeCount> nodes{};
    std::size_t node_count = 1;
    bool valid = true;
};

constexpr bool string_view_equal(std::string_view lhs, std::string_view rhs)
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i] != rhs[i]) {
            return false;
        }
    }
    return true;
}

constexpr bool stopword_list_is_valid()
{
    for (std::size_t i = 0; i < kStopwords.size(); ++i) {
        if (kStopwords[i].empty()) {
            return false;
        }
        for (std::size_t char_index = 0; char_index < kStopwords[i].size(); ++char_index) {
            const unsigned char c = static_cast<unsigned char>(kStopwords[i][char_index]);
            if (c >= kAsciiAlphabetSize || (c >= 'A' && c <= 'Z')) {
                return false;
            }
        }
        for (std::size_t previous = 0; previous < i; ++previous) {
            if (string_view_equal(kStopwords[i], kStopwords[previous])) {
                return false;
            }
        }
    }
    return true;
}

constexpr StopwordTrieTable build_stopword_trie()
{
    StopwordTrieTable trie{};
    for (std::size_t node = 0; node < trie.nodes.size(); ++node) {
        for (std::size_t c = 0; c < kAsciiAlphabetSize; ++c) {
            trie.nodes[node].next[c] = -1;
        }
    }

    for (std::size_t word_index = 0; word_index < kStopwords.size(); ++word_index) {
        const std::string_view word = kStopwords[word_index];
        std::size_t node = 0;
        for (std::size_t char_index = 0; char_index < word.size(); ++char_index) {
            const unsigned char c = static_cast<unsigned char>(word[char_index]);
            if (c >= kAsciiAlphabetSize) {
                trie.valid = false;
                return trie;
            }

            int next = trie.nodes[node].next[c];
            if (next < 0) {
                if (trie.node_count >= trie.nodes.size()) {
                    trie.valid = false;
                    return trie;
                }
                next = static_cast<int>(trie.node_count);
                ++trie.node_count;
                trie.nodes[node].next[c] = next;
            }
            node = static_cast<std::size_t>(next);
        }
        trie.nodes[node].terminal = true;
    }
    return trie;
}

constexpr StopwordTrieTable kStopwordTrie = build_stopword_trie();

constexpr bool trie_contains_all_stopwords()
{
    for (std::size_t word_index = 0; word_index < kStopwords.size(); ++word_index) {
        const std::string_view word = kStopwords[word_index];
        std::size_t node = 0;
        for (std::size_t char_index = 0; char_index < word.size(); ++char_index) {
            const unsigned char c = static_cast<unsigned char>(word[char_index]);
            if (c >= kAsciiAlphabetSize) {
                return false;
            }
            const int next = kStopwordTrie.nodes[node].next[c];
            if (next < 0) {
                return false;
            }
            node = static_cast<std::size_t>(next);
        }
        if (!kStopwordTrie.nodes[node].terminal) {
            return false;
        }
    }
    return true;
}

static_assert(stopword_list_is_valid(),
              "stopword list must contain unique, non-empty ASCII words without uppercase letters");
static_assert(kStopwordTrie.valid, "compile-time stopword trie capacity is too small");
static_assert(kStopwordTrie.node_count == kStopwordTrieNodeCount,
              "kStopwordTrieNodeCount must match the generated trie");
static_assert(trie_contains_all_stopwords(), "compile-time stopword trie is incomplete");

}  // namespace

const std::array<std::string_view, kStopwordCount> &stopword_words() noexcept { return kStopwords; }
const std::array<StopwordNode, kStopwordTrieNodeCount> &stopword_trie() noexcept
{
    return kStopwordTrie.nodes;
}
std::size_t stopword_count() noexcept { return kStopwords.size(); }
std::size_t stopword_trie_node_count() noexcept { return kStopwordTrie.node_count; }

bool is_delimiter(unsigned char c)
{
    return c == '/' || c == '?' || c == ',' || c == '=' || c == '&' || c == '.' || c == ':';
}
bool is_ascii_digit(unsigned char c) { return c >= '0' && c <= '9'; }
bool is_ascii_alpha(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
unsigned char to_ascii_lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }

void update_stopword_state(TokenState &state, unsigned char c)
{
    if (state.stopword_dead) return;
    const unsigned char lower = to_ascii_lower(c);
    if (lower >= kAsciiAlphabetSize) { state.stopword_dead = true; return; }
    const int next = stopword_trie()[state.stopword_node].next[lower];
    if (next < 0) state.stopword_dead = true;
    else state.stopword_node = next;
}

void update_token_state(TokenState &state, std::size_t index, unsigned char c)
{
    if (state.len == 0) state.start = index;
    ++state.len;
    state.overflow_len |= state.len > kMaxTokenLength;
    state.all_digits &= is_ascii_digit(c);
    if (is_ascii_alpha(c)) {
        if (state.alpha_count_state < 2) ++state.alpha_count_state;
        if (state.alpha_count_state > 1) state.single_alpha_candidate = false;
    } else if (!(is_ascii_digit(c) || c == '_' || c == '-')) {
        state.single_alpha_candidate = false;
    }
    update_stopword_state(state, c);
}

bool is_stopword_terminal(const TokenState &state)
{
    return !state.stopword_dead && state.stopword_node >= 0 &&
           stopword_trie()[state.stopword_node].terminal;
}

bool should_accept_token(const TokenState &state)
{
    if (state.len < 2 || state.overflow_len || state.all_digits) return false;
    if (state.single_alpha_candidate && state.alpha_count_state == 1) return false;
    return !is_stopword_terminal(state);
}

void finalize_token(std::string_view input, const TokenState &state, std::vector<std::string> &out,
                    std::size_t max_tokens)
{
    if (state.len != 0 && out.size() < max_tokens && should_accept_token(state)) {
        out.emplace_back(input.substr(state.start, state.len));
    }
}

std::vector<std::string> collect_filtered_tokens(std::string_view input, std::size_t max_tokens)
{
    std::vector<std::string> out;
    if (input.empty() || max_tokens == 0) return out;
    out.reserve(std::min<std::size_t>(8, max_tokens));
    TokenState state;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(input[i]);
        if (is_delimiter(c)) {
            finalize_token(input, state, out, max_tokens);
            if (out.size() >= max_tokens) return out;
            state.reset();
        } else {
            update_token_state(state, i, c);
        }
    }
    finalize_token(input, state, out, max_tokens);
    return out;
}

}  // namespace rawbow_dfa
