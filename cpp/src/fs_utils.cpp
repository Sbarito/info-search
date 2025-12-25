#include "fs_utils.h"
#include <fstream>
#include <string>
#include <vector>
#include <cstdint>

bool read_file_utf8(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    std::streamsize size = in.tellg();
    if (size < 0) size = 0;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) in.read(&out[0], size);
    return true;
}

std::string trim(const std::string& s) {
    size_t l = 0;
    size_t r = s.size();
    while (l < r && (s[l] == ' ' || s[l] == '\t' || s[l] == '\n' || s[l] == '\r')) l++;
    while (r > l && (s[r - 1] == ' ' || s[r - 1] == '\t' || s[r - 1] == '\n' || s[r - 1] == '\r')) r--;
    return s.substr(l, r - l);
}

void split_by_char(const std::string& s, char delim, std::vector<std::string>& out) {
    out.clear();
    std::string cur;
    for (char c : s) {
        if (c == delim) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
}

bool str_starts_with(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    return s.compare(0, prefix.size(), prefix) == 0;
}

static void merge_sort_strings_rec(std::vector<std::string>& a,
                                   std::vector<std::string>& tmp,
                                   int l,
                                   int r) {
    if (r - l <= 1) return;
    int m = (l + r) / 2;
    merge_sort_strings_rec(a, tmp, l, m);
    merge_sort_strings_rec(a, tmp, m, r);
    int i = l, j = m, k = l;
    while (i < m && j < r) {
        if (a[i] <= a[j]) tmp[k++] = a[i++];
        else tmp[k++] = a[j++];
    }
    while (i < m) tmp[k++] = a[i++];
    while (j < r) tmp[k++] = a[j++];
    for (int p = l; p < r; ++p) a[p] = tmp[p];
}

void merge_sort_strings(std::vector<std::string>& a) {
    std::vector<std::string> tmp(a.size());
    merge_sort_strings_rec(a, tmp, 0, static_cast<int>(a.size()));
}

static void merge_sort_pairs_rec(std::vector<std::string>& terms,
                                 std::vector<uint32_t>& docs,
                                 std::vector<std::string>& t2,
                                 std::vector<uint32_t>& d2,
                                 int l,
                                 int r) {
    if (r - l <= 1) return;
    int m = (l + r) / 2;
    merge_sort_pairs_rec(terms, docs, t2, d2, l, m);
    merge_sort_pairs_rec(terms, docs, t2, d2, m, r);
    int i = l, j = m, k = l;
    while (i < m && j < r) {
        bool left =
            (terms[i] < terms[j]) ||
            (terms[i] == terms[j] && docs[i] <= docs[j]);
        if (left) {
            t2[k] = terms[i];
            d2[k] = docs[i];
            i++;
        } else {
            t2[k] = terms[j];
            d2[k] = docs[j];
            j++;
        }
        k++;
    }
    while (i < m) {
        t2[k] = terms[i];
        d2[k] = docs[i];
        i++; k++;
    }
    while (j < r) {
        t2[k] = terms[j];
        d2[k] = docs[j];
        j++; k++;
    }
    for (int p = l; p < r; ++p) {
        terms[p] = t2[p];
        docs[p] = d2[p];
    }
}

void merge_sort_pairs_term_doc(std::vector<std::string>& terms,
                               std::vector<uint32_t>& docs) {
    std::vector<std::string> t2(terms.size());
    std::vector<uint32_t> d2(docs.size());
    merge_sort_pairs_rec(terms, docs, t2, d2, 0,
                         static_cast<int>(terms.size()));
}

static void merge_sort_td_rec(std::vector<TermDoc>& a,
                              std::vector<TermDoc>& tmp,
                              int l,
                              int r) {
    if (r - l <= 1) return;
    int m = (l + r) / 2;
    merge_sort_td_rec(a, tmp, l, m);
    merge_sort_td_rec(a, tmp, m, r);
    int i = l, j = m, k = l;
    while (i < m && j < r) {
        bool left =
            (a[i].term < a[j].term) ||
            (a[i].term == a[j].term && a[i].doc <= a[j].doc);
        if (left) tmp[k++] = a[i++];
        else tmp[k++] = a[j++];
    }
    while (i < m) tmp[k++] = a[i++];
    while (j < r) tmp[k++] = a[j++];
    for (int p = l; p < r; ++p) a[p] = tmp[p];
}

void merge_sort_termdoc(std::vector<TermDoc>& a) {
    std::vector<TermDoc> tmp(a.size());
    merge_sort_td_rec(a, tmp, 0, static_cast<int>(a.size()));
}

int bin_search_terms(const std::vector<std::string>& terms,
                     const std::string& key) {
    int l = 0;
    int r = static_cast<int>(terms.size()) - 1;
    while (l <= r) {
        int m = l + (r - l) / 2;
        if (terms[m] == key) return m;
        if (terms[m] < key) l = m + 1;
        else r = m - 1;
    }
    return -1;
}
