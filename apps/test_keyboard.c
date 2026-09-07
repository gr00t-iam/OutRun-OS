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
    puts("keyboard: make/break and shifted-key release PASS");
}
