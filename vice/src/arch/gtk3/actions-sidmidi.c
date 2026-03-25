/** \file   actions-sidmidi.c
 * \brief   UI action implementations for SID MIDI recording
 *
 * Provides a toggle action (ACTION_SID_MIDI_RECORD_TOGGLE) that starts or
 * stops recording SID register writes as a Standard MIDI File.  The output
 * file is named with a timestamp: "vice-sid-YYYYMMDD-HHMMSS.mid".
 *
 * \author  VICE contributors
 */

/*
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA.
 */

#include "vice.h"

#include <stddef.h>
#include <stdio.h>
#include <time.h>

#include "sound.h"
#include "uiactions.h"
#include "log.h"

#include "actions-sidmidi.h"


/** \brief  Toggle SID MIDI recording on/off
 *
 * If recording is currently active it is stopped and the MIDI file is
 * written to disk.  Otherwise a new recording session is started and the
 * output file is named "vice-sid-YYYYMMDD-HHMMSS.mid".
 *
 * \param[in]   self    action map (unused)
 */
static void sid_midi_record_toggle_action(ui_action_map_t *self)
{
    if (sid_midi_is_recording()) {
        sid_midi_record_stop();
    } else {
        char     filename[64];
        time_t   t  = time(NULL);
        struct tm *tm_info = localtime(&t);

        if (tm_info) {
            strftime(filename, sizeof(filename),
                     "vice-sid-%Y%m%d-%H%M%S.mid", tm_info);
        } else {
            /* Fallback if localtime() fails */
            snprintf(filename, sizeof(filename), "vice-sid.mid");
        }

        if (sid_midi_record_start(filename) < 0) {
            log_message(LOG_DEFAULT,
                        "sid-midi: ERROR failed to start MIDI recording");
        }
    }

    ui_action_finish(ACTION_SID_MIDI_RECORD_TOGGLE);
}


/** \brief  List of SID MIDI recording actions */
static const ui_action_map_t sidmidi_actions[] = {
    {   .action   = ACTION_SID_MIDI_RECORD_TOGGLE,
        .handler  = sid_midi_record_toggle_action,
        .uithread = true
    },
    UI_ACTION_MAP_TERMINATOR
};


/** \brief  Register SID MIDI recording actions */
void actions_sidmidi_register(void)
{
    ui_actions_register(sidmidi_actions);
}
