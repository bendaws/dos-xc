/*
 * DOS-XC /desktop/solitaire.c
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 *
 * SOLITAIRE.EXE: Klondike, drawing one card at a time.  Drag cards to move
 * them; double click sends a card to the foundations.
 */
#include "wm.h"

#define CARD_W 71
#define CARD_H 96
#define GAP 10
#define TOP (MENUBAR_H + 10)
#define MARGIN 12
#define ROW2 (TOP + CARD_H + 16)
#define FACE_DOWN 8
#define FACE_UP 22
#define FELT 0x1d6b3a
#define FELT_DARK 0x14522c

static const struct menubar bar = {{"Game"}, {"New Game\n-\nExit\n"}};
enum { STOCK, WASTE, FOUND0, TABLE0 = FOUND0 + 4, PILES = TABLE0 + 7 };
static signed char piles[PILES][52];
static int counts[PILES], hidden[PILES];
static struct { int active, pile, index, count, grab_x, grab_y, x, y, moved; } drag;
static int won_shown;

static const char *const suit_art[4] = {
    ".XX.XX."
    "XXXXXXX"
    "XXXXXXX"
    "XXXXXXX"
    ".XXXXX."
    "..XXX.."
    "...X...",
    "...X..."
    "..XXX.."
    ".XXXXX."
    "XXXXXXX"
    ".XXXXX."
    "..XXX.."
    "...X...",
    "..XXX.."
    "..XXX.."
    "XX.X.XX"
    "XXXXXXX"
    "XX.X.XX"
    "...X..."
    "..XXX..",
    "...X..."
    "..XXX.."
    ".XXXXX."
    "XXXXXXX"
    "XXXXXXX"
    "..XXX.."
    ".XXXXX.",
};

static void pip(const struct canvas *c, int x, int y, int suit, int scale, u32 color) {
    const char *art = suit_art[suit];
    for (int j = 0; j < 7; ++j)
        for (int i = 0; i < 7; ++i)
            if (art[j * 7 + i] == 'X') fill(c, x + i * scale, y + j * scale, scale, scale, color);
}

static int rank_of(int card) { return card % 13; }
static int suit_of(int card) { return card / 13; }
static int is_red(int card) { return suit_of(card) < 2; }

static void card_face(const struct canvas *c, int x, int y, int card) {
    static const char *const ranks[13] = {"A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K"};
    u32 color = is_red(card) ? 0xc41818 : 0x101010;
    fill(c, x + 1, y, CARD_W - 2, CARD_H, C_BASE);
    fill(c, x, y + 1, CARD_W, CARD_H - 2, C_BASE);
    frame_box(c, x, y, CARD_W, CARD_H, 0x202020);
    text(c, x + 4, y + 4, &font_bold, ranks[rank_of(card)], color);
    pip(c, x + 5, y + 16, suit_of(card), 1, color);
    if (rank_of(card) >= 10) {
        char big[2] = {ranks[rank_of(card)][0], 0};
        frame_box(c, x + 16, y + 28, CARD_W - 32, CARD_H - 44, color);
        text_big(c, x + 28, y + 36, big, color, 3);
    } else pip(c, x + 21, y + 32, suit_of(card), 4, color);
}

static void card_back(const struct canvas *c, int x, int y) {
    fill(c, x + 1, y, CARD_W - 2, CARD_H, C_BASE);
    fill(c, x, y + 1, CARD_W, CARD_H - 2, C_BASE);
    frame_box(c, x, y, CARD_W, CARD_H, 0x202020);
    fill(c, x + 4, y + 4, CARD_W - 8, CARD_H - 8, 0x7a2a12);
    for (int j = 0; j < CARD_H - 8; j += 4)
        for (int i = (j / 4) % 2 ? 2 : 0; i < CARD_W - 8; i += 4)
            fill(c, x + 4 + i, y + 4 + j, 2, 2, 0xe0a070);
}

static void empty_slot(const struct canvas *c, int x, int y) { frame_box(c, x, y, CARD_W, CARD_H, FELT_DARK); }

static int pile_x(int pile) {
    if (pile == STOCK) return MARGIN;
    if (pile == WASTE) return MARGIN + CARD_W + GAP;
    if (pile < TABLE0) return MARGIN + (3 + pile - FOUND0) * (CARD_W + GAP);
    return MARGIN + (pile - TABLE0) * (CARD_W + GAP);
}

