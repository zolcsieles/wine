/*
 * Window related functions
 *
 * Copyright 1993, 1994, 1995, 1996, 2001 Alexandre Julliard
 * Copyright 1993 David Metcalfe
 * Copyright 1995, 1996 Alex Korobka
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"

#include <stdarg.h>
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winreg.h"
#include "winuser.h"
#include "wine/unicode.h"

#include "wine/debug.h"
#include "x11drv.h"
#include "win.h"
#include "winpos.h"
#include "mwm.h"

WINE_DEFAULT_DEBUG_CHANNEL(x11drv);

/* X context to associate a hwnd to an X window */
XContext winContext = 0;

Atom X11DRV_Atoms[NB_XATOMS - FIRST_XATOM];

static const char * const atom_names[NB_XATOMS - FIRST_XATOM] =
{
    "CLIPBOARD",
    "COMPOUND_TEXT",
    "MULTIPLE",
    "SELECTION_DATA",
    "TARGETS",
    "TEXT",
    "UTF8_STRING",
    "RAW_ASCENT",
    "RAW_DESCENT",
    "RAW_CAP_HEIGHT",
    "WM_PROTOCOLS",
    "WM_DELETE_WINDOW",
    "WM_TAKE_FOCUS",
    "KWM_DOCKWINDOW",
    "DndProtocol",
    "DndSelection",
    "_MOTIF_WM_HINTS",
    "_KDE_NET_WM_SYSTEM_TRAY_WINDOW_FOR",
    "_NET_WM_MOVERESIZE",
    "_NET_WM_PID",
    "_NET_WM_PING",
    "_NET_WM_NAME",
    "_NET_WM_WINDOW_TYPE",
    "_NET_WM_WINDOW_TYPE_UTILITY",
    "XdndAware",
    "XdndEnter",
    "XdndPosition",
    "XdndStatus",
    "XdndLeave",
    "XdndFinished",
    "XdndDrop",
    "XdndActionCopy",
    "XdndActionMove",
    "XdndActionLink",
    "XdndActionAsk",
    "XdndActionPrivate",
    "XdndSelection",
    "XdndTarget",
    "XdndTypeList",
    "WCF_DIB",
    "image/gif",
    "text/html",
    "text/plain",
    "text/rtf",
    "text/richtext"
};

static LPCSTR whole_window_atom;
static LPCSTR client_window_atom;
static LPCSTR icon_window_atom;

/***********************************************************************
 *		is_window_managed
 *
 * Check if a given window should be managed
 */
inline static BOOL is_window_managed( HWND hwnd )
{
    DWORD style, ex_style;

    if (!managed_mode) return FALSE;
    /* tray window is always managed */
    ex_style = GetWindowLongW( hwnd, GWL_EXSTYLE );
    if (ex_style & WS_EX_TRAYWINDOW) return TRUE;
    /* child windows are not managed */
    style = GetWindowLongW( hwnd, GWL_STYLE );
    if (style & WS_CHILD) return FALSE;
    /* windows with caption are managed */
    if ((style & WS_CAPTION) == WS_CAPTION) return TRUE;
    /* tool windows are not managed  */
    if (ex_style & WS_EX_TOOLWINDOW) return FALSE;
    /* windows with thick frame are managed */
    if (style & WS_THICKFRAME) return TRUE;
    /* application windows are managed */
    if (ex_style & WS_EX_APPWINDOW) return TRUE;
    /* full-screen popup windows are managed */
    if (style & WS_POPUP)
    {
        RECT rect;
        GetWindowRect( hwnd, &rect );
        if ((rect.right - rect.left) == screen_width && (rect.bottom - rect.top) == screen_height)
            return TRUE;
    }
    /* default: not managed */
    return FALSE;
}


/***********************************************************************
 *		is_client_window_mapped
 *
 * Check if the X client window should be mapped
 */
inline static BOOL is_client_window_mapped( struct x11drv_win_data *data )
{
    return !(GetWindowLongW( data->hwnd, GWL_STYLE ) & WS_MINIMIZE) && !IsRectEmpty( &data->client_rect );
}


/***********************************************************************
 *		X11DRV_is_window_rect_mapped
 *
 * Check if the X whole window should be mapped based on its rectangle
 */
BOOL X11DRV_is_window_rect_mapped( const RECT *rect )
{
    /* don't map if rect is empty */
    if (IsRectEmpty( rect )) return FALSE;

    /* don't map if rect is off-screen */
    if (rect->left >= (int)screen_width || rect->top >= (int)screen_height) return FALSE;
    if (rect->right < 0 || rect->bottom < 0) return FALSE;

    return TRUE;
}


/***********************************************************************
 *              get_window_attributes
 *
 * Fill the window attributes structure for an X window.
 */
static int get_window_attributes( struct x11drv_win_data *data, XSetWindowAttributes *attr )
{
    BOOL is_top_level = is_window_top_level( data->hwnd );
    BOOL managed = is_top_level && is_window_managed( data->hwnd );
    DWORD ex_style = GetWindowLongW( data->hwnd, GWL_EXSTYLE );

    if (managed) WIN_SetExStyle( data->hwnd, ex_style | WS_EX_MANAGED );
    else WIN_SetExStyle( data->hwnd, ex_style & ~WS_EX_MANAGED );

    attr->override_redirect = !managed;
    attr->colormap          = X11DRV_PALETTE_PaletteXColormap;
    attr->save_under        = ((GetClassLongW( data->hwnd, GCL_STYLE ) & CS_SAVEBITS) != 0);
    attr->cursor            = x11drv_thread_data()->cursor;
    attr->event_mask        = (ExposureMask | PointerMotionMask |
                               ButtonPressMask | ButtonReleaseMask | EnterWindowMask);

    if (is_top_level)
        attr->event_mask |= (KeyPressMask | KeyReleaseMask | StructureNotifyMask |
                             FocusChangeMask | KeymapStateMask);

    return (CWOverrideRedirect | CWSaveUnder | CWEventMask | CWColormap | CWCursor);
}


/***********************************************************************
 *              X11DRV_sync_window_style
 *
 * Change the X window attributes when the window style has changed.
 */
void X11DRV_sync_window_style( Display *display, struct x11drv_win_data *data )
{
    XSetWindowAttributes attr;
    int mask = get_window_attributes( data, &attr );

    wine_tsx11_lock();
    XChangeWindowAttributes( display, data->whole_window, mask, &attr );
    wine_tsx11_unlock();
}


