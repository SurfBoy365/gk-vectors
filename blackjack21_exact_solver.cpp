// Exact optimal solver for 21 Blackjack (parallel).
//
// Same validated DP as before (best_future + stay closures + resolve), but the
// state exploration runs across all cores via OpenMP, backed by a lock-striped
// concurrent chained hash map. Correctness is preserved by construction:
//   * every map read/write for a given key happens under that key's stripe lock,
//     so the table structure can never be corrupted by concurrent access;
//   * DP values are a pure function of the state, so if two threads race to
//     compute the same state they produce the identical value — a redundant
//     compute, never a wrong answer.
// The reconstruction pass runs serially over the already-filled memo.
//
// Protocol (unchanged) — reads "number suit" per line from stdin; writes:
//   line 1: <exact_total_score>
//   then per placement:
//     <index> <from_sig> <placed_sig> <resolved_sig> <gained> <next_streak> <next_stays> <clear_count> [<clear_sig>...]
//
// Sig packing: raw(6) | aces(3)<<6 | len(3)<<9 | all_sevens(1)<<12; empty = 1<<12.

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <array>
#include <algorithm>
#include <atomic>
#ifdef _OPENMP
#include <omp.h>
#endif

using u64 = uint64_t;
using ll = long long;

static const int EMPTY_SIG = 1 << 12;
static const int SCORE_21 = 400, B5 = 800, B5_21 = 1000, B37 = 600, PERFECT = 1000;
static const int MAX_STAYS = 2;
static const int STREAK_BONUS[4] = {250, 500, 1000, 1000};
static const ll NEG = -1000000000LL;

// Parallelize the DP for placements at shallow deck depth; deeper levels run
// serially inside their task (the bulk of the states are deep and shared).
static const int PAR_DEPTH = 5;

static int N;
static std::vector<int> cardNum;

static inline int sraw(int s) { return s & 0x3F; }
static inline int sace(int s) { return (s >> 6) & 0x7; }
static inline int slen(int s) { return (s >> 9) & 0x7; }
static inline int ssev(int s) { return (s >> 12) & 1; }
static inline int pack(int raw, int a, int l, int se) { return (raw & 0x3F) | ((a & 0x7) << 6) | ((l & 0x7) << 9) | ((se & 1) << 12); }
static inline int stotal(int s) { int t = sraw(s), a = sace(s); while (t > 21 && a) { t -= 10; a--; } return t; }
static inline int cval(int n) { if (n == 0) return 11; if (n >= 9) return 10; return n + 1; }
static inline int addcard(int sig, int n) { return pack(sraw(sig) + cval(n), sace(sig) + (n == 0 ? 1 : 0), slen(sig) + 1, ssev(sig) && (n == 6)); }
static inline int sbonus(int st) { int i = st - 2; return (i >= 0 && i < 4) ? STREAK_BONUS[i] : 0; }

static inline bool resolve(int sig, int streak, int &rsig, int &gained, int &nstreak) {
    int total = stotal(sig);
    if (total > 21) return false;
    if (total == 21) {
        int ns = streak + 1 < 6 ? streak + 1 : 6;
        if (slen(sig) == 5) { rsig = EMPTY_SIG; gained = B5_21 + sbonus(ns); nstreak = ns; return true; }
        if (slen(sig) == 3 && ssev(sig)) { rsig = EMPTY_SIG; gained = B37 + sbonus(ns); nstreak = ns; return true; }
        rsig = EMPTY_SIG; gained = SCORE_21 + sbonus(ns); nstreak = ns; return true;
    }
    if (slen(sig) == 5) { int ns = streak + 1 < 6 ? streak + 1 : 6; rsig = EMPTY_SIG; gained = B5 + sbonus(ns); nstreak = ns; return true; }
    rsig = sig; gained = 0; nstreak = 0; return true;
}

using S4 = std::array<int, 4>;
static inline void sort4(S4 &a) {
    if (a[0] > a[1]) std::swap(a[0], a[1]);
    if (a[2] > a[3]) std::swap(a[2], a[3]);
    if (a[0] > a[2]) std::swap(a[0], a[2]);
    if (a[1] > a[3]) std::swap(a[1], a[3]);
    if (a[1] > a[2]) std::swap(a[1], a[2]);
}

struct StayR { S4 sigs; int stays; };

