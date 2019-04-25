/* Copyright 2019 Pierre Ossman <ossman@cendio.se> for Cendio AB
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#if ! (defined(WIN32) || defined(__APPLE__))
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XI2.h>
#endif

#include <FL/Fl.H>
#include <FL/x.H>

#include <rfb/LogWriter.h>

#include "touch.h"

static rfb::LogWriter vlog("Touch");

#if !defined(WIN32) && !defined(__APPLE__)
static int xi_major;
#endif

#if !defined(WIN32) && !defined(__APPLE__)
static int handleXinputEvent(void *event, void *data)
{
  XEvent *xevent = (XEvent*)event;

  if (xevent->type == MapNotify) {
    XIEventMask eventmask;
    unsigned char flags[XIMaskLen(XI_LASTEVENT)] = { 0 };

    eventmask.deviceid = XIAllMasterDevices;
    eventmask.mask_len = sizeof(flags);
    eventmask.mask = flags;

    XISetMask(flags, XI_ButtonPress);
    XISetMask(flags, XI_Motion);
    XISetMask(flags, XI_ButtonRelease);
    /*
    XISetMask(flags, XI_TouchBegin);
    XISetMask(flags, XI_TouchUpdate);
    XISetMask(flags, XI_TouchEnd);
    */

    XISelectEvents(fl_display, xevent->xmap.window, &eventmask, 1);

    // Fall through as we don't want to interfere with whatever someone
    // else might want to do with this event

  } else if (xevent->type == GenericEvent) {
    if (xevent->xgeneric.extension == xi_major) {
      XIDeviceEvent *devev;
      XEvent fakeEvent;

      if (!XGetEventData(fl_display, &xevent->xcookie)) {
        vlog.error("Failed to get event data for X Input event");
        return 1;
      }

      devev = (XIDeviceEvent*)xevent->xcookie.data;

      // FLTK doesn't understand X Input events, and we've stopped
      // delivery of Core events by enabling the X Input ones. Make
      // FLTK happy by faking Core events based on the X Input ones.

      memset(&fakeEvent, 0, sizeof(XEvent));

      switch (xevent->xgeneric.evtype) {
      case XI_Motion:
        fakeEvent.type = MotionNotify;
        fakeEvent.xmotion.is_hint = False;
        break;
      case XI_ButtonPress:
        fakeEvent.type = ButtonPress;
        fakeEvent.xbutton.button = devev->detail;
        break;
      case XI_ButtonRelease:
        fakeEvent.type = ButtonRelease;
        fakeEvent.xbutton.button = devev->detail;
        break;
      }

      // XButtonEvent and XMotionEvent are almost identical, so we
      // don't have to care which it is for these fields
      fakeEvent.xbutton.serial = xevent->xgeneric.serial;
      fakeEvent.xbutton.display = xevent->xgeneric.display;
      fakeEvent.xbutton.window = devev->event;
      fakeEvent.xbutton.root = devev->root;
      fakeEvent.xbutton.subwindow = devev->child;
      fakeEvent.xbutton.time = devev->time;
      fakeEvent.xbutton.x = devev->event_x;
      fakeEvent.xbutton.y = devev->event_y;
      fakeEvent.xbutton.x_root = devev->root_x;
      fakeEvent.xbutton.y_root = devev->root_y;
      fakeEvent.xbutton.state = devev->mods.effective;
      fakeEvent.xbutton.state |= ((devev->buttons.mask[0] >> 1) & 0x1f) << 8;
      fakeEvent.xbutton.same_screen = True; // FIXME

      XFreeEventData(fl_display, &xevent->xcookie);

      fl_handle(fakeEvent);

      return 1;
    }
  }

  return 0;
}
#endif

void enable_touch()
{
#if !defined(WIN32) && !defined(__APPLE__)
  int ev, err;
  int major_ver, minor_ver;

  fl_open_display();

  if (!XQueryExtension(fl_display, "XInputExtension", &xi_major, &ev, &err)) {
    vlog.error("X Input extension not available.");
    // FIXME: fatal
    return;
  }

  major_ver = 2;
  minor_ver = 2;
  if (XIQueryVersion(fl_display, &major_ver, &minor_ver) != Success) {
    vlog.error("X Input 2 (or newer) is not available.");
    // FIXME: fatal
    return;
  }

  if ((major_ver == 2) && (minor_ver < 2))
    vlog.error("X Input 2.2 (or newer) is not available. Touch gestures will not be supported.");

  Fl::add_system_handler(handleXinputEvent, NULL);
#endif
}

void disable_touch()
{
#if !defined(WIN32) && !defined(__APPLE__)
  Fl::remove_system_handler(handleXinputEvent);
#endif
}
