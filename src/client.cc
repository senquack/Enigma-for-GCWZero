/*
 * Copyright (C) 2004 Daniel Heck
 * Copyright (C) 2006,2007,2008,2009,2010 Ronald Lamprecht
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

#include "client.hh"
#include "game.hh"
#include "display.hh"
#include "options.hh"
#include "server.hh"
#include "gui/HelpMenu.hh"
#include "main.hh"
#include "gui/GameMenu.hh"
#include "SoundEngine.hh"
#include "SoundEffectManager.hh"
#include "MusicManager.hh"
#include "player.hh"
#include "world.hh"
#include "nls.hh"
#include "StateManager.hh"
#include "lev/Index.hh"
#include "lev/PersistentIndex.hh"
#include "lev/Proxy.hh"
#include "lev/RatingManager.hh"
#include "lev/ScoreManager.hh"

#include "ecl_sdl.hh"

#include "enet/enet.h"

#include <cctype>
#include <cstring>
#include <cassert>
#include <algorithm>
#include <iostream>

using namespace enigma::client;
using namespace ecl;
using namespace std;

#include "client_internal.hh"

/* -------------------- Auxiliary functions -------------------- */

namespace
{
    /*! Display a message and change the current mouse speed. */
    void set_mousespeed (double speed)
    {
        int s = round_nearest<int>(speed);
        options::SetMouseSpeed (s);
        s = round_nearest<int> (options::GetMouseSpeed ());
        Msg_ShowText(strf(_("Mouse speed: %d"), s), false, 2.0);
    }

    /*! Generate the message that is displayed when the level starts. */
    std::string displayedLevelInfo (lev::Proxy *level) {
        std::string text;
        std::string tmp;

        tmp  = level->getLocalizedString("title");
        if (tmp.empty())
            tmp = _("Another nameless level");
        text = string("\"")+ tmp +"\"";
        tmp = level->getAuthor();
        if (!tmp.empty())
            text += _(" by ") + tmp;
        tmp = level->getLocalizedString("subtitle");
        if (!tmp.empty() && tmp != "subtitle")
           text += string(" - \"")+ tmp + "\"";
        tmp = level->getCredits(false);
        if (!tmp.empty())
            text += string(" - Credits: ")+ tmp;
        tmp = level->getDedication(false);
        if (!tmp.empty())
            text += string(" - Dedication: ")+ tmp;
        return text;
    }
}


/* -------------------- Variables -------------------- */

namespace
{
    Client     client_instance;
    const char HSEP = '^'; // history separator (use character that user cannot use)
}

#define CLIENT client_instance

/* -------------------- Client class -------------------- */

Client::Client()
: m_state (cls_idle),
  m_levelname(),
  m_hunt_against_time(0),
  m_cheater(false),
  m_user_input()
{
    m_network_host = 0;
}

Client::~Client()
{
    network_stop();
}

void Client::init() {
    std::string command;
    for (int i = 0; i < 10; i++) {
        command = app.state->getString(ecl::strf("CommandHistory#%d", i).c_str());
        if (command.size() > 0)
            commandHistory.push_back(command);
        else
            break;
    }
}

void Client::shutdown() {
    for (int i = 0; i < commandHistory.size(); i++)
        app.state->setProperty(ecl::strf("CommandHistory#%d", i).c_str(), commandHistory[i].c_str());
}

bool Client::network_start()
{
    if (m_network_host)
        return true;

    m_network_host = enet_host_create (NULL,
                                       1 /* only allow 1 outgoing connection */,
                                       57600 / 8 /* 56K modem with 56 Kbps downstream bandwidth */,
                                       14400 / 8 /* 56K modem with 14 Kbps upstream bandwidth */);

    if (m_network_host == NULL) {
        fprintf (stderr,
                 "An error occurred while trying to create an ENet client host.\n");
        return false;
    }


    // ----- Connect to server

    ENetAddress sv_address;
    ENetPeer *m_server;

    /* Connect to some.server.net:1234. */
    enet_address_set_host (&sv_address, "localhost");
    sv_address.port = 12345;

    /* Initiate the connection, allocating the two channels 0 and 1. */
    m_server = enet_host_connect (m_network_host, &sv_address, 2);

    if (m_server == NULL) {
       fprintf (stderr,
                "No available peers for initiating an ENet connection.\n");
       return false;
    }

    // Wait up to 5 seconds for the connection attempt to succeed.
    ENetEvent event;
    if (enet_host_service (m_network_host, &event, 5000) > 0 &&
        event.type == ENET_EVENT_TYPE_CONNECT)
    {
        fprintf (stderr, "Connection to some.server.net:1234 succeeded.");
        return true;
    }
    else
    {
        /* Either the 5 seconds are up or a disconnect event was */
        /* received. Reset the peer in the event the 5 seconds   */
        /* had run out without any significant event.            */
        enet_peer_reset (m_server);
        m_server = 0;

        fprintf (stderr, "Connection to localhost:12345 failed.");
        return false;
    }
}

void Client::network_stop ()
{
    if (m_network_host)
        enet_host_destroy (m_network_host);
    if (m_server)
        enet_peer_reset (m_server);
}


/* ---------- Event handling ---------- */

