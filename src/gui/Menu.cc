/*
 * Copyright (C) 2002,2003,2004,2005,2006 Daniel Heck
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

#include "gui/Menu.hh"
#include "SoundEffectManager.hh"
#include "MusicManager.hh"
#include "video.hh"
#include "options.hh"
#include "main.hh"
#include "nls.hh"
#include "ecl.hh"
#include <cassert>
#include <algorithm>
#include <iostream>


using namespace ecl;
using namespace std;

#define SCREEN ecl::Screen::get_instance()

namespace enigma { namespace gui {
    //senquack - added to facilitate joystick-driven cursor w/ DPAD acting as keyboard arrow keys
    static int dpad_used_last = 0;

    /* -------------------- Menu -------------------- */
    
    Menu::Menu()
    : active_widget(NULL), key_focus_widget(NULL), quitp(false), abortp(false) {
    }
    
    void Menu::add(Widget *w) {
        Container::add_child(w); 
    }
    
    void Menu::add(Widget *w, ecl::Rect r) {
        w->move (r.x, r.y);
        w->resize (r.w, r.h);
        add(w);
    }
    
    
    void Menu::quit() {
        quitp=true;
    }
    
    void Menu::abort() {
        abortp=true;
    }
    
    void Menu::set_key_focus(Widget *newfocus) {
        Widget *oldfocus = key_focus_widget;
        key_focus_widget = newfocus;
        if (oldfocus)
            oldfocus->invalidate();
    }
    
    bool Menu::is_key_focus(Widget *focus) {
        return (key_focus_widget == focus);
    }
    
    bool Menu::manage() {
        //senquack - added joystick support (this is the GUI cursor-emulation side of it)
        int joyx = 0, joyy = 0;
        static int old_joyx = 0, old_joyy = 0;
        double fjoyx = 0, fjoyy = 0;
//        uint8_t hat_status;     // For reading DPAD
        int cursorx, cursory;
        const double accel_cutoff = 4000;       // If new axis reading is off by this much or more from the last 
                                                //      axis reading,  reset acceleration
        const double accel_inc = 0.1;
        const double max_speed = 6.0;
        const double init_speed = 1.5;
        static double cur_speed = init_speed;

        quitp=abortp=false;
        SDL_Event e;
        Uint32 enterTickTime = SDL_GetTicks(); // protection against ESC D.o.S. attacks
        while (SDL_PollEvent(&e)) {}  // clear event queue
        draw_all();
        while (!(quitp || abortp)) {
            SCREEN->flush_updates();
            while (SDL_PollEvent(&e)) {
                handle_event(e);
                if (app.bossKeyPressed) return true;
            }

            //senquack - added joystick support
            if (joy_gcw0) {
               // If DPAD is not being used, read the analog stick instead
               joyx=(double)SDL_JoystickGetAxis(joy_gcw0, 0);
               joyy=(double)SDL_JoystickGetAxis(joy_gcw0, 1); 

               // Only move cursor if stick has been moved at least a little bit
               if (abs(joyx) > 500 || abs(joyy) > 500) {
                  dpad_used_last = 0;
                  if (abs(old_joyx - joyx) > accel_cutoff ||
                        abs(old_joyy - joyy) > accel_cutoff) {
                     // Reset acceleration
                     cur_speed = init_speed;
                  } else {
                     // Accelerate
                     cur_speed += accel_inc;
                     if (cur_speed > max_speed) {
                        cur_speed = max_speed;
                     }
                  }
                  old_joyx = joyx; old_joyy = joyy;
                  fjoyx = (double)joyx; fjoyy = (double)joyy;
                  fjoyx = fjoyx / 32767.0; fjoyy = fjoyy / 32767.0;
                  fjoyx = fjoyx * cur_speed; fjoyy = fjoyy * cur_speed;
                  SDL_GetMouseState(&cursorx, &cursory);
                  cursorx += (int)fjoyx;
                  cursory += (int)fjoyy;
                  SDL_WarpMouse(cursorx, cursory);
                  SDL_GetMouseState(&cursorx, &cursory);
                  video::MoveMouse(cursorx, cursory);
                  // We now let SDL_MOUSEMOTION in the handle_events() function deal with this:
                  // (or else DPAD controlling the arrow keys doesn't work)
                  // track_active_widget( cursorx, cursory );
               }
            }

            SDL_Delay(10);
            if(active_widget) active_widget->tick(0.01);
            if(key_focus_widget && (key_focus_widget != active_widget)) key_focus_widget->tick(0.01);
            tick(0.01);
            sound::MusicTick(0.01);
            refresh();
        }
        sound::EmitSoundEvent ("menuexit");
        // protection against ESC D.o.S. attacks
        Uint32 menuTickDuration = SDL_GetTicks() - enterTickTime;
        Uint32 minMenuTickDuration = 300;
        if (menuTickDuration < minMenuTickDuration)
            SDL_Delay(minMenuTickDuration - menuTickDuration);
        while (SDL_PollEvent(&e)) {}  // clear event queue
        return !abortp;
    }
    
    void Menu::goto_adjacent_widget(int xdir, int ydir) {
        Widget *next_widget = 0;
        if (active_widget) {
            next_widget = find_adjacent_widget(active_widget, xdir, ydir);
        }
        else { // no active_widget yet
            if ((xdir+ydir)>0) { // take first
                next_widget = *begin();
            }
            else { // take last
                iterator e = end();
                for (iterator i = begin(); i != e; ++i) {
                    next_widget = *i;
                }
            }
        }
    
        if (next_widget) {
            switch_active_widget(next_widget);
        }
        else { // no more widgets into that direction found
            sound::EmitSoundEvent ("menustop");
        }
    }
    
    void Menu::handle_event(const SDL_Event &e) {
        //senquack - added to facilitate joystick support:
        int mousex = 0, mousey = 0;
        SDL_Event new_event;
        
        //senquack - don't need this on GCW:
//        // Alt && Return for Fullscreen Toggle on Linux only
//        if (e.type == SDL_KEYDOWN &&  e.key.keysym.sym == SDLK_RETURN && 
//                e.key.keysym.mod & KMOD_ALT) {
//            video::ToggleFullscreen();
//            return;
//        }
//        
//        // Boss quit key Shift && ESC or Mac OS X application quit sequence
//        if (e.type == SDL_KEYDOWN && ((e.key.keysym.sym == SDLK_ESCAPE && 
//                (e.key.keysym.mod & KMOD_SHIFT)) || (e.key.keysym.sym == SDLK_q && 
//                (e.key.keysym.mod & KMOD_META)))) {
//            abort();
//            app.bossKeyPressed = true;
//            return;
//        }

        //senquack - added to allow DPAD to simulate keyboard arrow keys:
        if (e.type == SDL_JOYHATMOTION && e.jhat.value) {
            dpad_used_last = 1;  // So we know to now simulate ENTER key with A button, not mouse click

            //First, simulate the key down event:
            new_event.key.type = new_event.type = SDL_KEYDOWN;
            new_event.key.state = SDL_PRESSED;
            new_event.key.keysym.mod = KMOD_NONE;
            switch (e.jhat.value) {
                case SDL_HAT_DOWN:  new_event.key.keysym.sym = SDLK_DOWN; break;
                case SDL_HAT_UP:    new_event.key.keysym.sym = SDLK_UP; break;
                case SDL_HAT_RIGHT: new_event.key.keysym.sym = SDLK_RIGHT; break;
                case SDL_HAT_LEFT:  new_event.key.keysym.sym = SDLK_LEFT; break;
                default: break;
            }
            if (active_widget && active_widget->on_event(new_event)) return;
            if (key_focus_widget && key_focus_widget->on_event(new_event)) return;
            if (on_event(new_event)) return;   // track active widgets (LevelWidget)

            // Then, simulate the matching key-up event:
            new_event.key.type = new_event.type = SDL_KEYUP;
            new_event.key.state = SDL_RELEASED;
            switch (e.jhat.value) {
                case SDL_HAT_DOWN:  new_event.key.keysym.sym = SDLK_DOWN; break;
                case SDL_HAT_UP:    new_event.key.keysym.sym = SDLK_UP; break;
                case SDL_HAT_RIGHT: new_event.key.keysym.sym = SDLK_RIGHT; break;
                case SDL_HAT_LEFT:  new_event.key.keysym.sym = SDLK_LEFT; break;
                default: break;
            }

            if (active_widget && active_widget->on_event(new_event)) return;
            if (key_focus_widget && key_focus_widget->on_event(new_event)) return;
            if (on_event(new_event)) return;   // track active widgets (LevelWidget)
        } else {
        
        // first allow active widget to handle event
        if (active_widget && active_widget->on_event(e))
            return;
        
        // key focus handler next to catch key events
        if (key_focus_widget && key_focus_widget->on_event(e))
            return;
        
        // menu subclass with special handling
        if (on_event(e) && e.type != SDL_MOUSEMOTION)   // track active widgets (LevelWidget)
            return;

        }
    
        //debug
        printf("dpad_used_last: %d\n", dpad_used_last);
        switch (e.type) {
            case SDL_QUIT:
                abort();
                app.bossKeyPressed = true;
                break;
            case SDL_MOUSEMOTION:
                track_active_widget( e.motion.x, e.motion.y );
                break;
            case SDL_KEYDOWN:
                if (!active_widget || !active_widget->on_event(e)) {
                    // if not handled by active_widget
                    switch (e.key.keysym.sym) {
                        case SDLK_ESCAPE:
                            abort();
                            break;
                            // TODO replace cursor keys with tab for gotos
                        case SDLK_DOWN:  goto_adjacent_widget( 0,  1); break;
                        case SDLK_UP:    goto_adjacent_widget( 0, -1); break;
                        case SDLK_RIGHT: goto_adjacent_widget( 1,  0); break;
                        case SDLK_LEFT:  goto_adjacent_widget(-1,  0); break;
                        default:
                                         break;
                    }
                }
                break;
            //senquack - added joystick support:
            case SDL_JOYAXISMOTION:
                // is this is an event from the analog stick?
                if (e.jaxis.which == 0) {
                    dpad_used_last = 0;  // So we know to NOT simulate ENTER key with A button, but instead, mouse click
                }
                break;
            case SDL_JOYHATMOTION:
                dpad_used_last = 1;  // So we know to now simulate ENTER key with A button, not mouse click
                switch (e.jhat.value) {
                    case SDL_HAT_DOWN:  goto_adjacent_widget( 0,  1); break;
                    case SDL_HAT_UP:    goto_adjacent_widget( 0, -1); break;
                    case SDL_HAT_RIGHT: goto_adjacent_widget( 1,  0); break;
                    case SDL_HAT_LEFT:  goto_adjacent_widget(-1,  0); break;
                    default: break;
                }
                break;
            case SDL_JOYBUTTONDOWN:
            case SDL_JOYBUTTONUP:
                if (e.jbutton.button == 0 || e.jbutton.button == 1) {
                    // Button A(1) or B(0) pressed 
                    if (e.jbutton.button == 1 && dpad_used_last) {
                        // A button is handled specially: it can either be the "enter key" or left-mouse-click,
                        //    depending on whether the analog stick or the DPAD was the most recently used control.
                        new_event.key.type = new_event.type = (e.type == SDL_JOYBUTTONDOWN) ? SDL_KEYDOWN : SDL_KEYUP;
                        new_event.key.state = (e.type == SDL_JOYBUTTONDOWN) ? SDL_PRESSED : SDL_RELEASED;
                        new_event.key.keysym.sym = SDLK_RETURN;
                        new_event.key.keysym.mod = KMOD_NONE;
                        on_event(new_event); // <- If I didn't add this, things didn't work right
                        if (active_widget) active_widget->on_event(new_event);
                    } else {
                        // Simulate mouse clicks
                        SDL_GetMouseState(&mousex, &mousey);
                        new_event.button.x = mousex;
                        new_event.button.y = mousey;
                        new_event.button.state = (e.type == SDL_JOYBUTTONDOWN) ? SDL_PRESSED : SDL_RELEASED;
                        new_event.button.type = new_event.type = 
                            (e.type == SDL_JOYBUTTONDOWN) ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
                        new_event.button.button = (e.jbutton.button == 0) ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
                        new_event.button.which = 0;      // Don't know what else to put here
                        track_active_widget( mousex, mousey );
                        if (active_widget) active_widget->on_event(new_event);
                    }
                } else if (e.jbutton.button == 6 || e.jbutton.button == 7) {
                    // L(6) or R(7) trigger pressed; simulate mouse wheel scroll
                    SDL_GetMouseState(&mousex, &mousey);
                    new_event.button.x = mousex;
                    new_event.button.y = mousey;
                    new_event.button.state = (e.type == SDL_JOYBUTTONDOWN) ? SDL_PRESSED : SDL_RELEASED;
                    new_event.button.type = new_event.type = 
                        (e.type == SDL_JOYBUTTONDOWN) ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
                    new_event.button.button = (e.jbutton.button == 7) ? SDL_BUTTON_WHEELUP : SDL_BUTTON_WHEELDOWN;
                    new_event.button.which = 0;      // Don't know what else to put here
                    track_active_widget(mousex, mousey);
                    if (active_widget) active_widget->on_event(new_event);
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                track_active_widget( e.button.x, e.button.y );
                if (active_widget) active_widget->on_event(e);
                break;
            case SDL_VIDEOEXPOSE:
                draw_all();
                break;
            default:
                if (active_widget) active_widget->on_event(e);
        }
    }
    
    void Menu::switch_active_widget(Widget *to_activate) {
        if (to_activate != active_widget) {
            if (active_widget)
                active_widget->deactivate();
            if (to_activate)
                to_activate->activate();
            active_widget = to_activate;
        }
    }
    
    void Menu::track_active_widget( int x, int y ) {
        switch_active_widget(find_widget(x, y));
    }
    
    
    void Menu::center() {
        if (m_widgets.size() > 0) {
            using std::min;
            using std::max;
    
            Rect a = m_widgets[0]->get_area();
            for (unsigned i=1; i<m_widgets.size(); ++i)
            {
                Rect r = m_widgets[i]->get_area();
                a.x = min(r.x, a.x);
                a.y = min(r.y, a.y);
                a.w += max(0, r.x+r.w-a.x-a.w);
                a.h += max(0, r.y+r.h-a.y-a.h);
            }
            Rect c=ecl::center(SCREEN->size(), a);
            int dx = c.x-a.x;
            int dy = c.y-a.y;
    
            for (unsigned i=0; i<m_widgets.size(); ++i) {
                Rect r = m_widgets[i]->get_area();
                r.x += dx;
                r.y += dy;
    
                m_widgets[i]->move (r.x, r.y);
                m_widgets[i]->resize (r.w, r.h);
            }
        }
    }
    
    void Menu::draw (ecl::GC &gc, const ecl::Rect &r)
    {
        clip(gc, r);
        draw_background(gc);
        Container::draw(gc,r);
    }
}} // namespace enigma::gui
