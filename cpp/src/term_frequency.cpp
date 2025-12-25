#include "text_tokenizer.h"
#include "word_stemmer.h"
#include "fs_utils.h"

#include <cstdio>
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

static std::string make_run_path(const std::string& dir, int idx) {
    return dir + "/run_" + std::to_string(idx) + ".txt";
}

static bool write_run(const std::string& path, std::vector<std::string>& tokens) {
    merge_sort_strings(tokens);
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    for (const auto& t : tokens) {
        out.write(t.data(), static_cast<std::streamsize>(t.size()));
        out.put('\n');
    }
    return true;
}

static bool read_token(std::ifstream& in, std::string& token) {
    token.clear();
    return static_cast<bool>(std::getline(in, token));
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: term_frequency <docs_list.txt> <out_termfreq.tsv> "
                     "[--stemming 0|1] [--chunk N]\n";
        return 1;
    }

    std::string docs_list_path = argv[1];
    std::string output_path = argv[2];

    bool use_stemming = false;
    int chunk_size = 2000000;

    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--stemming" && i + 1 < argc) {
            use_stemming = (std::string(argv[i + 1]) == "1");
            ++i;
        } else if (a == "--chunk" && i + 1 < argc) {
            chunk_size = std::stoi(argv[i + 1]);
            ++i;
        }
    }

    std::vector<std::string> doc_paths;
    if (!read_lines(docs_list_path, doc_paths)) {
        std::cerr << "Cannot read docs list\n";
        return 2;
    }

    TokenizerConfig tc;
    Tokenizer tokenizer(tc);
    RussianStemmer stemmer;

    std::vector<std::string> buffer;
    buffer.reserve(static_cast<size_t>(chunk_size));

    std::string tmp_dir = "tmp_term_frequency";

#ifdef _WIN32
    std::system(("mkdir " + tmp_dir).c_str());
#else
    std::system(("mkdir -p " + tmp_dir).c_str());
#endif

    int run_count = 0;
    std::vector<std::string> tokens;

    for (const auto& path : doc_paths) {
        std::string text;
        if (!read_file_utf8(path, text)) continue;

        tokenizer.tokenize(text, tokens);
        if (use_stemming) {
            for (auto& t : tokens) t = stemmer.stem(t);
        }

        for (const auto& t : tokens) {
            buffer.push_back(t);
            if ((int)buffer.size() >= chunk_size) {
                std::string run_path = make_run_path(tmp_dir, run_count++);
                if (!write_run(run_path, buffer)) return 3;
                buffer.clear();
            }
        }
    }

    if (!buffer.empty()) {
        std::string run_path = make_run_path(tmp_dir, run_count++);
        if (!write_run(run_path, buffer)) return 3;
        buffer.clear();
    }

    if (run_count == 0) {
        std::ofstream(output_path).close();
        return 0;
    }

    std::vector<std::ifstream*> inputs;
    std::vector<std::string> current;
    inputs.reserve(run_count);
    current.reserve(run_count);

    for (int i = 0; i < run_count; ++i) {
        auto* f = new std::ifstream(make_run_path(tmp_dir, i), std::ios::binary);
        if (!(*f)) return 4;
        inputs.push_back(f);
        std::string tok;
        if (read_token(*f, tok)) current.push_back(tok);
        else current.push_back("");
    }

    std::vector<char> finished(run_count, 0);
    for (int i = 0; i < run_count; ++i) {
        if (!(*inputs[i]) || (current[i].empty() && inputs[i]->eof()))
            finished[i] = 1;
    }

    std::ofstream out(output_path);
    if (!out) return 5;

    auto done = [&]() {
        for (char f : finished) if (!f) return false;
        return true;
    };

    std::string active_term;
    long long active_count = 0;

    while (!done()) {
        int best = -1;
        for (int i = 0; i < run_count; ++i) {
            if (finished[i]) continue;
            if (best < 0 || current[i] < current[best]) best = i;
        }
        if (best < 0) break;

        std::string tok = current[best];
        std::string next;
        if (read_token(*inputs[best], next)) {
            current[best] = next;
        } else {
            finished[best] = 1;
            current[best].clear();
        }

        if (active_term.empty()) {
            active_term = tok;
            active_count = 1;
        } else if (tok == active_term) {
            ++active_count;
        } else {
            out << active_term << "\t" << active_count << "\n";
            active_term = tok;
            active_count = 1;
        }
    }

    if (!active_term.empty()) {
        out << active_term << "\t" << active_count << "\n";
    }

    for (auto* f : inputs) {
        f->close();
        delete f;
    }

    return 0;
}