////senquack - added this so users can re-center the g-sensor on-the-fly in-game
//void Client::recenter_gsensor()
//{
//   int joyx = 0, joyy = 0;
//   char msg[] = "G-Sensor re-centered.";
//   joyx = SDL_JoystickGetAxis(joy_gsensor, 0);
//   joyy = SDL_JoystickGetAxis(joy_gsensor, 1); 
//   options::SetGsensorCenterX (joyx);
//   options::SetGsensorCenterY (joyy);
//   Msg_ShowText(msg, false);
//   //debug
//   printf("Gsensor re-centered at: %d, %d\n", joyx, joyy);
//}



void Client::handle_events()
{
    SDL_Event e;

    //senquack: added joystick support
    int joyx = 0, joyy = 0;
    int gsensor_movement = 0;
    double fjoyx = 0, fjoyy = 0;
    int analog_deadzone;
    double analog_speed;
    int gsensor_deadzone, gsensor_centerx, gsensor_centery;
    double gsensor_speed;

    static double speed_scale = 1.0;      // Can be modified on-the-fly when certain buttons are pressed

    // for reference for now:
//    int GetGsensorCenterX ();
//    int SetGsensorCenterX (int val);
//    int GetGsensorCenterY ();
//    int SetGsensorCenterY (int val);
//    int GetGsensorDeadzone ();
//    int SetGsensorDeadzone (int val);
//    int GetAnalogDeadzone ();
//    int SetAnalogDeadzone (int val);
//    int GetAnalogEnabled ();
//    int SetAnalogEnabled (int val);
//    int GetGsensorEnabled ();
//    int SetGsensorEnabled (int val);
//    int GetGsensorSpeed ();
//    int SetGsensorSpeed (int val);
//    int GetAnalogSpeed ();
//    int SetAnalogSpeed (int val);
//    int GetDPADSpeed ();
//    int SetDPADSpeed (int val);

//    if (app.analog_enabled && app.joy_gcw0) {
    
    // WORKING ON THIS
    if (options::GetAnalogEnabled() && joy_gcw0) {
       analog_deadzone = options::GetAnalogDeadzone() * 1000;
       //debug - working out good values
       analog_speed = (double)options::GetAnalogSpeed();
//       analog_speed = 30.0;

       joyx=SDL_JoystickGetAxis(joy_gcw0, 0);
       joyy=SDL_JoystickGetAxis(joy_gcw0, 1); 
       if (abs(joyx) > analog_deadzone || abs(joyy) > analog_deadzone) {
          fjoyx = (double)joyx / 32767.0;
          fjoyy = (double)joyy / 32767.0;
          fjoyx = fjoyx * fjoyx * fjoyx;
          fjoyy = fjoyy * fjoyy * fjoyy;
          //            printf("scaled x: %lf    scaled y: %lf\n", fjoyx, fjoyy);
//          server::Msg_MouseForce (options::GetDouble("MouseSpeed") *
//                //                                     V2 (fjoyx * 20.0, fjoyy * 20.0));
//             V2 (fjoyx * 3.0, fjoyy * 3.0));
          server::Msg_MouseForce (
             V2 (fjoyx * analog_speed * speed_scale, fjoyy * analog_speed * speed_scale));
       }
    }

//    //DEBUG
//    if (options::GetGsensorEnabled() && joy_gsensor) {
//       // First, see if a g-sensor recalibration is requested:
//       if (joy_gcw0) {
//          if (SDL_JoystickGetButton(joy_gcw0, 0) &&
//                SDL_JoystickGetButton(joy_gcw0, 2) &&
//                SDL_JoystickGetButton(joy_gcw0, 3)) {
//             recenter_gsensor();
//          }
//       }
//       gsensor_deadzone = options::GetGsensorDeadzone();
//       gsensor_speed = (double)options::GetGsensorSpeed();
//    }

//       joyx = SDL_JoystickGetAxis(app.joy_gsensor, 0);
//       joyy = SDL_JoystickGetAxis(app.joy_gsensor, 1); 
//             
//   //    joyx = joyx - app.gsensor_centerx;
//   //    joyy = joyy - app.gsensor_centery;
//   //    if (abs(joyx) > app.gsensor_deadzone || abs(joyy) > app.gsensor_deadzone) {
//   //       printf("gsensor movement detected\n");
//
//          printf("joyx: %d   joyy: %d", joyx, joyy);
////          if (joyx > 0) {
////             fjoyx = (double)(joyx - app.gsensor_deadzone - app.gsensor_centerx);
////          } else {
////             fjoyx = (double)(joyx + app.gsensor_deadzone + app.gsensor_centerx);
////          }
////
////          if (joyy > 0) {
////             fjoyy = (double)(joyy - app.gsensor_deadzone - app.gsensor_centery);
////          } else {
////             fjoyy = (double)(joyy + app.gsensor_deadzone + app.gsensor_centery);
////          }
//
//          joyx = joyx - app.gsensor_centerx;
//          joyy = -(joyy - app.gsensor_centery);
//
//          if (joyx > app.gsensor_deadzone) {
//             fjoyx = (double)(joyx - app.gsensor_deadzone);
//             gsensor_movement = 1;
//          } else if (joyx < -app.gsensor_deadzone) {
//             fjoyx = (double)(joyx + app.gsensor_deadzone);
//             gsensor_movement = 1;
//          }
//
//          if (joyy > app.gsensor_deadzone) {
//             fjoyy = (double)(joyy - app.gsensor_deadzone);
//             gsensor_movement = 1;
//          } else if (joyy <  app.gsensor_deadzone) {
//             fjoyy = (double)(joyy + app.gsensor_deadzone);
//             gsensor_movement = 1;
//          }
//
//          printf("   fjoyx: %lf   fjoyy: %lf", fjoyx, fjoyy);
//   //       fjoyx = fjoyx / (double)app.gsensor_max;
//   //       fjoyy = fjoyy / (double)app.gsensor_max;
//          if ((app.gsensor_max - app.gsensor_deadzone != 0) &&
//                gsensor_movement) {
//             fjoyx = fjoyx / (double)(app.gsensor_max - app.gsensor_deadzone);
//             fjoyy = fjoyy / (double)(app.gsensor_max - app.gsensor_deadzone);
//             //            fjoyx = fjoyx * fjoyx * fjoyx;
//             //            fjoyy = fjoyy * fjoyy * fjoyy;
//             //            printf("scaled x: %lf    scaled y: %lf\n", fjoyx, fjoyy);
//             printf("  fjoyx_scaled: %lf  fjoyy_scaled: %lf\n", fjoyx, fjoyy);
//             server::Msg_MouseForce (options::GetDouble("MouseSpeed") *
//                V2 (fjoyx * 4.0, fjoyy * 4.0));
//          } 
//          else {
//             printf("\n");
//          }
//   }
    
    // KOULES CODE FOR REF.:
//	if (gsensor_enabled && joy_gsensor) { // disabled in 2-player mode
//		Sint16 xmove, ymove;
//		xmove=SDL_JoystickGetAxis(joy_gsensor,0);
//		ymove=SDL_JoystickGetAxis(joy_gsensor,1);
//		control_state[CGSENSORLEFT] 	= (xmove < (gsensor_centerx - gsensor_deadzone));
//		control_state[CGSENSORRIGHT] 	= (xmove > (gsensor_centerx + gsensor_deadzone));
//		control_state[CGSENSORUP] 		= (ymove < (gsensor_centery - gsensor_deadzone));
//		control_state[CGSENSORDOWN] 	= (ymove > (gsensor_centery + gsensor_deadzone));
//	} else {
//		control_state[CGSENSORUP] = control_state[CGSENSORDOWN] =
//			control_state[CGSENSORLEFT] = control_state[CGSENSORRIGHT] = 0;
//	}
//        int analog_deadzone = 500;
//        int gsensor_deadzone = 1250;	// Seems to also work well on the GCW Zero's g-sensor
//        int gsensor_centerx = 0;		   // The g-sensor needs a re-settable center so the user can play at a normal tilt
//        int gsensor_centery = 13100;	//	13100 is a reasonable backwards-tilt to set as default.
//        int gsensor_recently_recentered = 0;	// Other code can use this to sense when to display message 
//                                             //  confirming recentering of g-sensor
//        const int gsensor_max = 26200;	// seems to be the maximum reading in any direction from the gsensor
//        int analog_active = 1;
//        int gsensor_active = 0;



    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_KEYDOWN:
            on_keydown(e);
            break;
        case SDL_MOUSEMOTION:
            if (abs(e.motion.xrel) > 300 || abs(e.motion.yrel) > 300) {
                fprintf(stderr, "mouse event with %i, %i\n", e.motion.xrel, e.motion.yrel);
            }
            else
                server::Msg_MouseForce (options::GetDouble("MouseSpeed") *
                                        V2 (e.motion.xrel, e.motion.yrel));
            break;
         //senquack - added joystick support:
        case SDL_JOYBUTTONDOWN:
            update_mouse_button_state();
            if (e.jbutton.button == 0) {
               // B button is pressed ; apply speed scale #1
               speed_scale = (double)options::GetSpeedScale1() / 10.0;
            } else if (e.jbutton.button == 3) {
               // X button is pressed ; apply speed scale #2
               speed_scale = (double)options::GetSpeedScale2() / 10.0;
            } else if (e.jbutton.button == 2) {
               // Y button is pressed ; apply speed scale #3
               speed_scale = (double)options::GetSpeedScale3() / 10.0;
            } else if (e.jbutton.button == 4) {
               // SELECT is pressed ; rotate inventory like mouse scrollwheel
               rotate_inventory(-1);
            } else if (e.jbutton.button == 6) {
               // L trigger is pressed ; rotate inventory like mouse scrollwheel
               rotate_inventory(+1);
            } else if (e.jbutton.button == 1 || e.jbutton.button == 7) {
               // A button or R trigger is pressed ; use inventory item
               server::Msg_ActivateItem();
            } else if (e.jbutton.button == 5) {
               // START button is pressed ; open menu 
               show_menu(true);
            }
            break;
        case SDL_JOYBUTTONUP:
            update_mouse_button_state();

            if (e.jbutton.button == 3) {
               // X button released ; normal marble movement
               speed_scale = 1.0;
            } else if (e.jbutton.button == 2) {
               // Y button is released ; normal marble movement
               speed_scale = 1.0;
            } else if (e.jbutton.button == 0) {
               // B button is released ; normal marble movement
               speed_scale = 1.0;
            }
            break;


        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            on_mousebutton(e);
            break;
        case SDL_ACTIVEEVENT: {
            update_mouse_button_state();
            if (e.active.gain == 0 && !video::IsFullScreen())
                show_menu(false);
            break;
        }

        case SDL_VIDEOEXPOSE: {
            display::RedrawAll(video::GetScreen());
            break;
        }

        case SDL_QUIT:
            client::Msg_Command("abort");
            app.bossKeyPressed = true;
            break;
        }
    }
}

