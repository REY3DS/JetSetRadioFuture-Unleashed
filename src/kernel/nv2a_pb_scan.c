#include <stddef.h>
/*
 * Read-only survey of the pushbuffer a title submits.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; nothing
 * here executes them, so the framebuffer stays black however far the game
 * gets. Before any of that can be made to draw, the question is what it
 * actually asks for -- which methods, on which object classes, how many of
 * them -- because that is the difference between "the existing PGRAPH
 * translator nearly covers this" and "this needs a real one".
 *
 * Purely a reader: it walks the buffer and counts, and never writes to guest
 * memory or to the GPU state. Enabled with RECOMP_PB_SCAN.
 *
 * Pushbuffer encoding (NV20/NV2A), one dword per command header:
 *   (w & 0xE0030003) == 0x00000000  increasing methods
 *   (w & 0xE0030003) == 0x40000000  non-increasing (same method, count params)
 *   (w & 0x00000003) == 0x00000001  jump
 *   (w & 0x00000003) == 0x00000002  call
 *   (w & 0xFFFF0003) == 0x00020000  return
 * For a method header: count = (w >> 18) & 0x7FF, subchannel = (w >> 13) & 7,
 * method = w & 0x1FFC.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);
/* A pushbuffer word. The ack thread hands this the contiguous-window
 * form of DMA_PUT (0x80000000 | physical), which on the Switch is a
 * separate allocation rather than an offset from RAM: adding it to the
 * RAM base read two gigabytes past the end of the heap. */
void *xbox_HostPtr(uint32_t guest_va);

static inline uint32_t pb_word(uint32_t va)
{
    return *(const uint32_t *)xbox_HostPtr(va);
}

/* Whether a pushbuffer address names guest memory at all.
 *
 * It has to be asked, because an address that names nothing does not fault
 * here: the translator wraps it on the 26-bit bus and hands back whatever
 * lives at the wrapped offset (recomp_xbox_ptr_slow's last line). A decoder
 * that walks off the end of the ring therefore keeps reading plausible
 * dwords -- floats, pointers, method parameters -- out of unrelated memory,
 * and every one of them is decoded as a command. One data word with its low
 * bits reading as a jump sends the walk to another wrapped address, and a
 * garbage stream of "methods" is executed. Measured on the Switch: the walk
 * was fifty seconds into one such stream while the title's deferred routine
 * spun on the display interrupt this same thread completes.
 *
 * The ring is always in the contiguous 64 MB the title calls its own memory;
 * anything outside it is not a pushbuffer. */
#define PB_MEM_SIZE (64u * 1024u * 1024u)
/* The first page of Xbox RAM is the real-mode interrupt vector table and the
 * loader's own structures; no pushbuffer has ever started there. A target of
 * 0x80000004 is a data word (5) being read as a jump, and following it
 * executes unrelated memory -- the walk has to refuse it, not wrap it. */
#define PB_MEM_LO 0x80001000u
static inline int pb_va_ok(uint32_t va)
{
    return va >= PB_MEM_LO
        && (va & 0xF0000000u) == 0x80000000u
        && (va & 0x0FFFFFFFu) < PB_MEM_SIZE;
}

/* A walk that is still going after seconds is walking something that is not a
 * command stream. Bound it: this runs on the thread that also completes the
 * display interrupt's acknowledge, and a walk that never returns takes the
 * whole machine with it. */
static long long s_walk_started;
static unsigned long s_walk_base_methods;
static int s_walk_depth;

static void pb_bad_target(const char *what, uint32_t at, uint32_t tgt, uint32_t w)
{
    static int n;
    if (n++ >= 8) return;
    fprintf(stderr, "  [PB] %s to 0x%08X outside guest memory, from 0x%08X"
                    " (word 0x%08X) -- stop\n", what, tgt, at, w);
    fflush(stderr);
}

#define PB_MAX_METHODS 4096

