/*****************************************************************************
 * vout_directx.c: Windows DirectX video output display method
 *****************************************************************************
 * Copyright (C) 2001 VideoLAN
 * $Id: vout_directx.c,v 1.36.2.1 2002/07/29 16:22:14 gbazin Exp $
 *
 * Authors: Gildas Bazin <gbazin@netcourrier.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble:
 *
 * This plugin will use YUV overlay if supported, using overlay will result in
 * the best video quality (hardware interpolation when rescaling the picture)
 * and the fastest display as it requires less processing.
 *
 * If YUV overlay is not supported this plugin will use RGB offscreen video
 * surfaces that will be blitted onto the primary surface (display) to
 * effectively display the pictures. This fallback method also enables us to
 * display video in window mode.
 * 
 *****************************************************************************/
#include <errno.h>                                                 /* ENOMEM */
#include <stdlib.h>                                                /* free() */
#include <string.h>                                            /* strerror() */

#include <videolan/vlc.h>

#include <ddraw.h>

#include "netutils.h"

#include "video.h"
#include "video_output.h"

#include "interface.h"

#include "vout_directx.h"

/*****************************************************************************
 * DirectDraw GUIDs.
 * Defining them here allows us to get rid of the dxguid library during
 * the linking stage.
 *****************************************************************************/
#include <initguid.h>
DEFINE_GUID( IID_IDirectDraw2, 0xB3A6F3E0,0x2B43,0x11CF,0xA2,0xDE,0x00,0xAA,0x00,0xB9,0x33,0x56 );
DEFINE_GUID( IID_IDirectDrawSurface2, 0x57805885,0x6eec,0x11cf,0x94,0x41,0xa8,0x23,0x03,0xc1,0x0e,0x27 );

/*****************************************************************************
 * Local prototypes.
 *****************************************************************************/
static int  vout_Create    ( vout_thread_t * );
static void vout_Destroy   ( vout_thread_t * );
static int  vout_Init      ( vout_thread_t * );
static void vout_End       ( vout_thread_t * );
static int  vout_Manage    ( vout_thread_t * );
static void vout_Render    ( vout_thread_t *, picture_t * );
static void vout_Display   ( vout_thread_t *, picture_t * );

static int  NewPictureVec  ( vout_thread_t *, picture_t *, int );
static void FreePictureVec ( vout_thread_t *, picture_t *, int );
static int  UpdatePictureStruct( vout_thread_t *, picture_t *, int );

static int  DirectXInitDDraw      ( vout_thread_t *p_vout );
static void DirectXCloseDDraw     ( vout_thread_t *p_vout );
static int  DirectXCreateDisplay  ( vout_thread_t *p_vout );
static void DirectXCloseDisplay   ( vout_thread_t *p_vout );
static int  DirectXCreateSurface  ( vout_thread_t *p_vout,
                                    LPDIRECTDRAWSURFACE2 *, int, int, int );
static void DirectXCloseSurface   ( vout_thread_t *p_vout,
                                    LPDIRECTDRAWSURFACE2 );
static int  DirectXCreateClipper  ( vout_thread_t *p_vout );
static void DirectXGetDDrawCaps   ( vout_thread_t *p_vout );
static int  DirectXGetSurfaceDesc ( picture_t *p_pic );

/*****************************************************************************
 * Functions exported as capabilities. They are declared as static so that
 * we don't pollute the namespace too much.
 *****************************************************************************/
void _M( vout_getfunctions )( function_list_t * p_function_list )
{
    p_function_list->functions.vout.pf_create     = vout_Create;
    p_function_list->functions.vout.pf_init       = vout_Init;
    p_function_list->functions.vout.pf_end        = vout_End;
    p_function_list->functions.vout.pf_destroy    = vout_Destroy;
    p_function_list->functions.vout.pf_manage     = vout_Manage;
    p_function_list->functions.vout.pf_render     = vout_Render;
    p_function_list->functions.vout.pf_display    = vout_Display;
}

/*****************************************************************************
 * vout_Create: allocate DirectX video thread output method
 *****************************************************************************
 * This function allocates and initialize the DirectX vout method.
 *****************************************************************************/
static int vout_Create( vout_thread_t *p_vout )
{
    /* Allocate structure */
    p_vout->p_sys = malloc( sizeof( vout_sys_t ) );
    if( p_vout->p_sys == NULL )
    {
        intf_ErrMsg( "vout error: can't create p_sys (%s)", strerror(ENOMEM) );
        return 1;
    }

    /* Initialisations */
    p_vout->p_sys->p_ddobject = NULL;
    p_vout->p_sys->p_display = NULL;
    p_vout->p_sys->p_current_surface = NULL;
    p_vout->p_sys->p_clipper = NULL;
    p_vout->p_sys->hbrush = NULL;
    p_vout->p_sys->hwnd = NULL;
    p_vout->p_sys->i_changes = 0;
    p_vout->p_sys->b_event_thread_die = 0;
    p_vout->p_sys->b_caps_overlay_clipping = 0;
    SetRectEmpty( &p_vout->p_sys->rect_display );
    p_vout->p_sys->b_using_overlay = config_GetIntVariable( "overlay" );
    p_vout->p_sys->b_use_sysmem = config_GetIntVariable( "directx-use-sysmem");
    p_vout->p_sys->b_hw_yuv = config_GetIntVariable( "directx-hw-yuv" );

    p_vout->p_sys->b_cursor_hidden = 0;
    p_vout->p_sys->i_lastmoved = mdate();

    /* Set main window's size */
    p_vout->p_sys->i_window_width = p_vout->i_window_width;
    p_vout->p_sys->i_window_height = p_vout->i_window_height;

    /* Set locks and condition variables */
    vlc_mutex_init( &p_vout->p_sys->event_thread_lock );
    vlc_cond_init( &p_vout->p_sys->event_thread_wait );
    p_vout->p_sys->i_event_thread_status = THREAD_CREATE;

    /* Create the DirectXEventThread, this thread is created by us to isolate
     * the Win32 PeekMessage function calls. We want to do this because
     * Windows can stay blocked inside this call for a long time, and when
     * this happens it thus blocks vlc's video_output thread.
     * DirectXEventThread will take care of the creation of the video
     * window (because PeekMessage has to be called from the same thread which
     * created the window). */
    intf_WarnMsg( 3, "vout: vout_Create creating DirectXEventThread" );
    if( vlc_thread_create( &p_vout->p_sys->event_thread_id,
                           "DirectX Events Thread",
                           (void *) DirectXEventThread, (void *) p_vout) )
    {
        intf_ErrMsg( "vout error: can't create DirectXEventThread" );
        intf_ErrMsg("vout error: %s", strerror(ENOMEM));
        goto error;
    }

    /* We need to wait for the actual creation of the thread and window */
    vlc_mutex_lock( &p_vout->p_sys->event_thread_lock );
    if( p_vout->p_sys->i_event_thread_status == THREAD_CREATE )
    {
        vlc_cond_wait ( &p_vout->p_sys->event_thread_wait,
                        &p_vout->p_sys->event_thread_lock );
    }
    vlc_mutex_unlock( &p_vout->p_sys->event_thread_lock );
    if( p_vout->p_sys->i_event_thread_status != THREAD_READY )
    {
        intf_ErrMsg( "vout error: DirectXEventThread failed" );
        goto error;
    }

    intf_WarnMsg( 3, "vout: vout_Create DirectXEventThread running" );

    /* Initialise DirectDraw */
    if( DirectXInitDDraw( p_vout ) )
    {
        intf_ErrMsg( "vout error: can't initialise DirectDraw" );
        goto error;
    }

    /* Create the directx display */
    if( DirectXCreateDisplay( p_vout ) )
    {
        intf_ErrMsg( "vout error: can't initialise DirectDraw" );
        goto error;
    }

    return 0;

 error:
    vout_Destroy( p_vout );
    return 1;
}