/***********************************************************************
 *              get_window_changes
 *
 * fill the window changes structure
 */
static int get_window_changes( XWindowChanges *changes, const RECT *old, const RECT *new )
{
    int mask = 0;

    if (old->right - old->left != new->right - new->left )
    {
        if (!(changes->width = new->right - new->left)) changes->width = 1;
        mask |= CWWidth;
    }
    if (old->bottom - old->top != new->bottom - new->top)
    {
        if (!(changes->height = new->bottom - new->top)) changes->height = 1;
        mask |= CWHeight;
    }
    if (old->left != new->left)
    {
        changes->x = new->left;
        mask |= CWX;
    }
    if (old->top != new->top)
    {
        changes->y = new->top;
        mask |= CWY;
    }
    return mask;
}


/***********************************************************************
 *              create_icon_window
 */
static Window create_icon_window( Display *display, struct x11drv_win_data *data )
{
    XSetWindowAttributes attr;

    attr.event_mask = (ExposureMask | KeyPressMask | KeyReleaseMask | PointerMotionMask |
                       ButtonPressMask | ButtonReleaseMask | EnterWindowMask);
    attr.bit_gravity = NorthWestGravity;
    attr.backing_store = NotUseful/*WhenMapped*/;
    attr.colormap      = X11DRV_PALETTE_PaletteXColormap; /* Needed due to our visual */

    wine_tsx11_lock();
    data->icon_window = XCreateWindow( display, root_window, 0, 0,
                                       GetSystemMetrics( SM_CXICON ),
                                       GetSystemMetrics( SM_CYICON ),
                                       0, screen_depth,
                                       InputOutput, visual,
                                       CWEventMask | CWBitGravity | CWBackingStore | CWColormap, &attr );
    XSaveContext( display, data->icon_window, winContext, (char *)data->hwnd );
    wine_tsx11_unlock();

    TRACE( "created %lx\n", data->icon_window );
    SetPropA( data->hwnd, icon_window_atom, (HANDLE)data->icon_window );
    return data->icon_window;
}



/***********************************************************************
 *              destroy_icon_window
 */
inline static void destroy_icon_window( Display *display, struct x11drv_win_data *data )
{
    if (!data->icon_window) return;
    if (x11drv_thread_data()->cursor_window == data->icon_window)
        x11drv_thread_data()->cursor_window = None;
    wine_tsx11_lock();
    XDeleteContext( display, data->icon_window, winContext );
    XDestroyWindow( display, data->icon_window );
    data->icon_window = 0;
    wine_tsx11_unlock();
    RemovePropA( data->hwnd, icon_window_atom );
}


/***********************************************************************
 *              set_icon_hints
 *
 * Set the icon wm hints
 */
static void set_icon_hints( Display *display, struct x11drv_win_data *data,
                            XWMHints *hints, HICON hIcon, DWORD ex_style )
{
    if (data->hWMIconBitmap) DeleteObject( data->hWMIconBitmap );
    if (data->hWMIconMask) DeleteObject( data->hWMIconMask);
    data->hWMIconBitmap = 0;
    data->hWMIconMask = 0;

    if (!(ex_style & WS_EX_MANAGED))
    {
        destroy_icon_window( display, data );
        hints->flags &= ~(IconPixmapHint | IconMaskHint | IconWindowHint);
    }
    else if (!hIcon)
    {
        if (!data->icon_window) create_icon_window( display, data );
        hints->icon_window = data->icon_window;
        hints->flags = (hints->flags & ~(IconPixmapHint | IconMaskHint)) | IconWindowHint;
    }
    else
    {
        HBITMAP hbmOrig;
        RECT rcMask;
        BITMAP bmMask;
        ICONINFO ii;
        HDC hDC;

        GetIconInfo(hIcon, &ii);

        GetObjectA(ii.hbmMask, sizeof(bmMask), &bmMask);
        rcMask.top    = 0;
        rcMask.left   = 0;
        rcMask.right  = bmMask.bmWidth;
        rcMask.bottom = bmMask.bmHeight;

        hDC = CreateCompatibleDC(0);
        hbmOrig = SelectObject(hDC, ii.hbmMask);
        InvertRect(hDC, &rcMask);
        SelectObject(hDC, ii.hbmColor);  /* force the color bitmap to x11drv mode too */
        SelectObject(hDC, hbmOrig);
        DeleteDC(hDC);

        data->hWMIconBitmap = ii.hbmColor;
        data->hWMIconMask = ii.hbmMask;

        hints->icon_pixmap = X11DRV_BITMAP_Pixmap(data->hWMIconBitmap);
        hints->icon_mask = X11DRV_BITMAP_Pixmap(data->hWMIconMask);
        destroy_icon_window( display, data );
        hints->flags = (hints->flags & ~IconWindowHint) | IconPixmapHint | IconMaskHint;
    }
}


/***********************************************************************
 *              set_size_hints
 *
 * set the window size hints
 */
static void set_size_hints( Display *display, struct x11drv_win_data *data, DWORD style )
{
    XSizeHints* size_hints;

    if ((size_hints = XAllocSizeHints()))
    {
        size_hints->win_gravity = StaticGravity;
        size_hints->x = data->whole_rect.left;
        size_hints->y = data->whole_rect.top;
        size_hints->flags = PWinGravity | PPosition;

        if ( !(style & WS_THICKFRAME) )
        {
            size_hints->max_width = data->whole_rect.right - data->whole_rect.left;
            size_hints->max_height = data->whole_rect.bottom - data->whole_rect.top;
            size_hints->min_width = size_hints->max_width;
            size_hints->min_height = size_hints->max_height;
            size_hints->flags |= PMinSize | PMaxSize;
        }
        XSetWMNormalHints( display, data->whole_window, size_hints );
        XFree( size_hints );
    }
}


/***********************************************************************
 *              X11DRV_set_wm_hints
 *
 * Set the window manager hints for a newly-created window
 */