static struct { uint32_t method, subch, count; } s_seen[PB_MAX_METHODS];
static int s_seen_count;

/* Parse health. An inventory is only worth reading if the walk stayed in step
 * with the command stream: a decoder that desynchronises produces plausible
 * looking method numbers out of parameter data, and the counts then describe
 * nothing. Unrecognised words are the tell. */
static uint32_t s_tot_words, s_tot_unknown, s_tot_jumps, s_tot_segments;

/* Executing is opt-in separately from surveying: a survey is read-only, while
 * the executor writes to guest memory. */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern uint32_t g_pb_exec_va;
extern void nv2a_pb_exec_report(void);
static int s_exec_enabled = -1;
static int s_scan_enabled = -1;

/* An index from (subchannel, method) straight to the inventory slot.
 *
 * This function used to walk the inventory looking for the pair, and it runs
 * on every word of the pushbuffer -- two million a second on this title, times
 * up to two hundred entries scanned. Measured on an M2 Pro, the pushbuffer
 * walk was 84% of the frame and this was most of it: an inventory kept for
 * diagnostics was costing more than the emulated GPU.
 *
 * A method is 13 bits and always 4-aligned, a subchannel is 3, so the pair
 * fits in a 14-bit index and the lookup is one load. Thirty-two kilobytes.
 */
#define PB_INDEX_BITS 14
static int16_t s_seen_index[1 << PB_INDEX_BITS];
static int     s_seen_index_ready;

static void note(uint32_t subch, uint32_t method)
{
    uint32_t key;

    if (!s_seen_index_ready) {
        memset(s_seen_index, 0xFF, sizeof s_seen_index);   /* -1 everywhere */
        s_seen_index_ready = 1;
    }
    key = ((subch & 7u) << 11) | ((method >> 2) & 0x7FFu);
    if (s_seen_index[key] >= 0) {
        s_seen[s_seen_index[key]].count++;
        return;
    }
    if (s_seen_count >= PB_MAX_METHODS) {
        /* Silently dropping past the cap is how a truncated inventory reads as
         * "the title never does that" -- exactly the wrong conclusion when the
         * inventory is being used to decide what to implement. */
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[PB] method table full at %d -- inventory is"
                            " truncated\n", PB_MAX_METHODS);
        }
    }
    if (s_seen_count < PB_MAX_METHODS) {
        s_seen[s_seen_count].method = method;
        s_seen[s_seen_count].subch  = subch;
        s_seen[s_seen_count].count  = 1;
        s_seen_index[key] = (int16_t)s_seen_count;
        s_seen_count++;
    }
}

/* NV097 (Kelvin 3D class) methods worth naming. The point of the survey is to
 * decide what a translator has to implement, and a bare method number does not
 * answer that -- "0x1808 x412" only means something once it reads
 * INLINE_ARRAY. Unnamed ones still get counted. */
