/* Copyright 2019 Pierre Ossman <ossman@cendio.se> for Cendio AB
 * Copyright 2019 Aaron Sowry for Cendio AB
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

static bool tracking_touch, grabbed;
static int touch_id;
#endif

#if !defined(WIN32) && !defined(__APPLE__)
int xi2_grabDevices(Window window)
{
  int ret, ndevices;

  XIEventMask eventmask;
  XIDeviceInfo *devices, *device;

  unsigned char flags[XIMaskLen(XI_LASTEVENT)] = { 0 };

  XISetMask(flags, XI_ButtonPress);
  XISetMask(flags, XI_Motion);
  XISetMask(flags, XI_ButtonRelease);
  XISetMask(flags, XI_TouchBegin);
  XISetMask(flags, XI_TouchUpdate);
  XISetMask(flags, XI_TouchEnd);

  eventmask.mask = flags;
  eventmask.mask_len = sizeof(flags);

  devices = XIQueryDevice(fl_display, XIAllMasterDevices, &ndevices);

  for (int i = 0; i < ndevices; i++) {
    device = &devices[i];

    if (device->use != XIMasterPointer)
      continue;

    eventmask.deviceid = device->deviceid;

    ret = XIGrabDevice(fl_display,
                       device->deviceid,
                       window,
                       CurrentTime,
                       None,
                       XIGrabModeAsync,
                       XIGrabModeAsync,
                       True,
                       &eventmask);

    if (ret) {
      if (ret == XIAlreadyGrabbed)
        continue;
      else {
        vlog.error("Failure grabbing mouse");
        return ret;
      }
    }
  }

  XIFreeDeviceInfo(devices);

  grabbed = true;

  return XIGrabSuccess;
}

void xi2_ungrabDevices()
{
  int ndevices;
  XIDeviceInfo *devices, *device;

  devices = XIQueryDevice(fl_display, XIAllMasterDevices, &ndevices);

  for (int i = 0; i < ndevices; i++) {
    device = &devices[i];

    if (device->use != XIMasterPointer)
      continue;

    XIUngrabDevice(fl_display, device->deviceid, CurrentTime);
  }

  XIFreeDeviceInfo(devices);

  grabbed = false;
}

static void prepXEvent(XEvent* dst, const XIDeviceEvent* src)
{
  // XButtonEvent and XMotionEvent are almost identical, so we
  // don't have to care which it is for these fields
  dst->xbutton.serial = src->serial;
  dst->xbutton.display = src->display;
  dst->xbutton.window = src->event;
  dst->xbutton.root = src->root;
  dst->xbutton.subwindow = src->child;
  dst->xbutton.time = src->time;
  dst->xbutton.x = src->event_x;
  dst->xbutton.y = src->event_y;
  dst->xbutton.x_root = src->root_x;
  dst->xbutton.y_root = src->root_y;
  dst->xbutton.state = src->mods.effective;
  dst->xbutton.state |= ((src->buttons.mask[0] >> 1) & 0x1f) << 8;
  dst->xbutton.same_screen = True; // FIXME

  if (tracking_touch)
    dst->xbutton.state |= Button1 << 8;
}

static void fakeMotionEvent(const XIDeviceEvent* origEvent)
{
  XEvent fakeEvent;

  memset(&fakeEvent, 0, sizeof(XEvent));

  fakeEvent.type = MotionNotify;
  fakeEvent.xmotion.is_hint = False;
  prepXEvent(&fakeEvent, origEvent);

  fl_handle(fakeEvent);
}

static void fakeButtonEvent(bool press, int button,
                            const XIDeviceEvent* origEvent)
{
  XEvent fakeEvent;

  memset(&fakeEvent, 0, sizeof(XEvent));

  fakeEvent.type = press ? ButtonPress : ButtonRelease;
  fakeEvent.xbutton.button = button;
  prepXEvent(&fakeEvent, origEvent);

  fl_handle(fakeEvent);
}

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

    XISetMask(flags, XI_TouchBegin);
    XISetMask(flags, XI_TouchUpdate);
    XISetMask(flags, XI_TouchEnd);

    XISelectEvents(fl_display, xevent->xmap.window, &eventmask, 1);

    // Fall through as we don't want to interfere with whatever someone
    // else might want to do with this event

  } else if (xevent->type == GenericEvent) {
    if (xevent->xgeneric.extension == xi_major) {
      XIDeviceEvent *devev;

      if (!XGetEventData(fl_display, &xevent->xcookie)) {
        vlog.error("Failed to get event data for X Input event");
        return 1;
      }

      devev = (XIDeviceEvent*)xevent->xcookie.data;

      // FLTK doesn't understand X Input events, and we've stopped
      // delivery of Core events by enabling the X Input ones. Make
      // FLTK happy by faking Core events based on the X Input ones.

      switch (xevent->xgeneric.evtype) {
      case XI_Motion:
        fakeMotionEvent(devev);
        break;
      case XI_ButtonPress:
        fakeButtonEvent(true, devev->detail, devev);
        break;
      case XI_ButtonRelease:
        fakeButtonEvent(false, devev->detail, devev);
        break;
      case XI_TouchBegin:
        if (tracking_touch)
          break;
        if (grabbed)
          XIAllowTouchEvents(fl_display,
                             devev->deviceid,
                             devev->detail,
                             devev->event,
                             XIAcceptTouch);
        fakeMotionEvent(devev);
        tracking_touch = true;
        touch_id = devev->detail;
        fakeButtonEvent(true, Button1, devev);
        break;
      case XI_TouchUpdate:
        if (!tracking_touch || devev->detail != touch_id)
          break;
        fakeMotionEvent(devev);
        break;
      case XI_TouchEnd:
        if (!tracking_touch || devev->detail != touch_id)
          break;
        fakeMotionEvent(devev);
        tracking_touch = false;
        fakeButtonEvent(false, Button1, devev);
        break;
      }

      XFreeEventData(fl_display, &xevent->xcookie);

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

  tracking_touch = grabbed = false;
#endif
}

void disable_touch()
{
#if !defined(WIN32) && !defined(__APPLE__)
  Fl::remove_system_handler(handleXinputEvent);
#endif
}

