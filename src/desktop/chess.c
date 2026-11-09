/*
 * DOS-XC /desktop/chess.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * CHESS.EXE: chess against the computer: all the rules, and an
 * alpha-beta search with a time limit.
 */
#include "wm.h"

#define SQ 60
#define BOARD_X 12
#define BOARD_Y (MENUBAR_H + 12)
#define PANEL_X (BOARD_X + 8 * SQ + 14)
#define WINDOW_W (PANEL_X + 180)
#define WINDOW_H (BOARD_Y + 8 * SQ + 16)

enum { EMPTY, PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING };
#define WHITE 1
#define BLACK (-1)
#define INF 1000000
#define MATE 900000

struct position {
    signed char b[64];
    int side, ep, castle, halfmove;     /* castle: 1 K, 2 Q, 4 k, 8 q */
};
struct move { signed char from, to, promotion, flag; };   /* flag: 1 double push, 2 en passant, 3 castle */
struct list { int n; struct move m[256]; };

static const struct menubar bar = {
    {"Game", "Level"},
    {"New Game as White\nNew Game as Black\nUndo Move\n-\nExit\n", "Easy\nNormal\nHard\n"},
};

static struct position pos;
static struct position undo_stack[600];
static int undo_count;
static int human = WHITE, level = 1, game_over, thinking_now;
static int selected = -1, last_from = -1, last_to = -1;
static struct list legal;
static char status[64] = "White to move.";
static struct move pending_promotion;
static long long think_deadline;
static int search_aborted;

static int file_of(int s) { return s & 7; }
static int rank_of(int s) { return s >> 3; }
static int on_board(int f, int r) { return f >= 0 && f < 8 && r >= 0 && r < 8; }

static void init_position(void) {
    static const signed char back[8] = {ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK};
    memset(&pos, 0, sizeof(pos));
    for (int f = 0; f < 8; ++f) {
        pos.b[f] = (signed char)-back[f];
        pos.b[f + 8] = -PAWN;
        pos.b[f + 48] = PAWN;
        pos.b[f + 56] = back[f];
    }
    pos.side = WHITE;
    pos.ep = -1;
    pos.castle = 15;
}

