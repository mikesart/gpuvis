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

_XTAG( col_ThemeAlpha, IM_COL32( 255, 255, 255, 255 ), "Global alpha value" )
_XTAG( col_ClearColor, IM_COL32( 114, 144, 154, 255 ), "Window background clear color" )

_XTAG( col_VBlank0, 0xd9ff00db, "VBlank crtc0" )
_XTAG( col_VBlank1, 0x80fff300, "VBlank crtc1" )
_XTAG( col_VBlank2, 0x8015cece, "VBlank crtc2" )
_XTAG( col_FtracePrintText, 0xffcca014, "Ftrace print text color" )
_XTAG( col_BrightText, 0xffcca014, "Gpuvis bright text color" )

// Event list colors
_XTAG( col_EventList_Text, 0xffe6e6e6, "Event list text" )
_XTAG( col_EventList_Sel, 0xff7a1822, "Event list selected text" )
_XTAG( col_EventList_Hov, 0xff78490b, "Event list hovered graph events" )

// Graph colors
_XTAG( col_Graph_Bk, IM_COL32( 0x2F, 0x4F, 0x4F, 255 ), "Graph Background" )
_XTAG( col_Graph_RowBk, 0xff141414, "Graph row background" )
_XTAG( col_Graph_RowLabelText, IM_COL32( 0xFF, 0xFF, 0xE0, 255 ), "Graph row label text" )
_XTAG( col_Graph_RowLabelTextBk, 0x94242424, "Graph row label text background" )
_XTAG( col_Graph_1Event, 0xd9fff300, "1 event in graph" )
_XTAG( col_Graph_2Event, 0xd9ffc200, "2 events grouped in graph" )
_XTAG( col_Graph_3Event, 0xd9ffaa00, "3 events grouped in graph" )
_XTAG( col_Graph_4Event, 0xd9ff7900, "3 events grouped in graph" )
_XTAG( col_Graph_5Event, 0xd9ff6100, "3 events grouped in graph" )
_XTAG( col_Graph_6Event, 0xd9ff4900, "3 events grouped in graph" )
_XTAG( col_Graph_HovEvent, IM_COL32( 0xc8, 0x1a, 0xff, 255 ), "Graph hovered event" )
_XTAG( col_Graph_SelEvent , IM_COL32( 0x17, 0xe6, 0x8d, 255 ), "Graph selected event" )
_XTAG( col_Graph_TimeTick, 0xd992ff00, "Graph ms time ticks" )
_XTAG( col_Graph_MousePos, 0x94fff300, "Graph mouse location line" )
_XTAG( col_Graph_MarkerA, IM_COL32( 0xEE, 0x82, 0xEE, 255 ), "Graph marker A" )
_XTAG( col_Graph_MarkerB, IM_COL32( 0xEE, 0xEE, 0x82, 255 ), "Graph marker B" )
_XTAG( col_Graph_ZoomSel, IM_COL32( 0xFF, 0xFF, 0xFF, 80 ), "Graph mouse zoom selection" )
_XTAG( col_Graph_PrintLabelSat, IM_COL32( 230, 230, 230, 230 ), "Graph print row label saturation" )
_XTAG( col_Graph_PrintLabelAlpha, IM_COL32( 255, 255, 255, 255 ), "Graph print row label text alpha" )
_XTAG( col_Graph_PrintBarAlpha, IM_COL32( 80, 80, 80, 80 ), "Graph print row bar alpha" )
_XTAG( col_Graph_TimelineLabelSat, IM_COL32( 230, 230, 230, 230 ), "Graph timeline row label saturation" )
_XTAG( col_Graph_TimelineLabelAlpha, IM_COL32( 255, 255, 255, 255 ), "Graph timeline row label text alpha" )
_XTAG( col_Graph_BarUserspace, 0xd9fff300, "Graph timeline userspace bar" )
_XTAG( col_Graph_BarHwQueue, 0x80613030, "Graph timeline hw queue bar" )
_XTAG( col_Graph_BarHwRunning, 0xd9ffaa00, "Graph timeline hw running bar" )
_XTAG( col_Graph_BarSelRect, 0xd9fff300, "Graph timeline selected bar rectangle" )
_XTAG( col_Graph_BarText, IM_COL32( 0xff, 0xff, 0xff, 255 ), "Graph timeline bar text" )
_XTAG( col_Graph_TaskRunning, 0x4fff00ff, "Sched_switch task running block" )
_XTAG( col_Graph_TaskSleeping, 0x4fffff00, "Sched_switch task sleeping block" )

_XTAG( col_Graph_Bari915ReqWait, 0x4f0000ff, "i915 reqwait bar" )

