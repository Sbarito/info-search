#include "text_tokenizer.h"
#include "word_stemmer.h"
#include "fs_utils.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static bool read_lines(const std::string& path, std::vector<std::string>& out) {
    out.clear();
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (!line.empty()) out.push_back(line);
    }
    return true;
}

static long long bytes_total(const std::vector<std::string>& files) {
    long long sum = 0;
    for (const auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        if (!in) continue;
        in.seekg(0, std::ios::end);
        auto sz = in.tellg();
        if (sz > 0) sum += static_cast<long long>(sz);
    }
    return sum;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: text_token_stats <docs_list.txt> [--stemming 0|1]\n";
        return 1;
    }

    std::string list_path = argv[1];
    bool use_stemming = false;

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--stemming" && i + 1 < argc) {
            use_stemming = (std::string(argv[i + 1]) == "1");
            i++;
        }
    }

    std::vector<std::string> files;
    if (!read_lines(list_path, files)) {
        std::cerr << "Cannot read list: " << list_path << "\n";
        return 2;
    }

    TokenizerConfig cfg;
    cfg.lowercase = true;
    cfg.normalize_yo = true;
    cfg.keep_numbers = true;
    cfg.min_len = 2;

    Tokenizer tokenizer(cfg);
    RussianStemmer stemmer;

    std::vector<std::string> tokens;
    long long token_count = 0;
    long long total_token_len_chars = 0;

    auto t0 = std::chrono::steady_clock::now();

    for (const auto& file : files) {
        std::string text;
        if (!read_file_utf8(file, text)) continue;

        tokenizer.tokenize(text, tokens);
        if (use_stemming) {
            for (auto& tok : tokens) tok = stemmer.stem(tok);
        }

        token_count += static_cast<long long>(tokens.size());

        for (const auto& s : tokens) {
            int n = 0;
            for (size_t k = 0; k < s.size();) {
                unsigned char c = static_cast<unsigned char>(s[k]);
                if (c < 0x80) {
                    k += 1;
                    n += 1;
                } else if (k + 1 < s.size()) {
                    k += 2;
                    n += 1;
                } else {
                    break;
                }
            }
            total_token_len_chars += n;
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();

    long long total_bytes = bytes_total(files);
    double kb = static_cast<double>(total_bytes) / 1024.0;
    double avg_len = token_count ? static_cast<double>(total_token_len_chars) / token_count : 0.0;
    double tokens_per_kb = kb > 0 ? static_cast<double>(token_count) / kb : 0.0;

    std::cout << "docs=" << files.size() << "\n";
    std::cout << "total_bytes=" << total_bytes << "\n";
    std::cout << "token_count=" << token_count << "\n";
    std::cout << "avg_token_len_chars=" << avg_len << "\n";
    std::cout << "time_sec=" << sec << "\n";
    std::cout << "tokens_per_kb=" << tokens_per_kb << "\n";
    std::cout << "stemming=" << (use_stemming ? 1 : 0) << "\n";

    return 0;
}