/*****************************************************************************
 * vout_Init: initialize DirectX video thread output method
 *****************************************************************************
 * This function create the directx surfaces needed by the output thread.
 * It is called at the beginning of the thread.
 *****************************************************************************/
static int vout_Init( vout_thread_t *p_vout )
{
    int i_chroma_backup;

    /* Initialize the output structure.
     * Since DirectDraw can do rescaling for us, stick to the default
     * coordinates and aspect. */
    p_vout->output.i_width  = p_vout->render.i_width;
    p_vout->output.i_height = p_vout->render.i_height;
    p_vout->output.i_aspect = p_vout->render.i_aspect;

#define MAX_DIRECTBUFFERS 1
    /* Right now we use only 1 directbuffer because we don't want the
     * video decoder to decode directly into direct buffers as they are
     * created into video memory and video memory is _really_ slow */

    /* Choose the chroma we will try first. */
    switch( p_vout->render.i_chroma )
    {
        case FOURCC_YUY2:
        case FOURCC_YUNV:
            p_vout->output.i_chroma = FOURCC_YUY2;
            break;
        case FOURCC_UYVY:
        case FOURCC_UYNV:
        case FOURCC_Y422:
            p_vout->output.i_chroma = FOURCC_UYVY;
            break;
        case FOURCC_YVYU:
            p_vout->output.i_chroma = FOURCC_YVYU;
            break;
        default:
            p_vout->output.i_chroma = FOURCC_YV12;
            break;
    }

    NewPictureVec( p_vout, p_vout->p_picture, MAX_DIRECTBUFFERS );

    i_chroma_backup = p_vout->output.i_chroma;

    if( !I_OUTPUTPICTURES )
    {
        /* hmmm, it didn't work! Let's try commonly supported chromas */
        p_vout->output.i_chroma = FOURCC_YV12;
        NewPictureVec( p_vout, p_vout->p_picture, MAX_DIRECTBUFFERS );
        if( !I_OUTPUTPICTURES )
        {
            /* hmmm, it didn't work! Let's try commonly supported chromas */
            p_vout->output.i_chroma = FOURCC_YUY2;
            NewPictureVec( p_vout, p_vout->p_picture, MAX_DIRECTBUFFERS );
        }
    }

    if( !I_OUTPUTPICTURES )
    {
        /* If it still didn't work then don't try to use an overlay */
        p_vout->output.i_chroma = i_chroma_backup;
        p_vout->p_sys->b_using_overlay = 0;
        NewPictureVec( p_vout, p_vout->p_picture, MAX_DIRECTBUFFERS );
    }

    /* Change the window title bar text */
    if( p_vout->p_sys->b_using_overlay )
        SetWindowText( p_vout->p_sys->hwnd,
                       VOUT_TITLE " (hardware YUV overlay DirectX output)" );
    else if( p_vout->p_sys->b_hw_yuv )
        SetWindowText( p_vout->p_sys->hwnd,
                       VOUT_TITLE " (hardware YUV DirectX output)" );
    else SetWindowText( p_vout->p_sys->hwnd,
                        VOUT_TITLE " (software RGB DirectX output)" );

    return 0;
}

/*****************************************************************************
 * vout_End: terminate Sys video thread output method
 *****************************************************************************
 * Terminate an output method created by vout_Create.
 * It is called at the end of the thread.
 *****************************************************************************/
static void vout_End( vout_thread_t *p_vout )
{
    FreePictureVec( p_vout, p_vout->p_picture, I_OUTPUTPICTURES );
    return;
}

/*****************************************************************************
 * vout_Destroy: destroy Sys video thread output method
 *****************************************************************************
 * Terminate an output method created by vout_Create
 *****************************************************************************/
static void vout_Destroy( vout_thread_t *p_vout )
{
    intf_WarnMsg( 3, "vout: vout_Destroy" );

    DirectXCloseDisplay( p_vout );
    DirectXCloseDDraw( p_vout );

    /* Kill DirectXEventThread */
    vlc_mutex_lock( &p_vout->p_sys->event_thread_lock );
    p_vout->p_sys->b_event_thread_die = 1;

    if( p_vout->p_sys->i_event_thread_status == THREAD_READY )
    {
        /* we need to be sure DirectXEventThread won't stay stuck in
         * GetMessage, so we send a fake message */
        if( p_vout->p_sys->hwnd )
            PostMessage( p_vout->p_sys->hwnd, WM_NULL, 0, 0);

        vlc_mutex_unlock( &p_vout->p_sys->event_thread_lock );
        vlc_thread_join( p_vout->p_sys->event_thread_id );
    }
    else
        vlc_mutex_unlock( &p_vout->p_sys->event_thread_lock );

    if( p_vout->p_sys != NULL )
    {
        free( p_vout->p_sys );
        p_vout->p_sys = NULL;
    }
}

/*****************************************************************************
 * vout_Manage: handle Sys events
 *****************************************************************************
 * This function should be called regularly by the video output thread.
 * It returns a non null value if an error occured.
 *****************************************************************************/