//senquack - added joystick support
//void Client::update_mouse_button_state()
//{
//    int b = SDL_GetMouseState(0, 0);
//    player::InhibitPickup((b & SDL_BUTTON(1)) || (b & SDL_BUTTON(3)));
//}
void Client::update_mouse_button_state()
{
   //senquack - normally, you can hold the left or right mouse buttons to inhibit picking up a item..
   //    since we aren't goint to have a right mouse button during play, we'll use the rotate-inventory-buttons for this.
    int b = SDL_GetMouseState(0, 0);
    int gcw_inhibit_pickup = 0;
    if (joy_gcw0) {
       gcw_inhibit_pickup = SDL_JoystickGetButton(joy_gcw0, 1) |
                            SDL_JoystickGetButton(joy_gcw0, 6) |
                            SDL_JoystickGetButton(joy_gcw0, 7);
    }
    player::InhibitPickup(gcw_inhibit_pickup || (b & SDL_BUTTON(1)) || (b & SDL_BUTTON(3)));
}

void Client::on_mousebutton(SDL_Event &e)
{
    if (e.button.state == SDL_PRESSED) {
        if (e.button.button == 1) {
            // left mousebutton -> activate first item in inventory
            server::Msg_ActivateItem ();
        }
        else if (e.button.button == 3|| e.button.button == 4) {
            // right mousebutton, wheel down -> rotate inventory
            rotate_inventory(+1);
        }
        else if (e.button.button == 5) {
            // wheel down -> inverse rotate inventory
            rotate_inventory(-1);
        }
    }
    update_mouse_button_state();
}