void X11DRV_set_wm_hints( Display *display, struct x11drv_win_data *data )
{
    Window group_leader;
    XClassHint *class_hints;
    XWMHints* wm_hints;
    Atom protocols[3];
    MwmHints mwm_hints;
    Atom dndVersion = 4;
    int i;
    DWORD style = GetWindowLongW( data->hwnd, GWL_STYLE );
    DWORD ex_style = GetWindowLongW( data->hwnd, GWL_EXSTYLE );
    HWND owner = GetWindow( data->hwnd, GW_OWNER );

    /* transient for hint */
    if (owner)
    {
        Window owner_win = X11DRV_get_whole_window( owner );
        wine_tsx11_lock();
        XSetTransientForHint( display, data->whole_window, owner_win );
        wine_tsx11_unlock();
        group_leader = owner_win;
    }
    else group_leader = data->whole_window;

    wine_tsx11_lock();

    /* wm protocols */
    i = 0;
    protocols[i++] = x11drv_atom(WM_DELETE_WINDOW);
    protocols[i++] = x11drv_atom(_NET_WM_PING);
    if (use_take_focus) protocols[i++] = x11drv_atom(WM_TAKE_FOCUS);
    XChangeProperty( display, data->whole_window, x11drv_atom(WM_PROTOCOLS),
                     XA_ATOM, 32, PropModeReplace, (char *)protocols, i );

    /* class hints */
    if ((class_hints = XAllocClassHint()))
    {
        class_hints->res_name = "wine";
        class_hints->res_class = "Wine";
        XSetClassHint( display, data->whole_window, class_hints );
        XFree( class_hints );
    }

    /* size hints */
    set_size_hints( display, data, style );

    /* systray properties (KDE only for now) */
    if (ex_style & WS_EX_TRAYWINDOW)
    {
        int val = 1;
        XChangeProperty( display, data->whole_window, x11drv_atom(KWM_DOCKWINDOW),
                         x11drv_atom(KWM_DOCKWINDOW), 32, PropModeReplace, (char*)&val, 1 );
        XChangeProperty( display, data->whole_window, x11drv_atom(_KDE_NET_WM_SYSTEM_TRAY_WINDOW_FOR),
                         XA_WINDOW, 32, PropModeReplace, (char*)&data->whole_window, 1 );
    }

    /* set the WM_CLIENT_MACHINE and WM_LOCALE_NAME properties */
    XSetWMProperties(display, data->whole_window, NULL, NULL, NULL, 0, NULL, NULL, NULL);
    /* set the pid. together, these properties are needed so the window manager can kill us if we freeze */
    i = getpid();
    XChangeProperty(display, data->whole_window, x11drv_atom(_NET_WM_PID),
                    XA_CARDINAL, 32, PropModeReplace, (char *)&i, 1);

   /* map WS_EX_TOOLWINDOW to _NET_WM_WINDOW_TYPE_UTILITY */
   if (ex_style & WS_EX_TOOLWINDOW)
   {
      Atom a = x11drv_atom(_NET_WM_WINDOW_TYPE_UTILITY);
      XChangeProperty(display, data->whole_window, x11drv_atom(_NET_WM_WINDOW_TYPE),
                      XA_ATOM, 32, PropModeReplace, (char*)&a, 1);
   }

    mwm_hints.flags = MWM_HINTS_FUNCTIONS | MWM_HINTS_DECORATIONS;
    mwm_hints.functions = 0;
    if ((style & WS_CAPTION) == WS_CAPTION) mwm_hints.functions |= MWM_FUNC_MOVE;
    if (style & WS_THICKFRAME) mwm_hints.functions |= MWM_FUNC_MOVE | MWM_FUNC_RESIZE;
    if (style & WS_MINIMIZEBOX) mwm_hints.functions |= MWM_FUNC_MINIMIZE;
    if (style & WS_MAXIMIZEBOX) mwm_hints.functions |= MWM_FUNC_MAXIMIZE;
    if (style & WS_SYSMENU)    mwm_hints.functions |= MWM_FUNC_CLOSE;
    mwm_hints.decorations = 0;
    if ((style & WS_CAPTION) == WS_CAPTION) mwm_hints.decorations |= MWM_DECOR_TITLE;
    if (ex_style & WS_EX_DLGMODALFRAME) mwm_hints.decorations |= MWM_DECOR_BORDER;
    else if (style & WS_THICKFRAME) mwm_hints.decorations |= MWM_DECOR_BORDER | MWM_DECOR_RESIZEH;
    else if ((style & (WS_DLGFRAME|WS_BORDER)) == WS_DLGFRAME) mwm_hints.decorations |= MWM_DECOR_BORDER;
    else if (style & WS_BORDER) mwm_hints.decorations |= MWM_DECOR_BORDER;
    else if (!(style & (WS_CHILD|WS_POPUP))) mwm_hints.decorations |= MWM_DECOR_BORDER;
    if (style & WS_SYSMENU)  mwm_hints.decorations |= MWM_DECOR_MENU;
    if (style & WS_MINIMIZEBOX) mwm_hints.decorations |= MWM_DECOR_MINIMIZE;
    if (style & WS_MAXIMIZEBOX) mwm_hints.decorations |= MWM_DECOR_MAXIMIZE;

    XChangeProperty( display, data->whole_window, x11drv_atom(_MOTIF_WM_HINTS),
                     x11drv_atom(_MOTIF_WM_HINTS), 32, PropModeReplace,
                     (char*)&mwm_hints, sizeof(mwm_hints)/sizeof(long) );

    XChangeProperty( display, data->whole_window, x11drv_atom(XdndAware),
                     XA_ATOM, 32, PropModeReplace, (unsigned char*)&dndVersion, 1 );

    wm_hints = XAllocWMHints();
    wine_tsx11_unlock();

    /* wm hints */
    if (wm_hints)
    {
        wm_hints->flags = InputHint | StateHint | WindowGroupHint;
        wm_hints->input = !(style & WS_DISABLED);

        set_icon_hints( display, data, wm_hints,
                        (HICON)GetClassLongW( data->hwnd, GCL_HICON ), ex_style );

        wm_hints->initial_state = (style & WS_MINIMIZE) ? IconicState : NormalState;
        wm_hints->window_group = group_leader;

        wine_tsx11_lock();
        XSetWMHints( display, data->whole_window, wm_hints );
        XFree(wm_hints);
        wine_tsx11_unlock();
    }
}


/***********************************************************************
 *              X11DRV_set_iconic_state
 *
 * Set the X11 iconic state according to the window style.
 */