static int vout_Manage( vout_thread_t *p_vout )
{
    WINDOWPLACEMENT window_placement;

    /* We used to call the Win32 PeekMessage function here to read the window
     * messages. But since window can stay blocked into this function for a
     * long time (for example when you move your window on the screen), I
     * decided to isolate PeekMessage in another thread. */

    /*
     * Scale Change 
     */
    if( p_vout->i_changes & VOUT_SCALE_CHANGE
        || p_vout->p_sys->i_changes & VOUT_SCALE_CHANGE)
    {
        intf_WarnMsg( 3, "vout: vout_Manage Scale Change" );
        if( !p_vout->p_sys->b_using_overlay )
            InvalidateRect( p_vout->p_sys->hwnd, NULL, TRUE );
        else
            DirectXUpdateOverlay( p_vout );
        p_vout->i_changes &= ~VOUT_SCALE_CHANGE;
        p_vout->p_sys->i_changes &= ~VOUT_SCALE_CHANGE;
    }

    /*
     * Size Change 
     */
    if( p_vout->i_changes & VOUT_SIZE_CHANGE
        || p_vout->p_sys->i_changes & VOUT_SIZE_CHANGE )
    {
        intf_WarnMsg( 3, "vout: vout_Manage Size Change" );
        if( !p_vout->p_sys->b_using_overlay )
            InvalidateRect( p_vout->p_sys->hwnd, NULL, TRUE );
        else
            DirectXUpdateOverlay( p_vout );
        p_vout->i_changes &= ~VOUT_SIZE_CHANGE;
        p_vout->p_sys->i_changes &= ~VOUT_SIZE_CHANGE;
    }

    /*
     * Fullscreen change
     */
    if( p_vout->i_changes & VOUT_FULLSCREEN_CHANGE
        || p_vout->p_sys->i_changes & VOUT_FULLSCREEN_CHANGE )
    {
        p_vout->b_fullscreen = ! p_vout->b_fullscreen;

        /* We need to switch between Maximized and Normal sized window */
        window_placement.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement( p_vout->p_sys->hwnd, &window_placement );
        if( p_vout->b_fullscreen )
        {
            /* Maximized window */
            window_placement.showCmd = SW_SHOWMAXIMIZED;
            /* Change window style, no borders and no title bar */
            SetWindowLong( p_vout->p_sys->hwnd, GWL_STYLE, 0 );

        }
        else
        {
            /* Normal window */
            window_placement.showCmd = SW_SHOWNORMAL;
            /* Change window style, borders and title bar */
            SetWindowLong( p_vout->p_sys->hwnd, GWL_STYLE,
                           WS_OVERLAPPEDWINDOW | WS_SIZEBOX | WS_VISIBLE );
        }

        SetWindowPlacement( p_vout->p_sys->hwnd, &window_placement );

        p_vout->i_changes &= ~VOUT_FULLSCREEN_CHANGE;
        p_vout->p_sys->i_changes &= ~VOUT_FULLSCREEN_CHANGE;
    }

    /*
     * Pointer change
     */
    if( (!p_vout->p_sys->b_cursor_hidden) &&
        ( (mdate() - p_vout->p_sys->i_lastmoved) > 5000000 ) )
    {
        /* Hide the mouse automatically */
        p_vout->p_sys->b_cursor_hidden = 1;
        PostMessage( p_vout->p_sys->hwnd, WM_VLC_HIDE_MOUSE, 0, 0 );
    }

    /* Check if the event thread is still running */
    if( p_vout->p_sys->b_event_thread_die )
        return 1; /* exit */

    return 0;
}

/*****************************************************************************
 * vout_Render: render previously calculated output
 *****************************************************************************/
static void vout_Render( vout_thread_t *p_vout, picture_t *p_pic )
{
    ;
}

/*****************************************************************************
 * vout_Display: displays previously rendered output
 *****************************************************************************
 * This function sends the currently rendered image to the display, wait until
 * it is displayed and switch the two rendering buffers, preparing next frame.
 *****************************************************************************/
static void vout_Display( vout_thread_t *p_vout, picture_t *p_pic )
{
    HRESULT dxresult;

    if( (p_vout->p_sys->p_display == NULL) )
    {
        intf_WarnMsg( 3, "vout error: vout_Display no display!!" );
        return;
    }

    if( !p_vout->p_sys->b_using_overlay )
    {
        DDBLTFX  ddbltfx;

        /* We ask for the "NOTEARING" option */
        memset( &ddbltfx, 0, sizeof(DDBLTFX) );
        ddbltfx.dwSize = sizeof(DDBLTFX);
        ddbltfx.dwDDFX = DDBLTFX_NOTEARING;

        /* Blit video surface to display */
        dxresult = IDirectDrawSurface2_Blt(p_vout->p_sys->p_display,
                                           &p_vout->p_sys->rect_dest_clipped,
                                           p_pic->p_sys->p_surface,
                                           &p_vout->p_sys->rect_src_clipped,
                                           DDBLT_ASYNC, &ddbltfx );
        if ( dxresult == DDERR_SURFACELOST )
        {
            /* Our surface can be lost so be sure
             * to check this and restore it if needed */
            IDirectDrawSurface2_Restore( p_vout->p_sys->p_display );

            /* Now that the surface has been restored try to display again */
            dxresult = IDirectDrawSurface2_Blt(p_vout->p_sys->p_display,
                                           &p_vout->p_sys->rect_dest_clipped,
                                           p_pic->p_sys->p_surface,
                                           &p_vout->p_sys->rect_src_clipped,
                                           DDBLT_ASYNC, &ddbltfx );
        }

        if( dxresult != DD_OK )
        {
            intf_WarnMsg( 3, "vout: could not Blit the surface" );
            return;
        }

    }
    else /* using overlay */
    {

        /* Flip the overlay buffers if we are using back buffers */
        if( p_pic->p_sys->p_front_surface == p_pic->p_sys->p_surface )
            return;

        dxresult = IDirectDrawSurface2_Flip( p_pic->p_sys->p_front_surface,
                                             NULL, DDFLIP_WAIT );
        if ( dxresult == DDERR_SURFACELOST )
        {
            /* Our surface can be lost so be sure
             * to check this and restore it if needed */
            IDirectDrawSurface2_Restore( p_vout->p_sys->p_display );
            IDirectDrawSurface2_Restore( p_pic->p_sys->p_front_surface );

            /* Now that the surface has been restored try to display again */
            dxresult = IDirectDrawSurface2_Flip( p_pic->p_sys->p_front_surface,
                                                 NULL, DDFLIP_WAIT );
            DirectXUpdateOverlay( p_vout );
        }

        if( dxresult != DD_OK )
            intf_WarnMsg( 8, "vout: couldn't flip overlay surface" );


        if( !DirectXGetSurfaceDesc( p_pic ) )
        {
            /* AAARRGG */
            intf_ErrMsg( "vout error: vout_Display cannot get surface desc" );
            return;
        }

        if( !UpdatePictureStruct( p_vout, p_pic, p_vout->output.i_chroma ) )
        {
            /* AAARRGG */
            intf_ErrMsg( "vout error: vout_Display unvalid pic chroma" );
            return;
        }

        /* set currently displayed pic */
        p_vout->p_sys->p_current_surface = p_pic->p_sys->p_front_surface;
    }

}


/* following functions are local */

/*****************************************************************************
 * DirectXInitDDraw: Takes care of all the DirectDraw initialisations
 *****************************************************************************
 * This function initialise and allocate resources for DirectDraw.
 *****************************************************************************/