void Client::rotate_inventory (int direction)
{
    m_user_input = "";
    STATUSBAR->hide_text();
    player::RotateInventory(direction);
}

/* -------------------- Console related -------------------- */

void Client::process_userinput() {
    // no addition of existing commands to history
    if (consoleIndex == 1) {
        for (int i = 0; i < commandHistory.size(); i++) {
            if (newCommand == commandHistory[i]) {
                // take existing history command instead of new command
                consoleIndex = i + 2;
                break;
            }
        }
    }
    // resort history with selected command at bottom
    if (consoleIndex == 1) {
        if (commandHistory.size() < 10)
             commandHistory.push_back(std::string(""));
        for (int i = 8; i >= 0; i--) {
            if (i < commandHistory.size() - 1)
                commandHistory[i+1] = commandHistory[i];
        }
    } else if (consoleIndex > 1) {
        newCommand = commandHistory[consoleIndex - 2];
        for (int i = consoleIndex - 3; i >= 0; i--) {
            if (i < commandHistory.size())
                commandHistory[i+1] = commandHistory[i];
        }
    } else { // document history or inventory
        return;
    }
    commandHistory[0] = newCommand;
    newCommand = "";
    consoleIndex = 0;
    STATUSBAR->hide_text();
    server::Msg_Command(commandHistory[0]);
}

void Client::user_input_append(char c) {
    if (consoleIndex <= 0) {
        consoleIndex = 1;
        newCommand = c;
    } else if (consoleIndex == 1) {
        newCommand += c;
    } else {
        newCommand =  commandHistory[consoleIndex - 2] + c;
        consoleIndex = 1;
    }
    Msg_ShowText(newCommand, false);
}

void Client::user_input_backspace() {
    if (consoleIndex == 1) {
        newCommand.erase(newCommand.size() - 1, 1);
        if (!newCommand.empty())
            Msg_ShowText(newCommand, false);
        else {
            consoleIndex = 0;
            STATUSBAR->hide_text();
        }
    } else if (consoleIndex > 1) {
        newCommand =  commandHistory[consoleIndex - 2];
        newCommand.erase(newCommand.size() - 1, 1);
        if (!newCommand.empty()) {
            consoleIndex = 1;
            Msg_ShowText(newCommand, false);
        } else {
            consoleIndex = 0;
            STATUSBAR->hide_text();
        }
    }
}

void Client::user_input_previous() {
    if (consoleIndex < 0) {
        ++consoleIndex;
        int docIndex = documentHistory.size() + consoleIndex;
        if (docIndex < documentHistory.size()) {
            Msg_ShowText(documentHistory[docIndex], true);
        } else {
            consoleIndex = 0;
            STATUSBAR->hide_text();
        }
    } else if (consoleIndex == 0) {
        if (newCommand.length() > 0) {
            consoleIndex = 1;
            Msg_ShowText(newCommand, false);
        } else if (commandHistory.size() > 0) {
            consoleIndex = 2;
            Msg_ShowText(commandHistory[0], false);
        }
    } else if (consoleIndex <= commandHistory.size()) {
        ++consoleIndex;
        Msg_ShowText(commandHistory[consoleIndex - 2], false);
    } else {  // top of history or new command without history
        consoleIndex = 0;
        STATUSBAR->hide_text();
    }
}