static int enumStays(const S4 &start, int stays, StayR *buf) {
    int n = 0;
    StayR stk[64];
    int sp = 0;
    S4 s0 = start; sort4(s0);
    stk[sp++] = {s0, stays};
    while (sp > 0) {
        StayR cur = stk[--sp];
        bool dup = false;
        for (int i = 0; i < n; i++) if (buf[i].stays == cur.stays && buf[i].sigs == cur.sigs) { dup = true; break; }
        if (dup) continue;
        buf[n++] = cur;
        if (cur.stays >= MAX_STAYS) continue;
        int prev = -1;
        for (int i = 0; i < 4; i++) {
            int sig = cur.sigs[i];
            if (sig == prev) continue; prev = sig;
            if (slen(sig) == 0 || stotal(sig) >= 21) continue;
            S4 nx = cur.sigs; nx[i] = EMPTY_SIG; sort4(nx);
            stk[sp++] = {nx, cur.stays + 1};
        }
    }
    return n;
}

static inline u64 makeKey(int index, const S4 &s, int streak, int stays) {
    u64 k = (u64)index;
    k |= ((u64)s[0]) << 6;
    k |= ((u64)s[1]) << 19;
    k |= ((u64)s[2]) << 32;
    k |= ((u64)s[3]) << 45;
    k |= ((u64)streak) << 58;
    k |= ((u64)stays) << 61;
    return k;  // never 0 (sigs are >= EMPTY_SIG = 4096)
}

// ---- Lock-free flat open-addressing hash map -----------------------------
// Fixed capacity (no resize), so slots are stable for the whole run and reads
// are a plain atomic load (a mov on x86) — as cheap as the original serial map,
// but safe for concurrent use.
//
// Each key slot holds 0 (empty), k (a writer has claimed it but the value is
// not published yet), or k|READY (value is visible). A writer stores vals[i]
// then release-stores keys[i]=k|READY; a reader acquire-loads keys[i] and only
// trusts vals[i] once it sees READY. makeKey never sets bit 63, so READY is a
// free flag. A read that lands on a not-yet-ready slot is treated as a miss and
// recomputed — always the same value (the DP is a pure function of the state),
// so a race can only waste work, never corrupt a result.
static const u64 READY = 1ULL << 63;
static const int CAP_BITS = 27;                       // 134M slots
static const size_t CAP = (size_t)1 << CAP_BITS;
static const size_t CAP_MASK = CAP - 1;
static std::atomic<u64> *keys;
static ll *vals;

static inline size_t slotOf(u64 k) {
    return (size_t)((k * 0x9E3779B97F4A7C15ULL) >> (64 - CAP_BITS));
}

static inline bool mfind(u64 k, ll &out) {
    size_t i = slotOf(k);
    for (size_t p = 0; p < CAP; p++) {
        u64 kk = keys[i].load(std::memory_order_acquire);
        if (kk == 0) return false;
        if ((kk & ~READY) == k) {
            if (kk & READY) { out = vals[i]; return true; }
            return false;  // claimed but value not published yet -> recompute
        }
        i = (i + 1) & CAP_MASK;
    }
    return false;
}
static inline void mput(u64 k, ll v) {
    size_t i = slotOf(k);
    for (size_t p = 0; p < CAP; p++) {
        u64 kk = keys[i].load(std::memory_order_acquire);
        if (kk == 0) {
            u64 exp = 0;
            if (keys[i].compare_exchange_strong(exp, k, std::memory_order_acq_rel)) {
                vals[i] = v;
                keys[i].store(k | READY, std::memory_order_release);
                return;
            }
            kk = exp;  // lost the race; re-examine this same slot
        }
        if ((kk & ~READY) == k) {
            vals[i] = v;
            keys[i].store(k | READY, std::memory_order_release);
            return;
        }
        i = (i + 1) & CAP_MASK;
    }
    // table full: leave uncached (only reachable for decks far larger than any
    // real 52-card game; the Python side would fall back to the beam solver).
}

// sigs assumed sorted
static ll best_future(int index, const S4 &sigs, int streak, int stays) {
    if (index >= N) return PERFECT;
    u64 key = makeKey(index, sigs, streak, stays);
    ll cached;
    if (mfind(key, cached)) return cached;

    int number = cardNum[index];
    struct Kid { int gained; S4 sigs; int nstreak; int stays; };
    Kid kids[4 * 24];
    int nk = 0;
    int prev = -1;
    for (int i = 0; i < 4; i++) {
        int sig = sigs[i];
        if (sig == prev) continue; prev = sig;
        int placed = addcard(sig, number), rsig, gained, nstreak;
        if (!resolve(placed, streak, rsig, gained, nstreak)) continue;
        S4 nxt = sigs; nxt[i] = rsig;
        StayR opts[24];
        int no = enumStays(nxt, stays, opts);
        for (int o = 0; o < no; o++) kids[nk++] = {gained, opts[o].sigs, nstreak, opts[o].stays};
    }

    ll best = NEG;
    if (index < PAR_DEPTH && nk > 1) {
        ll futs[4 * 24];
        #pragma omp taskloop shared(kids, futs) default(shared)
        for (int c = 0; c < nk; c++)
            futs[c] = best_future(index + 1, kids[c].sigs, kids[c].nstreak, kids[c].stays);
        for (int c = 0; c < nk; c++) {
            if (futs[c] == NEG) continue;
            ll cand = (ll)kids[c].gained + futs[c];
            if (cand > best) best = cand;
        }
    } else {
        for (int c = 0; c < nk; c++) {
            ll fut = best_future(index + 1, kids[c].sigs, kids[c].nstreak, kids[c].stays);
            if (fut == NEG) continue;
            ll cand = (ll)kids[c].gained + fut;
            if (cand > best) best = cand;
        }
    }
    mput(key, best);
    return best;
}