static int DirectXInitDDraw( vout_thread_t *p_vout )
{
    HRESULT    dxresult;
    HRESULT    (WINAPI *OurDirectDrawCreate)(GUID *,LPDIRECTDRAW *,IUnknown *);
    LPDIRECTDRAW  p_ddobject;

    intf_WarnMsg( 3, "vout: DirectXInitDDraw" );

    /* load direct draw DLL */
    p_vout->p_sys->hddraw_dll = LoadLibrary("DDRAW.DLL");
    if( p_vout->p_sys->hddraw_dll == NULL )
    {
        intf_WarnMsg( 3, "vout: DirectXInitDDraw failed loading ddraw.dll" );
        goto error;
    }
      
    OurDirectDrawCreate = 
      (void *)GetProcAddress(p_vout->p_sys->hddraw_dll, "DirectDrawCreate");
    if ( OurDirectDrawCreate == NULL )
    {
        intf_ErrMsg( "vout error: DirectXInitDDraw failed GetProcAddress" );
        goto error;
    }

    /* Initialize DirectDraw now */
    dxresult = OurDirectDrawCreate( NULL, &p_ddobject, NULL );
    if( dxresult != DD_OK )
    {
        intf_ErrMsg( "vout error: DirectXInitDDraw can't initialize DDraw" );
        goto error;
    }

    /* Get the IDirectDraw2 interface */
    dxresult = IDirectDraw_QueryInterface( p_ddobject, &IID_IDirectDraw2,
                                        (LPVOID *)&p_vout->p_sys->p_ddobject );
    /* Release the unused interface */
    IDirectDraw_Release( p_ddobject );
    if( dxresult != DD_OK )
    {
        intf_ErrMsg( "vout error: can't get IDirectDraw2 interface." );
        goto error;
    }

    /* Set DirectDraw Cooperative level, ie what control we want over Windows
     * display */
    dxresult = IDirectDraw2_SetCooperativeLevel( p_vout->p_sys->p_ddobject,
                                           p_vout->p_sys->hwnd, DDSCL_NORMAL );
    if( dxresult != DD_OK )
    {
        intf_ErrMsg( "vout error: can't set direct draw cooperative level." );
        goto error;
    }

    /* Probe the capabilities of the hardware */
    DirectXGetDDrawCaps( p_vout );

    intf_WarnMsg( 3, "vout: End DirectXInitDDraw" );
    return 0;

 error:
    if( p_vout->p_sys->p_ddobject )
        IDirectDraw2_Release( p_vout->p_sys->p_ddobject );
    if( p_vout->p_sys->hddraw_dll )
        FreeLibrary( p_vout->p_sys->hddraw_dll );
    p_vout->p_sys->hddraw_dll = NULL;
    p_vout->p_sys->p_ddobject = NULL;
    return 1;
}

/*****************************************************************************
 * DirectXCreateDisplay: create the DirectDraw display.
 *****************************************************************************
 * Create and initialize display according to preferences specified in the vout
 * thread fields.
 *****************************************************************************/
static int DirectXCreateDisplay( vout_thread_t *p_vout )
{
    HRESULT              dxresult;
    DDSURFACEDESC        ddsd;
    LPDIRECTDRAWSURFACE  p_display;
    DDPIXELFORMAT   pixel_format;

    intf_WarnMsg( 3, "vout: DirectXCreateDisplay" );

    /* Now get the primary surface. This surface is what you actually see
     * on your screen */
    memset( &ddsd, 0, sizeof( DDSURFACEDESC ));
    ddsd.dwSize = sizeof(DDSURFACEDESC);
    ddsd.dwFlags = DDSD_CAPS;
    ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;

    dxresult = IDirectDraw2_CreateSurface( p_vout->p_sys->p_ddobject,
                                           &ddsd,
                                           &p_display, NULL );
    if( dxresult != DD_OK )
    {
        intf_ErrMsg( "vout error: can't get direct draw primary surface." );
        return 1;
    }

    dxresult = IDirectDrawSurface_QueryInterface( p_display,
                                         &IID_IDirectDrawSurface2,
                                         (LPVOID *)&p_vout->p_sys->p_display );
    /* Release the old interface */
    IDirectDrawSurface_Release( p_display );
    if ( dxresult != DD_OK )
    {
        intf_ErrMsg( "vout error: can't get IDirectDrawSurface2 interface." );
        return 1;
    }

    /* The clipper will be used only in non-overlay mode */
    DirectXCreateClipper( p_vout );


#if 1
    /* compute the colorkey pixel value from the RGB value we've got */
    memset( &pixel_format, 0, sizeof( DDPIXELFORMAT ));
    pixel_format.dwSize = sizeof( DDPIXELFORMAT );
    dxresult = IDirectDrawSurface2_GetPixelFormat( p_vout->p_sys->p_display,
                                                   &pixel_format );
    if( dxresult != DD_OK )
        intf_WarnMsg( 3, "vout: DirectXUpdateOverlay GetPixelFormat failed" );
    p_vout->p_sys->i_colorkey = (DWORD)((( p_vout->p_sys->i_rgb_colorkey
                                           * pixel_format.dwRBitMask) / 255)
                                        & pixel_format.dwRBitMask);
#endif

    return 0;
}


/*****************************************************************************
 * DirectXCreateClipper: Create a clipper that will be used when blitting the
 *                       RGB surface to the main display.
 *****************************************************************************
 * This clipper prevents us to modify by mistake anything on the screen
 * which doesn't belong to our window. For example when a part of our video
 * window is hidden by another window.
 *****************************************************************************/
static int DirectXCreateClipper( vout_thread_t *p_vout )
{
    HRESULT dxresult;

    intf_WarnMsg( 3, "vout: DirectXCreateClipper" );

    /* Create the clipper */
    dxresult = IDirectDraw2_CreateClipper( p_vout->p_sys->p_ddobject, 0,
                                           &p_vout->p_sys->p_clipper, NULL );
    if( dxresult != DD_OK )
    {
        intf_WarnMsg( 3, "vout: DirectXCreateClipper can't create clipper." );
        goto error;
    }

    /* associate the clipper to the window */
    dxresult = IDirectDrawClipper_SetHWnd(p_vout->p_sys->p_clipper, 0,
                                          p_vout->p_sys->hwnd);
    if( dxresult != DD_OK )
    {
        intf_WarnMsg( 3,
            "vout: DirectXCreateClipper can't attach clipper to window." );
        goto error;
    }

    /* associate the clipper with the surface */
    dxresult = IDirectDrawSurface_SetClipper(p_vout->p_sys->p_display,
                                             p_vout->p_sys->p_clipper);
    if( dxresult != DD_OK )
    {
        intf_WarnMsg( 3,
            "vout: DirectXCreateClipper can't attach clipper to surface." );
        goto error;
    }    

    return 0;

 error:
    if( p_vout->p_sys->p_clipper )
        IDirectDrawClipper_Release( p_vout->p_sys->p_clipper );
    p_vout->p_sys->p_clipper = NULL;
    return 1;
}

