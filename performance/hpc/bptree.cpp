// bptree.cpp - C++ reference for the cache-line-blocked B+ tree benchmark.
// Mirrors performance/hpc/bptree.cb 1:1 (same layout, baseline, RNG, probe
// sequences, and workload) for a direct CFlat-vs-C++ comparison.
//
// Build (MSVC, from repo root - requires the x64 developer environment):
//   cmd /c "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 ^
//     && cl /O2 /std:c++17 /EHsc performance\hpc\bptree.cpp /Feperformance\hpc\bptree_cpp.exe
//
// Run:
//   performance\hpc\bptree_cpp.exe

#include <cstdint>
#include <cstdio>
#include <chrono>
#include <vector>
#include <xmmintrin.h>   // _mm_prefetch

static const int FANOUT = 16;
static const int MAXKEY = 15;   // FANOUT - 1

// Internal node: keys-only, children by index (mirrors BNode in the .cb).
struct BNode {
    int64_t keys[15];           // separator keys: keys[i] = min key of child[i+1]
    int32_t child[16];          // index into node arena (or leaf arena if childLeaf)
    int32_t count = 0;
    bool    childLeaf = false;
};

// Leaf: values + a next-leaf index for linear range scans (mirrors BLeaf).
struct BLeaf {
    int64_t keys[15];
    int64_t vals[15];
    int32_t count = 0;
    int32_t next = -1;
};

// --- Tree state (single-threaded; globals to match the .cb structure) ---

static std::vector<BNode> g_nodes;
static std::vector<BLeaf> g_leaves;
static int  g_nodeCount = 0;
static int  g_leafCount = 0;
static int  g_rootIdx = 0;
static bool g_rootIsLeaf = true;

// Bulk-load bottom-up from sorted (keys[i], vals[i]) input.
static void build(const std::vector<int64_t>& keys, const std::vector<int64_t>& vals) {
    int n = (int)keys.size();
    int nLeaves = (n + MAXKEY - 1) / MAXKEY;
    if (nLeaves < 1)
        nLeaves = 1;

    g_leaves.assign(nLeaves, BLeaf{});
    g_nodes.assign(nLeaves + 16, BNode{});
    g_nodeCount = 0;
    g_leafCount = 0;

    std::vector<int32_t> curIdx(nLeaves), nextIdx(nLeaves);
    std::vector<int64_t> curMin(nLeaves), nextMin(nLeaves);
    int curCount = 0;

    // Pass 1: pack the sorted input into leaves and link them in order.
    int pos = 0;
    while (pos < n) {
        int idx = g_leafCount++;
        BLeaf* lf = &g_leaves[idx];
        lf->next = -1;
        int64_t mn = keys[pos];

        int c = 0;
        while (c < MAXKEY && pos < n) {
            lf->keys[c] = keys[pos];
            lf->vals[c] = vals[pos];
            c++;
            pos++;
        }
        lf->count = c;

        if (idx > 0)
            g_leaves[idx - 1].next = idx;

        curIdx[curCount] = idx;
        curMin[curCount] = mn;
        curCount++;
    }

    // Pass 2: build internal levels until a single root remains.
    bool childLeaf = true;
    while (curCount > 1) {
        int nextCount = 0;
        int p = 0;
        while (p < curCount) {
            int idx = g_nodeCount++;
            BNode* nd = &g_nodes[idx];
            nd->childLeaf = childLeaf;

            int64_t mn = curMin[p];

            nd->child[0] = curIdx[p];
            p++;
            int c = 1;
            while (c < FANOUT && p < curCount) {
                nd->keys[c - 1] = curMin[p];
                nd->child[c] = curIdx[p];
                c++;
                p++;
            }
            nd->count = c - 1;

            nextIdx[nextCount] = idx;
            nextMin[nextCount] = mn;
            nextCount++;
        }

        for (int i = 0; i < nextCount; i++) {
            curIdx[i] = nextIdx[i];
            curMin[i] = nextMin[i];
        }
        curCount = nextCount;
        childLeaf = false;
    }

    g_rootIdx = curIdx[0];
    g_rootIsLeaf = (g_nodeCount == 0);
}

static int descendToLeaf(int64_t key) {
    if (g_rootIsLeaf)
        return g_rootIdx;

    int idx = g_rootIdx;
    while (true) {
        BNode* nd = &g_nodes[idx];
        // Branchless: the keys are sorted, so the first j with key < keys[j] is
        // just the count of separators <= key. Tallying that unconditionally
        // drops the data-dependent exit branch random keys mispredict ~50% of
        // the time, at every level. (Mirrors bptree.cb.)
        int cnt = nd->count;
        int j = 0;
        for (int k = 0; k < cnt; k++)
            j += (key >= nd->keys[k]);
        int child = nd->child[j];
        if (nd->childLeaf)
            return child;
        idx = child;
    }
}

