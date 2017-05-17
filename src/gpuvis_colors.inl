/*
 * Copyright 2017 Valve Software
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

_XTAG( ThemeAlpha, IM_COL32( 255, 255, 255, 255 ), "Global alpha value" )
_XTAG( ClearColor, IM_COL32( 114, 144, 154, 255 ), "Window background clear color" )

// Event list colors
_XTAG( EventListText, 0xffe6e6e6, "Event list text" )
_XTAG( EventListSel, 0xd9aa00ff, "Event list selected text" )
_XTAG( EventListHov, IM_COL32( 0xFF, 0x70, 0x60, 100 ), "Event list hovered graph events" )
_XTAG( VBlank0, 0xd9ff00db, "VBlank crtc0" )
_XTAG( VBlank1, 0x80fff300, "VBlank crtc1" )

// Graph colors
_XTAG( GraphBk, IM_COL32( 0x2F, 0x4F, 0x4F, 255 ), "Graph Background" )
_XTAG( GraphRowBk, 0xff141414, "Graph row background" )
_XTAG( RowLabelText, IM_COL32( 0xFF, 0xFF, 0xE0, 255 ), "Graph row label text" )
_XTAG( RowLabelTextBk, 0x94242424, "Graph row label text background" )
_XTAG( 1Event, 0xd9fff300, "1 event in graph" )
_XTAG( 2Event, 0xd9ffc200, "2 events grouped in graph" )
_XTAG( 3Event, 0xd9ffaa00, "3 events grouped in graph" )
_XTAG( 4Event, 0xd9ff7900, "3 events grouped in graph" )
_XTAG( 5Event, 0xd9ff6100, "3 events grouped in graph" )
_XTAG( 6Event, 0xd9ff4900, "3 events grouped in graph" )
_XTAG( HovEvent, IM_COL32( 0xc8, 0x1a, 0xff, 255 ), "Graph hovered event" )
_XTAG( SelEvent , IM_COL32( 0x17, 0xe6, 0x8d, 255 ), "Graph selected event" )
_XTAG( TimeTick, 0xd992ff00, "Graph ms time ticks" )
_XTAG( MousePos, 0x94fff300, "Graph mouse location line" )
_XTAG( MarkerA, IM_COL32( 0xEE, 0x82, 0xEE, 255 ), "Graph marker A" )
_XTAG( ZoomSel, IM_COL32( 0xFF, 0xFF, 0xFF, 80 ), "Graph mouse zoom selection" )
_XTAG( PrintLabelSat, IM_COL32( 230, 230, 230, 230 ), "Graph print row label saturation" )
_XTAG( PrintLabelAlpha, IM_COL32( 255, 255, 255, 255 ), "Graph print row label text alpha" )
_XTAG( BarUserspace, 0xd9fff300, "Graph timeline userspace bar" )
_XTAG( BarHwQueue, 0x80613030, "Graph timeline hw queue bar" )
_XTAG( BarHwRunning, 0xd9ffaa00, "Graph timeline hw running bar" )
_XTAG( BarSelRect, 0xd9fff300, "Graph timeline selected bar rectangle" )
_XTAG( BarText, IM_COL32( 0xff, 0xff, 0xff, 255 ), "Graph timeline bar text" )

_XTAG( FtracePrintText, IM_COL32( 0xFF, 0xFF, 0, 0xFF ), "Ftrace print text color" )
_XTAG( BrightText, IM_COL32( 0xFF, 0xFF, 0, 0xFF ), "Gpuvis bright text color" )

_XTAG( ImGui_Text, 0xffe6e6e6, "ImGui text" )
_XTAG( ImGui_TextDisabled, 0xff666666, "ImGui disabled text" )
_XTAG( ImGui_TextSelectedBg, 0x59fa9642, "ImGui selected text background" )
_XTAG( ImGui_TitleBg, 0xff303030, "ImGui title background" )
_XTAG( ImGui_TitleBgActive, 0xff2e2e2e, "ImGui active title background" )
_XTAG( ImGui_TitleBgCollapsed, 0x82000000, "ImGui active collapsed title background" )
_XTAG( ImGui_WindowBg, 0xfc0f0f0f, "ImGui background of normal windows" )
_XTAG( ImGui_Border, 0x30ffffff, "ImGui border" )
_XTAG( ImGui_BorderShadow, 0x1a000000, "ImGui border Shadow" )
_XTAG( ImGui_Button, 0xff333333, "ImGui button" )
_XTAG( ImGui_ButtonActive, 0xfffa870f, "ImGui active Button" )
_XTAG( ImGui_ButtonHovered, 0xfffa9642, "ImGui hovered Button" )
_XTAG( ImGui_CheckMark, 0xfffa9642, "ImGui check mark" )
_XTAG( ImGui_ChildWindowBg, 0xffffff, "ImGui background of child windows" )
_XTAG( ImGui_CloseButton, 0x80696969, "ImGui closed button" )
_XTAG( ImGui_CloseButtonActive, 0xff5c63fa, "ImGui active close button" )
_XTAG( ImGui_CloseButtonHovered, 0xff5c63fa, "ImGui hovered close button" )
_XTAG( ImGui_Column, 0xff9c9c9c, "ImGui column" )
_XTAG( ImGui_ColumnActive, 0xfffa9642, "ImGui active column" )
_XTAG( ImGui_ColumnHovered, 0xc7fa9642, "ImGui hovered column" )
_XTAG( ImGui_ComboBg, 0xfc242424, "ImGui combo background" )
_XTAG( ImGui_FrameBg, 0xff262626, "ImGui background of checkbox, radio button, plot, slider, text input" )
_XTAG( ImGui_FrameBgActive, 0xabfa9642, "ImGui active frame background" )
_XTAG( ImGui_FrameBgHovered, 0x66fa9642, "ImGui hovered frame background" )
_XTAG( ImGui_Header, 0xff1c1c1c, "ImGui header" )
_XTAG( ImGui_HeaderActive, 0xfffa9642, "ImGui active hader" )
_XTAG( ImGui_HeaderHovered, 0xccfa9642, "ImGui hovered header" )
_XTAG( ImGui_MenuBarBg, 0xff303030, "ImGui background menu bar" )
_XTAG( ImGui_ModalWindowDarkening, 0x59cccccc, "ImGui darken entire screen when a modal window is active" )
_XTAG( ImGui_PopupBg, 0xf0000000, "ImGui background of popups, menus, tooltips windows" )
_XTAG( ImGui_ResizeGrip, 0x40fa9642, "ImGui resize grip" )
_XTAG( ImGui_ResizeGripActive, 0xf2fa9642, "ImGui active resize grip" )
_XTAG( ImGui_ResizeGripHovered, 0xabfa9642, "ImGui hovered resize grip" )
_XTAG( ImGui_ScrollbarBg, 0x87050505, "ImGui scrollbar background" )
_XTAG( ImGui_ScrollbarGrab, 0xff4f4f4f, "ImGui scrollbar grab" )
_XTAG( ImGui_ScrollbarGrabActive, 0xff828282, "ImGui active scrollbar grab" )
_XTAG( ImGui_ScrollbarGrabHovered, 0xff696969, "ImGui hovered scrollbar grab" )
_XTAG( ImGui_SliderGrab, 0xffe0853d, "ImGui slider grab" )
_XTAG( ImGui_SliderGrabActive, 0xfffa9642, "ImGui active slider grab" )