static const struct { uint32_t m; const char *name; } NV097_NAMES[] = {
    { 0x0000, "SET_OBJECT" },
    { 0x0100, "NO_OPERATION" },
    { 0x0104, "SET_WARNING_ENABLE" },
    { 0x0130, "SET_FLIP_READ" },
    { 0x0200, "SET_SURFACE_CLIP_HORIZONTAL" },
    { 0x0204, "SET_SURFACE_CLIP_VERTICAL" },
    { 0x0208, "SET_SURFACE_FORMAT" },
    { 0x020C, "SET_SURFACE_PITCH" },
    { 0x0210, "SET_SURFACE_COLOR_OFFSET" },
    { 0x0214, "SET_SURFACE_ZETA_OFFSET" },
    { 0x0300, "SET_ALPHA_TEST_ENABLE" },
    { 0x0304, "SET_BLEND_ENABLE" },
    { 0x030C, "SET_DEPTH_TEST_ENABLE" },
    { 0x0310, "SET_DITHER_ENABLE" },
    { 0x0314, "SET_LIGHTING_ENABLE" },
    { 0x033C, "SET_CULL_FACE_ENABLE" },
    { 0x0340, "SET_DEPTH_MASK" },
    { 0x0350, "SET_CLEAR_DEPTH_VALUE" },
    { 0x1D8C, "SET_CLEAR_DEPTH" },
    { 0x1D90, "SET_COLOR_CLEAR_VALUE" },
    { 0x1D94, "CLEAR_SURFACE" },
    { 0x1D6C, "SET_ZSTENCIL_CLEAR" },
    { 0x0B80, "SET_TRANSFORM_PROGRAM" },
    { 0x0B00, "SET_TRANSFORM_CONSTANT" },
    { 0x1720, "SET_VERTEX_DATA_ARRAY_OFFSET" },
    { 0x1760, "SET_VERTEX_DATA_ARRAY_FORMAT" },
    { 0x17FC, "SET_BEGIN_END" },
    { 0x1800, "ARRAY_ELEMENT16" },
    { 0x1808, "INLINE_ARRAY" },
    { 0x1810, "DRAW_ARRAYS" },
    { 0x1B00, "SET_TEXTURE_OFFSET" },
    { 0x1B04, "SET_TEXTURE_FORMAT" },
    { 0x1B08, "SET_TEXTURE_ADDRESS" },
    { 0x1B0C, "SET_TEXTURE_CONTROL0" },
    { 0x1B14, "SET_TEXTURE_IMAGE_RECT" },
    { 0x0FD8, "SET_COMBINER_*" },
    { 0x0000, NULL },
};

static const char *nv097_name(uint32_t m)
{
    int i;
    for (i = 0; NV097_NAMES[i].name; i++)
        if (NV097_NAMES[i].m == m)
            return NV097_NAMES[i].name;
    return "";
}

void nv2a_pb_scan_report(void)
{
    int i;

    if (s_exec_enabled > 0)
        nv2a_pb_exec_report();
    if (!s_seen_count || s_scan_enabled <= 0)
        return;
    fprintf(stderr, "[PB] %u segments, %u words, %u jumps, %u unrecognised"
                    " -- %d distinct (subchannel, method) pairs\n",
            s_tot_segments, s_tot_words, s_tot_jumps, s_tot_unknown,
            s_seen_count);
    for (i = 0; i < s_seen_count; i++)
        fprintf(stderr, "  [PB]   subch %u  method 0x%04X  x%-6u %s\n",
                s_seen[i].subch, s_seen[i].method, s_seen[i].count,
                nv097_name(s_seen[i].method));
    fflush(stderr);
}

/*
 * CALL and RETURN.
 *
 * A call word ((w & 3) == 2) runs the commands at its target until a RETURN
 * (0x00020000), then carries on after the call -- one level deep, as the
 * NV2A's single subroutine register allows. Xbox D3D uses it for
 * RunPushBuffer: a recorded command stream replayed by reference. This walk
 * used to step over the call word, so everything a title drew through a
 * recorded pushbuffer was never drawn. RECOMP_PB_NOCALL=1: the old behaviour.
 */
static uint32_t s_tot_calls, s_tot_returns;

/* Where the walk is, for another thread to read.
 *
 * A walk is the longest thing the ack thread does, and it is the thread that
 * also completes the display interrupt's acknowledge: a title acknowledges
 * PCRTC_INTR_0 and spins until PMC_INTR_0 says the source is quiet, and that
 * second half only happens when this thread next runs vblank_intr_tick().
 * While the walk is inside one method -- a clear or a large draw -- that had
 * no bound at all, so the title's own deferred routine could wait out the
 * whole walk with the guest lock in its hands, and the picture stopped.
 *
 * The same numbers say where a walk that never returns is: the last method it
 * started, and how long it has been in the same 4096-word window. */