void Client::user_input_next() {
    if (consoleIndex <= 0) {
        --consoleIndex;
        int docIndex = documentHistory.size() + consoleIndex;
        if (docIndex >= 0) {
            Msg_ShowText(documentHistory[docIndex], true);
        } else {
            consoleIndex = 0;
            STATUSBAR->hide_text();
        }
    } else if (consoleIndex == 1 || (consoleIndex == 2 && newCommand.size() == 0)) {
        consoleIndex = 0;
        STATUSBAR->hide_text();
    } else if (consoleIndex > 1) {
        --consoleIndex;
        Msg_ShowText(consoleIndex == 1 ? newCommand : commandHistory[consoleIndex - 2], false);
    }
}

void Client::on_keydown(SDL_Event &e)
{
    SDLKey keysym = e.key.keysym.sym;
    SDLMod keymod = e.key.keysym.mod;

    if (keymod & KMOD_CTRL) {
        switch (keysym) {
        case SDLK_a:
            server::Msg_Command ("restart");
            break;
        case SDLK_F3:
            if (keymod & KMOD_SHIFT) {
                // force a reload from file
                lev::Proxy::releaseCache();
                server::Msg_Command ("restart");
            }
        default:
            break;
        };
    }
    else if (keymod & KMOD_ALT) {
        switch (keysym) {
        case SDLK_x:
            abort(); break;
        case SDLK_t:
            if (enigma::WizardMode) {
                Screen *scr = video::GetScreen();
                ecl::TintRect(scr->get_surface (), display::GetGameArea(),
                             100, 100, 100, 0);
                scr->update_all();
            }
            break;
        case SDLK_s:
            if (enigma::WizardMode) {
                server::Msg_Command ("god");
            }
            break;
        case SDLK_RETURN:
            {
                video::TempInputGrab (false);
                video::ToggleFullscreen ();
                sdl::FlushEvents();
            }
            break;
        default:
            break;
        };
    }
    else if (keymod & KMOD_META) {
        switch (keysym) {
            case SDLK_q:    // Mac OS X application quit sequence
                app.bossKeyPressed = true;
                abort();
                break;
            default:
                break;
        };
    }
    else {
        switch (keysym) {
        case SDLK_ESCAPE:
            if (keymod & KMOD_SHIFT) {
                app.bossKeyPressed = true;
                abort();
            } else {
                show_menu(true);
            }
            break;
        case SDLK_LEFT:   set_mousespeed(options::GetMouseSpeed() - 1); break;
        case SDLK_RIGHT:  set_mousespeed(options::GetMouseSpeed() + 1); break;
        case SDLK_TAB:    rotate_inventory(+1); break;
        case SDLK_F1:     show_help(); break;
        case SDLK_F2:
            // display hint
            break;
        case SDLK_F3:
            if (keymod & KMOD_SHIFT)
                server::Msg_Command ("restart");
            else
                server::Msg_Command ("suicide");
            break;

        case SDLK_F4: Msg_AdvanceLevel(lev::ADVANCE_STRICTLY); break;
        case SDLK_F5: Msg_AdvanceLevel(lev::ADVANCE_UNSOLVED); break;
        case SDLK_F6: Msg_JumpBack(); break;

        case SDLK_F10: {
            lev::Proxy *level = server::LoadedProxy;
            std::string basename = std::string("screenshots/") +
                    level->getLocalSubstitutionLevelPath();
            std::string fname = basename + ".png";
            std::string fullPath;
            int i = 1;
            while (app.resourceFS->findFile(fname, fullPath)) {
                fname = basename + ecl::strf("#%d", i++) + ".png";
            }
            std::string savePath = app.userImagePath + "/" + fname;
            video::Screenshot(savePath);
            break;
        }
        case SDLK_RETURN: process_userinput(); break;
        case SDLK_BACKSPACE: user_input_backspace(); break;
        case SDLK_UP: user_input_previous(); break;
        case SDLK_DOWN: user_input_next(); break;
        default:
            if (e.key.keysym.unicode  && (e.key.keysym.unicode & 0xff80) == 0) {
                char ascii = static_cast<char>(e.key.keysym.unicode & 0x7f);
                if (isalnum (ascii) ||
                    strchr(" .-!\"$%&/()=?{[]}\\#'+*~_,;.:<>|", ascii)) // don't add '^' or change history code
                {
                    user_input_append(ascii);
                }
            }

            break;
        }
    }
}

static const char *helptext_ingame[] = {
    N_("Left mouse button:"),    N_("Activate/drop leftmost inventory item"),
    N_("Right mouse button:"),      N_("Rotate inventory items"),
    N_("Escape:"),                  N_("Show game menu"),
    N_("Shift+Escape:"),            N_("Quit game immediately"),
    N_("F1:"),                      N_("Show this help"),
    N_("F3:"),                      N_("Kill current marble"),
    N_("Shift+F3:"),                N_("Restart the current level"),
    N_("F4:"),                      N_("Skip to next level"),
    N_("F5:"),                      0, // see below
    N_("F6:"),                      N_("Jump back to last level"),
    N_("F10:"),                     N_("Make screenshot"),
    N_("Left/right arrow:"),        N_("Change mouse speed"),
    N_("Alt+x:"),                   N_("Return to level menu"),
//    N_("Alt+Return:"),              N_("Switch between fullscreen and window"),
    0
};