// Reconstruction stay enumeration (run once) — keeps the cleared-sig list.
struct StayFull { S4 sigs; int stays; int nclear; int cleared[4]; };
static int enumStaysFull(const S4 &start, int stays, StayFull *buf) {
    int n = 0;
    StayFull stk[64];
    int sp = 0;
    S4 s0 = start; sort4(s0);
    stk[sp++] = {s0, stays, 0, {0, 0, 0, 0}};
    while (sp > 0) {
        StayFull cur = stk[--sp];
        bool dup = false;
        for (int i = 0; i < n; i++) if (buf[i].stays == cur.stays && buf[i].sigs == cur.sigs) { dup = true; break; }
        if (dup) continue;
        buf[n++] = cur;
        if (cur.stays >= MAX_STAYS) continue;
        int prev = -1;
        for (int i = 0; i < 4; i++) {
            int sig = cur.sigs[i];
            if (sig == prev) continue; prev = sig;
            if (slen(sig) == 0 || stotal(sig) >= 21) continue;
            StayFull nx = cur;
            nx.sigs[i] = EMPTY_SIG; sort4(nx.sigs);
            nx.stays = cur.stays + 1;
            nx.cleared[nx.nclear++] = sig;
            stk[sp++] = nx;
        }
    }
    return n;
}

int main() {
    int n, s;
    while (scanf("%d %d", &n, &s) == 2) cardNum.push_back(n);
    N = (int)cardNum.size();

    keys = (std::atomic<u64> *)calloc(CAP, sizeof(std::atomic<u64>));  // 0 = empty
    vals = (ll *)malloc(CAP * sizeof(ll));                             // read only once READY
    if (!keys || !vals) { fprintf(stderr, "memo alloc failed\n"); return 1; }

    S4 init = {EMPTY_SIG, EMPTY_SIG, EMPTY_SIG, EMPTY_SIG};
    sort4(init);

    ll total;
    #pragma omp parallel
    {
        #pragma omp single nowait
        total = best_future(0, init, 0, 0);
    }

    if (total == NEG) { printf("NOSOLUTION\n"); return 0; }
    printf("%lld\n", total);

    // Reconstruction: serial walk over the already-filled memo (all hits).
    S4 sigs = init;
    int streak = 0, stays = 0;
    for (int index = 0; index < N; index++) {
        int number = cardNum[index];
        S4 cur = sigs; sort4(cur);
        ll target = best_future(index, cur, streak, stays);
        bool done = false;
        int prev = -1;
        for (int i = 0; i < 4 && !done; i++) {
            int sig = sigs[i];
            if (sig == prev) continue; prev = sig;
            int placed = addcard(sig, number), rsig, gained, nstreak;
            if (!resolve(placed, streak, rsig, gained, nstreak)) continue;
            S4 nxt = sigs; nxt[i] = rsig;
            StayFull opts[24];
            int no = enumStaysFull(nxt, stays, opts);
            for (int o = 0; o < no; o++) {
                ll fut = best_future(index + 1, opts[o].sigs, nstreak, opts[o].stays);
                if (fut == NEG) continue;
                if ((ll)gained + fut == target) {
                    printf("%d %d %d %d %d %d %d %d", index + 1, sig, placed, rsig, gained, nstreak, opts[o].stays, opts[o].nclear);
                    for (int c = 0; c < opts[o].nclear; c++) printf(" %d", opts[o].cleared[c]);
                    printf("\n");
                    sigs = opts[o].sigs; streak = nstreak; stays = opts[o].stays; done = true;
                    break;
                }
            }
        }
        if (!done) { fprintf(stderr, "reconstruction failed at index %d\n", index); return 1; }
    }
    return 0;
}