static int attacked(const struct position *p, int s, int by) {
    static const int knight[8][2] = {{1, 2}, {2, 1}, {-1, 2}, {-2, 1}, {1, -2}, {2, -1}, {-1, -2}, {-2, -1}};
    static const int rays[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
    int f = file_of(s), r = rank_of(s);
    for (int i = 0; i < 8; ++i) {
        int nf = f + knight[i][0], nr = r + knight[i][1];
        if (on_board(nf, nr) && p->b[nr * 8 + nf] == by * KNIGHT) return 1;
    }
    int pr = r + by;                    /* white pawns attack toward lower ranks */
    if (on_board(f - 1, pr) && p->b[pr * 8 + f - 1] == by * PAWN) return 1;
    if (on_board(f + 1, pr) && p->b[pr * 8 + f + 1] == by * PAWN) return 1;
    for (int d = 0; d < 8; ++d) {
        int nf = f + rays[d][0], nr = r + rays[d][1], step = 1;
        while (on_board(nf, nr)) {
            int piece = p->b[nr * 8 + nf];
            if (piece) {
                int kind = piece * by;
                if (kind > 0) {
                    if (kind == QUEEN || (d < 4 && kind == ROOK) || (d >= 4 && kind == BISHOP) || (step == 1 && kind == KING)) return 1;
                }
                break;
            }
            nf += rays[d][0];
            nr += rays[d][1];
            ++step;
        }
    }
    return 0;
}

static int king_square(const struct position *p, int side) {
    for (int s = 0; s < 64; ++s) if (p->b[s] == side * KING) return s;
    return -1;
}

static void add(struct list *l, int from, int to, int promotion, int flag) {
    l->m[l->n++] = (struct move){(signed char)from, (signed char)to, (signed char)promotion, (signed char)flag};
}

static void pawn_moves(const struct position *p, struct list *l, int s, int side) {
    int f = file_of(s), r = rank_of(s), dir = side == WHITE ? -1 : 1, start = side == WHITE ? 6 : 1, last = side == WHITE ? 0 : 7;
    int nr = r + dir;
    if (!on_board(f, nr)) return;
    if (!p->b[nr * 8 + f]) {
        if (nr == last) for (int q = QUEEN; q >= KNIGHT; --q) add(l, s, nr * 8 + f, q, 0);
        else add(l, s, nr * 8 + f, 0, 0);
        if (r == start && !p->b[(nr + dir) * 8 + f]) add(l, s, (nr + dir) * 8 + f, 0, 1);
    }
    for (int df = -1; df <= 1; df += 2) {
        if (!on_board(f + df, nr)) continue;
        int t = nr * 8 + f + df, victim = p->b[t];
        if (victim * side < 0) {
            if (nr == last) for (int q = QUEEN; q >= KNIGHT; --q) add(l, s, t, q, 0);
            else add(l, s, t, 0, 0);
        } else if (t == p->ep) add(l, s, t, 0, 2);
    }
}

static void piece_moves(const struct position *p, struct list *l, int s, int side, int kind) {
    static const int knight[8][2] = {{1, 2}, {2, 1}, {-1, 2}, {-2, 1}, {1, -2}, {2, -1}, {-1, -2}, {-2, -1}};
    static const int rays[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
    int f = file_of(s), r = rank_of(s);
    if (kind == KNIGHT) {
        for (int i = 0; i < 8; ++i) {
            int nf = f + knight[i][0], nr = r + knight[i][1];
            if (on_board(nf, nr) && p->b[nr * 8 + nf] * side <= 0) add(l, s, nr * 8 + nf, 0, 0);
        }
        return;
    }
    int from_ray = kind == BISHOP ? 4 : 0, to_ray = kind == ROOK ? 4 : 8;
    for (int d = from_ray; d < to_ray; ++d) {
        int nf = f + rays[d][0], nr = r + rays[d][1];
        while (on_board(nf, nr)) {
            int v = p->b[nr * 8 + nf];
            if (v * side > 0) break;
            add(l, s, nr * 8 + nf, 0, 0);
            if (v || kind == KING) break;
            nf += rays[d][0];
            nr += rays[d][1];
        }
    }
}

static void generate(const struct position *p, struct list *l) {
    l->n = 0;
    int side = p->side;
    for (int s = 0; s < 64; ++s) {
        int v = p->b[s] * side;
        if (v <= 0) continue;
        if (v == PAWN) pawn_moves(p, l, s, side);
        else piece_moves(p, l, s, side, v);
    }
    int rank = side == WHITE ? 7 : 0, k = side == WHITE ? 1 : 4, q = side == WHITE ? 2 : 8;
    int king = rank * 8 + 4;
    if (p->b[king] == side * KING && !attacked(p, king, -side)) {
        if ((p->castle & k) && !p->b[king + 1] && !p->b[king + 2] && p->b[king + 3] == side * ROOK &&
            !attacked(p, king + 1, -side) && !attacked(p, king + 2, -side)) add(l, king, king + 2, 0, 3);
        if ((p->castle & q) && !p->b[king - 1] && !p->b[king - 2] && !p->b[king - 3] && p->b[king - 4] == side * ROOK &&
            !attacked(p, king - 1, -side) && !attacked(p, king - 2, -side)) add(l, king, king - 2, 0, 3);
    }
}

static void make_move(struct position *p, struct move m) {
    int piece = p->b[m.from], side = p->side, captured = p->b[m.to];
    p->ep = -1;
    p->halfmove = abs(piece) == PAWN || captured ? 0 : p->halfmove + 1;
    p->b[m.from] = 0;
    p->b[m.to] = (signed char)(m.promotion ? side * m.promotion : piece);
    if (m.flag == 1) p->ep = (m.from + m.to) / 2;
    else if (m.flag == 2) p->b[m.to + (side == WHITE ? 8 : -8)] = 0;
    else if (m.flag == 3) {
        int kingside = m.to > m.from, rook_from = kingside ? m.to + 1 : m.to - 2, rook_to = kingside ? m.to - 1 : m.to + 1;
        p->b[rook_to] = p->b[rook_from];
        p->b[rook_from] = 0;
    }
    if (m.from == 60 || m.to == 60) p->castle &= ~3;
    if (m.from == 4 || m.to == 4) p->castle &= ~12;
    if (m.from == 63 || m.to == 63) p->castle &= ~1;
    if (m.from == 56 || m.to == 56) p->castle &= ~2;
    if (m.from == 7 || m.to == 7) p->castle &= ~4;
    if (m.from == 0 || m.to == 0) p->castle &= ~8;
    p->side = -side;
}

static int leaves_check(const struct position *p, struct move m) {
    struct position n = *p;
    int side = n.side;
    make_move(&n, m);
    return attacked(&n, king_square(&n, side), -side);
}

static void legal_moves(const struct position *p, struct list *out) {
    struct list all;
    generate(p, &all);
    out->n = 0;
    for (int i = 0; i < all.n; ++i)
        if (!leaves_check(p, all.m[i])) out->m[out->n++] = all.m[i];
}

/* ---- evaluation and search ---------------------------------------------- */

static const int value_of[7] = {0, 100, 320, 330, 500, 900, 20000};

static int center_bonus(int s) {
    int f = file_of(s), r = rank_of(s);
    int cf = f < 4 ? f : 7 - f, cr = r < 4 ? r : 7 - r;
    return cf + cr;
}

static int evaluate(const struct position *p) {
    int score = 0;
    for (int s = 0; s < 64; ++s) {
        int v = p->b[s];
        if (!v) continue;
        int side = v > 0 ? 1 : -1, kind = abs(v);
        int bonus = value_of[kind];
        if (kind == PAWN) bonus += (side == WHITE ? 6 - rank_of(s) : rank_of(s) - 1) * 6;
        else if (kind == KNIGHT || kind == BISHOP) bonus += center_bonus(s) * 5;
        else if (kind == KING) bonus -= center_bonus(s) * 4;
        score += side * bonus;
    }
    return score * p->side;
}

static long long nodes;
static int out_of_time(void) {
    if ((++nodes & 1023) == 0 && (long long)dosxc_sys_nanoseconds() > think_deadline) search_aborted = 1;
    return search_aborted;
}

static int move_order_score(const struct position *p, struct move m) {
    int score = 0, victim = abs(p->b[m.to]);
    if (victim) score += 10 * value_of[victim] - value_of[abs(p->b[m.from])];
    if (m.promotion) score += value_of[(int)m.promotion];
    return score;
}

static void sort_moves(const struct position *p, struct list *l) {
    for (int i = 1; i < l->n; ++i) {
        struct move m = l->m[i];
        int score = move_order_score(p, m), j = i - 1;
        while (j >= 0 && move_order_score(p, l->m[j]) < score) { l->m[j + 1] = l->m[j]; --j; }
        l->m[j + 1] = m;
    }
}

static int quiesce(const struct position *p, int alpha, int beta, int depth) {
    if (out_of_time()) return 0;
    int stand = evaluate(p);
    if (stand >= beta || depth <= 0) return stand;
    if (stand > alpha) alpha = stand;
    struct list l;
    generate(p, &l);
    sort_moves(p, &l);
    for (int i = 0; i < l.n; ++i) {
        if (!p->b[l.m[i].to] && !l.m[i].promotion) continue;
        if (leaves_check(p, l.m[i])) continue;
        struct position n = *p;
        make_move(&n, l.m[i]);
        int score = -quiesce(&n, -beta, -alpha, depth - 1);
        if (search_aborted) return 0;
        if (score >= beta) return score;
        if (score > alpha) alpha = score;
    }
    return alpha;
}

static int negamax(const struct position *p, int depth, int alpha, int beta, int ply) {
    if (out_of_time()) return 0;
    if (depth <= 0) return quiesce(p, alpha, beta, 4);
    struct list l;
    generate(p, &l);
    sort_moves(p, &l);
    int moved = 0, side = p->side;
    for (int i = 0; i < l.n; ++i) {
        if (leaves_check(p, l.m[i])) continue;
        moved = 1;
        struct position n = *p;
        make_move(&n, l.m[i]);
        int score = -negamax(&n, depth - 1, -beta, -alpha, ply + 1);
        if (search_aborted) return 0;
        if (score >= beta) return score;
        if (score > alpha) alpha = score;
    }
    if (!moved) return attacked(p, king_square(p, side), -side) ? -MATE + ply : 0;
    return alpha;
}

static struct move search_best(void) {
    static const int limits_ms[3] = {300, 1200, 3000};
    static const int depths[3] = {2, 4, 6};
    think_deadline = (long long)dosxc_sys_nanoseconds() + limits_ms[level] * 1000000ll;
    search_aborted = 0;
    nodes = 0;
    struct list root;
    legal_moves(&pos, &root);
    sort_moves(&pos, &root);
    struct move best = root.m[0];
    for (int depth = 1; depth <= depths[level]; ++depth) {
        struct move this_best = root.m[0];
        int alpha = -INF;
        for (int i = 0; i < root.n; ++i) {
            struct position n = pos;
            make_move(&n, root.m[i]);
            int score = -negamax(&n, depth - 1, -INF, -alpha, 1);
            if (search_aborted) break;
            if (score > alpha) { alpha = score; this_best = root.m[i]; }
        }
        if (search_aborted) break;
        best = this_best;
    }
    return best;
}

/* ---- the game ------------------------------------------------------------ */

static int insufficient_material(void) {
    int minor = 0;
    for (int s = 0; s < 64; ++s) {
        int k = abs(pos.b[s]);
        if (k == PAWN || k == ROOK || k == QUEEN) return 0;
        if (k == KNIGHT || k == BISHOP) ++minor;
    }
    return minor <= 1;
}

static void refresh_game(void) {
    legal_moves(&pos, &legal);
    const char *mover = pos.side == WHITE ? "White" : "Black";
    int check = attacked(&pos, king_square(&pos, pos.side), -pos.side);
    game_over = 1;
    if (!legal.n && check) snprintf(status, sizeof(status), "Checkmate. %s wins.", pos.side == WHITE ? "Black" : "White");
    else if (!legal.n) snprintf(status, sizeof(status), "Stalemate.");
    else if (pos.halfmove >= 100) snprintf(status, sizeof(status), "Draw by the 50-move rule.");
    else if (insufficient_material()) snprintf(status, sizeof(status), "Draw: not enough pieces.");
    else {
        game_over = 0;
        snprintf(status, sizeof(status), "%s to move%s", mover, check ? ": check!" : ".");
    }
}

static void new_game(int as) {
    human = as;
    init_position();
    undo_count = 0;
    selected = last_from = last_to = -1;
    refresh_game();
}

static void play(struct move m) {
    if (undo_count < 600) undo_stack[undo_count++] = pos;
    last_from = m.from;
    last_to = m.to;
    make_move(&pos, m);
    selected = -1;
    refresh_game();
}

static void undo(void) {
    if (undo_count < 2) { if (undo_count) pos = undo_stack[--undo_count]; }
    else { undo_count -= 2; pos = undo_stack[undo_count]; }
    selected = last_from = last_to = -1;
    refresh_game();
}

/* The computer moves on the timer tick, so the board is on screen first. */
static void chess_tick(struct window *w) {
    if (game_over || pos.side == human) {
        if (thinking_now) { thinking_now = 0; window_paint(w); }
        return;
    }
    if (!thinking_now) { thinking_now = 1; window_paint(w); return; }
    struct move m = search_best();
    thinking_now = 0;
    play(m);
    window_paint(w);
}

/* ---- drawing the pieces: outlined shapes, drawn twice ------------------- */

static void disk(const struct canvas *c, int cx, int cy, int r, u32 color) {
    for (int dy = -r; dy <= r; ++dy) {
        int span = 0;
        while (span * span + dy * dy <= r * r) ++span;
        if (span) fill(c, cx - span + 1, cy + dy, 2 * span - 1, 1, color);
    }
}

/* A shape widening from half-width W0 at Y0 to W1 at Y1. */
static void slab(const struct canvas *c, int cx, int y0, int y1, int w0, int w1, u32 color) {
    for (int y = y0; y <= y1; ++y) {
        int w = y1 == y0 ? w0 : w0 + (w1 - w0) * (y - y0) / (y1 - y0);
        fill(c, cx - w, y, 2 * w + 1, 1, color);
    }
}

static void piece_shape(const struct canvas *c, int kind, int cx, int base, int grow, u32 color) {
    slab(c, cx, base - 6 - grow, base + grow, 17 + grow, 17 + grow, color);    /* foot */
    slab(c, cx, base - 18, base - 6, 7 + grow, 12 + grow, color);
    switch (kind) {
    case PAWN:
        disk(c, cx, base - 26, 9 + grow, color);
        break;
    case ROOK:
        slab(c, cx, base - 36 - grow, base - 18, 11 + grow, 11 + grow, color);
        slab(c, cx, base - 44 - grow, base - 34 + grow, 15 + grow, 15 + grow, color);
        break;
    case KNIGHT:
        slab(c, cx - 2, base - 34, base - 18, 8 + grow, 10 + grow, color);
        slab(c, cx + 2, base - 42 - grow, base - 32 + grow, 5 + grow, 13 + grow, color);
        disk(c, cx - 6, base - 42, 7 + grow, color);
        slab(c, cx - 6, base - 50 - grow, base - 44, 2 + grow, 4 + grow, color);
        break;
    case BISHOP:
        disk(c, cx, base - 30, 11 + grow, color);
        disk(c, cx, base - 42, 4 + grow, color);
        break;
    case QUEEN:
        slab(c, cx, base - 34, base - 18, 8 + grow, 9 + grow, color);
        slab(c, cx, base - 42 - grow, base - 32 + grow, 16 + grow, 12 + grow, color);
        for (int i = -2; i <= 2; ++i) disk(c, cx + i * 7, base - 44, 3 + grow, color);
        break;
    default:                            /* king */
        slab(c, cx, base - 34, base - 18, 8 + grow, 9 + grow, color);
        slab(c, cx, base - 40 - grow, base - 32 + grow, 13 + grow, 11 + grow, color);
        fill(c, cx - 2 - grow, base - 52 - grow, 5 + 2 * grow, 14 + 2 * grow, color);
        fill(c, cx - 7 - grow, base - 48 - grow, 15 + 2 * grow, 5 + 2 * grow, color);
        break;
    }
}

static void draw_piece(const struct canvas *c, int x, int y, int piece) {
    int cx = x + SQ / 2, base = y + SQ - 8;
    u32 fill_color = piece > 0 ? 0xf0ece0 : 0x383838;
    piece_shape(c, abs(piece), cx, base, 1, 0x101010);
    piece_shape(c, abs(piece), cx, base, 0, fill_color);
}

static int screen_square(int s) { return human == WHITE ? s : 63 - s; }
static int square_at(int x, int y) {
    if (x < BOARD_X || y < BOARD_Y || x >= BOARD_X + 8 * SQ || y >= BOARD_Y + 8 * SQ) return -1;
    int s = (y - BOARD_Y) / SQ * 8 + (x - BOARD_X) / SQ;
    return human == WHITE ? s : 63 - s;
}

static void chess_paint(struct window *w, const struct canvas *c) {
    menubar_paint(c, &bar, w->content.w);
    frame_box(c, BOARD_X - 2, BOARD_Y - 2, 8 * SQ + 4, 8 * SQ + 4, C_SHADOW2);
    for (int s = 0; s < 64; ++s) {
        int view = screen_square(s), x = BOARD_X + (view & 7) * SQ, y = BOARD_Y + (view >> 3) * SQ;
        u32 color = (((view >> 3) + (view & 7)) & 1) ? 0xb58863 : 0xf0d9b5;
        if (s == last_from || s == last_to) color = (((view >> 3) + (view & 7)) & 1) ? 0xbaa24a : 0xd8c770;
        if (s == selected) color = 0x7fa650;
        fill(c, x, y, SQ, SQ, color);
    }
    for (int i = 0; i < legal.n; ++i)
        if (legal.m[i].from == selected) {
            int view = screen_square(legal.m[i].to);
            disk(c, BOARD_X + (view & 7) * SQ + SQ / 2, BOARD_Y + (view >> 3) * SQ + SQ / 2, 6, 0x5a8040);
        }
    for (int s = 0; s < 64; ++s)
        if (pos.b[s]) {
            int view = screen_square(s);
            draw_piece(c, BOARD_X + (view & 7) * SQ, BOARD_Y + (view >> 3) * SQ, pos.b[s]);
        }
    text(c, PANEL_X, BOARD_Y, &font_bold, "Chess", C_TEXT);
    text(c, PANEL_X, BOARD_Y + 22, &font_regular, status, C_TEXT);
    if (thinking_now) text(c, PANEL_X, BOARD_Y + 42, &font_regular, "Thinking...", C_TEXT);
    char line[40];
    static const char *const names[3] = {"Easy", "Normal", "Hard"};
    snprintf(line, sizeof(line), "Level: %s", names[level]);
    text(c, PANEL_X, BOARD_Y + 66, &font_regular, line, C_TEXT);
    snprintf(line, sizeof(line), "You play %s.", human == WHITE ? "White" : "Black");
    text(c, PANEL_X, BOARD_Y + 82, &font_regular, line, C_TEXT);
}

static void chess_mouse(struct window *w, int x, int y, int kind, int value) {
    (void)value;
    if (menubar_mouse(w, &bar, x, y, kind)) return;
    if ((kind != M_DOWN && kind != M_DOUBLE) || game_over || pos.side != human) return;
    int s = square_at(x, y);
    if (s < 0) { selected = -1; window_paint(w); return; }
    if (selected >= 0) {
        for (int i = 0; i < legal.n; ++i) {
            struct move m = legal.m[i];
            if (m.from != selected || m.to != s) continue;
            if (m.promotion) {
                pending_promotion = m;
                window_popup(w, x, y, "Queen\nRook\nBishop\nKnight\n", 9);
                return;
            }
            play(m);
            window_paint(w);
            return;
        }
    }
    selected = pos.b[s] * human > 0 ? s : -1;
    window_paint(w);
}

static void chess_popup(struct window *w, int tag, int index) {
    if (index < 0) return;
    if (tag == 9) {
        static const int pieces[4] = {QUEEN, ROOK, BISHOP, KNIGHT};
        struct move m = pending_promotion;
        m.promotion = (signed char)pieces[index];
        play(m);
    } else if (tag == 0) {
        if (index == 0) new_game(WHITE);
        else if (index == 1) new_game(BLACK);
        else if (index == 2) undo();
        else if (index == 4) { window_close(w); return; }
    } else if (tag == 1) level = index;
    window_paint(w);
}

static const struct app_ops chess_ops = {chess_paint, chess_mouse, 0, 0, chess_popup, chess_tick};

int main(void) {
    if (gui_connect()) return 1;
    new_game(WHITE);
    window_open(WINDOW_W, WINDOW_H, "Chess", ICON_CHESS, &chess_ops, 0);
    gui_run();
    return 0;
}
