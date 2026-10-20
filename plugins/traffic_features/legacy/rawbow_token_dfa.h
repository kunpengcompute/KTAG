/*
 * Frozen streaming DFA for legacy RawBoW token collection.
 *
 * Preserves the established tokenize_url + filter_tokens behavior for ASCII
 * URL/domain tokens,
 * while avoiding intermediate vectors and strings for rejected tokens.
 */
#ifndef RAWBOW_TOKEN_DFA_H
#define RAWBOW_TOKEN_DFA_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace rawbow_dfa {

constexpr std::size_t kMaxTokenLength = 20;
constexpr std::size_t kUnlimitedTokens = std::numeric_limits<std::size_t>::max();
constexpr std::size_t kAsciiAlphabetSize = 128;
constexpr std::size_t kStopwordCount = 69;
constexpr std::size_t kStopwordTrieNodeCount = 154;

struct StopwordNode {
    std::array<int, kAsciiAlphabetSize> next{};
    bool terminal = false;
};

const std::array<std::string_view, kStopwordCount> &stopword_words() noexcept;
const std::array<StopwordNode, kStopwordTrieNodeCount> &stopword_trie() noexcept;
std::size_t stopword_count() noexcept;
std::size_t stopword_trie_node_count() noexcept;

struct TokenState {
    std::size_t start = 0;
    std::size_t len = 0;
    bool overflow_len = false;
    bool all_digits = true;
    unsigned char alpha_count_state = 0;  // 0, 1, 2 means 2+
    bool single_alpha_candidate = true;
    int stopword_node = 0;
    bool stopword_dead = false;

    void reset()
    {
        start = 0;
        len = 0;
        overflow_len = false;
        all_digits = true;
        alpha_count_state = 0;
        single_alpha_candidate = true;
        stopword_node = 0;
        stopword_dead = false;
    }
};

bool is_delimiter(unsigned char c);
bool is_ascii_digit(unsigned char c);
bool is_ascii_alpha(unsigned char c);
unsigned char to_ascii_lower(unsigned char c);
void update_stopword_state(TokenState &state, unsigned char c);
void update_token_state(TokenState &state, std::size_t index, unsigned char c);
bool is_stopword_terminal(const TokenState &state);
bool should_accept_token(const TokenState &state);
void finalize_token(std::string_view input, const TokenState &state, std::vector<std::string> &out,
                    std::size_t max_tokens);
std::vector<std::string> collect_filtered_tokens(std::string_view input,
                                                 std::size_t max_tokens = kUnlimitedTokens);

}  // namespace rawbow_dfa

#endif  // RAWBOW_TOKEN_DFA_H
