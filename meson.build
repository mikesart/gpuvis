project('gpuvis', 'c', 'cpp',
        version : '1.0.0',
        default_options : [
          'warning_level=2',
          'c_std=gnu99',
          'cpp_std=c++11',
          'default_library=static',
        ],
        license : 'MIT',
        meson_version : '>= 0.44.0')

add_project_arguments(['-Wno-unused-parameter',
                       '-Wno-sign-compare'],
                      language: ['cpp'])

if ['linux', 'sunos'].contains(host_machine.system())
  add_project_arguments(['-D_LARGEFILE64_SOURCE=1', '-D_FILE_OFFSET_BITS=64'],
                        language: ['c', 'cpp'])
endif

c = meson.get_compiler('c')
cc = meson.get_compiler('cpp')

all_deps = [
  cc.find_library('dl', required: false),
  dependency('sdl2'),
]
compile_flags = [
  '-Wno-unused-parameter'
]

if get_option('use_freetype')
  compile_flags += '-DUSE_FREETYPE=1'
  all_deps += dependency('freetype2')
endif

if get_option('use_gtk3')
  compile_flags += '-DUSE_GTK3=1'
  all_deps += dependency('gtk+-3.0')
endif

if get_option('use_i915_perf')
  compile_flags += '-DUSE_I915_PERF=1'
  all_deps += dependency('i915-perf',
                         version : '>= 1.5.0')
  all_deps += dependency('xe-oa')
endif

if get_option('have_rapidjson')
  compile_flags += '-DHAVE_RAPIDJSON=1'
  all_deps += dependency('RapidJSON')
endif

gpuvis_files = files(
  'src/gpuvis.cpp',
  'src/gpuvis_graph.cpp',
  'src/gpuvis_framemarkers.cpp',
  'src/gpuvis_plots.cpp',
  'src/gpuvis_graphrows.cpp',
  'src/gpuvis_ftrace_print.cpp',
  'src/gpuvis_i915_perfcounters.cpp',
  'src/gpuvis_utils.cpp',
  'src/gpuvis_etl.cpp',
  'src/etl_utils.cpp',
  'src/tdopexpr.cpp',
  'src/ya_getopt.c',
  'src/MurmurHash3.cpp',
  'src/miniz.c',
  'src/stlini.cpp',
  'src/imgui/imgui_impl_sdl_gl3.cpp',
  'src/imgui/imgui.cpp',
  'src/imgui/imgui_demo.cpp',
  'src/imgui/imgui_draw.cpp',
  'src/imgui/imgui_freetype.cpp',
  'src/GL/gl3w.c',
  'src/i915-perf/i915-perf-read.cpp',
  'src/libtraceevent/src/event-parse.c',
  'src/libtraceevent/src/event-parse-api.c',
  'src/libtraceevent/src/event-plugin.c',
  'src/libtraceevent/src/kbuffer-parse.c',
  'src/libtraceevent/src/parse-utils.c',
  'src/libtraceevent/src/trace-seq.c',
  'src/trace-cmd/trace-read.cpp',
)

incdir = include_directories('src/libtraceevent/include')

executable('gpuvis', gpuvis_files,
           c_args : compile_flags,
           cpp_args : compile_flags,
           dependencies : all_deps,
           install : true,
           include_directories : incdir)