void X11DRV_set_iconic_state( HWND hwnd )
{
    Display *display = thread_display();
    struct x11drv_win_data *data;
    RECT rect;
    XWMHints* wm_hints;
    DWORD style = GetWindowLongW( hwnd, GWL_STYLE );
    BOOL iconic = (style & WS_MINIMIZE) != 0;

    if (!(data = X11DRV_get_win_data( hwnd ))) return;

    GetWindowRect( hwnd, &rect );

    wine_tsx11_lock();

    if (iconic) XUnmapWindow( display, data->client_window );
    else if (!IsRectEmpty( &data->client_rect )) XMapWindow( display, data->client_window );

    if (!(wm_hints = XGetWMHints( display, data->whole_window ))) wm_hints = XAllocWMHints();
    wm_hints->flags |= StateHint | IconPositionHint;
    wm_hints->initial_state = iconic ? IconicState : NormalState;
    wm_hints->icon_x = rect.left;
    wm_hints->icon_y = rect.top;
    XSetWMHints( display, data->whole_window, wm_hints );

    if (style & WS_VISIBLE)
    {
        if (iconic)
            XIconifyWindow( display, data->whole_window, DefaultScreen(display) );
        else
            if (X11DRV_is_window_rect_mapped( &rect ))
                XMapWindow( display, data->whole_window );
    }

    XFree(wm_hints);
    wine_tsx11_unlock();
}


/***********************************************************************
 *		X11DRV_window_to_X_rect
 *
 * Convert a rect from client to X window coordinates
 */
void X11DRV_window_to_X_rect( HWND hwnd, RECT *rect )
{
    RECT rc;
    DWORD ex_style = GetWindowLongW( hwnd, GWL_EXSTYLE );

    if (!(ex_style & WS_EX_MANAGED)) return;
    if (IsRectEmpty( rect )) return;

    rc.top = rc.bottom = rc.left = rc.right = 0;

    AdjustWindowRectEx( &rc, GetWindowLongW(hwnd, GWL_STYLE) & ~(WS_HSCROLL|WS_VSCROLL),
                        FALSE, ex_style );

    rect->left   -= rc.left;
    rect->right  -= rc.right;
    rect->top    -= rc.top;
    rect->bottom -= rc.bottom;
    if (rect->top >= rect->bottom) rect->bottom = rect->top + 1;
    if (rect->left >= rect->right) rect->right = rect->left + 1;
}


/***********************************************************************
 *		X11DRV_X_to_window_rect
 *
 * Opposite of X11DRV_window_to_X_rect
 */
void X11DRV_X_to_window_rect( HWND hwnd, RECT *rect )
{
    DWORD ex_style = GetWindowLongW( hwnd, GWL_EXSTYLE );

    if (!(ex_style & WS_EX_MANAGED)) return;
    if (IsRectEmpty( rect )) return;

    AdjustWindowRectEx( rect, GetWindowLongW(hwnd, GWL_STYLE) & ~(WS_HSCROLL|WS_VSCROLL),
                        FALSE, ex_style );

    if (rect->top >= rect->bottom) rect->bottom = rect->top + 1;
    if (rect->left >= rect->right) rect->right = rect->left + 1;
}


/***********************************************************************
 *		X11DRV_sync_whole_window_position
 *
 * Synchronize the X whole window position with the Windows one
 */
int X11DRV_sync_whole_window_position( Display *display, struct x11drv_win_data *data, int zorder )
{
    XWindowChanges changes;
    int mask;
    RECT whole_rect;

    whole_rect = data->window_rect;
    X11DRV_window_to_X_rect( data->hwnd, &whole_rect );
    mask = get_window_changes( &changes, &data->whole_rect, &whole_rect );

    if (zorder)
    {
        if (is_window_top_level( data->hwnd ))
        {
            /* find window that this one must be after */
            HWND prev = GetWindow( data->hwnd, GW_HWNDPREV );
            while (prev && !(GetWindowLongW( prev, GWL_STYLE ) & WS_VISIBLE))
                prev = GetWindow( prev, GW_HWNDPREV );
            if (!prev)  /* top child */
            {
                changes.stack_mode = Above;
                mask |= CWStackMode;
            }
            else
            {
                /* should use stack_mode Below but most window managers don't get it right */
                /* so move it above the next one in Z order */
                HWND next = GetWindow( data->hwnd, GW_HWNDNEXT );
                while (next && !(GetWindowLongW( next, GWL_STYLE ) & WS_VISIBLE))
                    next = GetWindow( next, GW_HWNDNEXT );
                if (next)
                {
                    changes.stack_mode = Above;
                    changes.sibling = X11DRV_get_whole_window(next);
                    mask |= CWStackMode | CWSibling;
                }
            }
        }
        else
        {
            HWND next = GetWindow( data->hwnd, GW_HWNDNEXT );

            if (GetAncestor( data->hwnd, GA_PARENT ) == GetDesktopWindow() &&
                root_window != DefaultRootWindow(display))
            {
                /* in desktop mode we need the sibling to belong to the same process */
                while (next)
                {
                    if (X11DRV_get_win_data( next )) break;
                    next = GetWindow( next, GW_HWNDNEXT );
                }
            }

            if (!next)  /* bottom child */
            {
                changes.stack_mode = Below;
                mask |= CWStackMode;
            }
            else
            {
                changes.stack_mode = Above;
                changes.sibling = X11DRV_get_whole_window(next);
                mask |= CWStackMode | CWSibling;
            }
        }
    }

    data->whole_rect = whole_rect;

    if (mask)
    {
        TRACE( "setting win %lx pos %ld,%ld,%ldx%ld after %lx changes=%x\n",
               data->whole_window, whole_rect.left, whole_rect.top,
               whole_rect.right - whole_rect.left, whole_rect.bottom - whole_rect.top,
               changes.sibling, mask );
        wine_tsx11_lock();
        XSync( gdi_display, False );  /* flush graphics operations before moving the window */
        wine_tsx11_unlock();

        if (is_window_top_level( data->hwnd ))
        {
            DWORD style = GetWindowLongW( data->hwnd, GWL_STYLE );

            wine_tsx11_lock();
            if (mask & (CWWidth|CWHeight)) set_size_hints( display, data, style );
            XReconfigureWMWindow( display, data->whole_window,
                                  DefaultScreen(display), mask, &changes );
            wine_tsx11_unlock();
        }
        else
        {
            wine_tsx11_lock();
            XConfigureWindow( display, data->whole_window, mask, &changes );
            wine_tsx11_unlock();
        }
    }
    return mask;
}


/***********************************************************************
 *		X11DRV_sync_client_window_position
 *
 * Synchronize the X client window position with the Windows one
 */