/*****************************************************************************
 * DirectXCreateSurface: create an YUV overlay or RGB surface for the video.
 *****************************************************************************
 * The best method of display is with an YUV overlay because the YUV->RGB
 * conversion is done in hardware.
 * You can also create a plain RGB surface.
 * ( Maybe we could also try an RGB overlay surface, which could have hardware
 * scaling and which would also be faster in window mode because you don't
 * need to do any blitting to the main display...)
 *****************************************************************************/
static int DirectXCreateSurface( vout_thread_t *p_vout,
                                 LPDIRECTDRAWSURFACE2 *pp_surface_final,
                                 int i_chroma, int b_overlay,
                                 int i_backbuffers )
{
    HRESULT dxresult;
    LPDIRECTDRAWSURFACE p_surface;
    DDSURFACEDESC ddsd;

    /* Create the video surface */
    if( b_overlay )
    {
        /* Now try to create the YUV overlay surface.
         * This overlay will be displayed on top of the primary surface.
         * A color key is used to determine whether or not the overlay will be
         * displayed, ie the overlay will be displayed in place of the primary
         * surface wherever the primary surface will have this color.
         * The video window has been created with a background of this color so
         * the overlay will be only displayed on top of this window */

        memset( &ddsd, 0, sizeof( DDSURFACEDESC ));
        ddsd.dwSize = sizeof(DDSURFACEDESC);
        ddsd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
        ddsd.ddpfPixelFormat.dwFlags = DDPF_FOURCC;
        ddsd.ddpfPixelFormat.dwFourCC = i_chroma;
        ddsd.dwFlags = DDSD_CAPS |
                       DDSD_HEIGHT |
                       DDSD_WIDTH |
                       DDSD_PIXELFORMAT;
        ddsd.dwFlags |= (i_backbuffers ? DDSD_BACKBUFFERCOUNT : 0);
        ddsd.ddsCaps.dwCaps = DDSCAPS_OVERLAY |
                              DDSCAPS_VIDEOMEMORY;
        ddsd.ddsCaps.dwCaps |= (i_backbuffers ? DDSCAPS_COMPLEX | DDSCAPS_FLIP
                                : 0 );
        ddsd.dwHeight = p_vout->render.i_height;
        ddsd.dwWidth = p_vout->render.i_width;
        ddsd.dwBackBufferCount = i_backbuffers;

        dxresult = IDirectDraw2_CreateSurface( p_vout->p_sys->p_ddobject,
                                               &ddsd,
                                               &p_surface, NULL );
        if( dxresult != DD_OK )
        {
            *pp_surface_final = NULL;
            return 0;
        }
    }

    if( !b_overlay )
    {
        boolean_t b_rgb_surface = ( i_chroma == FOURCC_RGB2 ) ||
            ( i_chroma == FOURCC_RV15 ) || ( i_chroma == FOURCC_RV16 ) ||
            ( i_chroma == FOURCC_RV24 ) || ( i_chroma == FOURCC_RV32 );

        memset( &ddsd, 0, sizeof( DDSURFACEDESC ) );
        ddsd.dwSize = sizeof(DDSURFACEDESC);
        ddsd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
        ddsd.dwFlags = DDSD_HEIGHT |
                       DDSD_WIDTH |
                       DDSD_CAPS;
        ddsd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
        ddsd.dwHeight = p_vout->render.i_height;
        ddsd.dwWidth = p_vout->render.i_width;

        if( p_vout->p_sys->b_use_sysmem )
            ddsd.ddsCaps.dwCaps |= DDSCAPS_SYSTEMMEMORY;
        else
            ddsd.ddsCaps.dwCaps |= DDSCAPS_VIDEOMEMORY;

        if( !b_rgb_surface )
        {
            ddsd.dwFlags |= DDSD_PIXELFORMAT;
            ddsd.ddpfPixelFormat.dwFlags = DDPF_FOURCC;
            ddsd.ddpfPixelFormat.dwFourCC = i_chroma;
        }

        dxresult = IDirectDraw2_CreateSurface( p_vout->p_sys->p_ddobject,
                                               &ddsd,
                                               &p_surface, NULL );
        if( dxresult != DD_OK )
        {
            *pp_surface_final = NULL;
            return 0;
        }
    }

    /* Now that the surface is created, try to get a newer DirectX interface */
    dxresult = IDirectDrawSurface_QueryInterface( p_surface,
                                     &IID_IDirectDrawSurface2,
                                     (LPVOID *)pp_surface_final );
    IDirectDrawSurface_Release( p_surface );    /* Release the old interface */
    if ( dxresult != DD_OK )
    {
        intf_ErrMsg( "vout error: can't get IDirectDrawSurface2 interface." );
        *pp_surface_final = NULL;
        return 0;
    }

    return 1;
}

/*****************************************************************************
 * DirectXUpdateOverlay: Move or resize overlay surface on video display.
 *****************************************************************************
 * This function is used to move or resize an overlay surface on the screen.
 * Ususally the overlay is moved by the user and thus, by a move or resize
 * event (in vout_Manage).
 *****************************************************************************/
void DirectXUpdateOverlay( vout_thread_t *p_vout )
{
    DDOVERLAYFX     ddofx;
    DWORD           dwFlags;
    HRESULT         dxresult;

    if( p_vout->p_sys->p_current_surface == NULL ||
        !p_vout->p_sys->b_using_overlay )
        return;

    /* The new window dimensions should already have been computed by the
     * caller of this function */

    /* Position and show the overlay */
    memset(&ddofx, 0, sizeof(DDOVERLAYFX));
    ddofx.dwSize = sizeof(DDOVERLAYFX);
    ddofx.dckDestColorkey.dwColorSpaceLowValue = p_vout->p_sys->i_colorkey;
    ddofx.dckDestColorkey.dwColorSpaceHighValue = p_vout->p_sys->i_colorkey;

    dwFlags = DDOVER_SHOW;
    if( !p_vout->p_sys->b_caps_overlay_clipping )
        dwFlags |= DDOVER_KEYDESTOVERRIDE;

    dxresult = IDirectDrawSurface2_UpdateOverlay(
                                         p_vout->p_sys->p_current_surface,
                                         &p_vout->p_sys->rect_src_clipped,
                                         p_vout->p_sys->p_display,
                                         &p_vout->p_sys->rect_dest_clipped,
                                         dwFlags,
                                         &ddofx );
    if(dxresult != DD_OK)
    {
        intf_WarnMsg( 3,
          "vout: DirectXUpdateOverlay can't move or resize overlay" );
    }

}

/*****************************************************************************
 * DirectXCloseDDraw: Release the DDraw object allocated by DirectXInitDDraw
 *****************************************************************************
 * This function returns all resources allocated by DirectXInitDDraw.
 *****************************************************************************/
