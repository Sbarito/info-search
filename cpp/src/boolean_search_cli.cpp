#include "text_tokenizer.h"
#include "word_stemmer.h"
#include "fs_utils.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

struct LexEntry {
    std::string term;
    uint64_t offset;
    uint32_t df;
};

static uint16_t read_u16(std::ifstream& in) { uint16_t x; in.read(reinterpret_cast<char*>(&x), sizeof(x)); return x; }
static uint32_t read_u32(std::ifstream& in) { uint32_t x; in.read(reinterpret_cast<char*>(&x), sizeof(x)); return x; }
static uint64_t read_u64(std::ifstream& in) { uint64_t x; in.read(reinterpret_cast<char*>(&x), sizeof(x)); return x; }

static bool load_terms(const std::string& path, std::vector<LexEntry>& lex) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    char magic[4];
    in.read(magic, 4);
    if (std::string(magic, 4) != "BIDX") return false;

    uint32_t ver = read_u32(in);
    if (ver != 1) return false;

    uint32_t n = read_u32(in);
    lex.clear();
    lex.reserve(n);

    for (uint32_t i = 0; i < n; ++i) {
        uint16_t len = read_u16(in);
        std::string term(len, '\0');
        if (len) in.read(&term[0], len);
        uint64_t off = read_u64(in);
        uint32_t df = read_u32(in);
        lex.push_back({term, off, df});
    }
    return true;
}

static bool load_docs(const std::string& path,
                      std::vector<std::string>& urls,
                      std::vector<std::string>& titles) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    char magic[4];
    in.read(magic, 4);
    if (std::string(magic, 4) != "DOCS") return false;

    uint32_t ver = read_u32(in);
    if (ver != 1) return false;

    uint32_t n = read_u32(in);
    urls.resize(n);
    titles.resize(n);

    for (uint32_t i = 0; i < n; ++i) {
        uint16_t ulen = read_u16(in);
        std::string url(ulen, '\0');
        if (ulen) in.read(&url[0], ulen);

        uint16_t tlen = read_u16(in);
        std::string title(tlen, '\0');
        if (tlen) in.read(&title[0], tlen);

        urls[i] = url;
        titles[i] = title;
    }
    return true;
}

static int lex_find(const std::vector<LexEntry>& lex, const std::string& term) {
    int l = 0;
    int r = (int)lex.size() - 1;
    while (l <= r) {
        int m = l + (r - l) / 2;
        if (lex[m].term == term) return m;
        if (lex[m].term < term) l = m + 1;
        else r = m - 1;
    }
    return -1;
}

static void read_postings(std::ifstream& in, const LexEntry& e, std::vector<uint32_t>& out) {
    out.resize(e.df);
    in.seekg((std::streamoff)e.offset);
    if (e.df) in.read(reinterpret_cast<char*>(out.data()),
                      e.df * sizeof(uint32_t));
}

static void intersect(const std::vector<uint32_t>& a,
                      const std::vector<uint32_t>& b,
                      std::vector<uint32_t>& out) {
    out.clear();
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == b[j]) { out.push_back(a[i]); ++i; ++j; }
        else if (a[i] < b[j]) ++i;
        else ++j;
    }
}

static void unite(const std::vector<uint32_t>& a,
                  const std::vector<uint32_t>& b,
                  std::vector<uint32_t>& out) {
    out.clear();
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == b[j]) { out.push_back(a[i]); ++i; ++j; }
        else if (a[i] < b[j]) out.push_back(a[i++]);
        else out.push_back(b[j++]);
    }
    while (i < a.size()) out.push_back(a[i++]);
    while (j < b.size()) out.push_back(b[j++]);
}

static void diff(const std::vector<uint32_t>& a,
                 const std::vector<uint32_t>& b,
                 std::vector<uint32_t>& out) {
    out.clear();
    size_t i = 0, j = 0;
    while (i < a.size()) {
        if (j >= b.size()) out.push_back(a[i++]);
        else if (a[i] == b[j]) { ++i; ++j; }
        else if (a[i] < b[j]) out.push_back(a[i++]);
        else ++j;
    }
}

static void complement(uint32_t doc_count,
                       const std::vector<uint32_t>& a,
                       std::vector<uint32_t>& out) {
    out.clear();
    size_t j = 0;
    for (uint32_t d = 0; d < doc_count; ++d) {
        while (j < a.size() && a[j] < d) ++j;
        if (j < a.size() && a[j] == d) continue;
        out.push_back(d);
    }
}

enum TokenType { TT_TERM, TT_AND, TT_OR, TT_NOT, TT_LP, TT_RP };

struct QueryToken {
    TokenType type;
    std::string term;
};

static int precedence(TokenType t) {
    if (t == TT_NOT) return 3;
    if (t == TT_AND) return 2;
    if (t == TT_OR) return 1;
    return 0;
}

static bool is_unary(TokenType t) { return t == TT_NOT; }

