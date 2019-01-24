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

HOOK_FUNC( GTypeInstance *, g_type_check_instance_cast, ( instance, iface_type ), GTypeInstance *instance, GType iface_type )

HOOK_FUNC( gint, gtk_dialog_run, ( dialog ), GtkDialog *dialog )
HOOK_FUNC( GType, gtk_dialog_get_type, (), void )

HOOK_FUNC( gboolean, gtk_init_check, ( argc, argv ), int *argc, char ***argv )
HOOK_FUNC( gboolean, gtk_events_pending, (), void )
HOOK_FUNC( gboolean, gtk_main_iteration, (), void )
HOOK_FUNCV( void, gtk_widget_destroy, ( widget ), GtkWidget *widget )

HOOK_FUNC( GType, gtk_file_chooser_get_type, (), void )
HOOK_FUNC( gboolean, gtk_file_chooser_set_filename, (chooser, filename), GtkFileChooser *chooser, const char *filename )
HOOK_FUNCV( void, gtk_file_chooser_set_current_name, ( chooser, name ), GtkFileChooser *chooser, const gchar *name )
HOOK_FUNCV( void, gtk_file_chooser_set_do_overwrite_confirmation, ( chooser, do_overwrite_confirmation ), GtkFileChooser *chooser, gboolean do_overwrite_confirmation )
HOOK_FUNC( gchar *, gtk_file_chooser_get_filename, ( chooser ), GtkFileChooser *chooser )
HOOK_FUNC( GtkWidget *, gtk_file_chooser_dialog_new, ( title, parent, action, first_button_text, NULL ), const gchar *title, GtkWindow *parent, GtkFileChooserAction action, const gchar *first_button_text, ... )
HOOK_FUNCV( void, gtk_file_chooser_add_filter, ( chooser, filter ), GtkFileChooser *chooser, GtkFileFilter *filter )

HOOK_FUNC( GtkFileFilter *, gtk_file_filter_new, (), void )
HOOK_FUNCV( void, gtk_file_filter_set_name, ( filter, name ), GtkFileFilter *filter, const gchar *name )
HOOK_FUNCV( void, gtk_file_filter_add_pattern, ( filter, pattern ), GtkFileFilter *filter, const gchar *pattern )