static void DirectXCloseDDraw( vout_thread_t *p_vout )
{
    intf_WarnMsg(3, "vout: DirectXCloseDDraw" );
    if( p_vout->p_sys->p_ddobject != NULL )
    {
        IDirectDraw2_Release(p_vout->p_sys->p_ddobject);
        p_vout->p_sys->p_ddobject = NULL;
    }

    if( p_vout->p_sys->hddraw_dll != NULL )
    {
        FreeLibrary( p_vout->p_sys->hddraw_dll );
        p_vout->p_sys->hddraw_dll = NULL;
    }
}

/*****************************************************************************
 * DirectXCloseDisplay: close and reset the DirectX display device
 *****************************************************************************
 * This function returns all resources allocated by DirectXCreateDisplay.
 *****************************************************************************/
static void DirectXCloseDisplay( vout_thread_t *p_vout )
{
    intf_WarnMsg( 3, "vout: DirectXCloseDisplay" );

    if( p_vout->p_sys->p_clipper != NULL )
    {
        intf_WarnMsg( 3, "vout: DirectXCloseDisplay clipper" );
        IDirectDrawClipper_Release( p_vout->p_sys->p_clipper );
        p_vout->p_sys->p_clipper = NULL;
    }

    if( p_vout->p_sys->p_display != NULL )
    {
        intf_WarnMsg( 3, "vout: DirectXCloseDisplay display" );
        IDirectDrawSurface2_Release( p_vout->p_sys->p_display );
        p_vout->p_sys->p_display = NULL;
    }
}

/*****************************************************************************
 * DirectXCloseSurface: close the YUV overlay or RGB surface.
 *****************************************************************************
 * This function returns all resources allocated for the surface.
 *****************************************************************************/
static void DirectXCloseSurface( vout_thread_t *p_vout,
                                 LPDIRECTDRAWSURFACE2 p_surface )
{
    intf_WarnMsg( 3, "vout: DirectXCloseSurface" );
    if( p_surface != NULL )
    {
        IDirectDrawSurface2_Release( p_surface );
    }
}

/*****************************************************************************
 * NewPictureVec: allocate a vector of identical pictures
 *****************************************************************************
 * Returns 0 on success, -1 otherwise
 *****************************************************************************/
static int NewPictureVec( vout_thread_t *p_vout, picture_t *p_pic,
                          int i_num_pics )
{
    int i;
    boolean_t b_result_ok;
    LPDIRECTDRAWSURFACE2 p_surface;

    intf_WarnMsg( 3, "vout: NewPictureVec" );

    I_OUTPUTPICTURES = 0;

    /* First we try to use an YUV overlay surface.
     * The overlay surface that we create won't be used to decode directly
     * into it because accessing video memory directly is way to slow (remember
     * that pictures are decoded macroblock per macroblock). Instead the video
     * will be decoded in picture buffers in system memory which will then be
     * memcpy() to the overlay surface. */
    if( p_vout->p_sys->b_using_overlay )
    {
        /* Triple buffering rocks! it doesn't have any processing overhead
         * (you don't have to wait for the vsync) and provides for a very nice
         * video quality (no tearing). */

        b_result_ok = DirectXCreateSurface( p_vout, &p_surface,
                                            p_vout->output.i_chroma,
                                            p_vout->p_sys->b_using_overlay,
                                            2 /* number of backbuffers */ );

        if( !b_result_ok )
            /* Try to reduce the number of backbuffers */
            b_result_ok = DirectXCreateSurface( p_vout, &p_surface,
                                                p_vout->output.i_chroma,
                                                p_vout->p_sys->b_using_overlay,
                                                0 /* number of backbuffers */);

        if( b_result_ok )
        {
            DDSCAPS dds_caps;
            picture_t front_pic;
            picture_sys_t front_pic_sys;
            front_pic.p_sys = &front_pic_sys;

            /* Allocate internal structure */
            p_pic[0].p_sys = malloc( sizeof( picture_sys_t ) );
            if( p_pic[0].p_sys == NULL )
            {
                DirectXCloseSurface( p_vout, p_surface );
                return -1;
            }

            /* set front buffer */
            p_pic[0].p_sys->p_front_surface = p_surface;

            /* Get the back buffer */
            memset( &dds_caps, 0, sizeof( DDSCAPS ) );
            dds_caps.dwCaps = DDSCAPS_BACKBUFFER;
            if( DD_OK != IDirectDrawSurface2_GetAttachedSurface(
                                                p_surface, &dds_caps,
                                                &p_pic[0].p_sys->p_surface ) )
            {
                intf_WarnMsg( 3, "vout: NewPictureVec couldn't get "
                              "back buffer" );
                /* front buffer is the same as back buffer */
                p_pic[0].p_sys->p_surface = p_surface;
            }


            p_vout->p_sys->p_current_surface = front_pic.p_sys->p_surface =
                p_pic[0].p_sys->p_front_surface;

            /* reset the front buffer memory */
            if( DirectXGetSurfaceDesc( &front_pic ) &&
                UpdatePictureStruct( p_vout, &front_pic,
                                     p_vout->output.i_chroma ) )
            {
                int i,j;
                for( i = 0; i < front_pic.i_planes; i++ )
                    for( j = 0; j < front_pic.p[i].i_lines; j++)
                        memset( front_pic.p[i].p_pixels + j *
                                front_pic.p[i].i_pitch, 127,
                                front_pic.p[i].i_visible_bytes );
            }

            DirectXUpdateOverlay( p_vout );
            I_OUTPUTPICTURES = 1;
            intf_WarnMsg( 3,"vout: DirectX YUV overlay created successfully" );
        }
    }

    /* As we can't have an overlay, we'll try to create a plain offscreen
     * surface. This surface will reside in video memory because there's a
     * better chance then that we'll be able to use some kind of hardware
     * acceleration like rescaling, blitting or YUV->RGB conversions.
     * We then only need to blit this surface onto the main display when we
     * want to display it */
    if( !p_vout->p_sys->b_using_overlay )
    {

        if( p_vout->p_sys->b_hw_yuv )
            b_result_ok = DirectXCreateSurface( p_vout, &p_surface,
                                                p_vout->output.i_chroma,
                                                p_vout->p_sys->b_using_overlay,
                                                0 /* no back buffers */ );

        if( !p_vout->p_sys->b_hw_yuv || !b_result_ok )
        {
            /* Our last choice is to use a plain RGB surface */
            DDPIXELFORMAT ddpfPixelFormat;

            ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
            IDirectDrawSurface2_GetPixelFormat( p_vout->p_sys->p_display,
                                                &ddpfPixelFormat );

            if( ddpfPixelFormat.dwFlags & DDPF_RGB )
            {
                switch( ddpfPixelFormat.dwRGBBitCount )
                {
                case 8: /* FIXME: set the palette */
                    p_vout->output.i_chroma = FOURCC_RGB2; break;
                case 15:
                    p_vout->output.i_chroma = FOURCC_RV15; break;
                case 16:
                    p_vout->output.i_chroma = FOURCC_RV16; break;
                case 24:
                    p_vout->output.i_chroma = FOURCC_RV24; break;
                case 32:
                    p_vout->output.i_chroma = FOURCC_RV32; break;
                default:
                    intf_ErrMsg( "vout error: unknown screen depth" );
                    return 0;
                }
                p_vout->output.i_rmask = ddpfPixelFormat.dwRBitMask;
                p_vout->output.i_gmask = ddpfPixelFormat.dwGBitMask;
                p_vout->output.i_bmask = ddpfPixelFormat.dwBBitMask;
            }

            p_vout->p_sys->b_hw_yuv = 0;

            b_result_ok = DirectXCreateSurface( p_vout, &p_surface,
                                                p_vout->output.i_chroma,
                                                p_vout->p_sys->b_using_overlay,
                                                0 /* no back buffers */ );
        }

        if( b_result_ok )
        {
            /* Allocate internal structure */
            p_pic[0].p_sys = malloc( sizeof( picture_sys_t ) );
            if( p_pic[0].p_sys == NULL )
            {
                DirectXCloseSurface( p_vout, p_surface );
                return -1;
            }
            p_pic[0].p_sys->p_surface = p_pic[0].p_sys->p_front_surface
                = p_surface;

            I_OUTPUTPICTURES = 1;

            intf_WarnMsg( 3, "vout: DirectX plain surface created "
                             "successfully" );
        }
    }


    /* Now that we've got all our direct-buffers, we can finish filling in the
     * picture_t structures */
    for( i = 0; i < I_OUTPUTPICTURES; i++ )
    {
        p_pic[i].i_status = DESTROYED_PICTURE;
        p_pic[i].i_type   = DIRECT_PICTURE;
        PP_OUTPUTPICTURE[i] = &p_pic[i];

        if( !DirectXGetSurfaceDesc( &p_pic[i] ) )
        {
            /* AAARRGG */
            FreePictureVec( p_vout, p_pic, I_OUTPUTPICTURES );
            I_OUTPUTPICTURES = 0;
            return -1;
        }

        if( !UpdatePictureStruct(p_vout, &p_pic[i], p_vout->output.i_chroma) )
        {

            /* Unknown chroma, tell the guy to get lost */
            intf_ErrMsg( "vout error: never heard of chroma 0x%.8x (%4.4s)",
                         p_vout->output.i_chroma,
                         (char*)&p_vout->output.i_chroma );
            FreePictureVec( p_vout, p_pic, I_OUTPUTPICTURES );
            I_OUTPUTPICTURES = 0;
            return -1;
        }
    }

    intf_WarnMsg( 3, "vout: End NewPictureVec");
    return 0;
}