volatile uint32_t g_pb_walk_va, g_pb_cur_va, g_pb_cur_method, g_pb_cur_param;
volatile long long g_pb_walk_ms;

/* Where the last top-level walk stopped, and whether that was the end of the
 * segment. A walk that stops early has left part of a submitted segment
 * unread: the ack thread needs to know, because the fences the title waits on
 * can be in that part. */
uint32_t g_pb_walk_end_va;
int g_pb_walk_complete = 1;

/* The last packets the decoder read, in order.
 *
 * When the walk and the executor disagree about where a command starts, the
 * stream is decoded from the middle of a buffer and every word after it is
 * noise -- and the only question worth asking is which packet was the last
 * one that made sense. A title waiting on a fence the executor never wrote
 * cannot answer that from the fence's side. */
enum { PB_TRACE_N = 32 };
static struct {
    uint32_t va, word, param;
    unsigned subch, method, count, noninc;
} s_pb_trace[PB_TRACE_N];
static unsigned s_pb_trace_at, s_pb_trace_n;

static void pb_trace_packet(uint32_t hdr_va, uint32_t w)
{
    unsigned i = s_pb_trace_at;
    s_pb_trace[i].va     = hdr_va;
    s_pb_trace[i].word   = w;
    s_pb_trace[i].subch  = (w >> 13) & 7u;
    s_pb_trace[i].method = w & 0x1FFCu;
    s_pb_trace[i].count  = (w >> 18) & 0x7FFu;
    s_pb_trace[i].noninc = (w & 0xE0000000u) == 0x40000000u;
    s_pb_trace_at = (i + 1) % PB_TRACE_N;
    if (s_pb_trace_n < PB_TRACE_N) s_pb_trace_n++;
}

/* The words around the last method the executor ran, once, when a walk has
 * not moved for seconds. That is the only place where the decoder and the
 * data can be compared: everything after the last packet the decoder believed
 * is the part of the stream it could not parse. */
static void pb_dump_tail(void)
{
    static int done;
    uint32_t base, k;
    if (done++ > 0 || !g_pb_exec_va) return;
    base = (g_pb_exec_va >= 0x80u) ? g_pb_exec_va - 0x80u : g_pb_exec_va;
    if (!((base & 0xF0000000u) == 0x80000000u
          && (base & 0x0FFFFFFFu) < PB_MEM_SIZE - 0x200u))
        return;
    fprintf(stderr, "  [PB] tail from %08X (last method %08X):\n",
            base, g_pb_exec_va);
    for (k = 0; k < 0x60u; k += 8u) {
        uint32_t a = base + k * 4u, j;
        fprintf(stderr, "  [PB]  %08X:", a);
        for (j = 0; j < 8u; j++)
            fprintf(stderr, " %08X", *(volatile uint32_t *)xbox_HostPtr(a + j * 4u));
        fprintf(stderr, "%s\n", a + 0x1Cu >= g_pb_exec_va ? "  <=" : "");
    }
    fflush(stderr);
}

static void pb_trace_print(void)
{
    unsigned i, n = s_pb_trace_n < 12u ? s_pb_trace_n : 12u;
    fprintf(stderr, "  [PB] last %u packets (oldest first):\n", n);
    for (i = 0; i < n; i++) {
        unsigned k = (s_pb_trace_at + PB_TRACE_N - n + i) % PB_TRACE_N;
        fprintf(stderr, "  [PB]   hdr %08X word %08X count %3u subch %u"
                        " method %03X%s\n",
                s_pb_trace[k].va, s_pb_trace[k].word, s_pb_trace[k].count,
                s_pb_trace[k].subch, s_pb_trace[k].method,
                s_pb_trace[k].noninc ? " (non-inc)" : "");
    }
    fflush(stderr);
}
extern unsigned long g_pb_exec_methods;
extern unsigned long GetTickCount(void);