int X11DRV_sync_client_window_position( Display *display, struct x11drv_win_data *data,
                                        const RECT *new_client_rect )
{
    XWindowChanges changes;
    int mask;
    RECT client_rect = *new_client_rect;

    OffsetRect( &client_rect, -data->whole_rect.left, -data->whole_rect.top );

    if ((mask = get_window_changes( &changes, &data->client_rect, &client_rect )))
    {
        BOOL is_mapped;

        TRACE( "setting win %lx pos %ld,%ld,%ldx%ld (was %ld,%ld,%ldx%ld) after %lx changes=%x\n",
               data->client_window, client_rect.left, client_rect.top,
               client_rect.right - client_rect.left, client_rect.bottom - client_rect.top,
               data->client_rect.left, data->client_rect.top,
               data->client_rect.right - data->client_rect.left,
               data->client_rect.bottom - data->client_rect.top,
               changes.sibling, mask );

        data->client_rect = client_rect;
        is_mapped = is_client_window_mapped( data );
        wine_tsx11_lock();
        XSync( gdi_display, False );  /* flush graphics operations before moving the window */
        if (!is_mapped) XUnmapWindow( display, data->client_window );
        XConfigureWindow( display, data->client_window, mask, &changes );
        if (is_mapped) XMapWindow( display, data->client_window );
        wine_tsx11_unlock();
    }
    return mask;
}


/***********************************************************************
 *		X11DRV_register_window
 *
 * Associate an X window to a HWND.
 */
void X11DRV_register_window( Display *display, HWND hwnd, struct x11drv_win_data *data )
{
    wine_tsx11_lock();
    XSaveContext( display, data->whole_window, winContext, (char *)hwnd );
    XSaveContext( display, data->client_window, winContext, (char *)hwnd );
    wine_tsx11_unlock();
}


/**********************************************************************
 *		create_desktop
 */
static void create_desktop( Display *display, struct x11drv_win_data *data )
{
    VisualID visualid;

    wine_tsx11_lock();
    winContext = XUniqueContext();
    XInternAtoms( display, (char **)atom_names, NB_XATOMS - FIRST_XATOM, False, X11DRV_Atoms );
    visualid = XVisualIDFromVisual(visual);
    wine_tsx11_unlock();

    whole_window_atom  = MAKEINTATOMA( GlobalAddAtomA( "__wine_x11_whole_window" ));
    client_window_atom = MAKEINTATOMA( GlobalAddAtomA( "__wine_x11_client_window" ));
    icon_window_atom   = MAKEINTATOMA( GlobalAddAtomA( "__wine_x11_icon_window" ));

    data->whole_window = data->client_window = root_window;
    data->whole_rect = data->client_rect = data->window_rect;

    SetPropA( data->hwnd, whole_window_atom, (HANDLE)root_window );
    SetPropA( data->hwnd, client_window_atom, (HANDLE)root_window );
    SetPropA( data->hwnd, "__wine_x11_visual_id", (HANDLE)visualid );

    X11DRV_InitClipboard();

    if (root_window != DefaultRootWindow(display)) X11DRV_create_desktop_thread();
}


/**********************************************************************
 *		create_whole_window
 *
 * Create the whole X window for a given window
 */
static Window create_whole_window( Display *display, struct x11drv_win_data *data, DWORD style )
{
    int cx, cy, mask;
    XSetWindowAttributes attr;
    Window parent;
    RECT rect;
    BOOL is_top_level = is_window_top_level( data->hwnd );

    rect = data->window_rect;
    X11DRV_window_to_X_rect( data->hwnd, &rect );

    if (!(cx = rect.right - rect.left)) cx = 1;
    if (!(cy = rect.bottom - rect.top)) cy = 1;

    parent = X11DRV_get_client_window( GetAncestor( data->hwnd, GA_PARENT ) );

    mask = get_window_attributes( data, &attr );

    /* set the attributes that don't change over the lifetime of the window */
    attr.bit_gravity       = ForgetGravity;
    attr.win_gravity       = NorthWestGravity;
    attr.backing_store     = NotUseful/*WhenMapped*/;
    mask |= CWBitGravity | CWWinGravity | CWBackingStore;

    wine_tsx11_lock();

    data->whole_rect = rect;
    data->whole_window = XCreateWindow( display, parent, rect.left, rect.top, cx, cy,
                                        0, screen_depth, InputOutput, visual,
                                        mask, &attr );

    if (!data->whole_window)
    {
        wine_tsx11_unlock();
        return 0;
    }

    /* non-maximized child must be at bottom of Z order */
    if ((style & (WS_CHILD|WS_MAXIMIZE)) == WS_CHILD)
    {
        XWindowChanges changes;
        changes.stack_mode = Below;
        XConfigureWindow( display, data->whole_window, CWStackMode, &changes );
    }

    wine_tsx11_unlock();

    if (is_top_level)
    {
        XIM xim = x11drv_thread_data()->xim;
        if (xim) data->xic = X11DRV_CreateIC( xim, display, data->whole_window );
        X11DRV_set_wm_hints( display, data );
    }

    return data->whole_window;
}


/**********************************************************************
 *		create_client_window
 *
 * Create the client window for a given window
 */
static Window create_client_window( Display *display, struct x11drv_win_data *data )
{
    RECT rect = data->whole_rect;
    XSetWindowAttributes attr;
    BOOL is_mapped;

    OffsetRect( &rect, -data->whole_rect.left, -data->whole_rect.top );
    data->client_rect = rect;
    is_mapped = is_client_window_mapped( data );

    attr.event_mask = (ExposureMask | PointerMotionMask |
                       ButtonPressMask | ButtonReleaseMask | EnterWindowMask);
    attr.bit_gravity = (GetClassLongW( data->hwnd, GCL_STYLE ) & (CS_VREDRAW | CS_HREDRAW)) ?
                       ForgetGravity : NorthWestGravity;
    attr.backing_store = NotUseful/*WhenMapped*/;

    wine_tsx11_lock();
    data->client_window = XCreateWindow( display, data->whole_window, 0, 0,
                                         max( rect.right - rect.left, 1 ),
                                         max( rect.bottom - rect.top, 1 ),
                                         0, screen_depth,
                                         InputOutput, visual,
                                         CWEventMask | CWBitGravity | CWBackingStore, &attr );
    if (data->client_window && is_mapped) XMapWindow( display, data->client_window );
    wine_tsx11_unlock();
    return data->client_window;
}


/*****************************************************************
 *		SetWindowText   (X11DRV.@)
 */
