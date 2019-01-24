/*
 * Copyright 2019 Valve Software
 *
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <dlfcn.h>

char *g_errstr = NULL;
static void *g_libgtk = NULL;

// Function pointer typedefs
extern "C"
{
  #define HOOK_FUNC( _ret, _func, _args, ... ) typedef _ret ( _func ## _t )( __VA_ARGS__ );
  #define HOOK_FUNCV( _ret, _func, _args, ... ) typedef _ret ( _func ## _t )( __VA_ARGS__ );
    #include "hook_gtk3_funcs.inl"
  #undef HOOK_FUNC
  #undef HOOK_FUNCV
}

// Function pointers struct
struct
{
  #define HOOK_FUNC( _ret, _func, _args, ... ) _func ## _t *_func;
  #define HOOK_FUNCV( _ret, _func, _args, ... ) _func ## _t *_func;
    #include "hook_gtk3_funcs.inl"
  #undef HOOK_FUNC
  #undef HOOK_FUNCV
} g_gtk;

// Init function pointers struct
static const char *hook_gtk3_init()
{
    const char *fail_func = NULL;

    // Already loaded?
    if ( g_libgtk )
        return NULL;

    // Already error'd out?
    if ( g_errstr )
        return g_errstr;

    // Try to load gtk3 dso
    g_libgtk = dlopen( "libgtk-3.so.0", RTLD_NOW | RTLD_GLOBAL );
    if ( !g_libgtk )
        g_libgtk = dlopen( "libgtk-3.so", RTLD_NOW | RTLD_GLOBAL );
    if ( !g_libgtk )
    {
        if (-1 == asprintf( &g_errstr, "dlopen( libgtk-3.so ) failed: %s\n", dlerror() ))
          g_errstr = NULL;
        return g_errstr;
    }

  // Get addresses of gtk3 functions
  #define DLSYM_FUNC( _func )                                       \
      {                                                             \
          g_gtk._func = ( _func ## _t * )dlsym( g_libgtk, #_func ); \
          if ( !g_gtk._func )                                       \
          {                                                         \
              fail_func = #_func;                                   \
              break;                                                \
          }                                                         \
      }
  #define HOOK_FUNC( _ret, _func, _args, ... ) DLSYM_FUNC( _func )
  #define HOOK_FUNCV( _ret, _func, _args, ... )  DLSYM_FUNC( _func )

    do
    {
       #include "hook_gtk3_funcs.inl"
    } while ( 0 );

  #undef HOOK_FUNC
  #undef HOOK_FUNCV

    // Error out and free library if any dlsym calls failed
    if ( fail_func )
    {
        if (-1 == asprintf( &g_errstr, "dlsym( %s ) failed: %s\n", fail_func, dlerror() ))
          g_errstr = NULL;

        dlclose( g_libgtk );
        g_libgtk = NULL;
    }

    return g_errstr;
}

#define g_type_check_instance_cast                      g_gtk.g_type_check_instance_cast
#define gtk_dialog_run                                  g_gtk.gtk_dialog_run
#define gtk_dialog_get_type                             g_gtk.gtk_dialog_get_type
#define gtk_init_check                                  g_gtk.gtk_init_check
#define gtk_events_pending                              g_gtk.gtk_events_pending
#define gtk_main_iteration                              g_gtk.gtk_main_iteration
#define gtk_widget_destroy                              g_gtk.gtk_widget_destroy
#define gtk_file_chooser_get_type                       g_gtk.gtk_file_chooser_get_type
#define gtk_file_chooser_set_filename                   g_gtk.gtk_file_chooser_set_filename
#define gtk_file_chooser_set_current_name               g_gtk.gtk_file_chooser_set_current_name
#define gtk_file_chooser_set_do_overwrite_confirmation  g_gtk.gtk_file_chooser_set_do_overwrite_confirmation
#define gtk_file_chooser_get_filename                   g_gtk.gtk_file_chooser_get_filename
#define gtk_file_chooser_dialog_new                     g_gtk.gtk_file_chooser_dialog_new
#define gtk_file_chooser_add_filter                     g_gtk.gtk_file_chooser_add_filter
#define gtk_file_filter_new                             g_gtk.gtk_file_filter_new
#define gtk_file_filter_set_name                        g_gtk.gtk_file_filter_set_name
#define gtk_file_filter_add_pattern                     g_gtk.gtk_file_filter_add_pattern
