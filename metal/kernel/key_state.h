#ifndef OUTRUN_KEY_STATE_H
#define OUTRUN_KEY_STATE_H
/* Keep the character chosen on make until the corresponding physical break.
 * IRQ producer and desktop consumer run on the BSP; volatile makes the IRQ's
 * changes visible between input passes. Zero never counts as a held key. */
static inline void key_state_update(volatile unsigned char held[128],unsigned char sc,unsigned char ch) {
    held[sc & 127]=(sc & 128)?0:ch;
}
static inline int key_state_held(const volatile unsigned char held[128],int ch) {
    if(!ch) return 0;
    for(int i=0;i<128;i++) if(held[i]==ch) return 1;
    return 0;
}
/* Ctrl+<letter> -> the ASCII control block (1..26), which is the encoding every
 * text application in this tree already decodes: vault_pad reads 19/15/26/25 as
 * save/open/undo/redo, outrun_term routes ^A/^E/^U/^P/^N.
 *
 * It lives HERE, in the header the host test already includes, rather than
 * inline in keyboard_irq(), so the mapping can be asserted on the build host in
 * milliseconds instead of only inside an emulated boot. The IRQ handler calls
 * it; there is one definition and no second copy.
 *
 * `base` must be the UNSHIFTED character: Ctrl+Shift+S is still ^S, and reading
 * the shifted table here would map it through 'S' and then miss every lowercase
 * letter. A chord on a non-letter returns 0, and the caller drops it -- passing
 * Ctrl+1 through as '1' would type into the document of a user reaching for a
 * shortcut. */
static inline unsigned char key_ctrl_char(unsigned char base) {
    if(base>='a' && base<='z') return (unsigned char)(base-'a'+1);
    if(base>='A' && base<='Z') return (unsigned char)(base-'A'+1);
    return 0;
}
#endif
