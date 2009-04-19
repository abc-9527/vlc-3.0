/**
 * @file common.c
 * @brief Common code for XCB video output plugins
 */
/*****************************************************************************
 * Copyright © 2009 Rémi Denis-Courmont
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2.0
 * of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 ****************************************************************************/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <assert.h>

#include <sys/types.h>
#include <sys/shm.h>

#include <xcb/xcb.h>
#include <xcb/shm.h>

#include <vlc_common.h>
#include <vlc_vout.h>
#include <vlc_window.h>

#include "xcb_vlc.h"

/**
 * Connect to the X server.
 */
xcb_connection_t *Connect (vlc_object_t *obj)
{
    char *display = var_CreateGetNonEmptyString (obj, "x11-display");
    xcb_connection_t *conn = xcb_connect (display, NULL);

    free (display);
    if (xcb_connection_has_error (conn) /*== NULL*/)
    {
        msg_Err (obj, "cannot connect to X server");
        xcb_disconnect (conn);
        return NULL;
    }
    return conn;
}


/**
 * Create a VLC video X window object, find the corresponding X server screen,
 * and probe the MIT-SHM extension.
 */
vout_window_t *GetWindow (vout_thread_t *obj,
                          xcb_connection_t *conn,
                          const xcb_screen_t **restrict pscreen,
                          bool *restrict pshm)
{
    /* Get window */
    xcb_window_t root;
    vout_window_t *wnd = vout_RequestXWindow (obj, &(int){ 0 }, &(int){ 0 },
                                        &(unsigned){ 0 }, &(unsigned){ 0 });
    if (wnd == NULL)
    {
        msg_Err (obj, "parent window not available");
        return NULL;
    }
    else
    {
        xcb_get_geometry_reply_t *geo;
        xcb_get_geometry_cookie_t ck;

        ck = xcb_get_geometry (conn, wnd->handle.xid);
        geo = xcb_get_geometry_reply (conn, ck, NULL);
        if (geo == NULL)
        {
            msg_Err (obj, "parent window not valid");
            goto error;
        }
        root = geo->root;
        free (geo);

        /* Subscribe to parent window resize events */
        const uint32_t value = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
        xcb_change_window_attributes (conn, wnd->handle.xid,
                                      XCB_CW_EVENT_MASK, &value);
    }

    /* Find the selected screen */
    const xcb_setup_t *setup = xcb_get_setup (conn);
    xcb_screen_t *screen = NULL;
    for (xcb_screen_iterator_t i = xcb_setup_roots_iterator (setup);
         i.rem > 0 && screen == NULL; xcb_screen_next (&i))
    {
        if (i.data->root == root)
            screen = i.data;
    }

    if (screen == NULL)
    {
        msg_Err (obj, "parent window screen not found");
        goto error;
    }
    msg_Dbg (obj, "using screen 0x%"PRIx32, root);

    /* Check MIT-SHM shared memory support */
    bool shm = var_CreateGetBool (obj, "x11-shm") > 0;
    if (shm)
    {
        xcb_shm_query_version_cookie_t ck;
        xcb_shm_query_version_reply_t *r;

        ck = xcb_shm_query_version (conn);
        r = xcb_shm_query_version_reply (conn, ck, NULL);
        if (!r)
        {
            msg_Err (obj, "shared memory (MIT-SHM) not available");
            msg_Warn (obj, "display will be slow");
            shm = false;
        }
        free (r);
    }

    *pscreen = screen;
    *pshm = shm;
    return wnd;

error:
    vout_ReleaseWindow (wnd);
    return NULL;
}

/**
 * Gets the size of an X window.
 */
int GetWindowSize (struct vout_window_t *wnd, xcb_connection_t *conn,
                   unsigned *restrict width, unsigned *restrict height)
{
    xcb_get_geometry_cookie_t ck = xcb_get_geometry (conn, wnd->handle.xid);
    xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply (conn, ck, NULL);

    if (!geo)
        return -1;

    *width = geo->width;
    *height = geo->height;
    free (geo);
    return 0;
}