static bool lookup(int64_t key, int64_t* out) {
    int lf = descendToLeaf(key);
    BLeaf* leaf = &g_leaves[lf];
    for (int i = 0; i < leaf->count; i++) {
        if (leaf->keys[i] == key) {
            *out = leaf->vals[i];
            return true;
        }
    }
    return false;
}

// Scan one leaf for `key`, adding its value into *acc if present. Shared by the
// scalar lookup and the pipelined batch path. (Mirrors bptree.cb.)
static void scanLeafInto(int lf, int64_t key, int64_t* acc) {
    BLeaf* leaf = &g_leaves[lf];
    int c = leaf->count;
    for (int i = 0; i < c; i++) {
        if (leaf->keys[i] == key) {
            *acc += leaf->vals[i];
            return;
        }
    }
}

// Software-pipeline depth (power of two so the ring index is a mask).
static const int BATCH_DEPTH = 8;

// Pipelined batch lookups - the real lever for a latency-bound index. Keeps
// BATCH_DEPTH independent probes in flight: probe i's leaf is prefetched, then
// scanned BATCH_DEPTH iterations later once resident, so each leaf miss overlaps
// the descents of the probes behind it. Returns the sum of found values.
// (Mirrors bptree.cb.)
static int64_t lookupBatch(const int64_t* probes, int n) {
    const int D = BATCH_DEPTH;
    int64_t sum = 0;
    int     leafRing[BATCH_DEPTH] = {0};
    int64_t keyRing[BATCH_DEPTH]  = {0};

    int primed = D < n ? D : n;
    int i = 0;
    for (; i < primed; i++) {
        int64_t key = probes[i];
        int lf = descendToLeaf(key);
        _mm_prefetch((const char*)&g_leaves[lf], _MM_HINT_T0);
        leafRing[i] = lf;
        keyRing[i]  = key;
    }

    for (; i < n; i++) {
        int slot = i % D;
        scanLeafInto(leafRing[slot], keyRing[slot], &sum);
        int64_t key = probes[i];
        int lf = descendToLeaf(key);
        _mm_prefetch((const char*)&g_leaves[lf], _MM_HINT_T0);
        leafRing[slot] = lf;
        keyRing[slot]  = key;
    }

    for (int d = 0; d < primed; d++) {
        int slot = (n - primed + d) % D;
        scanLeafInto(leafRing[slot], keyRing[slot], &sum);
    }
    return sum;
}

static int64_t rangeSum(int64_t lo, int64_t hi, int64_t* outCount) {
    int lf = descendToLeaf(lo);
    int64_t sum = 0;
    int64_t cnt = 0;

    while (lf >= 0) {
        BLeaf* leaf = &g_leaves[lf];

        if (leaf->next >= 0)
            _mm_prefetch((const char*)&g_leaves[leaf->next], _MM_HINT_T0);

        bool done = false;
        for (int i = 0; i < leaf->count; i++) {
            int64_t k = leaf->keys[i];
            if (k > hi) {
                done = true;
                break;
            }
            if (k >= lo) {
                sum += leaf->vals[i];
                cnt++;
            }
        }
        if (done)
            break;
        lf = leaf->next;
    }

    *outCount = cnt;
    return sum;
}

// --- Sorted-array baseline (binary search) ---

static std::vector<int64_t> g_baseKeys;
static std::vector<int64_t> g_baseVals;

