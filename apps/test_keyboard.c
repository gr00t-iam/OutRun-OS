#include <assert.h>
#include <stdio.h>
#include "../metal/kernel/key_state.h"
int main(void) {
    volatile unsigned char held[128]={0};
    key_state_update(held,0x03,'2'); assert(key_state_held(held,'2'));
    key_state_update(held,0x83,0); assert(!key_state_held(held,'2'));
    /* Release uses physical scancode even if Shift changed after make. */
    key_state_update(held,0x1e,'A'); assert(key_state_held(held,'A'));
    key_state_update(held,0x9e,0); assert(!key_state_held(held,'A'));
    assert(!key_state_held(held,0));
    /* Ctrl chords. These constants are not arbitrary: they are the codes the
     * applications ALREADY decode, and which nothing could produce until the
     * PS/2 handler learned to track Ctrl. Asserting the exact values is what
     * ties the driver to vault_pad's save/open/undo and the terminal's line
     * editing -- a mapping that drifted would leave both silently inert. */
    assert(key_ctrl_char('s')==19);   /* Ctrl+S -> vault_pad SAVE  */
    assert(key_ctrl_char('o')==15);   /* Ctrl+O -> vault_pad OPEN  */
    assert(key_ctrl_char('z')==26);   /* Ctrl+Z -> undo            */
    assert(key_ctrl_char('y')==25);   /* Ctrl+Y -> redo            */
    assert(key_ctrl_char('a')==1 && key_ctrl_char('e')==5);   /* term line edit */
    /* Shift must not change a chord: Ctrl+Shift+S is still ^S. */
    assert(key_ctrl_char('S')==19);
    /* A chord on a non-letter yields nothing, so the caller drops it rather
     * than typing the bare character into the focused document. */
    assert(key_ctrl_char('1')==0 && key_ctrl_char(' ')==0 && key_ctrl_char(0)==0);
    puts("keyboard: make/break, shifted-key release and Ctrl chords PASS");
}
