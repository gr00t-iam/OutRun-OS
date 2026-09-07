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
#endif