void Client::show_help()
{
    server::Msg_Pause (true);
    video::TempInputGrab grab(false);

    helptext_ingame[17] = app.state->getInt("NextLevelMode") == lev::NEXT_LEVEL_NOT_BEST
        ? _("Skip to next level for best score hunt")
        : _("Skip to next unsolved level");

    video::ShowMouse();
    gui::displayHelp(helptext_ingame, 200);
    video::HideMouse();

    update_mouse_button_state();
    if (m_state == cls_game)
        display::RedrawAll(video::GetScreen());

    server::Msg_Pause (false);
    game::ResetGameTimer();

    if (app.state->getInt("NextLevelMode") == lev::NEXT_LEVEL_NOT_BEST)
        server::Msg_Command ("restart"); // inhibit cheating

}


void Client::show_menu(bool isESC) {
    if (isESC && server::LastMenuTime != 0.0 && server::LevelTime - server::LastMenuTime < 0.3) {
        return; // protection against ESC D.o.S. attacks
    }
    if (isESC && server::LastMenuTime != 0.0 && server::LevelTime - server::LastMenuTime < 0.35) {
        server::MenuCount++;
        if (server::MenuCount > 10)
            mark_cheater();
    }

    server::Msg_Pause (true);

    ecl::Screen *screen = video::GetScreen();

    video::TempInputGrab grab (false);

    video::ShowMouse();
    {
        int x, y;
        display::GetReferencePointCoordinates(&x, &y);
        enigma::gui::GameMenu(x, y).manage();
    }
    video::HideMouse();
    update_mouse_button_state();
    if (m_state == cls_game)
        display::RedrawAll(screen);

    server::Msg_Pause (false);
    game::ResetGameTimer();

    if (isESC)  // protection against ESC D.o.S. attacks
        server::LastMenuTime = server::LevelTime;
}

void Client::draw_screen()
{
    switch (m_state) {
    case cls_error: {
        Screen *scr = video::GetScreen();
        GC gc (scr->get_surface());
        blit(gc, 0,0, enigma::GetImage("menu_bg", ".jpg"));
        Font *f = enigma::GetFont("menufont");

        vector<string> lines;

        ecl::split_copy (m_error_message, '\n', back_inserter(lines));
        int x     = 60;
        int y     = 60;
        int yskip = 25;
        const video::VMInfo *vminfo = video::GetInfo();
        int width = vminfo->width - 120;
        for (unsigned i=0; i<lines.size(); ) {
            std::string::size_type breakPos = ecl::breakString (f, lines[i],
                                                                " ", width);
            f->render(gc, x,  y, lines[i].substr(0,breakPos).c_str());
            y += yskip;
            if (breakPos != lines[i].size()) {
                // process rest of line
                lines[i] = lines[i].substr(breakPos);
            } else {
                // process next line
                i++;
            }
        }
        scr->update_all();
        scr->flush_updates();
        break;
    }
    default:
        break;
    }
}


std::string Client::init_hunted_time()
{
    std::string hunted;
    m_hunt_against_time = 0;
    if (app.state->getInt("NextLevelMode") == lev::NEXT_LEVEL_NOT_BEST) {
        lev::Index *ind = lev::Index::getCurrentIndex();
        lev::ScoreManager *scm = lev::ScoreManager::instance();
        lev::Proxy *curProxy = ind->getCurrent();
        lev::RatingManager *ratingMgr = lev::RatingManager::instance();

        int   difficulty     = app.state->getInt("Difficulty");
        int   wr_time       = ratingMgr->getBestScore(curProxy, difficulty);
        int   best_user_time = scm->getBestUserScore(curProxy, difficulty);

        if (best_user_time>0 && (wr_time == -1 || best_user_time<wr_time)) {
            m_hunt_against_time = best_user_time;
            hunted              = "you";
        }
        else if (wr_time>0) {
            m_hunt_against_time = wr_time;
            hunted              = ratingMgr->getBestScoreHolder(curProxy, difficulty);
        }

        // STATUSBAR->set_timerstart(-m_hunt_against_time);
    }
    return hunted;
}

