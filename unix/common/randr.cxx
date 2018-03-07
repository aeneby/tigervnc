/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright 2009-2017 Pierre Ossman for Cendio AB
 * Copyright 2014 Brian P. Hinz
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

#include <stdlib.h>

#include <unixcommon.h>
#include <rfb/screenTypes.h>
#include <rfb/LogWriter.h>
#include <RandrGlue.h>
static rfb::LogWriter vlog("RandR");

rfb::ScreenSet computeScreenLayout(int screenIndex, OutputIdMap *outputIdMap)
{
  rfb::ScreenSet layout;
  OutputIdMap newIdMap;

  for (int i = 0;i < vncRandRGetOutputCount(screenIndex);i++) {
    unsigned int outputId;
    int x, y, width, height;

    /* Disabled? */
    if (!vncRandRIsOutputEnabled(screenIndex, i))
      continue;

    outputId = vncRandRGetOutputId(screenIndex, i);

    /* Known output? */
    if (outputIdMap->count(outputId) == 1)
      newIdMap[outputId] = (*outputIdMap)[outputId];
    else {
      rdr::U32 id;
      OutputIdMap::const_iterator iter;

      while (true) {
        id = rand();
        for (iter = outputIdMap->begin();iter != outputIdMap->end();++iter) {
          if (iter->second == id)
            break;
        }
        if (iter == outputIdMap->end())
          break;
      }

      newIdMap[outputId] = id;
    }

    vncRandRGetOutputDimensions(screenIndex, i, &x, &y, &width, &height);

    layout.add_screen(rfb::Screen(newIdMap[outputId], x, y, width, height, 0));
  }

  /* Only keep the entries that are currently active */
  *outputIdMap = newIdMap;

  /*
   * Make sure we have something to display. Hopefully it's just temporary
   * that we have no active outputs...
   */
  if (layout.num_screens() == 0)
    layout.add_screen(rfb::Screen(0, 0, 0, vncGetScreenWidth(screenIndex),
                                  vncGetScreenHeight(screenIndex), 0));

  return layout;
}

unsigned int setScreenLayout(int screenIndex,
                             int fb_width, int fb_height, const rfb::ScreenSet& layout,
                             OutputIdMap *outputIdMap)
{
  int ret;
  int availableOutputs;

  // RandR support?
  if (vncRandRGetOutputCount(screenIndex) == 0)
    return rfb::resultProhibited;

  /*
   * First check that we don't have any active clone modes. That's just
   * too messy to deal with.
   */
  if (vncRandRHasOutputClones(screenIndex)) {
    vlog.error("Clone mode active. Refusing to touch screen layout.");
    return rfb::resultInvalid;
  }

  /* Next count how many useful outputs we have... */
  availableOutputs = vncRandRGetAvailableOutputs(screenIndex);

  /* Try to create more outputs if needed... (only works on Xvnc) */
  if (layout.num_screens() > availableOutputs) {
    vlog.debug("Insufficient screens. Need to create %d more.",
               layout.num_screens() - availableOutputs);
    ret = vncRandRCreateOutputs(screenIndex,
                                layout.num_screens() - availableOutputs);
    if (ret < 0) {
      vlog.error("Unable to create more screens, as needed by the new client layout.");
      return rfb::resultInvalid;
    }
  }

  /* First we might need to resize the screen */
  if ((fb_width != vncGetScreenWidth(screenIndex)) ||
      (fb_height != vncGetScreenHeight(screenIndex))) {
    ret = vncRandRResizeScreen(screenIndex, fb_width, fb_height);
    if (!ret) {
      vlog.error("Failed to resize screen to %dx%d", fb_width, fb_height);
      return rfb::resultInvalid;
    }
  }

  /* Next, reconfigure all known outputs, and turn off the other ones */
  for (int i = 0;i < vncRandRGetOutputCount(screenIndex);i++) {
    unsigned int output;

    rfb::ScreenSet::const_iterator iter;

    output = vncRandRGetOutputId(screenIndex, i);

    /* Known? */
    if (outputIdMap->count(output) == 0)
      continue;

    /* Find the corresponding screen... */
    for (iter = layout.begin();iter != layout.end();++iter) {
      if (iter->id == (*outputIdMap)[output])
        break;
    }

    /* Missing? */
    if (iter == layout.end()) {
      /* Disable and move on... */
      ret = vncRandRDisableOutput(screenIndex, i);
      if (!ret) {
        char *name = vncRandRGetOutputName(screenIndex, i);
        vlog.error("Failed to disable unused output '%s'",
                   name);
        free(name);
        return rfb::resultInvalid;
      }
      outputIdMap->erase(output);
      continue;
    }

    /* Reconfigure new mode and position */
    ret = vncRandRReconfigureOutput(screenIndex, i,
                                    iter->dimensions.tl.x,
                                    iter->dimensions.tl.y,
                                    iter->dimensions.width(),
                                    iter->dimensions.height());
    if (!ret) {
      char *name = vncRandRGetOutputName(screenIndex, i);
      vlog.error("Failed to reconfigure output '%s' to %dx%d+%d+%d",
                 name,
                 iter->dimensions.width(), iter->dimensions.height(),
                 iter->dimensions.tl.x, iter->dimensions.tl.y);
      free(name);
      return rfb::resultInvalid;
    }
  }

  /* Finally, allocate new outputs for new screens */
  rfb::ScreenSet::const_iterator iter;
  for (iter = layout.begin();iter != layout.end();++iter) {
    OutputIdMap::const_iterator oi;
    unsigned int output;
    int i;

    /* Does this screen have an output already? */
    for (oi = outputIdMap->begin();oi != outputIdMap->end();++oi) {
      if (oi->second == iter->id)
        break;
    }

    if (oi != outputIdMap->end())
      continue;

    /* Find an unused output */
    for (i = 0;i < vncRandRGetOutputCount(screenIndex);i++) {
      output = vncRandRGetOutputId(screenIndex, i);

      /* In use? */
      if (outputIdMap->count(output) == 1)
        continue;

      /* Can it be used? */
      if (!vncRandRIsOutputUsable(screenIndex, i))
        continue;

      break;
    }

    /* Shouldn't happen */
    if (i == vncRandRGetOutputCount(screenIndex))
      return rfb::resultInvalid;

    /*
     * Make sure we already have an entry for this, or
     * computeScreenLayout() will think it is a brand new output and
     * assign it a random id.
     */
    (*outputIdMap)[output] = iter->id;

    /* Reconfigure new mode and position */
    ret = vncRandRReconfigureOutput(screenIndex, i,
                                    iter->dimensions.tl.x,
                                    iter->dimensions.tl.y,
                                    iter->dimensions.width(),
                                    iter->dimensions.height());
    if (!ret) {
      char *name = vncRandRGetOutputName(screenIndex, i);
      vlog.error("Failed to reconfigure output '%s' to %dx%d+%d+%d",
                 name,
                 iter->dimensions.width(), iter->dimensions.height(),
                 iter->dimensions.tl.x, iter->dimensions.tl.y);
      free(name);
      return rfb::resultInvalid;
    }
  }

  /*
   * Update timestamp for when screen layout was last changed.
   * This is normally done in the X11 request handlers, which is
   * why we have to deal with it manually here.
   */
  vncRandRUpdateSetTime(screenIndex);

  return rfb::resultSuccess;
}