BOOL X11DRV_SetWindowText( HWND hwnd, LPCWSTR text )
{
    Display *display = thread_display();
    UINT count;
    char *buffer;
    char *utf8_buffer;
    Window win;
    XTextProperty prop;

    if ((win = X11DRV_get_whole_window( hwnd )))
    {
        /* allocate new buffer for window text */
        count = WideCharToMultiByte(CP_UNIXCP, 0, text, -1, NULL, 0, NULL, NULL);
        if (!(buffer = HeapAlloc( GetProcessHeap(), 0, count )))
        {
            ERR("Not enough memory for window text\n");
            return FALSE;
        }
        WideCharToMultiByte(CP_UNIXCP, 0, text, -1, buffer, count, NULL, NULL);

        count = WideCharToMultiByte(CP_UTF8, 0, text, strlenW(text), NULL, 0, NULL, NULL);
        if (!(utf8_buffer = HeapAlloc( GetProcessHeap(), 0, count )))
        {
            ERR("Not enough memory for window text in UTF-8\n");
            HeapFree( GetProcessHeap(), 0, buffer );
            return FALSE;
        }
        WideCharToMultiByte(CP_UTF8, 0, text, strlenW(text), utf8_buffer, count, NULL, NULL);

        wine_tsx11_lock();
	if (XmbTextListToTextProperty( display, &buffer, 1, XStdICCTextStyle, &prop ) == Success)
	{
	    XSetWMName( display, win, &prop );
	    XSetWMIconName( display, win, &prop );
	    XFree( prop.value );
	}
        /*
        Implements a NET_WM UTF-8 title. It should be without a trailing \0,
        according to the standard
        ( http://www.pps.jussieu.fr/~jch/software/UTF8_STRING/UTF8_STRING.text ).
        */
        XChangeProperty( display, win, x11drv_atom(_NET_WM_NAME), x11drv_atom(UTF8_STRING),
                         8, PropModeReplace, (unsigned char *) utf8_buffer, count);
        wine_tsx11_unlock();

        HeapFree( GetProcessHeap(), 0, utf8_buffer );
        HeapFree( GetProcessHeap(), 0, buffer );
    }
    return TRUE;
}


/***********************************************************************
 *		DestroyWindow   (X11DRV.@)
 */
BOOL X11DRV_DestroyWindow( HWND hwnd )
{
    struct x11drv_thread_data *thread_data = x11drv_thread_data();
    Display *display = thread_data->display;
    WND *wndPtr = WIN_GetPtr( hwnd );
    struct x11drv_win_data *data = wndPtr->pDriverData;

    if (!data) goto done;

    if (data->whole_window)
    {
        TRACE( "win %p xwin %lx/%lx\n", hwnd, data->whole_window, data->client_window );
        if (thread_data->cursor_window == data->whole_window) thread_data->cursor_window = None;
        if (thread_data->last_focus == hwnd) thread_data->last_focus = 0;
        wine_tsx11_lock();
        XSync( gdi_display, False );  /* flush any reference to this drawable in GDI queue */
        XDeleteContext( display, data->whole_window, winContext );
        XDeleteContext( display, data->client_window, winContext );
        XDestroyWindow( display, data->whole_window );  /* this destroys client too */
        if (data->xic)
        {
            XUnsetICFocus( data->xic );
            XDestroyIC( data->xic );
        }
        wine_tsx11_unlock();
        destroy_icon_window( display, data );
    }

    if (data->hWMIconBitmap) DeleteObject( data->hWMIconBitmap );
    if (data->hWMIconMask) DeleteObject( data->hWMIconMask);
    HeapFree( GetProcessHeap(), 0, data );
    wndPtr->pDriverData = NULL;
 done:
    WIN_ReleasePtr( wndPtr );
    return TRUE;
}


/**********************************************************************
 *		CreateWindow   (X11DRV.@)
 */
