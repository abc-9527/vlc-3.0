/*****************************************************************************
 * directory.c: expands a directory (directory: access plug-in)
 *****************************************************************************
 * Copyright (C) 2002-2007 the VideoLAN team
 * $Id$
 *
 * Authors: Derk-Jan Hartman <hartman at videolan dot org>
 *          Rémi Denis-Courmont
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#include <vlc/vlc.h>
#include <vlc_playlist.h>
#include <vlc_input.h>
#include <vlc_access.h>
#include <vlc_demux.h>

#include <stdlib.h>
#include <string.h>
#ifdef HAVE_SYS_TYPES_H
#   include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#   include <sys/stat.h>
#endif
#ifdef HAVE_ERRNO_H
#   include <errno.h>
#endif
#ifdef HAVE_FCNTL_H
#   include <fcntl.h>
#endif

#ifdef HAVE_UNISTD_H
#   include <unistd.h>
#elif defined( WIN32 ) && !defined( UNDER_CE )
#   include <io.h>
#elif defined( UNDER_CE )
#   define strcoll strcmp
#endif

#ifdef HAVE_DIRENT_H
#   include <dirent.h>
#endif

#include <vlc_charset.h>

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Open ( vlc_object_t * );
static void Close( vlc_object_t * );

static int  DemuxOpen ( vlc_object_t * );

#define RECURSIVE_TEXT N_("Subdirectory behavior")
#define RECURSIVE_LONGTEXT N_( \
        "Select whether subdirectories must be expanded.\n" \
        "none: subdirectories do not appear in the playlist.\n" \
        "collapse: subdirectories appear but are expanded on first play.\n" \
        "expand: all subdirectories are expanded.\n" )

static const char *psz_recursive_list[] = { "none", "collapse", "expand" };
static const char *psz_recursive_list_text[] = { N_("none"), N_("collapse"),
                                                 N_("expand") };

#define IGNORE_TEXT N_("Ignored extensions")
#define IGNORE_LONGTEXT N_( \
        "Files with these extensions will not be added to playlist when " \
        "opening a directory.\n" \
        "This is useful if you add directories that contain playlist files " \
        "for instance. Use a comma-separated list of extensions." )

vlc_module_begin();
    set_category( CAT_INPUT );
    set_shortname( _("Directory" ) );
    set_subcategory( SUBCAT_INPUT_ACCESS );
    set_description( _("Standard filesystem directory input") );
    set_capability( "access2", 55 );
    add_shortcut( "directory" );
    add_shortcut( "dir" );
    add_shortcut( "file" );
    add_string( "recursive", "expand" , NULL, RECURSIVE_TEXT,
                RECURSIVE_LONGTEXT, VLC_FALSE );
      change_string_list( psz_recursive_list, psz_recursive_list_text, 0 );
    add_string( "ignore-filetypes", "m3u,db,nfo,jpg,gif,sfv,txt,sub,idx,srt,cue",
                NULL, IGNORE_TEXT, IGNORE_LONGTEXT, VLC_FALSE );
    set_callbacks( Open, Close );

    add_submodule();
        set_description( "Directory EOF");
        set_capability( "demux2", 0 );
        set_callbacks( DemuxOpen, NULL );
vlc_module_end();


/*****************************************************************************
 * Local prototypes, constants, structures
 *****************************************************************************/

enum
{
    MODE_EXPAND,
    MODE_COLLAPSE,
    MODE_NONE
};

typedef struct stat_list_t stat_list_t;

static int Read( access_t *, uint8_t *, int );
static int ReadNull( access_t *, uint8_t *, int );
static int Control( access_t *, int, va_list );

static int Demux( demux_t *p_demux );
static int DemuxControl( demux_t *p_demux, int i_query, va_list args );


static int ReadDir( playlist_t *, const char *psz_name, int i_mode,
                    playlist_item_t *, playlist_item_t *, input_item_t *,
                    DIR *handle, stat_list_t *stats );

static DIR *OpenDir (vlc_object_t *obj, const char *psz_name);