/* Screen y of card INDEX of tableau pile P. */
static int card_y(int p, int index) {
    int y = ROW2;
    for (int i = 0; i < index; ++i) y += i < hidden[p] ? FACE_DOWN : FACE_UP;
    return y;
}

static void deal(void) {
    memset(counts, 0, sizeof(counts));
    memset(hidden, 0, sizeof(hidden));
    int deck[52];
    for (int i = 0; i < 52; ++i) deck[i] = i;
    srand((unsigned)dosxc_sys_nanoseconds());
    for (int i = 51; i > 0; --i) { int k = rand() % (i + 1), t = deck[i]; deck[i] = deck[k]; deck[k] = t; }
    int at = 0;
    for (int p = 0; p < 7; ++p) {
        for (int n = 0; n <= p; ++n) piles[TABLE0 + p][n] = (signed char)deck[at++];
        counts[TABLE0 + p] = p + 1;
        hidden[TABLE0 + p] = p;
    }
    while (at < 52) piles[STOCK][counts[STOCK]++] = (signed char)deck[at++];
    drag.active = 0;
    won_shown = 0;
}

static void flip_top(int p) { if (p >= TABLE0 && counts[p] && hidden[p] == counts[p]) --hidden[p]; }

static int can_found(int card, int pile) {
    int n = counts[pile];
    if (!n) return rank_of(card) == 0;
    int top = piles[pile][n - 1];
    return suit_of(top) == suit_of(card) && rank_of(card) == rank_of(top) + 1;
}

static int can_table(int card, int pile) {
    int n = counts[pile];
    if (!n) return rank_of(card) == 12;
    int top = piles[pile][n - 1];
    return is_red(top) != is_red(card) && rank_of(top) == rank_of(card) + 1;
}

static void move_cards(int from, int index, int to) {
    int count = counts[from] - index;
    memcpy(&piles[to][counts[to]], &piles[from][index], (size_t)count);
    counts[to] += count;
    counts[from] = index;
    flip_top(from);
}

static void check_won(void) {
    int total = 0;
    for (int f = FOUND0; f < TABLE0; ++f) total += counts[f];
    if (total == 52 && !won_shown) {
        won_shown = 1;
        message_box("Solitaire", "You won!", "Start a new game from the Game menu.");
    }
}

static void paint_pile_cards(const struct canvas *c, int p, int upto) {
    for (int i = 0; i < upto; ++i) {
        int y = card_y(p, i);
        if (i < hidden[p]) card_back(c, pile_x(p), y);
        else card_face(c, pile_x(p), y, piles[p][i]);
    }
}

static void sol_paint(struct window *w, const struct canvas *c) {
    fill(c, 0, 0, w->content.w, w->content.h, FELT);
    menubar_paint(c, &bar, w->content.w);
    if (counts[STOCK]) card_back(c, pile_x(STOCK), TOP);
    else empty_slot(c, pile_x(STOCK), TOP);
    int waste_top = counts[WASTE] - (drag.active && drag.pile == WASTE ? 1 : 0);
    if (waste_top > 0) card_face(c, pile_x(WASTE), TOP, piles[WASTE][waste_top - 1]);
    else empty_slot(c, pile_x(WASTE), TOP);
    for (int f = FOUND0; f < TABLE0; ++f) {
        int n = counts[f] - (drag.active && drag.pile == f ? 1 : 0);
        if (n > 0) card_face(c, pile_x(f), TOP, piles[f][n - 1]);
        else {
            empty_slot(c, pile_x(f), TOP);
            pip(c, pile_x(f) + 24, TOP + 34, f - FOUND0, 3, FELT_DARK);
        }
    }
    for (int t = TABLE0; t < PILES; ++t) {
        int n = counts[t];
        if (drag.active && drag.pile == t) n = drag.index;
        if (!n && !(drag.active && drag.pile == t && counts[t])) empty_slot(c, pile_x(t), ROW2);
        paint_pile_cards(c, t, n);
    }
    if (drag.active)
        for (int i = 0; i < drag.count; ++i)
            card_face(c, drag.x, drag.y + i * FACE_UP, piles[drag.pile][drag.index + i]);
}