void nv2a_pb_state_line(void)
{
    pb_trace_print();
    if (g_pb_walk_ms && (long long)GetTickCount() - g_pb_walk_ms > 5000)
        pb_dump_tail();
    fprintf(stderr, "  [PB] walk va %08X for %lld ms | executing at %08X"
                    " subch %u method %03X param %08X | methods %lu\n",
            (unsigned)g_pb_walk_va,
            g_pb_walk_ms ? (long long)GetTickCount() - g_pb_walk_ms : 0,
            (unsigned)g_pb_cur_va,
            (unsigned)((g_pb_cur_method >> 13) & 7u),
            (unsigned)(g_pb_cur_method & 0x1FFCu),
            (unsigned)g_pb_cur_param,
            g_pb_exec_methods);
    fflush(stderr);
}
static int s_in_sub;
static uint32_t pb_walk_sub(uint32_t target_va);

/* The next submission boundary after a word the decoder cannot follow.
 *
 * A pushbuffer is a byte stream written by the driver, and the decoder reads
 * it as commands. When a word is not a command -- a data word the stream's
 * own length fields do not account for -- carrying on reads the rest of the
 * buffer as noise, and stopping loses everything the driver submitted after
 * that point. Both were measured on the Switch: stopping 725 dwords before
 * PUT left the fence the title was waiting on (0x57DF) unwritten and the
 * loader stopped at 0x001914F0 forever.
 *
 * The resync is not a scan for "something that looks like a command". It is
 * one exact control word, BACK_END_WRITE_SEMAPHORE_RELEASE (method 0x1D70,
 * subchannel 0, one parameter), whose parameter is the ring position the
 * driver is waiting for -- a number that only ever goes up. The pair is eight
 * bytes and its value has to be past the last release this executor made, so
 * an accidental match inside vertex or index data would also have to carry
 * the next ring position, which is not what that data holds.
 *
 * Bounded by the segment, and reported, because a silent resync would hide
 * the decode bug that caused it. */
static int pb_resync(uint32_t *va_io, uint32_t end_va, uint32_t *words_io)
{
    extern uint32_t g_pb_sem_val;
    uint32_t a;

    if (s_in_sub) return 0;            /* a subroutine ends at its RETURN */
    for (a = *va_io; a + 8u <= end_va && a - *va_io < 0x40000u; a += 4u) {
        uint32_t v;
        if (pb_word(a) != 0x00041D70u) continue;
        v = pb_word(a + 4u);
        if (v <= g_pb_sem_val || v < 0x100u) continue;
        {
            static int said;
            if (said++ < 24)
                fprintf(stderr, "  [PB] resync at %08X: skipped %u dwords of"
                                " data before the fence %08X\n",
                        a, (a - *va_io) / 4u, v);
        }
        *words_io += (a - *va_io) / 4u;
        *va_io = a;
        return 1;
    }
    return 0;
}