BOOL X11DRV_CreateWindow( HWND hwnd, CREATESTRUCTA *cs, BOOL unicode )
{
    Display *display = thread_display();
    WND *wndPtr;
    struct x11drv_win_data *data;
    HWND insert_after;
    RECT rect;
    DWORD style;
    CBT_CREATEWNDA cbtc;
    BOOL ret = FALSE;

    if (cs->cx > 65535)
    {
        ERR( "invalid window width %d\n", cs->cx );
        cs->cx = 65535;
    }
    if (cs->cy > 65535)
    {
        ERR( "invalid window height %d\n", cs->cy );
        cs->cy = 65535;
    }
    if (cs->cx < 0)
    {
        ERR( "invalid window width %d\n", cs->cx );
        cs->cx = 0;
    }
    if (cs->cy < 0)
    {
        ERR( "invalid window height %d\n", cs->cy );
        cs->cy = 0;
    }

    if (!(data = HeapAlloc(GetProcessHeap(), 0, sizeof(*data)))) return FALSE;
    data->hwnd          = hwnd;
    data->whole_window  = 0;
    data->client_window = 0;
    data->icon_window   = 0;
    data->xic           = 0;
    data->hWMIconBitmap = 0;
    data->hWMIconMask   = 0;

    wndPtr = WIN_GetPtr( hwnd );
    wndPtr->pDriverData = data;

    /* initialize the dimensions before sending WM_GETMINMAXINFO */
    SetRect( &rect, cs->x, cs->y, cs->x + cs->cx, cs->y + cs->cy );
    X11DRV_set_window_pos( hwnd, 0, &rect, &rect, SWP_NOZORDER, 0 );

    if (!wndPtr->parent)
    {
        create_desktop( display, data );
        WIN_ReleasePtr( wndPtr );
        return TRUE;
    }
    WIN_ReleasePtr( wndPtr );

    if (!create_whole_window( display, data, cs->style )) goto failed;
    if (!create_client_window( display, data )) goto failed;
    wine_tsx11_lock();
    XSync( display, False );
    wine_tsx11_unlock();

    SetPropA( hwnd, whole_window_atom, (HANDLE)data->whole_window );
    SetPropA( hwnd, client_window_atom, (HANDLE)data->client_window );

    /* Call the WH_CBT hook */
    cbtc.lpcs = cs;
    cbtc.hwndInsertAfter = HWND_TOP;
    if (HOOK_CallHooks( WH_CBT, HCBT_CREATEWND, (WPARAM)hwnd, (LPARAM)&cbtc, unicode ))
    {
        TRACE("CBT-hook returned !0\n");
        goto failed;
    }

    /* Send the WM_GETMINMAXINFO message and fix the size if needed */
    if ((cs->style & WS_THICKFRAME) || !(cs->style & (WS_POPUP | WS_CHILD)))
    {
        POINT maxSize, maxPos, minTrack, maxTrack;

        WINPOS_GetMinMaxInfo( hwnd, &maxSize, &maxPos, &minTrack, &maxTrack);
        if (maxSize.x < cs->cx) cs->cx = maxSize.x;
        if (maxSize.y < cs->cy) cs->cy = maxSize.y;
        if (cs->cx < 0) cs->cx = 0;
        if (cs->cy < 0) cs->cy = 0;

        SetRect( &rect, cs->x, cs->y, cs->x + cs->cx, cs->y + cs->cy );
        if (!X11DRV_set_window_pos( hwnd, 0, &rect, &rect, SWP_NOZORDER, 0 )) return FALSE;
    }

    /* send WM_NCCREATE */
    TRACE( "hwnd %p cs %d,%d %dx%d\n", hwnd, cs->x, cs->y, cs->cx, cs->cy );
    if (unicode)
        ret = SendMessageW( hwnd, WM_NCCREATE, 0, (LPARAM)cs );
    else
        ret = SendMessageA( hwnd, WM_NCCREATE, 0, (LPARAM)cs );
    if (!ret)
    {
        WARN("aborted by WM_xxCREATE!\n");
        return FALSE;
    }

    /* make sure the window is still valid */
    if (!(data = X11DRV_get_win_data( hwnd ))) return FALSE;
    X11DRV_sync_window_style( display, data );

    /* send WM_NCCALCSIZE */
    rect = data->window_rect;
    SendMessageW( hwnd, WM_NCCALCSIZE, FALSE, (LPARAM)&rect );

    if (!(wndPtr = WIN_GetPtr(hwnd))) return FALSE;
    if (rect.left < wndPtr->rectWindow.left) rect.left = wndPtr->rectWindow.left;
    if (rect.right > wndPtr->rectWindow.right) rect.right = wndPtr->rectWindow.right;
    if (rect.top < wndPtr->rectWindow.top) rect.top = wndPtr->rectWindow.top;
    if (rect.bottom > wndPtr->rectWindow.bottom) rect.bottom = wndPtr->rectWindow.bottom;
    if (rect.left > rect.right || rect.top > rect.bottom) rect = wndPtr->rectWindow;

    /* yes, even if the CBT hook was called with HWND_TOP */
    insert_after = ((wndPtr->dwStyle & (WS_CHILD|WS_MAXIMIZE)) == WS_CHILD) ? HWND_BOTTOM : HWND_TOP;

    X11DRV_set_window_pos( hwnd, insert_after, &wndPtr->rectWindow, &rect, 0, 0 );
    X11DRV_register_window( display, hwnd, data );

    TRACE( "win %p window %ld,%ld,%ld,%ld client %ld,%ld,%ld,%ld whole %ld,%ld,%ld,%ld X client %ld,%ld,%ld,%ld xwin %x/%x\n",
           hwnd, wndPtr->rectWindow.left, wndPtr->rectWindow.top,
           wndPtr->rectWindow.right, wndPtr->rectWindow.bottom,
           wndPtr->rectClient.left, wndPtr->rectClient.top,
           wndPtr->rectClient.right, wndPtr->rectClient.bottom,
           data->whole_rect.left, data->whole_rect.top,
           data->whole_rect.right, data->whole_rect.bottom,
           data->client_rect.left, data->client_rect.top,
           data->client_rect.right, data->client_rect.bottom,
           (unsigned int)data->whole_window, (unsigned int)data->client_window );

    WIN_ReleasePtr( wndPtr );

    if (unicode)
        ret = (SendMessageW( hwnd, WM_CREATE, 0, (LPARAM)cs ) != -1);
    else
        ret = (SendMessageA( hwnd, WM_CREATE, 0, (LPARAM)cs ) != -1);

    if (!ret)
    {
        WIN_UnlinkWindow( hwnd );
        return FALSE;
    }

    /* Send the size messages */

    if (!(wndPtr = WIN_GetPtr(hwnd)) || wndPtr == WND_OTHER_PROCESS) return FALSE;
    if (!(wndPtr->flags & WIN_NEED_SIZE))
    {
        RECT rect = wndPtr->rectClient;
        WIN_ReleasePtr( wndPtr );
        /* send it anyway */
        if (((rect.right-rect.left) <0) ||((rect.bottom-rect.top)<0))
            WARN("sending bogus WM_SIZE message 0x%08lx\n",
                 MAKELONG(rect.right-rect.left, rect.bottom-rect.top));
        SendMessageW( hwnd, WM_SIZE, SIZE_RESTORED,
                      MAKELONG(rect.right-rect.left, rect.bottom-rect.top));
        SendMessageW( hwnd, WM_MOVE, 0, MAKELONG( rect.left, rect.top ) );
    }
    else WIN_ReleasePtr( wndPtr );

    /* Show the window, maximizing or minimizing if needed */

    style = GetWindowLongW( hwnd, GWL_STYLE );
    if (style & (WS_MINIMIZE | WS_MAXIMIZE))
    {
        extern UINT WINPOS_MinMaximize( HWND hwnd, UINT cmd, LPRECT rect ); /*FIXME*/

        RECT newPos;
        UINT swFlag = (style & WS_MINIMIZE) ? SW_MINIMIZE : SW_MAXIMIZE;
        WIN_SetStyle( hwnd, style & ~(WS_MAXIMIZE | WS_MINIMIZE) );
        WINPOS_MinMaximize( hwnd, swFlag, &newPos );
        swFlag = ((style & WS_CHILD) || GetActiveWindow())
            ? SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED
            : SWP_NOZORDER | SWP_FRAMECHANGED;
        SetWindowPos( hwnd, 0, newPos.left, newPos.top,
                      newPos.right, newPos.bottom, swFlag );
    }

    return TRUE;

 failed:
    X11DRV_DestroyWindow( hwnd );
    return FALSE;
}


/***********************************************************************
 *		X11DRV_get_win_data
 *
 * Return the X11 data structure associated with a window.
 */
struct x11drv_win_data *X11DRV_get_win_data( HWND hwnd )
{
    struct x11drv_win_data *ret = NULL;
    WND *win = WIN_GetPtr( hwnd );