/*****************************************************************************
 * Open: open the directory
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    access_t *p_access = (access_t*)p_this;

    DIR *handle = OpenDir (p_this, p_access->psz_path);
    if (handle == NULL)
        return VLC_EGENERIC;

    p_access->p_sys = (access_sys_t *)handle;

    p_access->pf_read  = Read;
    p_access->pf_block = NULL;
    p_access->pf_seek  = NULL;
    p_access->pf_control= Control;

    /* Force a demux */
    p_access->psz_demux = strdup( "directory" );

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close: close the target
 *****************************************************************************/
static void Close( vlc_object_t * p_this )
{
    access_t *p_access = (access_t*)p_this;
    DIR *handle = (DIR *)p_access->p_sys;
    closedir (handle);
}

/*****************************************************************************
 * ReadNull: read the directory
 *****************************************************************************/
static int ReadNull( access_t *p_access, uint8_t *p_buffer, int i_len)
{
    /* Return fake data */
    memset( p_buffer, 0, i_len );
    return i_len;
}

/*****************************************************************************
 * Read: read the directory
 *****************************************************************************/
static int Read( access_t *p_access, uint8_t *p_buffer, int i_len)
{
    char               *psz;
    int                 i_mode, i_activity;
    char               *psz_name = strdup (p_access->psz_path);

    if( psz_name == NULL )
        return VLC_ENOMEM;

    playlist_t         *p_playlist = pl_Yield( p_access );
    input_thread_t     *p_input = (input_thread_t*)vlc_object_find( p_access, VLC_OBJECT_INPUT, FIND_PARENT );

    playlist_item_t    *p_item_in_category;
    input_item_t       *p_current_input;
    playlist_item_t    *p_current;

    if( !p_input )
    {
        msg_Err( p_access, "unable to find input (internal error)" );
        vlc_object_release( p_playlist );
        return VLC_ENOOBJ;
    }

    p_current_input = input_GetItem( p_input );
    p_current = playlist_ItemGetByInput( p_playlist, p_current_input, VLC_FALSE );

    if( !p_current )
    {
        msg_Err( p_access, "unable to find item in playlist" );
        vlc_object_release( p_input );
        vlc_object_release( p_playlist );
        return VLC_ENOOBJ;
    }

    /* Remove the ending '/' char */
    if( psz_name[0] )
    {
        char *ptr = psz_name + strlen (psz_name);
        switch (*--ptr)
        {
            case '/':
            case '\\':
                *ptr = '\0';
        }
    }

    /* Handle mode */
    psz = var_CreateGetString( p_access, "recursive" );
    if( *psz == '\0' || !strncmp( psz, "none" , 4 )  )
        i_mode = MODE_NONE;
    else if( !strncmp( psz, "collapse", 8 )  )
        i_mode = MODE_COLLAPSE;
    else
        i_mode = MODE_EXPAND;
    free( psz );

    p_current->p_input->i_type = ITEM_TYPE_DIRECTORY;
    p_item_in_category = playlist_ItemToNode( p_playlist, p_current,
                                              VLC_FALSE );

    i_activity = var_GetInteger( p_playlist, "activity" );
    var_SetInteger( p_playlist, "activity", i_activity +
                    DIRECTORY_ACTIVITY );

    ReadDir( p_playlist, psz_name, i_mode, p_current, p_item_in_category,
             p_current_input, (DIR *)p_access->p_sys, NULL );

    i_activity = var_GetInteger( p_playlist, "activity" );
    var_SetInteger( p_playlist, "activity", i_activity -
                    DIRECTORY_ACTIVITY );

    playlist_Signal( p_playlist );

    if( psz_name ) free( psz_name );
    vlc_object_release( p_input );
    vlc_object_release( p_playlist );

    /* Return fake data forever */
    p_access->pf_read = ReadNull;
    return ReadNull( p_access, p_buffer, i_len );
}

/*****************************************************************************
 * Control:
 *****************************************************************************/