void Client::tick (double dtime)
{
    const double timestep = 0.01; // 10ms

    switch (m_state) {
    case cls_idle:
        break;

    case cls_preparing_game: {
        video::TransitionEffect *fx = m_effect.get();
        if (fx && !fx->finished()) {
            fx->tick (dtime);
        }
        else {
            m_effect.reset();
            server::Msg_StartGame();

            m_state = cls_game;
            m_timeaccu = 0;
            m_total_game_time = 0;
            sdl::FlushEvents();
        }
        break;
    }

    case cls_game:
        if (app.state->getInt("NextLevelMode") == lev::NEXT_LEVEL_NOT_BEST) {
            int old_second = round_nearest<int> (m_total_game_time);
            int second     = round_nearest<int> (m_total_game_time + dtime);

            if (m_hunt_against_time && old_second <= m_hunt_against_time) {
                if (second > m_hunt_against_time) { // happens exactly once when par has passed by
                    lev::Index *ind = lev::Index::getCurrentIndex();
                    lev::ScoreManager *scm = lev::ScoreManager::instance();
                    lev::Proxy *curProxy = ind->getCurrent();
                    lev::RatingManager *ratingMgr = lev::RatingManager::instance();
                    int    difficulty     = app.state->getInt("Difficulty");
                    int    wr_time        = ratingMgr->getBestScore(curProxy, difficulty);
                    int    best_user_time = scm->getBestUserScore(curProxy, difficulty);
                    string message;

                    if (wr_time>0 && (best_user_time<0 || best_user_time>wr_time)) {
                        message = string(_("Too slow for ")) +
                            ratingMgr->getBestScoreHolder(curProxy, difficulty) +
                            "... Ctrl-A";
                    }
                    else {
                        message = string(_("You are slow today ... Ctrl-A"));
                    }

                    client::Msg_PlaySound("shatter", 1.0);
                    Msg_ShowText(message, true, 2.0);
                }
                else {
                    if (old_second<second && // tick every second
                        (second >= (m_hunt_against_time-5) || // at least 5 seconds
                         second >= round_nearest<int> (m_hunt_against_time*.8))) // or the last 20% before par
                    {
                        client::Msg_PlaySound("pickup", 1.0);
                    }
                }
            }
        }

        m_total_game_time += dtime;
        STATUSBAR->set_time (m_total_game_time);
        // fall through
    case cls_finished: {
        m_timeaccu += dtime;
        for (;m_timeaccu >= timestep; m_timeaccu -= timestep) {
            display::Tick (timestep);
        }
        display::Redraw(video::GetScreen());
        handle_events();
        break;
    }

    case cls_gamemenu:
        break;
    case cls_gamehelp:
        break;
    case cls_abort:
        break;
    case cls_error:
        {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                switch (e.type) {
                    case SDL_QUIT:
                        app.bossKeyPressed = true;
                        // fall through
                    case SDL_KEYDOWN:
                        client::Msg_Command("abort");
                        break;
                }
            }
        }
        break;
    }
}

void Client::level_finished()
{
    lev::Index *ind = lev::Index::getCurrentIndex();
    lev::ScoreManager *scm = lev::ScoreManager::instance();
    lev::Proxy *curProxy = ind->getCurrent();
    lev::RatingManager *ratingMgr = lev::RatingManager::instance();
    int    difficulty     = app.state->getInt("Difficulty");
    int    wr_time       = ratingMgr->getBestScore(curProxy, difficulty);
    int    best_user_time = scm->getBestUserScore(curProxy, difficulty);
    int par_time   = ratingMgr->getParScore(curProxy, difficulty);

    int    level_time     = round_nearest<int> (m_total_game_time);

    string      text;
    bool        timehunt_restart = false;

    std::string par_name  = ratingMgr->getBestScoreHolder(curProxy, difficulty);
    for (int cut = 2; par_name.length() > 55; cut++)
        par_name  = ratingMgr->getBestScoreHolder(curProxy, difficulty, cut);

    if (wr_time > 0) {
        if (best_user_time<0 || best_user_time>wr_time) {
            if (level_time == wr_time)
                text = string(_("Exactly the world record of "))+par_name+"!";
            else if (level_time<wr_time)
                text = _("Great! A new world record!");
        }
    }
    if (text.length() == 0 && best_user_time>0) {
        if (level_time == best_user_time) {
            text = _("Again your personal record...");
            if (app.state->getInt("NextLevelMode") == lev::NEXT_LEVEL_NOT_BEST)
                timehunt_restart = true; // when hunting yourself: Equal is too slow
        }
        else if (level_time<best_user_time) {
            if (par_time >= 0 && level_time <= par_time)
                text = _("New personal record - better than par!");
            // Uncomment the following lines to show the "but over par!"-part.
            // (This has been criticised as demoralizing.)
            //else if (par_time >= 0)
            //    text = _("New personal record, but over par!");
            else
                text = _("New personal record!");
        }
    }

    if (app.state->getInt("NextLevelMode") == lev::NEXT_LEVEL_NOT_BEST &&
        (wr_time>0 || best_user_time>0))
    {
        bool with_par = best_user_time == -1 || (wr_time >0 && wr_time<best_user_time);
        int  behind   = level_time - (with_par ? wr_time : best_user_time);

        if (behind>0) {
            if (best_user_time>0 && level_time<best_user_time && with_par) {
                text = _("Your record, ");
            }
            else {
                text = "";
            }
            text += ecl::timeformat(behind);
            if (with_par)
                text += _("behind world record.");
            else
                text += _("behind your record.");

            timehunt_restart = true; // time hunt failed -> repeat level
        }
    }

    if (text.length() == 0) {
        if (par_time >= 0 && level_time <= par_time)
            text = _("Level finished - better than par!");
        // Uncomment the following lines to show the "but over par!"-part.
        // (This has been criticised as demoralizing.)
        //else if (par_time >= 0)
        //    text = _("Level finished, but over par!");
        else
            text = _("Level finished!");
    }
    if (m_cheater)
        text += _(" Cheater!");

    Msg_ShowText(text, false);

    if (!m_cheater) {
        scm->updateUserScore(curProxy, difficulty, level_time);

        // save score (just in case Enigma crashes when loading next level)
        lev::ScoreManager::instance()->save();

    }

    if (timehunt_restart)
        server::Msg_Command("restart");
    else
        m_state = cls_finished;
}

