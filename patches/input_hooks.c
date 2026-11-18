#include "patches.h"
#include "bm64_debug.h"

typedef struct InputHistoryEntry {
    /* 0x0 */ u16 held;
    /* 0x2 */ u16 pressed;
    /* 0x4 */ s8 stick_x;
    /* 0x5 */ s8 stick_y;
} InputHistoryEntry; // size = 0x6

#define INPUT_HISTORY_LEN 16

extern OSMesgQueue D_802AC3E0;
extern OSContPad D_802AC410[MAXCONTROLLERS];
extern InputHistoryEntry D_802AC428[MAXCONTROLLERS][INPUT_HISTORY_LEN];
extern s32 D_802AC5A8[MAXCONTROLLERS];

// Reads all controller ports and appends them to the per-port input history.
// The host filter sees the raw pads first so it can record them or substitute replayed/remote input.
RECOMP_PATCH void func_8023A104(void) {
    s32 i;

    osRecvMesg(&D_802AC3E0, NULL, OS_MESG_BLOCK);

    for (i = 0; i < MAXCONTROLLERS; i++) {
        D_802AC410[i].button = 0;
        D_802AC410[i].stick_x = 0;
        D_802AC410[i].stick_y = 0;
        D_802AC410[i].errno = 0;
    }

    osContGetReadData(D_802AC410);
    bm64_input_filter(D_802AC410);

    for (i = 0; i < MAXCONTROLLERS; i++) {
        s32 prev_index = D_802AC5A8[i];
        s32 next_index = prev_index + 1;
        InputHistoryEntry* prev;
        InputHistoryEntry* next;
        u16 button = D_802AC410[i].button;

        if (next_index >= INPUT_HISTORY_LEN) {
            next_index = 0;
        }
        D_802AC5A8[i] = next_index;

        prev = &D_802AC428[i][prev_index];
        next = &D_802AC428[i][next_index];
        next->held = button;
        next->stick_x = D_802AC410[i].stick_x;
        next->stick_y = D_802AC410[i].stick_y;
        next->pressed = (button ^ prev->held) & button;
    }
}
