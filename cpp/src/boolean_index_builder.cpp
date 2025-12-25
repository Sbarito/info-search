#include "text_tokenizer.h"
#include "word_stemmer.h"
#include "fs_utils.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static void write_u16(std::ofstream& out, uint16_t x) { out.write(reinterpret_cast<char*>(&x), sizeof(x)); }
static void write_u32(std::ofstream& out, uint32_t x) { out.write(reinterpret_cast<char*>(&x), sizeof(x)); }
static void write_u64(std::ofstream& out, uint64_t x) { out.write(reinterpret_cast<char*>(&x), sizeof(x)); }

static uint16_t read_u16(std::ifstream& in) { uint16_t x; in.read(reinterpret_cast<char*>(&x), sizeof(x)); return x; }
static uint32_t read_u32(std::ifstream& in) { uint32_t x; in.read(reinterpret_cast<char*>(&x), sizeof(x)); return x; }

static std::string clean_field(std::string s) {
    for (char& c : s) {
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    }
    return s;
}

struct ProgramArgs {
    std::string docs_list;
    std::string meta_tsv;
    std::string out_dir;
    bool use_stemming = true;
    uint64_t chunk_pairs = 2000000;
};

static bool parse_args(int argc, char** argv, ProgramArgs& a) {
    if (argc < 4) return false;
    a.docs_list = argv[1];
    a.meta_tsv = argv[2];
    a.out_dir = argv[3];

    for (int i = 4; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "--stemming" && i + 1 < argc) {
            a.use_stemming = (std::string(argv[i + 1]) == "1");
            ++i;
        } else if (s == "--chunk_pairs" && i + 1 < argc) {
            a.chunk_pairs = std::stoull(argv[i + 1]);
            ++i;
        }
    }
    return true;
}

static bool read_lines(const std::string& path, std::vector<std::string>& out) {
    std::ifstream in(path);
    if (!in) return false;
    out.clear();
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (!line.empty()) out.push_back(line);
    }
    return true;
}

static bool split_tsv6(const std::string& line, std::string p[6]) {
    size_t pos = 0;
    for (int i = 0; i < 5; ++i) {
        size_t tab = line.find('\t', pos);
        if (tab == std::string::npos) return false;
        p[i] = line.substr(pos, tab - pos);
        pos = tab + 1;
    }
    p[5] = line.substr(pos);
    return true;
}

static bool build_docs_file(const std::string& meta_tsv,
                            uint32_t doc_count,
                            const std::string& out_path) {
    std::vector<std::string> urls(doc_count);
    std::vector<std::string> titles(doc_count);

    std::ifstream in(meta_tsv);
    if (!in) return false;

    std::string header;
    if (!std::getline(in, header)) return false;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::string p[6];
        if (!split_tsv6(line, p)) continue;

        int id;
        try { id = std::stoi(p[0]); }
        catch (...) { continue; }

        if (id < 0 || (uint32_t)id >= doc_count) continue;

        urls[id] = clean_field(p[1]);
        titles[id] = clean_field(p[4]);
    }

    std::ofstream out(out_path, std::ios::binary);
    if (!out) return false;

    out.write("DOCS", 4);
    write_u32(out, 1);
    write_u32(out, doc_count);

    for (uint32_t i = 0; i < doc_count; ++i) {
        const auto& u = urls[i];
        const auto& t = titles[i];

        uint16_t ul = static_cast<uint16_t>(std::min<size_t>(u.size(), 65535));
        uint16_t tl = static_cast<uint16_t>(std::min<size_t>(t.size(), 65535));

        write_u16(out, ul);
        if (ul) out.write(u.data(), ul);

        write_u16(out, tl);
        if (tl) out.write(t.data(), tl);
    }

    return true;
}