static bool baselineLookup(int64_t key, int64_t* out) {
    int lo = 0;
    int hi = (int)g_baseKeys.size() - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int64_t k = g_baseKeys[mid];
        if (k == key) {
            *out = g_baseVals[mid];
            return true;
        }
        if (k < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return false;
}

// Same LCG as the .cb so both languages probe identical key sequences.
static uint64_t g_rng = 0x9e3779b97f4a7c15ULL;
static uint64_t nextRand() {
    g_rng = g_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return g_rng;
}

static double msSince(std::chrono::high_resolution_clock::time_point t0) {
    auto t1 = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

int main() {
    int N = 1000000;
    int M = 2000000;
    printf("Building B+ tree over %d sorted keys (FANOUT=%d)...\n", N, FANOUT);

    std::vector<int64_t> keys(N), vals(N);
    for (int i = 0; i < N; i++) {
        keys[i] = (int64_t)i * 2;
        vals[i] = (int64_t)i * 10;
    }

    build(keys, vals);
    printf("  leaves=%d internal_nodes=%d root_is_leaf=%d\n",
           g_leafCount, g_nodeCount, (int)g_rootIsLeaf);

    g_baseKeys = keys;
    g_baseVals = vals;

    int64_t maxKey = (int64_t)N * 2;

    // --- Correctness check against the baseline ---
    int mismatches = 0;
    for (int i = 0; i < 10000; i++) {
        int64_t probe = (int64_t)(nextRand() % (uint64_t)maxKey);
        int64_t a = 0, b = 0;
        bool fa = lookup(probe, &a);
        bool fb = baselineLookup(probe, &b);
        if (fa != fb || (fa && a != b))
            mismatches++;
    }
    printf("Correctness vs baseline: %d mismatches (expect 0)\n", mismatches);

    // Each phase is timed REPS times; we report the average and the best run.
    // Best filters out OS-scheduler noise; average shows the typical case.
    const int REPS = 7;
    int64_t sink = 0;

    // Materialize the probe sequence ONCE, outside every timed region, so the
    // 64-bit modulo (~20-cycle divide) does not bury the lookup cost. (Mirrors
    // bptree.cb.)
    std::vector<int64_t> probes(M);
    g_rng = 0x1234567ULL;
    for (int i = 0; i < M; i++)
        probes[i] = (int64_t)(nextRand() % (uint64_t)maxKey);
    const int64_t* probePtr = probes.data();

    // The pipelined path must sum to exactly the scalar path. Verify once.
    int64_t scalarCheck = 0;
    for (int i = 0; i < M; i++) {
        int64_t v = 0;
        if (lookup(probePtr[i], &v))
            scalarCheck += v;
    }
    int64_t batchCheck = lookupBatch(probePtr, M);
    printf("Batch vs scalar lookup sum: match=%d\n", (int)(scalarCheck == batchCheck));

    // --- B+ tree lookups (scalar, one independent call per probe) ---
    double treeSum = 0.0, treeBest = 1.0e30;
    for (int r = 0; r < REPS; r++) {
        auto t0 = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < M; i++) {
            int64_t v = 0;
            if (lookup(probePtr[i], &v))
                sink += v;
        }
        double m = msSince(t0);
        treeSum += m;
        if (m < treeBest) treeBest = m;
    }
    double treeMs = treeSum / REPS;

    // --- B+ tree lookups (pipelined batch with BATCH_DEPTH-ahead prefetch) ---
    double batchSum = 0.0, batchBest = 1.0e30;
    for (int r = 0; r < REPS; r++) {
        auto t0 = std::chrono::high_resolution_clock::now();
        sink += lookupBatch(probePtr, M);
        double m = msSince(t0);
        batchSum += m;
        if (m < batchBest) batchBest = m;
    }
    double batchMs = batchSum / REPS;

    // --- Sorted-array binary search baseline ---
    double baseSum = 0.0, baseBest = 1.0e30;
    for (int r = 0; r < REPS; r++) {
        auto t0 = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < M; i++) {
            int64_t v = 0;
            if (baselineLookup(probePtr[i], &v))
                sink += v;
        }
        double m = msSince(t0);
        baseSum += m;
        if (m < baseBest) baseBest = m;
    }
    double baseMs = baseSum / REPS;

    printf("\nLookups (%d random, %d runs, avg / best):\n", M, REPS);
    printf("  B+ tree scalar: %8.2f / %8.2f ms  (%6.1f M ops/s best)\n",
           treeMs, treeBest, (double)M / 1000.0 / treeBest);
    printf("  B+ tree batch : %8.2f / %8.2f ms  (%6.1f M ops/s best)\n",
           batchMs, batchBest, (double)M / 1000.0 / batchBest);
    printf("  sorted+bsearch: %8.2f / %8.2f ms  (%6.1f M ops/s best)\n",
           baseMs, baseBest, (double)M / 1000.0 / baseBest);

    // --- Range query throughput ---
    int R = 100000;
    int64_t width = 1000;
    int64_t totalMatches = 0;
    double rangeSumMs = 0.0, rangeBest = 1.0e30;
    for (int r = 0; r < REPS; r++) {
        g_rng = 0xabcdefULL;
        totalMatches = 0;
        auto t0 = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < R; i++) {
            int64_t lo = (int64_t)(nextRand() % (uint64_t)maxKey);
            int64_t cnt = 0;
            int64_t s = rangeSum(lo, lo + width, &cnt);
            sink += s;
            totalMatches += cnt;
        }
        double m = msSince(t0);
        rangeSumMs += m;
        if (m < rangeBest) rangeBest = m;
    }
    double rangeMs = rangeSumMs / REPS;
    printf("\nRange queries (%d, width=%lld, %d runs): %8.2f / %8.2f ms  (%6.1f M ops/s best), avg %lld matches/query\n",
           R, (long long)width, REPS, rangeMs, rangeBest,
           (double)R / 1000.0 / rangeBest, (long long)(totalMatches / (int64_t)R));

    printf("\nsink=%lld (printed so the optimizer can't delete the work)\n", (long long)sink);
    return 0;
}
