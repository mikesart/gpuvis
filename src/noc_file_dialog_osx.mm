#define NOC_FILE_DIALOG_IMPLEMENTATION
#define NOC_FILE_DIALOG_OSX
#include "noc_file_dialog.h"

#include <AppKit/AppKit.h>

const char *noc_file_dialog_open(int flags,
                                 const char *filters,
                                 const char *default_path,
                                 const char *default_name)
{
    NSURL *url;
    const char *utf8_path;
    NSSavePanel *panel;
    NSOpenPanel *open_panel;
    NSMutableArray *types_array;
    NSURL *default_url;
    char buf[128], *patterns;
    // XXX: I don't know about memory management with cococa, need to check
    // if I leak memory here.
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

    if (flags & NOC_FILE_DIALOG_OPEN) {
        panel = open_panel = [NSOpenPanel openPanel];
    } else {
        panel = [NSSavePanel savePanel];
    }

    if (flags & NOC_FILE_DIALOG_DIR) {
        [open_panel setCanChooseDirectories:YES];
        [open_panel setCanChooseFiles:NO];
    }

    if (default_path) {
        default_url = [NSURL fileURLWithPath:
            [NSString stringWithUTF8String:default_path]];
        [panel setDirectoryURL:default_url];
        [panel setNameFieldStringValue:default_url.lastPathComponent];
    }

    if (filters) {
        types_array = [NSMutableArray array];
        while (*filters) {
            filters += strlen(filters) + 1; // skip the name
            // Split the filter pattern with ';'.
            strcpy(buf, filters);
            buf[strlen(buf) + 1] = '\0';
            for (patterns = buf; *patterns; patterns++)
                if (*patterns == ';') *patterns = '\0';
            patterns = buf;
            while (*patterns) {
                assert(strncmp(patterns, "*.", 2) == 0);
                patterns += 2; // Skip the "*."
                [types_array addObject:[NSString stringWithUTF8String: patterns]];
                patterns += strlen(patterns) + 1;
            }
            filters += strlen(filters) + 1;
        }
        [panel setAllowedFileTypes:types_array];
    }

    free(g_noc_file_dialog_ret);
    g_noc_file_dialog_ret = NULL;
    if ( [panel runModal] == NSModalResponseOK ) {
        url = [panel URL];
        utf8_path = [[url path] UTF8String];
        g_noc_file_dialog_ret = strdup(utf8_path);
    }

    [pool release];
    return g_noc_file_dialog_ret;
}