_XTAG( col_Graph_Bari915Queue, 0xc81d740c, "Request queued waiting to be added" )
_XTAG( col_Graph_Bari915SubmitDelay, 0xc8f8552e, "Requests waiting on fences and dependencies before they are runnable" )
_XTAG( col_Graph_Bari915ExecuteDelay, 0xc89e9e9e, "Runnable requests waiting for a slot on GPU" )
_XTAG( col_Graph_Bari915Execute, 0xc8aa149a, "Requests executing on the GPU" )
_XTAG( col_Graph_Bari915CtxCompleteDelay, 0xc800a5ff, "User interrupt to context complete" )

_XTAG( col_FrameMarkerBk0, 0x32323232, "Frame Marker Background #0" )
_XTAG( col_FrameMarkerBk1, 0x64646464, "Frame Marker Background #1" )
_XTAG( col_FrameMarkerSelected, 0x0000ff31, "Selected Frame Marker Background" )

// ImGui colors
_XTAG( col_ImGui_Text, 0xffe6e6e6, "ImGui text" )
_XTAG( col_ImGui_TextDisabled, 0xff666666, "ImGui disabled text" )
_XTAG( col_ImGui_TextSelectedBg, 0x59fa9642, "ImGui selected text background" )
_XTAG( col_ImGui_TitleBg, 0xff303030, "ImGui title background" )
_XTAG( col_ImGui_TitleBgActive, 0xff2e2e2e, "ImGui active title background" )
_XTAG( col_ImGui_TitleBgCollapsed, 0x82000000, "ImGui active collapsed title background" )
_XTAG( col_ImGui_WindowBg, 0xfc0f0f0f, "ImGui background of normal windows" )
_XTAG( col_ImGui_Border, 0x30ffffff, "ImGui border" )
_XTAG( col_ImGui_BorderShadow, 0x1a000000, "ImGui border Shadow" )
_XTAG( col_ImGui_Button, 0xff333333, "ImGui button" )
_XTAG( col_ImGui_ButtonActive, 0xfffa870f, "ImGui active Button" )
_XTAG( col_ImGui_ButtonHovered, 0xfffa9642, "ImGui hovered Button" )
_XTAG( col_ImGui_CheckMark, 0xfffa9642, "ImGui check mark" )
_XTAG( col_ImGui_ChildBg, 0xffffff, "ImGui background of child windows" )
_XTAG( col_ImGui_CloseButton, 0x80696969, "ImGui closed button" )
_XTAG( col_ImGui_CloseButtonActive, 0xff5c63fa, "ImGui active close button" )
_XTAG( col_ImGui_CloseButtonHovered, 0xff5c63fa, "ImGui hovered close button" )
_XTAG( col_ImGui_Separator, 0xff9c9c9c, "ImGui column" )
_XTAG( col_ImGui_SeparatorActive, 0xfffa9642, "ImGui active column" )
_XTAG( col_ImGui_SeparatorHovered, 0xc7fa9642, "ImGui hovered column" )
_XTAG( col_ImGui_FrameBg, 0xff262626, "ImGui background of checkbox, radio button, plot, slider, text input" )
_XTAG( col_ImGui_FrameBgActive, 0xabfa9642, "ImGui active frame background" )
_XTAG( col_ImGui_FrameBgHovered, 0x66fa9642, "ImGui hovered frame background" )
_XTAG( col_ImGui_Header, 0xff1c1c1c, "ImGui header" )
_XTAG( col_ImGui_HeaderActive, 0xfffa9642, "ImGui active hader" )
_XTAG( col_ImGui_HeaderHovered, 0xccfa9642, "ImGui hovered header" )
_XTAG( col_ImGui_MenuBarBg, 0xff303030, "ImGui background menu bar" )
_XTAG( col_ImGui_ModalWindowDarkening, 0x59cccccc, "ImGui darken entire screen when a modal window is active" )
_XTAG( col_ImGui_PopupBg, 0xf0000000, "ImGui background of popups, menus, tooltips windows" )
_XTAG( col_ImGui_ResizeGrip, 0x40fa9642, "ImGui resize grip" )
_XTAG( col_ImGui_ResizeGripActive, 0xf2fa9642, "ImGui active resize grip" )
_XTAG( col_ImGui_ResizeGripHovered, 0xabfa9642, "ImGui hovered resize grip" )
_XTAG( col_ImGui_ScrollbarBg, 0x87050505, "ImGui scrollbar background" )
_XTAG( col_ImGui_ScrollbarGrab, 0xff4f4f4f, "ImGui scrollbar grab" )
_XTAG( col_ImGui_ScrollbarGrabActive, 0xff828282, "ImGui active scrollbar grab" )
_XTAG( col_ImGui_ScrollbarGrabHovered, 0xff696969, "ImGui hovered scrollbar grab" )
_XTAG( col_ImGui_SliderGrab, 0xffe0853d, "ImGui slider grab" )
_XTAG( col_ImGui_SliderGrabActive, 0xfffa9642, "ImGui active slider grab" )