void Client::level_loaded(bool isRestart)
{
    lev::Index *ind = lev::Index::getCurrentIndex();
    lev::ScoreManager *scm = lev::ScoreManager::instance();
    lev::Proxy *curProxy = ind->getCurrent();

    // update window title
    video::SetCaption(ecl::strf(_("Enigma pack %s - level #%d: %s"), ind->getName().c_str(),
            ind->getCurrentLevel(), curProxy->getTitle().c_str()).c_str());

    std::string hunted = init_hunted_time();   // sets m_hunt_against_time (used below)
    documentHistory.clear();
    consoleIndex = 0;

    // show level information (name, author, etc.)
    std::string displayed_info = "";
    if (m_hunt_against_time>0) {
        if (hunted == "you")
            displayed_info = _("Your record: ");
        else
            displayed_info = _("World record to beat: ");
        displayed_info += ecl::timeformat(m_hunt_against_time);
//+ _(" by ") +hunted;
// makes the string too long in many levels
        Msg_ShowDocument(displayed_info, true, 4.0);
    } else {
        displayed_info = displayedLevelInfo(curProxy);
        Msg_ShowDocument(displayed_info, true, 2.0);
    }

    sound::StartLevelMusic();

    // start screen transition
    GC gc(video::BackBuffer());
    display::DrawAll(gc);

    m_effect.reset (video::MakeEffect ((isRestart ? video::TM_SQUARES :
            video::TM_PUSH_RANDOM), video::BackBuffer()));
    m_cheater = false;
    m_state   = cls_preparing_game;
}


void Client::handle_message (Message *m) { // @@@ unused
    switch (m->type) {
    case CLMSG_LEVEL_LOADED:

        break;
    default:
        fprintf (stderr, "Unhandled client event: %d\n", m->type);
        break;
    }
}

void Client::error (const string &text)
{
    m_error_message = text;
    m_state = cls_error;
    draw_screen();
}

void Client::registerDocument(std::string text) {
    documentHistory.push_back(text);
    consoleIndex = -1;
}

void Client::finishedText() {
    consoleIndex = 0;
}

/* -------------------- Functions -------------------- */

void client::ClientInit() {
    CLIENT.init();
}

void client::ClientShutdown() {
    CLIENT.shutdown();
}

bool client::NetworkStart()
{
    return CLIENT.network_start();
}

void client::Msg_LevelLoaded(bool isRestart)
{
    CLIENT.level_loaded(isRestart);
}

void client::Tick (double dtime) {
    CLIENT.tick (dtime);
    sound::Tick (dtime);
}

void client::Stop() {
    CLIENT.stop ();
}

void client::Msg_AdvanceLevel (lev::LevelAdvanceMode mode) {

    lev::Index *ind = lev::Index::getCurrentIndex();
    // log last played level
    lev::PersistentIndex::addCurrentToHistory();

    if (ind->advanceLevel(mode)) {
        // now we may advance
        server::Msg_LoadLevel(ind->getCurrent(), false);
    }
    else
        client::Msg_Command("abort");
}

void client::Msg_JumpBack() {
    // log last played level
    lev::PersistentIndex::addCurrentToHistory();
    server::Msg_JumpBack();
}

bool client::AbortGameP() {
    return CLIENT.abort_p();
}

void client::Msg_Command(const string& cmd) {
    if (cmd == "abort") {
        CLIENT.abort();
    }
    else if (cmd == "level_finished") {
        client::Msg_PlaySound("finished", 1.0);
        CLIENT.level_finished();
    }
    else if (cmd == "cheater") {
        CLIENT.mark_cheater();
    }
    else if (cmd == "easy_going") {
        CLIENT.easy_going();
    }
    else {
        enigma::Log << "Warning: Client received unknown command '" << cmd << "'\n";
    }
}

void client::Msg_PlayerPosition (unsigned iplayer, const V2 &pos)
{
    if (iplayer == (unsigned)player::CurrentPlayer()) {
        sound::SetListenerPosition (pos);
        display::SetReferencePoint (pos);
    }
}

void client::Msg_PlaySound (const std::string &wavfile,
                            const ecl::V2 &pos,
                            double relative_volume)
{
    sound::EmitSoundEvent (wavfile.c_str(), pos, relative_volume);
}

void client::Msg_PlaySound (const std::string &wavfile, double relative_volume)
{
    sound::EmitSoundEvent (wavfile.c_str(), V2(), relative_volume);
}

void client::Msg_Sparkle (const ecl::V2 &pos) {
    display::AddEffect (pos, "ring-anim", true);
}


void client::Msg_ShowText(const std::string &text, bool scrolling, double duration) {
    STATUSBAR->show_text (text, scrolling, duration);
}

void client::Msg_ShowDocument(const std::string &text, bool scrolling, double duration) {
    CLIENT.registerDocument(text);
    Msg_ShowText(text, scrolling, duration);
}

void client::Msg_FinishedText() {
    CLIENT.finishedText();
}

void client::Msg_Error (const std::string &text)
{
    CLIENT.error (text);
}