static int Control( access_t *p_access, int i_query, va_list args )
{
    vlc_bool_t   *pb_bool;
    int          *pi_int;
    int64_t      *pi_64;

    switch( i_query )
    {
        /* */
        case ACCESS_CAN_SEEK:
        case ACCESS_CAN_FASTSEEK:
        case ACCESS_CAN_PAUSE:
        case ACCESS_CAN_CONTROL_PACE:
            pb_bool = (vlc_bool_t*)va_arg( args, vlc_bool_t* );
            *pb_bool = VLC_FALSE;    /* FIXME */
            break;

        /* */
        case ACCESS_GET_MTU:
            pi_int = (int*)va_arg( args, int * );
            *pi_int = 0;
            break;

        case ACCESS_GET_PTS_DELAY:
            pi_64 = (int64_t*)va_arg( args, int64_t * );
            *pi_64 = DEFAULT_PTS_DELAY * 1000;
            break;

        /* */
        case ACCESS_SET_PAUSE_STATE:
        case ACCESS_GET_TITLE_INFO:
        case ACCESS_SET_TITLE:
        case ACCESS_SET_SEEKPOINT:
        case ACCESS_SET_PRIVATE_ID_STATE:
            return VLC_EGENERIC;

        default:
            msg_Warn( p_access, "unimplemented query in control" );
            return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

/*****************************************************************************
 * DemuxOpen:
 *****************************************************************************/
static int DemuxOpen ( vlc_object_t *p_this )
{
    demux_t *p_demux = (demux_t*)p_this;

    if( strcmp( p_demux->psz_demux, "directory" ) )
        return VLC_EGENERIC;

    p_demux->pf_demux   = Demux;
    p_demux->pf_control = DemuxControl;
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Demux: EOF
 *****************************************************************************/
static int Demux( demux_t *p_demux )
{
    return 0;
}

/*****************************************************************************
 * DemuxControl:
 *****************************************************************************/
static int DemuxControl( demux_t *p_demux, int i_query, va_list args )
{
    return demux2_vaControlHelper( p_demux->s, 0, 0, 0, 1, i_query, args );
}


static int Sort (const char **a, const char **b)
{
    return strcoll (*a, *b);
}

struct stat_list_t
{
    stat_list_t *parent;
    struct stat st;
};


/*****************************************************************************
 * ReadDir: read a directory and add its content to the list
 *****************************************************************************/
static int ReadDir( playlist_t *p_playlist, const char *psz_name,
                    int i_mode, playlist_item_t *p_parent,
                    playlist_item_t *p_parent_category,
                    input_item_t *p_current_input,
                    DIR *handle, stat_list_t *stparent )
{
    char **pp_dir_content = NULL;
    int             i_dir_content, i, i_return = VLC_SUCCESS;
    playlist_item_t *p_node;

    char **ppsz_extensions = NULL;
    int i_extensions = 0;
    char *psz_ignore;

    struct stat_list_t stself;
#ifndef WIN32
    int fd = dirfd (handle);

    if ((fd == -1) || fstat (fd, &stself.st))
    {
        msg_Err (p_playlist, "cannot stat `%s': %s", psz_name,
                 strerror (errno));
        return VLC_EGENERIC;
    }

    for (stat_list_t *stats = stparent; stats != NULL; stats = stats->parent)
    {
        if ((stself.st.st_ino == stats->st.st_ino)
         && (stself.st.st_dev == stats->st.st_dev))
        {
            msg_Warn (p_playlist,
                      "ignoring infinitely recursive directory `%s'",
                      psz_name);
            return VLC_SUCCESS;
        }
    }
#else
        /* Windows has st_dev (driver letter - 'A'), but it zeroes st_ino,
         * so that the test above will always incorrectly succeed.
         * Besides, Windows does not have dirfd(). */
#endif

    stself.parent = stparent;

    /* Get the first directory entry */
    i_dir_content = utf8_loaddir (handle, &pp_dir_content, NULL, Sort);
    if( i_dir_content == -1 )
    {
        msg_Err (p_playlist, "cannot read `%s': %s", psz_name,
                 strerror (errno));
        return VLC_EGENERIC;
    }
    else if( i_dir_content <= 0 )
    {
        /* directory is empty */
        msg_Dbg( p_playlist, "%s directory is empty", psz_name );
        free( pp_dir_content );
        return VLC_SUCCESS;
    }

    /* Build array with ignores */
    psz_ignore = var_CreateGetString( p_playlist, "ignore-filetypes" );
    if( psz_ignore && *psz_ignore )
    {
        char *psz_parser = psz_ignore;
        int a;

        for( a = 0; psz_parser[a] != '\0'; a++ )
        {
            if( psz_parser[a] == ',' ) i_extensions++;
        }

        ppsz_extensions = (char **)calloc (i_extensions, sizeof (char *));

        for( a = 0; a < i_extensions; a++ )
        {
            char *tmp, *ptr;

            while( psz_parser[0] != '\0' && psz_parser[0] == ' ' ) psz_parser++;
            ptr = strchr( psz_parser, ',');
            tmp = ( ptr == NULL )
                 ? strdup( psz_parser )
                 : strndup( psz_parser, ptr - psz_parser );

            ppsz_extensions[a] = tmp;
            psz_parser = ptr + 1;
        }
    }
    if( psz_ignore ) free( psz_ignore );

    /* While we still have entries in the directory */
    for( i = 0; i < i_dir_content; i++ )
    {
        const char *entry = pp_dir_content[i];
        int i_size_entry = strlen( psz_name ) +
                           strlen( entry ) + 2 + 7 /* strlen("file://") */;
        char psz_uri[i_size_entry];

        sprintf( psz_uri, "%s/%s", psz_name, entry);

        /* if it starts with '.' then forget it */
        if (entry[0] != '.')
        {
            DIR *subdir = (i_mode != MODE_COLLAPSE)
                    ? OpenDir (VLC_OBJECT (p_playlist), psz_uri) : NULL;

            if (subdir != NULL) /* Recurse into subdirectory */
            {
                if( i_mode == MODE_NONE )
                {
                    msg_Dbg( p_playlist, "skipping subdirectory `%s'",
                             psz_uri );
                    closedir (subdir);
                    continue;
                }

                msg_Dbg (p_playlist, "creating subdirectory %s", psz_uri);

                p_node = playlist_NodeCreate( p_playlist, entry,
                                              p_parent_category,
                                              PLAYLIST_NO_REBUILD );

                /* If we had the parent in category, the it is now node.
                 * Else, we still don't have  */
                i_return = ReadDir( p_playlist, psz_uri , MODE_EXPAND,
                                    p_node, p_parent_category ? p_node : NULL,
                                    p_current_input, subdir, &stself );
                closedir (subdir);
                if (i_return)
                    break; // error :-(
            }
            else
            {
                input_item_t *p_input;

                if( i_extensions > 0 )
                {
                    const char *psz_dot = strrchr (entry, '.' );
                    if( psz_dot++ && *psz_dot )
                    {
                        int a;
                        for( a = 0; a < i_extensions; a++ )
                        {
                            if( !strcmp( psz_dot, ppsz_extensions[a] ) )
                                break;
                        }
                        if( a < i_extensions )
                        {
                            msg_Dbg( p_playlist, "ignoring file %s", psz_uri );
                            continue;
                        }
                    }
                }

                memmove (psz_uri + 7, psz_uri, sizeof (psz_uri) - 7);
                memcpy (psz_uri, "file://", 7);
                p_input = input_ItemNewWithType( VLC_OBJECT(p_playlist),
                                                 psz_uri, entry, 0, NULL,
                                                 -1, ITEM_TYPE_VFILE );
                if (p_input != NULL)
                {
                    if( p_current_input )
                        input_ItemCopyOptions( p_current_input, p_input );
                    playlist_BothAddInput( p_playlist, p_input,
                                           p_parent_category,
                                           PLAYLIST_APPEND|PLAYLIST_PREPARSE|
                                           PLAYLIST_NO_REBUILD,
                                           PLAYLIST_END, NULL, NULL,
                                           VLC_FALSE );
                }
            }
        }
    }

    for( i = 0; i < i_extensions; i++ )
        if( ppsz_extensions[i] ) free( ppsz_extensions[i] );
    if( ppsz_extensions ) free( ppsz_extensions );

    for( i = 0; i < i_dir_content; i++ )
        if( pp_dir_content[i] ) free( pp_dir_content[i] );
    if( pp_dir_content ) free( pp_dir_content );

    return i_return;
}


static DIR *OpenDir (vlc_object_t *obj, const char *path)
{
    msg_Dbg (obj, "opening directory `%s'", path);
    DIR *handle = utf8_opendir (path);
    if (handle == NULL)
    {
        int err = errno;
        if (err != ENOTDIR)
            msg_Err (obj, "%s: %s", path, strerror (err));
        else
            msg_Dbg (obj, "skipping non-directory `%s'", path);
        errno = err;

        return NULL;
    }
    return handle;
}
