#pragma once
// Reader of the differential-oracle vectors (tests/data/oracle/*.json, docs/oracle.md) for the
// tests that check code against them: a minimal JSON parser, families, rows and a per-family
// tally that prints the first mismatches. Same format as tests/oracle_test.cpp. GROMADA_ORACLE_DIR
// must be defined by the build.
#include "test.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace oracle_vectors {

struct Json {
    enum Kind { Null, Num, Str, Arr, Obj } kind = Null;
    std::int64_t num = 0;
    std::string str;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;
    const Json* find(const std::string& k) const {
        auto it = obj.find(k);
        return it == obj.end() ? nullptr : &it->second;
    }
};

struct Parser {
    std::string_view s;
    std::size_t p = 0;
    bool ok = true;
    void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t')) ++p; }
    bool eat(char c) { ws(); if (p < s.size() && s[p] == c) { ++p; return true; } return false; }
    std::string string() {
        std::string out;
        if (!eat('"')) { ok = false; return out; }
        while (p < s.size() && s[p] != '"') {
            if (s[p] == '\\' && p + 1 < s.size()) { ++p; out += s[p] == 'n' ? '\n' : s[p]; }
            else out += s[p];
            ++p;
        }
        ++p;
        return out;
    }
    Json value() {
        Json j;
        ws();
        if (p >= s.size()) { ok = false; return j; }
        const char c = s[p];
        if (c == '{') {
            ++p; j.kind = Json::Obj;
            if (eat('}')) return j;
            do { std::string k = string(); if (!eat(':')) { ok = false; return j; } j.obj[k] = value(); } while (ok && eat(','));
            if (!eat('}')) ok = false;
        } else if (c == '[') {
            ++p; j.kind = Json::Arr;
            if (eat(']')) return j;
            do { j.arr.push_back(value()); } while (ok && eat(','));
            if (!eat(']')) ok = false;
        } else if (c == '"') {
            j.kind = Json::Str; j.str = string();
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            j.kind = Json::Num;
            const bool neg = c == '-';
            if (neg) ++p;
            std::int64_t v = 0;
            while (p < s.size() && s[p] >= '0' && s[p] <= '9') v = v * 10 + (s[p++] - '0');
            j.num = neg ? -v : v;
        } else if (s.substr(p, 4) == "null") {
            p += 4;
        } else {
            ok = false;
        }
        return j;
    }
};

struct Family {
    std::string name;
    Json root;
    std::vector<std::string> columns;
    std::vector<std::string> objectColumns;
    bool loaded = false;
    int col(const std::string& c) const {
        for (std::size_t i = 0; i < columns.size(); ++i) if (columns[i] == c) return int(i);
        std::fprintf(stderr, "%s: no column %s\n", name.c_str(), c.c_str());
        std::exit(2);
    }
    int ocol(const std::string& c) const {
        for (std::size_t i = 0; i < objectColumns.size(); ++i) if (objectColumns[i] == c) return int(i);
        std::fprintf(stderr, "%s: no object column %s\n", name.c_str(), c.c_str());
        std::exit(2);
    }
    const std::vector<Json>& cases() const { return root.find("cases")->arr; }
};

inline Family load(const std::string& name) {
    Family f;
    f.name = name;
    std::ifstream in(std::string(GROMADA_ORACLE_DIR) + "/" + name + ".json", std::ios::binary);
    if (!in) { std::fprintf(stderr, "missing vectors %s.json\n", name.c_str()); ++gromada_test::failures; return f; }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    Parser p{text};
    f.root = p.value();
    if (!p.ok || f.root.kind != Json::Obj || !f.root.find("cases")) {
        std::fprintf(stderr, "bad JSON in %s.json\n", name.c_str()); ++gromada_test::failures; return f;
    }
    for (auto& c : f.root.find("columns")->arr) f.columns.push_back(c.str);
    if (auto* oc = f.root.find("objectColumns")) for (auto& c : oc->arr) f.objectColumns.push_back(c.str);
    f.loaded = true;
    return f;
}

// Row accessor: r("name") reads a column of the current case, r.obj(k, "name") a column of the
// k-th variable-length group (objectColumns).
struct Row {
    const Family& f;
    const std::vector<Json>& v;
    std::int64_t operator()(const std::string& c) const { return v[std::size_t(f.col(c))].num; }
    std::int64_t at(std::size_t i) const { return v[i].num; }
    std::size_t size() const { return v.size(); }
    std::int64_t obj(int k, const std::string& c) const {
        return v[f.columns.size() + std::size_t(k) * f.objectColumns.size() + std::size_t(f.ocol(c))].num;
    }
};

struct Tally {
    std::string name;
    int cases = 0, bad = 0, skipped = 0;
    void check(bool good, int index, const char* what, std::int64_t expected, std::int64_t got) {
        if (good) return;
        if (bad < 8) std::fprintf(stderr, "  %s case %d: %s expected %lld got %lld\n", name.c_str(), index, what,
                                  (long long)expected, (long long)got);
        ++bad;
    }
    ~Tally() {
        std::fprintf(stderr, "%-20s %6d cases%s %s\n", name.c_str(), cases,
                     skipped ? (" (" + std::to_string(skipped) + " documented divergences)").c_str() : "",
                     bad ? "MISMATCH" : "ok");
        if (bad) { std::fprintf(stderr, "  %d mismatching values\n", bad); ++gromada_test::failures; }
    }
};

#define EXPECT(t, idx, what, expected, got) \
    do { const std::int64_t _e = (expected), _g = (got); (t).check(_e == _g, (idx), (what), _e, _g); } while (0)

template <class Fn>
void forCases(const char* name, Fn&& fn) {
    Family f = load(name);
    if (!f.loaded) return;
    Tally t{name};
    int i = 0;
    for (auto& c : f.cases()) {
        if (c.arr.size() < f.columns.size()) { t.check(false, i, "row length", std::int64_t(f.columns.size()), std::int64_t(c.arr.size())); continue; }
        fn(t, i, Row{f, c.arr}, f);
        ++t.cases;
        ++i;
    }
    if (t.cases < 1) t.check(false, 0, "no cases", 1, 0);
}

} // namespace oracle_vectors
