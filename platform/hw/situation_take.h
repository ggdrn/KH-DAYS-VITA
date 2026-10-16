#ifndef KH_HW_SITUATION_TAKE_H
#define KH_HW_SITUATION_TAKE_H

#include "ui/situation.h"

/* The game's thread, for each object update it runs (Obj_UpdateAll, decomp patch): marks the
 * watched ones (KH_UPD_*). */
void kh_situation_update_ran(int fn);

/* The display thread: this frame's inputs. movie: a MobiClip movie plays; dual: the game shows
 * its 3D on both screens. */
void kh_situation_take(KhSituationInputs *in, int movie, int dual);

#endif
