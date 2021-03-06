/*
 * Copyright (C) 2002,2003,2004 Daniel Heck
 * Copyright (C) 2007,2008,2009 Ronald Lamprecht
 * Copyright (C) 2008 Raoul Bourquin
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *  
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#include "stones/FloppySwitch.hh"
#include "server.hh"
#include "Inventory.hh"
#include "player.hh"

namespace enigma {
    FloppySwitch::FloppySwitch() : Stone () {
    }

    void FloppySwitch::setAttr(const string& key, const Value &val) {
        if (key == "target") {
            Stone::setAttr("destination", val);
        }
        Stone::setAttr(key, val);
    }
    
    void FloppySwitch::setState(int extState) {
        if (isDisplayable()) {
            if (extState != state) {
                sound_event ("switchon");
                state = ON - state;
                init_model();
                performAction(state == ON);
            }
        } else
            state = extState;
    }

    void FloppySwitch::init_model() {
        set_model(ecl::strf("st_floppy_%s", state == ON ? "on" : "off"));
    }

    void FloppySwitch::actor_hit(const StoneContact &sc) {
        if (enigma::Inventory *inv = player::GetInventory(sc.actor)) {
            const GridPos apos = sc.actor->get_gridpos();
            bool safe = true;
            ecl::V2 dest;
            int idx = 0;
            if (server::EnigmaCompatibility >= 1.10 && getAttr("secure").to_bool()) {
                while (getDestinationByIndex(idx++, dest)) {
                    if (apos == (GridPos)dest)
                        safe = false;
                }
            }
            if (safe) {
                if (state == ON) {
                    if (!inv->is_full()) {
                        inv->add_item (MakeItem("it_floppy"));
                        setState(OFF);
                    }
                }
                else if (player::WieldedItemIs (sc.actor, "it_floppy")) {
                    DisposeObject(inv->yield_first());
                    setState(ON);
                }
                player::RedrawInventory (inv);
            }
        }
    }

    const char *FloppySwitch::collision_sound() {
         return "metal";
    }

    DEF_TRAITS(FloppySwitch, "st_floppy", st_floppy);

    BOOT_REGISTER_START
        BootRegister(new FloppySwitch(), "st_floppy");
    BOOT_REGISTER_END

} // namespace enigma