static uint32_t pb_walk_impl(uint32_t start_va, uint32_t end_va, int find_only)
{
    uint32_t va = start_va;
    uint32_t words = 0, jumps = 0, unknown = 0, jump_to = 0, hops = 0;
    /* find_only is the cheap tail scan of a wrapped ring: report the jump,
     * execute nothing, follow nothing. */
    const int exec = find_only ? 0 : s_exec_enabled;
    const int follow = !find_only;

    /* Both read once. This runs on every pushbuffer segment the title submits
     * -- thousands a second -- and getenv walks the environment doing a string
     * compare per entry every time it is called. */
    if (s_exec_enabled < 0)
        s_exec_enabled = getenv("RECOMP_PB_EXEC") != NULL;
    if (s_scan_enabled < 0)
        s_scan_enabled = getenv("RECOMP_PB_SCAN") != NULL;
    if (!(s_scan_enabled || s_exec_enabled) || end_va <= start_va)
        return 0;

    /*
     * How much of the frame this walk costs.
     *
     * The GL profile accounts for the GL calls and the vertex decode and finds
     * 94% of the frame somewhere else. The two remaining candidates are this
     * -- walking the pushbuffer and running every method through the backend's
     * state machine -- and the recompiled guest code itself, and they are
     * indistinguishable from the frame rate. One clock pair per segment
     * measures it without the measurement becoming the cost: a segment is
     * hundreds of methods.
     */
    { extern void nv2a_pb_time_begin(void); nv2a_pb_time_begin(); }
    if (end_va - start_va > 0x400000u)        /* a sane single-frame bound */
        end_va = start_va + 0x400000u;
    if (!pb_va_ok(start_va)) {
        pb_bad_target("segment start", end_va, start_va, 0);
        return 0;
    }
    if (s_walk_depth++ == 0) {
        s_walk_started = (long long)GetTickCount();
        s_walk_base_methods = g_pb_exec_methods;
    }
    if (!pb_va_ok(end_va - 4u))
        end_va = start_va + PB_MEM_SIZE - (start_va & 0x0FFFFFFFu);

    while (va < end_va && words < 0x100000u) {
        uint32_t w = pb_word(va);
        /* Every few thousand words, say where the walk is and let the display
         * interrupt finish. A walk is the ack thread's longest errand and the
         * title's deferred routine is spinning on the second half of that
         * interrupt while it runs: without this the two wait for each other
         * and the whole machine stops. */
        if ((words & 0x3FFu) == 0u) {
            extern void xbox_nv2a_vblank_service(void);
            long long t = (long long)GetTickCount();
            g_pb_walk_va = va;
            g_pb_walk_ms = t;
            xbox_nv2a_vblank_service();
            if (t - s_walk_started > 3000
                || g_pb_exec_methods - s_walk_base_methods > 2000000ul) {
                static int said;
                if (said++ < 6)
                    fprintf(stderr, "  [PB] walk abandoned at 0x%08X after %lld ms"
                                    " and %lu methods: not a command stream\n",
                            va, t - s_walk_started,
                            (unsigned long)(g_pb_exec_methods - s_walk_base_methods));
                s_tot_unknown += words;
                s_walk_depth--;
                g_pb_walk_va = 0;
                g_pb_walk_ms = 0;
                return jump_to;
            }
        }
        va += 4;
        words++;

        if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
            /* A jump ends this segment. Its target is handed back so the
             * caller can carry on from there: the ring wraps with a jump
             * to its start, and the commands the title wrote after it --
             * including the fence the title then waits for -- are at the
             * target, not here. */
            jumps++;
            jump_to = ((w & 3u) == 1u) ? (w & ~3u) : (w & 0x1FFFFFFCu);
            /* A jump whose target is not guest memory is a data word being
             * read as a command: this is where a desynchronised walk leaves
             * the ring, and following it executes unrelated memory. The
             * segment is not over, though -- pb_resync finds the next
             * submission boundary. */
            if (!pb_va_ok(0x80000000u | (jump_to & 0x0FFFFFFFu))) {
                pb_bad_target("jump", va - 4, jump_to, w);
                jump_to = 0;
                if (exec && follow && pb_resync(&va, end_va, &words))
                    continue;
                break;
            }
            /* A jump is a control transfer, not the end of the data.
             *
             * The driver chains a submission to a continuation block with
             * one, and the fence that ends the submission sits after the
             * transfer. Ending the walk here -- which is what the executor
             * used to do, handing the target back to a caller that ignored
             * it on the non-wrapping path -- left that fence unwritten. The
             * title then waited on it forever at 0x001914F0 while the ring's
             * consume counter sat two dwords short of what it asked for. */
            if (exec && follow && hops < 8) {
                hops++;
                va = 0x80000000u | (jump_to & 0x0FFFFFFFu);
                jump_to = 0;
                continue;
            }
            break;
        }
        if ((w & 3u) == 2u) {
            static int nocall = -1;
            uint32_t tgt = 0x80000000u | ((w & ~3u) & 0x0FFFFFFFu);
            if (nocall < 0) nocall = getenv("RECOMP_PB_NOCALL") != NULL;
            s_tot_calls++;
            if (s_tot_calls <= 12 || (s_tot_calls & (s_tot_calls - 1)) == 0)
                fprintf(stderr, "[PB] call #%u at %08X -> %08X%s\n", s_tot_calls,
                        va - 4, tgt, s_in_sub ? " (inside a call: ignored)"
                                               : nocall ? " (RECOMP_PB_NOCALL: skipped)" : "");
            if (!pb_va_ok(tgt)) {
                pb_bad_target("call", va - 4, tgt, w);
                continue;
            }
            if (!s_in_sub && !nocall && exec) {
                uint32_t n;
                s_in_sub = 1;
                n = pb_walk_sub(tgt);
                s_in_sub = 0;
                words += n;
            }
            continue;
        }
        if ((w & 0xFFFF0003u) == 0x00020000u) {
            s_tot_returns++;
            if (s_in_sub) break;          /* back to the caller */
            continue;
        }
        if ((w & 0x00030003u) == 0u) {
            uint32_t count  = (w >> 18) & 0x7FFu;
            uint32_t subch  = (w >> 13) & 7u;
            uint32_t method =  w & 0x1FFCu;
            int noninc = (w & 0xE0000000u) == 0x40000000u;

            if (exec) pb_trace_packet(va - 4, w);
            for (uint32_t i = 0; i < count && va < end_va; i++) {
                uint32_t m = noninc ? method : method + i * 4;
                /* The inventory is only ever printed under RECOMP_PB_SCAN, so
                 * when nobody is going to read it, do not build it. */
                if (s_scan_enabled) note(subch, m);
                /* Same walk, two consumers: the survey counts, the executor
                 * acts. Keeping them on one decode means they can never
                 * disagree about what the stream said. */
                if (exec) {
                    g_pb_exec_va = va - 4;
                    g_pb_cur_va = va - 4;
                    g_pb_cur_method = w;
                    g_pb_cur_param = pb_word(va);
                    nv2a_pb_exec_method(subch, m, pb_word(va));
                }
                va += 4;
                words++;
            }
            continue;
        }
        if (exec && follow && pb_resync(&va, end_va, &words))
            continue;
        unknown++;
    }

    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
    if (s_walk_depth == 1) {
        g_pb_walk_end_va = va;
        g_pb_walk_complete = (va >= end_va) || find_only;
    }
    s_walk_depth--;
    { extern void nv2a_pb_time_end(unsigned methods); nv2a_pb_time_end(words); }
    return jump_to;
}

uint32_t nv2a_pb_scan(uint32_t start_va, uint32_t end_va)
{
    return pb_walk_impl(start_va, end_va, 0);
}

/* Where the tail of a wrapped ring ends: the address just past its jump.
 * Executing while looking for it would run the tail's methods twice. */
uint32_t nv2a_pb_find_jump(uint32_t start_va, uint32_t end_va)
{
    return pb_walk_impl(start_va, end_va, 1);
}

/* The body of a call: from its target up to the RETURN, through the same
 * decoder (nv2a_pb_scan with s_in_sub set stops at the RETURN). A jump inside
 * a subroutine is followed once, which is how a recorded buffer longer than
 * one segment continues. Returns the words walked. */
static uint32_t pb_walk_sub(uint32_t target_va)
{
    uint32_t before = s_tot_words, j, hops = 0;
    if (!pb_va_ok(target_va))
        return 0;
    j = nv2a_pb_scan(target_va, target_va + 0x400000u);
    while (j && hops++ < 8) {
        uint32_t t = 0x80000000u | (j & 0x0FFFFFFFu);
        if (!pb_va_ok(t)) break;
        j = nv2a_pb_scan(t, t + 0x400000u);
    }
    return s_tot_words - before;
}