/* The pile and card under a point; -1 when none. */
static int hit_card(int x, int y, int *index) {
    if (x >= pile_x(STOCK) && x < pile_x(STOCK) + CARD_W && y >= TOP && y < TOP + CARD_H) { *index = -1; return STOCK; }
    if (x >= pile_x(WASTE) && x < pile_x(WASTE) + CARD_W && y >= TOP && y < TOP + CARD_H && counts[WASTE]) {
        *index = counts[WASTE] - 1;
        return WASTE;
    }
    for (int f = FOUND0; f < TABLE0; ++f)
        if (x >= pile_x(f) && x < pile_x(f) + CARD_W && y >= TOP && y < TOP + CARD_H && counts[f]) {
            *index = counts[f] - 1;
            return f;
        }
    for (int t = TABLE0; t < PILES; ++t) {
        if (x < pile_x(t) || x >= pile_x(t) + CARD_W) continue;
        for (int i = counts[t] - 1; i >= hidden[t]; --i) {
            int top = card_y(t, i), bottom = i + 1 < counts[t] ? card_y(t, i + 1) : top + CARD_H;
            if (y >= top && y < bottom) { *index = i; return t; }
        }
    }
    return -1;
}

static void to_foundation(int pile, struct window *w) {
    if (!counts[pile]) return;
    int card = piles[pile][counts[pile] - 1];
    for (int f = FOUND0; f < TABLE0; ++f)
        if (can_found(card, f)) {
            piles[f][counts[f]++] = (signed char)card;
            --counts[pile];
            flip_top(pile);
            check_won();
            window_paint(w);
            return;
        }
}

static void sol_mouse(struct window *w, int x, int y, int kind, int value) {
    (void)value;
    if (menubar_mouse(w, &bar, x, y, kind)) return;
    if (kind == M_DOWN || kind == M_DOUBLE) {
        int index = 0, p = hit_card(x, y, &index);
        if (p == STOCK) {
            if (counts[STOCK]) {
                piles[WASTE][counts[WASTE]++] = piles[STOCK][--counts[STOCK]];
            } else {
                while (counts[WASTE]) piles[STOCK][counts[STOCK]++] = piles[WASTE][--counts[WASTE]];
            }
            window_paint(w);
            return;
        }
        if (p < 0) return;
        if (kind == M_DOUBLE && (p == WASTE || (p >= TABLE0 && index == counts[p] - 1))) { to_foundation(p, w); return; }
        drag.active = 1;
        drag.pile = p;
        drag.index = index;
        drag.count = counts[p] - index;
        int ox = pile_x(p), oy = p >= TABLE0 ? card_y(p, index) : TOP;
        drag.grab_x = x - ox;
        drag.grab_y = y - oy;
        drag.x = ox;
        drag.y = oy;
        window_paint(w);
    } else if (kind == M_MOVE && drag.active) {
        drag.x = x - drag.grab_x;
        drag.y = y - drag.grab_y;
        window_paint(w);
    } else if (kind == M_UP && drag.active) {
        int card = piles[drag.pile][drag.index];
        int best = -1;
        for (int t = TABLE0; t < PILES; ++t) {
            if (t == drag.pile || !can_table(card, t)) continue;
            int cx = pile_x(t), cy = counts[t] ? card_y(t, counts[t] - 1) : ROW2;
            if (drag.x + CARD_W > cx && drag.x < cx + CARD_W && drag.y + CARD_H > cy && drag.y < cy + CARD_H) best = t;
        }
        if (best < 0 && drag.count == 1)
            for (int f = FOUND0; f < TABLE0; ++f)
                if (f != drag.pile && can_found(card, f) && drag.x + CARD_W > pile_x(f) && drag.x < pile_x(f) + CARD_W && drag.y < TOP + CARD_H) best = f;
        int from = drag.pile, index = drag.index;
        drag.active = 0;
        if (best >= 0) move_cards(from, index, best);
        check_won();
        window_paint(w);
    }
}

static void sol_popup(struct window *w, int tag, int index) {
    (void)tag;
    if (index == 0) { deal(); window_paint(w); }
    else if (index == 2) window_close(w);
}

static const struct app_ops sol_ops = {sol_paint, sol_mouse, 0, 0, sol_popup};

int main(void) {
    if (gui_connect()) return 1;
    deal();
    window_open(MARGIN * 2 + 7 * CARD_W + 6 * GAP, ROW2 + 6 * FACE_DOWN + 12 * FACE_UP + CARD_H + 12, "Solitaire", ICON_CARDS, &sol_ops, 0);
    gui_run();
    return 0;
}