    if (win && win != WND_OTHER_PROCESS)
    {
        ret = win->pDriverData;
        WIN_ReleasePtr( win );
    }
    return ret;
}


/***********************************************************************
 *		X11DRV_get_client_window
 *
 * Return the X window associated with the client area of a window
 */
Window X11DRV_get_client_window( HWND hwnd )
{
    struct x11drv_win_data *data = X11DRV_get_win_data( hwnd );

    if (!data) return (Window)GetPropA( hwnd, client_window_atom );
    return data->client_window;
}


/***********************************************************************
 *		X11DRV_get_whole_window
 *
 * Return the X window associated with the full area of a window
 */
Window X11DRV_get_whole_window( HWND hwnd )
{
    struct x11drv_win_data *data = X11DRV_get_win_data( hwnd );

    if (!data) return (Window)GetPropA( hwnd, whole_window_atom );
    return data->whole_window;
}


/***********************************************************************
 *		X11DRV_get_ic
 *
 * Return the X input context associated with a window
 */
XIC X11DRV_get_ic( HWND hwnd )
{
    struct x11drv_win_data *data = X11DRV_get_win_data( hwnd );

    if (!data) return 0;
    return data->xic;
}


/*****************************************************************
 *		SetParent   (X11DRV.@)
 */
HWND X11DRV_SetParent( HWND hwnd, HWND parent )
{
    Display *display = thread_display();
    WND *wndPtr;
    HWND retvalue;

    /* Windows hides the window first, then shows it again
     * including the WM_SHOWWINDOW messages and all */
    BOOL was_visible = ShowWindow( hwnd, SW_HIDE );

    if (!IsWindow( parent )) return 0;
    if (!(wndPtr = WIN_GetPtr(hwnd)) || wndPtr == WND_OTHER_PROCESS) return 0;

    retvalue = wndPtr->parent;  /* old parent */
    if (parent != retvalue)
    {
        struct x11drv_win_data *data = wndPtr->pDriverData;
        Window new_parent = X11DRV_get_client_window( parent );

        WIN_LinkWindow( hwnd, parent, HWND_TOP );

        if (parent != GetDesktopWindow()) /* a child window */
        {
            if (!(wndPtr->dwStyle & WS_CHILD))
            {
                HMENU menu = (HMENU)SetWindowLongPtrW( hwnd, GWLP_ID, 0 );
                if (menu) DestroyMenu( menu );
            }
        }

        if (is_window_top_level( data->hwnd )) X11DRV_set_wm_hints( display, data );
        X11DRV_sync_window_style( display, data );
        wine_tsx11_lock();
        XReparentWindow( display, data->whole_window, new_parent,
                         data->whole_rect.left, data->whole_rect.top );
        wine_tsx11_unlock();
    }
    WIN_ReleasePtr( wndPtr );

    /* SetParent additionally needs to make hwnd the topmost window
       in the x-order and send the expected WM_WINDOWPOSCHANGING and
       WM_WINDOWPOSCHANGED notification messages.
    */
    SetWindowPos( hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                  SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | (was_visible ? SWP_SHOWWINDOW : 0) );
    /* FIXME: a WM_MOVE is also generated (in the DefWindowProc handler
     * for WM_WINDOWPOSCHANGED) in Windows, should probably remove SWP_NOMOVE */

    return retvalue;
}


/*****************************************************************
 *		SetFocus   (X11DRV.@)
 *
 * Set the X focus.
 * Explicit colormap management seems to work only with OLVWM.
 */
void X11DRV_SetFocus( HWND hwnd )
{
    Display *display = thread_display();
    XWindowAttributes win_attr;
    Window win;

    /* Only mess with the X focus if there's */
    /* no desktop window and if the window is not managed by the WM. */
    if (root_window != DefaultRootWindow(display)) return;

    if (!hwnd)  /* If setting the focus to 0, uninstall the colormap */
    {
        wine_tsx11_lock();
        if (X11DRV_PALETTE_PaletteFlags & X11DRV_PALETTE_PRIVATE)
            XUninstallColormap( display, X11DRV_PALETTE_PaletteXColormap );
        wine_tsx11_unlock();
        return;
    }

    hwnd = GetAncestor( hwnd, GA_ROOT );
    if (GetWindowLongW( hwnd, GWL_EXSTYLE ) & WS_EX_MANAGED) return;
    if (!(win = X11DRV_get_whole_window( hwnd ))) return;

    /* Set X focus and install colormap */
    wine_tsx11_lock();
    if (XGetWindowAttributes( display, win, &win_attr ) &&
        (win_attr.map_state == IsViewable))
    {
        /* If window is not viewable, don't change anything */

        /* we must not use CurrentTime (ICCCM), so try to use last message time instead */
        /* FIXME: this is not entirely correct */
        XSetInputFocus( display, win, RevertToParent,
                        /* CurrentTime */
                        GetMessageTime() - EVENT_x11_time_to_win32_time(0));
        if (X11DRV_PALETTE_PaletteFlags & X11DRV_PALETTE_PRIVATE)
            XInstallColormap( display, X11DRV_PALETTE_PaletteXColormap );
    }
    wine_tsx11_unlock();
}


/**********************************************************************
 *		SetWindowIcon (X11DRV.@)
 *
 * hIcon or hIconSm has changed (or is being initialised for the
 * first time). Complete the X11 driver-specific initialisation
 * and set the window hints.
 *
 * This is not entirely correct, may need to create
 * an icon window and set the pixmap as a background
 */
void X11DRV_SetWindowIcon( HWND hwnd, UINT type, HICON icon )
{
    Display *display = thread_display();
    struct x11drv_win_data *data;
    DWORD ex_style;

    if (type != ICON_BIG) return;  /* nothing to do here */

    if (!(data = X11DRV_get_win_data( hwnd ))) return;

    ex_style = GetWindowLongW( hwnd, GWL_EXSTYLE );
    if (ex_style & WS_EX_MANAGED)
    {
        XWMHints* wm_hints;

        wine_tsx11_lock();
        if (!(wm_hints = XGetWMHints( display, data->whole_window ))) wm_hints = XAllocWMHints();
        wine_tsx11_unlock();
        if (wm_hints)
        {
            set_icon_hints( display, data, wm_hints, icon, ex_style );
            wine_tsx11_lock();
            XSetWMHints( display, data->whole_window, wm_hints );
            XFree( wm_hints );
            wine_tsx11_unlock();
        }
    }
}