/*****************************************************************************
 * FreePicture: destroy a picture vector allocated with NewPictureVec
 *****************************************************************************
 * 
 *****************************************************************************/
static void FreePictureVec( vout_thread_t *p_vout, picture_t *p_pic,
                            int i_num_pics )
{
    int i;

    for( i = 0; i < i_num_pics; i++ )
    {
        DirectXCloseSurface( p_vout, p_pic[i].p_sys->p_front_surface );

        for( i = 0; i < i_num_pics; i++ )
        {
            free( p_pic[i].p_sys );
        }
    }
}

/*****************************************************************************
 * UpdatePictureStruct: updates the internal data in the picture_t structure
 *****************************************************************************
 * This will setup stuff for use by the video_output thread
 *****************************************************************************/
static int UpdatePictureStruct( vout_thread_t *p_vout, picture_t *p_pic,
                                int i_chroma )
{

    switch( p_vout->output.i_chroma )
    {

        case FOURCC_RGB2:
        case FOURCC_RV15:
        case FOURCC_RV16:
        case FOURCC_RV24:
        case FOURCC_RV32:
            p_pic->p->p_pixels = p_pic->p_sys->ddsd.lpSurface;
            p_pic->p->i_lines = p_vout->output.i_height;
            p_pic->p->i_pitch = p_pic->p_sys->ddsd.lPitch;
            p_pic->p->b_margin = 0;
            p_pic->i_planes = 1;
            switch( p_vout->output.i_chroma )
            {
                case FOURCC_RGB2:
                    p_pic->p->i_pixel_bytes = 1;
                    break;
                case FOURCC_RV15:
                case FOURCC_RV16:
                    p_pic->p->i_pixel_bytes = 2;
                    break;
                case FOURCC_RV24:
                case FOURCC_RV32:
                    p_pic->p->i_pixel_bytes = 4;
                    break;
                default:
                    return -1;
            }
            p_pic->p->i_visible_bytes = p_vout->output.i_width *
              p_pic->p->i_pixel_bytes;
            break;

        case FOURCC_YV12:

            p_pic->Y_PIXELS = p_pic->p_sys->ddsd.lpSurface;
            p_pic->p[Y_PLANE].i_lines = p_vout->output.i_height;
            p_pic->p[Y_PLANE].i_pitch = p_pic->p_sys->ddsd.lPitch;
            p_pic->p[Y_PLANE].i_pixel_bytes = 1;
            p_pic->p[Y_PLANE].i_visible_bytes = p_vout->output.i_width *
              p_pic->p[Y_PLANE].i_pixel_bytes;
            p_pic->p[Y_PLANE].b_margin = 0;

            p_pic->V_PIXELS =  p_pic->Y_PIXELS
              + p_pic->p[Y_PLANE].i_lines * p_pic->p[Y_PLANE].i_pitch;
            p_pic->p[V_PLANE].i_lines = p_vout->output.i_height / 2;
            p_pic->p[V_PLANE].i_pitch = p_pic->p[Y_PLANE].i_pitch / 2;
            p_pic->p[V_PLANE].i_pixel_bytes = 1;
            p_pic->p[V_PLANE].i_visible_bytes = p_vout->output.i_width *
              p_pic->p[V_PLANE].i_pixel_bytes;
            p_pic->p[V_PLANE].b_margin = 0;

            p_pic->U_PIXELS = p_pic->V_PIXELS
              + p_pic->p[V_PLANE].i_lines * p_pic->p[V_PLANE].i_pitch;
            p_pic->p[U_PLANE].i_lines = p_vout->output.i_height / 2;
            p_pic->p[U_PLANE].i_pitch = p_pic->p[Y_PLANE].i_pitch / 2;
            p_pic->p[U_PLANE].i_pixel_bytes = 1;
            p_pic->p[U_PLANE].i_visible_bytes = p_vout->output.i_width *
              p_pic->p[U_PLANE].i_pixel_bytes;
            p_pic->p[U_PLANE].b_margin = 0;

            p_pic->i_planes = 3;
            break;

        case FOURCC_IYUV:

            p_pic->Y_PIXELS = p_pic->p_sys->ddsd.lpSurface;
            p_pic->p[Y_PLANE].i_lines = p_vout->output.i_height;
            p_pic->p[Y_PLANE].i_pitch = p_pic->p_sys->ddsd.lPitch;
            p_pic->p[Y_PLANE].i_pixel_bytes = 1;
            p_pic->p[Y_PLANE].i_visible_bytes = p_vout->output.i_width *
              p_pic->p[Y_PLANE].i_pixel_bytes;
            p_pic->p[Y_PLANE].b_margin = 0;

            p_pic->U_PIXELS = p_pic->Y_PIXELS
              + p_pic->p[Y_PLANE].i_lines * p_pic->p[Y_PLANE].i_pitch;
            p_pic->p[U_PLANE].i_lines = p_vout->output.i_height / 2;
            p_pic->p[U_PLANE].i_pitch = p_pic->p[Y_PLANE].i_pitch / 2;
            p_pic->p[U_PLANE].i_pixel_bytes = 1;
            p_pic->p[U_PLANE].i_visible_bytes = p_vout->output.i_width *
              p_pic->p[U_PLANE].i_pixel_bytes;
            p_pic->p[U_PLANE].b_margin = 0;

            p_pic->V_PIXELS =  p_pic->U_PIXELS
              + p_pic->p[U_PLANE].i_lines * p_pic->p[U_PLANE].i_pitch;
            p_pic->p[V_PLANE].i_lines = p_vout->output.i_height / 2;
            p_pic->p[V_PLANE].i_pitch = p_pic->p[Y_PLANE].i_pitch / 2;
            p_pic->p[V_PLANE].i_pixel_bytes = 1;
            p_pic->p[V_PLANE].i_visible_bytes = p_vout->output.i_width *
              p_pic->p[V_PLANE].i_pixel_bytes;
            p_pic->p[V_PLANE].b_margin = 0;

            p_pic->i_planes = 3;
            break;

        case FOURCC_YUY2:

            p_pic->p->p_pixels = p_pic->p_sys->ddsd.lpSurface;
            p_pic->p->i_lines = p_vout->output.i_height;
            p_pic->p->i_pitch = p_pic->p_sys->ddsd.lPitch;
            p_pic->p->i_pixel_bytes = 2;
            p_pic->p->i_visible_bytes = p_vout->output.i_width *
              p_pic->p->i_pixel_bytes;
            p_pic->p->b_margin = 1;

            p_pic->i_planes = 1;
            break;

        default:
            /* Not supported */
            return 0;

    }

    return 1;
}

