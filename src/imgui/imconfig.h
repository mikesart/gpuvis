//-----------------------------------------------------------------------------
// USER IMPLEMENTATION
// This file contains compile-time options for ImGui.
// Other options (memory allocation overrides, callbacks, etc.) can be set at runtime via the ImGuiIO structure - ImGui::GetIO().
//-----------------------------------------------------------------------------

#pragma once

//---- Define assertion handler. Defaults to calling assert().
//#define IM_ASSERT(_EXPR)  MyAssert(_EXPR)

//---- Define attributes of all API symbols declarations, e.g. for DLL under Windows.
//#define IMGUI_API __declspec( dllexport )
//#define IMGUI_API __declspec( dllimport )

//---- Include imgui_user.h at the end of imgui.h
//#define IMGUI_INCLUDE_IMGUI_USER_H

//---- Don't implement default handlers for Windows (so as not to link with OpenClipboard() and others Win32 functions)
//#define IMGUI_DISABLE_WIN32_DEFAULT_CLIPBOARD_FUNCS
//#define IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCS

//---- Don't implement test window functionality (ShowTestWindow()/ShowStyleEditor()/ShowUserGuide() methods will be empty)
//---- It is very strongly recommended to NOT disable the test windows. Please read the comment at the top of imgui_demo.cpp to learn why.
//#define IMGUI_DISABLE_TEST_WINDOWS

//---- Don't define obsolete functions names. Consider enabling from time to time or when updating to reduce like hood of using already obsolete function/names
//#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS

//---- Pack colors to BGRA instead of RGBA (remove need to post process vertex buffer in back ends)
//#define IMGUI_USE_BGRA_PACKED_COLOR

//---- Implement STB libraries in a namespace to avoid conflicts
//#define IMGUI_STB_NAMESPACE     ImGuiStb

//---- Define constructor and implicit cast operators to convert back<>forth from your math types and ImVec2/ImVec4.
/*
#define IM_VEC2_CLASS_EXTRA                                                 \
        ImVec2(const MyVec2& f) { x = f.x; y = f.y; }                       \
        operator MyVec2() const { return MyVec2(x,y); }

#define IM_VEC4_CLASS_EXTRA                                                 \
        ImVec4(const MyVec4& f) { x = f.x; y = f.y; z = f.z; w = f.w; }     \
        operator MyVec4() const { return MyVec4(x,y,z,w); }
*/

//---- Use 32-bit vertex indices (instead of default: 16-bit) to allow meshes with more than 64K vertices
//#define ImDrawIdx unsigned int

//---- Tip: You can add extra functions within the ImGui:: namespace, here or in your own headers files.
//---- e.g. create variants of the ImGui::Value() helper for your low-level math types, or your own widgets/helpers.
/*
namespace ImGui
{
    void    Value(const char* prefix, const MyMatrix44& v, const char* float_format = NULL);
}
*/

// Allow meshes with more than 64k indices (NB: probably a sign that we aren't clipping enough when using ImDrawList? but doesn't hurt/cost too much to have 32-bit indices anyway)
#define ImDrawIdx uint32_t

//$ TODO mikesart: quick hack to change text color until imgui gets official method.
//    https://github.com/ocornut/imgui/issues/902
#define IMGUI_TEXT_ESCAPE_SKIP()    \
        if (c == '\033' && s[1] && s[2] && s[3] && s[4]) \
        { \
            s += 5; \
            continue; \
        }

#define IMGUI_TEXT_ESCAPE_RENDER()  \
        if (c == '\033' && s[1] && s[2] && s[3] && s[4]) \
        { \
            const unsigned char *us = (const unsigned char *)s; \
            col = IM_COL32(us[1], us[2], us[3], us[4]); \
            s += 5; \
            continue; \
        }