static bool write_run(const std::string& path, std::vector<TermDoc>& data) {
    merge_sort_termdoc(data);

    if (!data.empty()) {
        size_t w = 1;
        for (size_t i = 1; i < data.size(); ++i) {
            if (data[i].term == data[w - 1].term &&
                data[i].doc == data[w - 1].doc) continue;
            data[w++] = data[i];
        }
        data.resize(w);
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    for (const auto& td : data) {
        uint16_t len = static_cast<uint16_t>(std::min<size_t>(td.term.size(), 65535));
        write_u16(out, len);
        if (len) out.write(td.term.data(), len);
        write_u32(out, td.doc);
    }

    return true;
}

struct RunReader {
    std::ifstream in;
    bool valid = false;
    std::string term;
    uint32_t doc = 0;

    bool open(const std::string& path) {
        in.open(path, std::ios::binary);
        valid = false;
        return static_cast<bool>(in);
    }

    bool next() {
        if (!in || in.peek() == EOF) { valid = false; return false; }
        uint16_t len;
        in.read(reinterpret_cast<char*>(&len), sizeof(len));
        if (!in) { valid = false; return false; }
        term.resize(len);
        if (len) in.read(&term[0], len);
        doc = read_u32(in);
        valid = static_cast<bool>(in);
        return valid;
    }
};

struct LexEntry {
    std::string term;
    uint64_t offset;
    uint32_t df;
};

static bool merge_runs(const std::vector<std::string>& run_paths,
                       const std::string& terms_path,
                       const std::string& postings_path) {
    std::vector<RunReader> runs(run_paths.size());
    for (size_t i = 0; i < run_paths.size(); ++i) {
        if (!runs[i].open(run_paths[i])) return false;
        runs[i].next();
    }

    std::ofstream postings(postings_path, std::ios::binary);
    if (!postings) return false;

    std::vector<LexEntry> lexicon;
    std::string current_term;
    std::vector<uint32_t> postings_buf;
    uint64_t offset = 0;

    std::string last_term;
    uint32_t last_doc = 0;
    bool has_last = false;

    auto flush = [&]() {
        if (current_term.empty()) return;
        postings.write(reinterpret_cast<char*>(postings_buf.data()),
                       postings_buf.size() * sizeof(uint32_t));
        lexicon.push_back({current_term, offset, (uint32_t)postings_buf.size()});
        offset += postings_buf.size() * sizeof(uint32_t);
        postings_buf.clear();
        current_term.clear();
    };

    while (true) {
        int best = -1;
        for (size_t i = 0; i < runs.size(); ++i) {
            if (!runs[i].valid) continue;
            if (best < 0 ||
                runs[i].term < runs[best].term ||
                (runs[i].term == runs[best].term && runs[i].doc < runs[best].doc)) {
                best = (int)i;
            }
        }
        if (best < 0) break;

        auto term = runs[best].term;
        auto doc = runs[best].doc;
        runs[best].next();

        if (has_last && term == last_term && doc == last_doc) continue;
        has_last = true;
        last_term = term;
        last_doc = doc;

        if (current_term.empty()) {
            current_term = term;
            postings_buf.push_back(doc);
        } else if (term == current_term) {
            if (postings_buf.back() != doc) postings_buf.push_back(doc);
        } else {
            flush();
            current_term = term;
            postings_buf.push_back(doc);
        }
    }

    flush();

    std::ofstream terms(terms_path, std::ios::binary);
    if (!terms) return false;

    terms.write("BIDX", 4);
    write_u32(terms, 1);
    write_u32(terms, (uint32_t)lexicon.size());

    for (const auto& e : lexicon) {
        uint16_t len = static_cast<uint16_t>(std::min<size_t>(e.term.size(), 65535));
        write_u16(terms, len);
        if (len) terms.write(e.term.data(), len);
        write_u64(terms, e.offset);
        write_u32(terms, e.df);
    }

    return true;
}

int main(int argc, char** argv) {
    ProgramArgs a;
    if (!parse_args(argc, argv, a)) return 1;

    std::system(("mkdir -p \"" + a.out_dir + "\"").c_str());

    std::vector<std::string> docs;
    if (!read_lines(a.docs_list, docs)) return 2;
    uint32_t doc_count = (uint32_t)docs.size();
    if (!doc_count) return 3;

    if (!build_docs_file(a.meta_tsv, doc_count, a.out_dir + "/docs.bin")) return 4;

    TokenizerConfig tc;
    tc.lowercase = true;
    tc.normalize_yo = true;
    Tokenizer tokenizer(tc);
    RussianStemmer stemmer;

    std::vector<TermDoc> chunk;
    chunk.reserve((size_t)a.chunk_pairs);

    std::vector<std::string> run_paths;
    int run_id = 0;

    for (uint32_t doc_id = 0; doc_id < doc_count; ++doc_id) {
        std::string text;
        if (!read_file_utf8(docs[doc_id], text)) continue;

        std::vector<std::string> toks;
        tokenizer.tokenize(text, toks);

        if (a.use_stemming) {
            for (auto& t : toks) t = stemmer.stem(t);
        }

        merge_sort_strings(toks);
        toks.erase(std::unique(toks.begin(), toks.end()), toks.end());

        for (const auto& t : toks) {
            if (t.empty()) continue;
            chunk.push_back({t, doc_id});
            if (chunk.size() >= a.chunk_pairs) {
                std::string path = a.out_dir + "/run_" + std::to_string(run_id++) + ".bin";
                if (!write_run(path, chunk)) return 5;
                run_paths.push_back(path);
                chunk.clear();
            }
        }
    }

    if (!chunk.empty()) {
        std::string path = a.out_dir + "/run_" + std::to_string(run_id++) + ".bin";
        if (!write_run(path, chunk)) return 6;
        run_paths.push_back(path);
        chunk.clear();
    }

    if (!merge_runs(run_paths,
                    a.out_dir + "/terms.bin",
                    a.out_dir + "/postings.bin")) return 7;

    for (const auto& p : run_paths) std::remove(p.c_str());

    return 0;
}