/*****************************************************************************
 * DirectXGetDDrawCaps: Probe the capabilities of the hardware
 *****************************************************************************
 * It is nice to know which features are supported by the hardware so we can
 * find ways to optimize our rendering.
 *****************************************************************************/
static void DirectXGetDDrawCaps( vout_thread_t *p_vout )
{
    DDCAPS ddcaps;
    HRESULT dxresult;

    /* This is just an indication of whether or not we'll support overlay,
     * but with this test we don't know if we support YUV overlay */
    memset( &ddcaps, 0, sizeof( DDCAPS ));
    ddcaps.dwSize = sizeof(DDCAPS);
    dxresult = IDirectDraw2_GetCaps( p_vout->p_sys->p_ddobject,
                                     &ddcaps, NULL );
    if(dxresult != DD_OK )
    {
        intf_WarnMsg( 3,"vout error: can't get caps." );
    }
    else
    {
        BOOL bHasOverlay, bHasOverlayFourCC, bCanClipOverlay,
             bHasColorKey, bCanStretch;

        /* Determine if the hardware supports overlay surfaces */
        bHasOverlay = ((ddcaps.dwCaps & DDCAPS_OVERLAY) ==
                       DDCAPS_OVERLAY) ? TRUE : FALSE;
        /* Determine if the hardware supports overlay surfaces */
        bHasOverlayFourCC = ((ddcaps.dwCaps & DDCAPS_OVERLAYFOURCC) ==
                       DDCAPS_OVERLAYFOURCC) ? TRUE : FALSE;
        /* Determine if the hardware supports overlay surfaces */
        bCanClipOverlay = ((ddcaps.dwCaps & DDCAPS_OVERLAYCANTCLIP) ==
                       0 ) ? TRUE : FALSE;
        /* Determine if the hardware supports colorkeying */
        bHasColorKey = ((ddcaps.dwCaps & DDCAPS_COLORKEY) ==
                        DDCAPS_COLORKEY) ? TRUE : FALSE;
        /* Determine if the hardware supports scaling of the overlay surface */
        bCanStretch = ((ddcaps.dwCaps & DDCAPS_OVERLAYSTRETCH) ==
                       DDCAPS_OVERLAYSTRETCH) ? TRUE : FALSE;
        intf_WarnMsg( 3, "vout: DirectDraw Capabilities:" );
        intf_WarnMsg( 3, "       overlay=%i yuvoverlay=%i can_clip_overlay=%i "
                         "colorkey=%i stretch=%i",
                      bHasOverlay, bHasOverlayFourCC, bCanClipOverlay,
                      bHasColorKey, bCanStretch );

        /* Overlay clipping support is interesting for us as it means we can
         * get rid of the colorkey alltogether */
        p_vout->p_sys->b_caps_overlay_clipping = bCanClipOverlay;

    }
}

/*****************************************************************************
 * DirectXGetSurfaceDesc: Get some more information about the surface
 *****************************************************************************
 * This function get and stores the surface descriptor which among things
 * has the pointer to the picture data.
 *****************************************************************************/
static int DirectXGetSurfaceDesc( picture_t *p_pic )
{
    HRESULT dxresult;

    /* Lock the surface to get a valid pointer to the picture buffer */
    memset( &p_pic->p_sys->ddsd, 0, sizeof( DDSURFACEDESC ));
    p_pic->p_sys->ddsd.dwSize = sizeof(DDSURFACEDESC);
    dxresult = IDirectDrawSurface2_Lock( p_pic->p_sys->p_surface,
                                         NULL, &p_pic->p_sys->ddsd,
                                         DDLOCK_NOSYSLOCK | DDLOCK_WAIT,
                                         NULL );
    if ( dxresult == DDERR_SURFACELOST )
    {
        /* Your surface can be lost so be sure
         * to check this and restore it if needed */
        dxresult = IDirectDrawSurface2_Restore( p_pic->p_sys->p_surface );
        dxresult = IDirectDrawSurface2_Lock( p_pic->p_sys->p_surface, NULL,
                                             &p_pic->p_sys->ddsd,
                                             DDLOCK_NOSYSLOCK | DDLOCK_WAIT,
                                             NULL);
    }
    if( dxresult != DD_OK )
    {
        intf_ErrMsg( "vout: DirectXGetSurfaceDesc can't lock surface" );
        return 0;
    }

    /* Unlock the Surface */
    dxresult = IDirectDrawSurface2_Unlock( p_pic->p_sys->p_surface, NULL );

    return 1;
}