static void tokenize_query(const std::string& q,
                           std::vector<QueryToken>& out,
                           const Tokenizer& tokenizer,
                           const RussianStemmer& stemmer,
                           bool stemming) {
    out.clear();
    std::string buf;

    auto flush = [&]() {
        if (buf.empty()) return;
        std::vector<std::string> ts;
        tokenizer.tokenize(buf, ts);
        bool first = true;
        for (auto& raw : ts) {
            std::string t = raw;
            if (stemming) t = stemmer.stem(t);
            if (t.empty()) continue;
            if (!first) out.push_back({TT_AND, ""});
            out.push_back({TT_TERM, t});
            first = false;
        }
        buf.clear();
    };

    for (char c : q) {
        if (c == '(') { flush(); out.push_back({TT_LP, ""}); }
        else if (c == ')') { flush(); out.push_back({TT_RP, ""}); }
        else if (c == '&') { flush(); out.push_back({TT_AND, ""}); }
        else if (c == '|') { flush(); out.push_back({TT_OR, ""}); }
        else if (c == '!') { flush(); out.push_back({TT_NOT, ""}); }
        else if (isspace((unsigned char)c)) flush();
        else buf.push_back(c);
    }
    flush();
}

static bool to_postfix(const std::vector<QueryToken>& in,
                       std::vector<QueryToken>& out) {
    out.clear();
    std::vector<QueryToken> ops;

    for (const auto& t : in) {
        if (t.type == TT_TERM) {
            out.push_back(t);
        } else if (t.type == TT_LP) {
            ops.push_back(t);
        } else if (t.type == TT_RP) {
            bool ok = false;
            while (!ops.empty()) {
                auto top = ops.back(); ops.pop_back();
                if (top.type == TT_LP) { ok = true; break; }
                out.push_back(top);
            }
            if (!ok) return false;
        } else {
            while (!ops.empty()) {
                auto top = ops.back();
                if (top.type == TT_LP) break;
                int p1 = precedence(top.type);
                int p2 = precedence(t.type);
                if (p1 > p2 || (p1 == p2 && !is_unary(t.type))) {
                    ops.pop_back();
                    out.push_back(top);
                } else break;
            }
            ops.push_back(t);
        }
    }

    while (!ops.empty()) {
        if (ops.back().type == TT_LP) return false;
        out.push_back(ops.back());
        ops.pop_back();
    }
    return true;
}

static bool eval_postfix(const std::vector<QueryToken>& pf,
                         uint32_t doc_count,
                         const std::vector<LexEntry>& lex,
                         std::ifstream& postings,
                         std::vector<uint32_t>& out) {
    std::vector<std::vector<uint32_t>> st;

    for (const auto& t : pf) {
        if (t.type == TT_TERM) {
            std::vector<uint32_t> v;
            int idx = lex_find(lex, t.term);
            if (idx >= 0) read_postings(postings, lex[idx], v);
            st.push_back(v);
        } else if (t.type == TT_NOT) {
            if (st.empty()) return false;
            std::vector<uint32_t> tmp;
            complement(doc_count, st.back(), tmp);
            st.pop_back();
            st.push_back(tmp);
        } else {
            if (st.size() < 2) return false;
            auto b = st.back(); st.pop_back();
            auto a = st.back(); st.pop_back();
            std::vector<uint32_t> r;
            if (t.type == TT_AND) intersect(a, b, r);
            else unite(a, b, r);
            st.push_back(r);
        }
    }

    if (st.size() != 1) return false;
    out = st.back();
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) return 1;

    std::string index_dir = argv[1];
    std::string query = argv[2];

    int limit = 20;
    bool stemming = true;

    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--limit" && i + 1 < argc) { limit = std::stoi(argv[++i]); }
        else if (a == "--stemming" && i + 1 < argc) { stemming = (std::string(argv[++i]) == "1"); }
    }

    std::vector<LexEntry> lex;
    if (!load_terms(index_dir + "/terms.bin", lex)) return 2;

    std::vector<std::string> urls, titles;
    if (!load_docs(index_dir + "/docs.bin", urls, titles)) return 3;
    uint32_t doc_count = (uint32_t)urls.size();

    std::ifstream postings(index_dir + "/postings.bin", std::ios::binary);
    if (!postings) return 4;

    TokenizerConfig tc;
    tc.lowercase = true;
    tc.normalize_yo = true;
    Tokenizer tokenizer(tc);
    RussianStemmer stemmer;

    std::vector<QueryToken> toks;
    tokenize_query(query, toks, tokenizer, stemmer, stemming);

    std::vector<QueryToken> pf;
    if (!to_postfix(toks, pf)) return 5;

    std::vector<uint32_t> res;
    if (!eval_postfix(pf, doc_count, lex, postings, res)) return 6;

    int shown = 0;
    for (uint32_t d : res) {
        if (shown >= limit) break;
        if (d >= doc_count) continue;
        std::string title = titles[d].empty() ? urls[d] : titles[d];
        std::cout << d << "\t" << urls[d] << "\t" << title << "\n";
        ++shown;
    }

    return 0;
}
